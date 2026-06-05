/* Copyright 2026 The ParaKV Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://gitcode.com/xLLM-AI/ParaKV/blob/main/LICENSE

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "parakv/core/index/index.h"

#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "parallel_hashmap/phmap_dump.h"

namespace parakv {
namespace index {

namespace {

bool FileExists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

std::string SnapshotPath(const std::string& dir) {
  if (dir.empty()) {
    return {};
  }
  if (dir.back() == '/') {
    return dir + "index.snapshot";
  }
  return dir + "/index.snapshot";
}

std::string SnapshotTmpPath(const std::string& dir) {
  return SnapshotPath(dir) + ".tmp";
}

}  // namespace

template <typename KeyT, typename Hash, typename Eq>
Index<KeyT, Hash, Eq>::Index(Options opts)
    : opts_(std::move(opts)), wal_bytes_(0), opened_(false) {}

template <typename KeyT, typename Hash, typename Eq>
Index<KeyT, Hash, Eq>::~Index() {
  // Best-effort persistence as a safety net. Callers should invoke
  // Checkpoint()/Close() explicitly during graceful shutdown so this path
  // does not have to fire during static destruction (where logging or
  // file I/O may be unreliable).
  //
  // Order matters: Checkpoint() needs an open WAL writer to rotate
  // (truncate) it after dumping the snapshot. Calling Close() first would
  // make RotateWal() observe wal_==nullptr and skip the rotation, leaving
  // a stale WAL behind.
  (void)Checkpoint();
  (void)Close();
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Open() {
  if (opened_.exchange(true)) {
    return Status::kOk;
  }

  // The segment layer stores raw bytes; its configured key size must match
  // the index's compile-time KeyT so that slots round-trip losslessly.
  if (opts_.segment_config.key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }

  // 1. Load snapshot if present.
  if (!opts_.snapshot_dir.empty()) {
    const std::string snap = SnapshotPath(opts_.snapshot_dir);
    if (FileExists(snap)) {
      const Status s = LoadSnapshot(snap);
      if (s != Status::kOk) {
        return s;
      }
    }
  }

  // 2. Replay the WAL if present.
  if (!opts_.wal_path.empty() && FileExists(opts_.wal_path)) {
    WalReader reader;
    Status s = reader.Open(opts_.wal_path);
    if (s != Status::kOk) {
      return s;
    }
    s = reader.template Replay<KeyT>(
        [this](const WalRecord<KeyT>& rec) { ApplyRecord(rec); });
    if (s != Status::kOk) {
      reader.Close();
      return s;
    }
    const uint64_t good = reader.LastGoodOffset();
    reader.Close();

    // Truncate any corrupt tail so that subsequent appends start from the
    // last known-good record.
    if (::truncate(opts_.wal_path.c_str(), static_cast<off_t>(good)) != 0) {
      return Status::kIOError;
    }
  }

  // 3. Open the WAL writer for subsequent appends.
  if (!opts_.wal_path.empty()) {
    wal_ = std::make_unique<WalWriter>();
    const Status s = wal_->Open(opts_.wal_path);
    if (s != Status::kOk) {
      return s;
    }
    wal_bytes_.store(wal_->BytesWritten());
  }

  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Close() {
  if (!opened_.exchange(false)) {
    return Status::kOk;
  }
  if (wal_ != nullptr) {
    (void)wal_->Close();
    wal_.reset();
  }
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Put(const KeyT& key, const void* value,
                                  size_t value_size) {
  if (value == nullptr) {
    return Status::kInvalidArgument;
  }
  if (value_size != opts_.segment_config.value_size) {
    return Status::kInvalidArgument;
  }
  if (opts_.segment_manager == nullptr) {
    return Status::kInvalidArgument;
  }

  uint64_t new_enc = 0;
  Status s = WriteToSegment(key, value, &new_enc);
  if (s != Status::kOk) {
    return s;
  }

  // Install in the map, capturing any previous encoded offset atomically
  // inside the sub-map's write lock.
  bool had_old = false;
  uint64_t old_enc = 0;
  map_.lazy_emplace_l(
      key,
      [&](typename Map::value_type& v) {
        had_old = true;
        old_enc = v.second;
        v.second = new_enc;
      },
      [&](const auto& ctor) { ctor(key, new_enc); });

  // Persist via WAL (Insert for a fresh key, Update otherwise).
  const Status wal_s =
      AppendWal(had_old ? WalRecordType::kUpdate : WalRecordType::kInsert, key,
                new_enc, had_old ? old_enc : 0);
  if (wal_s != Status::kOk) {
    // Best-effort rollback: revert the map and free the newly written slot.
    if (had_old) {
      map_.modify_if(key,
                     [&](typename Map::value_type& v) { v.second = old_enc; });
    } else {
      map_.erase(key);
    }
    (void)FreeSlot(new_enc);
    return wal_s;
  }

  if (had_old) {
    (void)FreeSlot(old_enc);
  }
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Get(const KeyT& key, void* value_out,
                                  size_t value_size) {
  if (value_out == nullptr) {
    return Status::kInvalidArgument;
  }
  if (value_size != opts_.segment_config.value_size) {
    return Status::kInvalidArgument;
  }

  uint64_t enc = 0;
  const Status s = Lookup(key, &enc);
  if (s != Status::kOk) {
    return s;
  }

  if (OffsetCodec::GetFlag(enc) != OffsetFlag::kSegmentSlot) {
    // Other storage tiers (memory, raw file offset) are not yet wired up in
    // this reference implementation.
    return Status::kNotFound;
  }

  const uint32_t seg_id = OffsetCodec::GetSegmentId(enc);
  const uint32_t slot_id = OffsetCodec::GetSlotId(enc);
  auto seg = opts_.segment_manager->GetSegment(seg_id);
  if (seg == nullptr) {
    return Status::kNotFound;
  }

  std::vector<uint8_t> key_buf(opts_.segment_config.key_size);
  return seg->Read(slot_id, key_buf.data(), value_out);
}

template <typename KeyT, typename Hash, typename Eq>
bool Index<KeyT, Hash, Eq>::Contains(const KeyT& key) const {
  return map_.contains(key);
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Delete(const KeyT& key) {
  // Atomically remove while capturing the previous encoded offset under the
  // sub-map's write lock.
  bool found = false;
  uint64_t old_enc = 0;
  map_.erase_if(key, [&](const typename Map::value_type& v) {
    found = true;
    old_enc = v.second;
    return true;
  });
  if (!found) {
    return Status::kNotFound;
  }

  const Status s = AppendWal(WalRecordType::kDelete, key, 0, old_enc);
  if (s != Status::kOk) {
    // Best-effort restore on WAL fsync failure.
    map_.try_emplace(key, old_enc);
    return s;
  }

  (void)FreeSlot(old_enc);
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Lookup(const KeyT& key,
                                     uint64_t* encoded_offset) const {
  if (encoded_offset == nullptr) {
    return Status::kInvalidArgument;
  }
  bool found = false;
  map_.if_contains(key, [&](const typename Map::value_type& v) {
    *encoded_offset = v.second;
    found = true;
  });
  return found ? Status::kOk : Status::kNotFound;
}

// ---- Raw (type-erased) data path ------------------------------------------

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::PutRaw(const void* key_input, size_t key_size,
                                     const void* value, size_t value_size) {
  if (key_input == nullptr || key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }
  KeyT key;
  std::memcpy(&key, key_input, sizeof(KeyT));
  return Put(key, value, value_size);
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::GetRaw(const void* key_input, size_t key_size,
                                     void* value_out, size_t value_size) {
  if (key_input == nullptr || key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }
  KeyT key;
  std::memcpy(&key, key_input, sizeof(KeyT));
  return Get(key, value_out, value_size);
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::DeleteRaw(const void* key_input,
                                        size_t key_size) {
  if (key_input == nullptr || key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }
  KeyT key;
  std::memcpy(&key, key_input, sizeof(KeyT));
  return Delete(key);
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::LookupRaw(const void* key_input, size_t key_size,
                                        uint64_t* encoded_offset) const {
  if (key_input == nullptr || key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }
  KeyT key;
  std::memcpy(&key, key_input, sizeof(KeyT));
  return Lookup(key, encoded_offset);
}

// ---- Persistence ----------------------------------------------------------

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::Checkpoint() {
  LOG(INFO) << opts_.namespace_name << " checkpointing";

  std::lock_guard<std::mutex> lock(checkpoint_mutex_);

  if (opts_.snapshot_dir.empty()) {
    LOG(INFO) << opts_.namespace_name
              << " checkpoint failed: snapshot_dir is empty";
    return Status::kInvalidArgument;
  }

  const std::string final_path = SnapshotPath(opts_.snapshot_dir);
  const std::string tmp_path = SnapshotTmpPath(opts_.snapshot_dir);

  const Status s = DumpSnapshot(tmp_path);
  if (s != Status::kOk) {
    return s;
  }

  // Atomic replace: the new snapshot becomes visible only after rename(2).
  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    return Status::kIOError;
  }

  return RotateWal();
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::DumpSnapshot(const std::string& path) const {
  LOG(INFO) << opts_.namespace_name << " dumping snapshot to " << path;

  phmap::BinaryOutputArchive ar(path.c_str());
  // parallel_flat_hash_map::phmap_dump iterates over all sub-maps, briefly
  // holding each sub-map's lock, so writers on other sub-maps keep running.
  if (!map_.phmap_dump(ar)) {
    return Status::kIOError;
  }

  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::LoadSnapshot(const std::string& path) {
  phmap::BinaryInputArchive ar(path.c_str());
  Map fresh;
  if (!fresh.phmap_load(ar)) {
    return Status::kCorruption;
  }
  map_.swap(fresh);
  return Status::kOk;
}

// ---- Compaction integration -----------------------------------------------

template <typename KeyT, typename Hash, typename Eq>
void Index<KeyT, Hash, Eq>::AttachToSegmentManager() {
  if (opts_.segment_manager == nullptr) {
    return;
  }
  const std::weak_ptr<Index<KeyT, Hash, Eq>> weak_self = this->weak_from_this();

  // When a slot moves during compaction, read the key stored at the new slot
  // to identify the index entry, then rewrite it with the new encoded offset.
  opts_.segment_manager->SetSlotMoveCallback([weak_self](void* key,
                                                         uint32_t old_seg,
                                                         uint32_t old_slot,
                                                         uint32_t new_seg,
                                                         uint32_t new_slot) {
    auto self = weak_self.lock();
    if (!self) {
      return;
    }
    const uint64_t old_enc = OffsetCodec::EncodeSegmentSlot(old_seg, old_slot);
    const uint64_t new_enc = OffsetCodec::EncodeSegmentSlot(new_seg, new_slot);

    // KeyT key{};
    // if (self->ReadKeyAtEncoded(new_enc, &key) != Status::kOk) {
    //   return;
    // }
    KeyT* key_ptr = static_cast<KeyT*>(key);

    // Compaction updates must also be durably logged.
    (void)self->AppendWal(WalRecordType::kUpdate, *key_ptr, new_enc, old_enc);
    (void)self->RemapIfEquals(*key_ptr, old_enc, new_enc);
  });
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::RemapIfEquals(const KeyT& key, uint64_t old_off,
                                            uint64_t new_off) {
  bool updated = false;
  map_.modify_if(key, [&](typename Map::value_type& v) {
    if (v.second == old_off) {
      v.second = new_off;
      updated = true;
    }
  });
  return updated ? Status::kOk : Status::kNotFound;
}

// ---- Stats ----------------------------------------------------------------

template <typename KeyT, typename Hash, typename Eq>
void Index<KeyT, Hash, Eq>::ForEachKey(
    const std::function<void(const KeyT&)>& callback) const {
  map_.for_each([&](const typename Map::value_type& v) { callback(v.first); });
}

template <typename KeyT, typename Hash, typename Eq>
size_t Index<KeyT, Hash, Eq>::Size() const {
  return map_.size();
}

template <typename KeyT, typename Hash, typename Eq>
uint64_t Index<KeyT, Hash, Eq>::WalBytes() const {
  return wal_bytes_.load();
}

// ---- Internal -------------------------------------------------------------

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::FreeSlot(uint64_t encoded) {
  if (OffsetCodec::GetFlag(encoded) != OffsetFlag::kSegmentSlot) {
    return Status::kOk;
  }
  const uint32_t seg_id = OffsetCodec::GetSegmentId(encoded);
  const uint32_t slot_id = OffsetCodec::GetSlotId(encoded);
  auto seg = opts_.segment_manager->GetSegment(seg_id);
  if (seg == nullptr) {
    return Status::kNotFound;
  }
  return seg->Delete(slot_id);
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::WriteToSegment(const KeyT& key, const void* value,
                                             uint64_t* encoded) {
  auto seg = opts_.segment_manager->GetActiveSegment();
  if (seg == nullptr) {
    return Status::kNoSpace;
  }

  uint32_t slot_id = 0;
  Status s = seg->Insert(&key, value, &slot_id);
  if (s == Status::kFull) {
    opts_.segment_manager->MarkSegmentFull(seg->GetSegmentId());
    seg = opts_.segment_manager->GetActiveSegment();
    if (seg == nullptr) {
      return Status::kNoSpace;
    }
    s = seg->Insert(&key, value, &slot_id);
  }
  if (s != Status::kOk) {
    return s;
  }

  *encoded = OffsetCodec::EncodeSegmentSlot(seg->GetSegmentId(), slot_id);
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::ReadKeyAtEncoded(uint64_t encoded,
                                               KeyT* key_out) {
  if (OffsetCodec::GetFlag(encoded) != OffsetFlag::kSegmentSlot) {
    return Status::kInvalidArgument;
  }
  if (opts_.segment_config.key_size != sizeof(KeyT)) {
    return Status::kInvalidArgument;
  }

  const uint32_t seg_id = OffsetCodec::GetSegmentId(encoded);
  const uint32_t slot_id = OffsetCodec::GetSlotId(encoded);
  auto seg = opts_.segment_manager->GetSegment(seg_id);
  if (seg == nullptr) {
    return Status::kNotFound;
  }

  std::vector<uint8_t> key_buf(opts_.segment_config.key_size);
  std::vector<uint8_t> val_buf(opts_.segment_config.value_size);
  const Status s = seg->Read(slot_id, key_buf.data(), val_buf.data());
  if (s != Status::kOk) {
    return s;
  }

  std::memcpy(key_out, key_buf.data(), sizeof(KeyT));
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::AppendWal(WalRecordType type, const KeyT& key,
                                        uint64_t disk_addr,
                                        uint64_t old_disk_addr) {
  if (wal_ == nullptr) {
    return Status::kOk;
  }

  WalRecord<KeyT> rec;
  rec.type = type;
  rec.key = key;
  rec.disk_addr = disk_addr;
  rec.old_disk_addr = old_disk_addr;

  const Status s = wal_->Append(rec);
  if (s != Status::kOk) {
    return s;
  }

  const uint64_t bytes = wal_->BytesWritten();
  wal_bytes_.store(bytes);

  // Auto-checkpoint once the WAL grows past the configured threshold.
  if (opts_.wal_checkpoint_bytes > 0 && bytes >= opts_.wal_checkpoint_bytes &&
      !opts_.snapshot_dir.empty()) {
    // A failed checkpoint is non-fatal here: we keep appending to the
    // existing WAL and will retry on the next threshold crossing.
    (void)Checkpoint();
  }
  return Status::kOk;
}

template <typename KeyT, typename Hash, typename Eq>
void Index<KeyT, Hash, Eq>::ApplyRecord(const WalRecord<KeyT>& rec) {
  switch (rec.type) {
    case WalRecordType::kInsert:
    case WalRecordType::kUpdate:
      map_.insert_or_assign(rec.key, rec.disk_addr);
      break;
    case WalRecordType::kDelete:
      map_.erase(rec.key);
      break;
  }
}

template <typename KeyT, typename Hash, typename Eq>
Status Index<KeyT, Hash, Eq>::RotateWal() {
  if (wal_ == nullptr) {
    return Status::kOk;
  }
  (void)wal_->Close();
  wal_.reset();

  auto fresh = std::make_unique<WalWriter>();
  const Status s = fresh->Open(opts_.wal_path, /*truncate=*/true);
  if (s != Status::kOk) {
    return s;
  }
  wal_ = std::move(fresh);
  wal_bytes_.store(0);
  return Status::kOk;
}

// Explicit instantiations for the supported key types.  Adding a new key
// width is a one-line addition below.
template class Index<Key64>;
template class Index<Key128, Key128Hash, Key128Eq>;

}  // namespace index
}  // namespace parakv
