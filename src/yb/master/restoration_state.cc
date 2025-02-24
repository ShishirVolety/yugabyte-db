// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#include "yb/docdb/key_bytes.h"
#include "yb/docdb/value_type.h"

#include "yb/master/restoration_state.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/master_backup.pb.h"
#include "yb/master/snapshot_coordinator_context.h"
#include "yb/master/snapshot_state.h"

#include "yb/tserver/tserver_error.h"
#include "yb/util/flags.h"
#include "yb/util/pb_util.h"

DEFINE_RUNTIME_int64(max_concurrent_restoration_rpcs, -1,
    "Maximum number of tablet restoration rpcs that can be outstanding. "
    "Only used if its value is >= 0. Value of 0 means that "
    "INT_MAX number of restoration rpcs can be concurrent."
    "If its value is < 0 then max_concurrent_restoration_rpcs_per_tserver "
    "gflag is used.");

DEFINE_RUNTIME_int64(max_concurrent_restoration_rpcs_per_tserver, 1,
    "Maximum number of tablet restoration rpcs per tserver that can be outstanding."
    "Only used if the value of gflag max_concurrent_restoration_rpcs is < 0. "
    "When used it is multiplied with the number of TServers in the active cluster "
    "(not read-replicas) to obtain the total maximum concurrent restoration rpcs. If "
    "the cluster config is not found and we are not able to determine the number of "
    "live tservers then the total maximum concurrent restoration RPCs is just the "
    "value of this flag.");

#include "yb/util/result.h"

namespace yb {
namespace master {

namespace {

std::string MakeRestorationStateLogPrefix(
    const TxnSnapshotRestorationId& restoration_id, SnapshotState* snapshot) {
  if (snapshot->schedule_id()) {
    return Format("Restoration[$0/$1/$2]: ",
                  restoration_id, snapshot->id(), snapshot->schedule_id());
  }
  return Format("Restoration[$0/$1]: ", restoration_id, snapshot->id());
}

std::string MakeRestorationStateLogPrefix(
    const TxnSnapshotRestorationId& restoration_id, const Result<TxnSnapshotId>& snapshot_id,
    const Result<SnapshotScheduleId>& schedule_id) {
  if (!snapshot_id.ok() || !schedule_id.ok()) {
    return Format("Restoration[$0]: ", restoration_id);
  }
  if (*schedule_id) {
    return Format("Restoration[$0/$1/$2]: ",
                  restoration_id, *snapshot_id, *schedule_id);
  }
  return Format("Restoration[$0/$1]: ", restoration_id, *snapshot_id);
}

} // namespace

RestorationState::RestorationState(
    SnapshotCoordinatorContext* context, const TxnSnapshotRestorationId& restoration_id,
    SnapshotState* snapshot, HybridTime restore_at, IsSysCatalogRestored is_sys_catalog_restored,
    uint64_t throttle_limit)
    : StateWithTablets(context, SysSnapshotEntryPB::RESTORING,
                       MakeRestorationStateLogPrefix(restoration_id, snapshot)),
      restoration_id_(restoration_id), snapshot_id_(snapshot->id()),
      schedule_id_(snapshot->schedule_id()),
      is_sys_catalog_restored_(is_sys_catalog_restored), restore_at_(restore_at),
      throttler_(throttle_limit), version_(1) {
  InitTabletIds(snapshot->TabletIdsInState(SysSnapshotEntryPB::COMPLETE));
}

RestorationState::RestorationState(
    SnapshotCoordinatorContext* context, const TxnSnapshotRestorationId& restoration_id,
    const SysRestorationEntryPB& entry)
    : StateWithTablets(context, entry.state(), MakeRestorationStateLogPrefix(
        restoration_id, FullyDecodeTxnSnapshotId(entry.snapshot_id()),
        FullyDecodeSnapshotScheduleId(entry.schedule_id()))),
      restoration_id_(restoration_id),
      is_sys_catalog_restored_(entry.is_sys_catalog_restored()),
      restore_at_(HybridTime::FromPB(entry.restore_at_ht())), version_(entry.version()) {
  auto snapshot_id = FullyDecodeTxnSnapshotId(entry.snapshot_id());
  if (snapshot_id.ok()) {
    snapshot_id_ = *snapshot_id;
  }
  auto schedule_id = FullyDecodeSnapshotScheduleId(entry.schedule_id());
  if (schedule_id.ok()) {
    schedule_id_ = *schedule_id;
  }
  InitTablets(entry.tablet_restorations());
  if (entry.has_complete_time_ht()) {
    Status s = complete_time_.FromUint64(entry.complete_time_ht());
    if (!s.ok()) {
      LOG(WARNING) << "Error loading complete time of restoration " << restoration_id;
    }
  }
  for (const auto& id_and_type : entry.master_metadata()) {
    master_metadata_.emplace(id_and_type.id(), id_and_type.type());
  }
}

Status RestorationState::ToPB(RestorationInfoPB* out) {
  out->set_id(restoration_id_.data(), restoration_id_.size());
  return ToEntryPB(out->mutable_entry());
}

void RestorationState::PrepareOperations(
    TabletRestoreOperations* operations, const std::unordered_set<TabletId>& snapshot_tablets,
    std::optional<int64_t> db_oid) {
  DoPrepareOperations(
      [this, &operations, &snapshot_tablets, &db_oid](const TabletData& data) -> bool {
    if (Throttler().Throttle()) {
      return false;
    }
    operations->push_back(TabletRestoreOperation {
      .tablet_id = data.id,
      .restoration_id = restoration_id_,
      .snapshot_id = snapshot_id_,
      .restore_at = restore_at_,
      .sys_catalog_restore_needed = !schedule_id_.IsNil(),
      .is_tablet_part_of_snapshot = snapshot_tablets.count(data.id) != 0,
      .db_oid = db_oid,
      .schedule_id = schedule_id_,
    });
    return true;
  });
}

bool RestorationState::IsTerminalFailure(const Status& status) {
  return status.IsAborted() ||
         tserver::TabletServerError(status) == tserver::TabletServerErrorPB::INVALID_SNAPSHOT;
}

Status RestorationState::ToEntryPB(SysRestorationEntryPB* out) {
  out->set_state(VERIFY_RESULT(AggregatedState()));
  if (complete_time_) {
    out->set_complete_time_ht(complete_time_.ToUint64());
  }

  TabletsToPB(out->mutable_tablet_restorations());
  out->set_snapshot_id(snapshot_id_.data(), snapshot_id_.size());
  out->set_schedule_id(schedule_id_.data(), schedule_id_.size());
  out->set_is_sys_catalog_restored(is_sys_catalog_restored_);
  out->set_restore_at_ht(restore_at_.ToUint64());
  out->set_version(version_);
  // Master metadata.
  for (const auto& id_and_type : master_metadata_) {
    auto* entry = out->add_master_metadata();
    entry->set_id(id_and_type.first);
    entry->set_type(id_and_type.second);
  }
  return Status::OK();
}

Status RestorationState::StoreToWriteBatch(docdb::KeyValueWriteBatchPB* write_batch) {
  auto pair = write_batch->add_write_pairs();
  return StoreToKeyValuePair(pair);
}

Status RestorationState::StoreToKeyValuePair(docdb::KeyValuePairPB* pair) {
  ++version_;
  docdb::KeyBytes encoded_key = VERIFY_RESULT(
      EncodedKey(SysRowEntryType::SNAPSHOT_RESTORATION, restoration_id_.AsSlice(), &context()));
  pair->set_key(encoded_key.AsSlice().cdata(), encoded_key.size());
  faststring value;
  value.push_back(docdb::ValueEntryTypeAsChar::kString);
  SysRestorationEntryPB entry;
  RETURN_NOT_OK(ToEntryPB(&entry));
  RETURN_NOT_OK(pb_util::AppendToString(entry, &value));
  pair->set_value(value.data(), value.size());
  return Status::OK();
}

} // namespace master
} // namespace yb
