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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "parakv/core/index/offset.h"
#include "parakv/core/index/wal.h"
#include "parakv/core/segment/segment_base.h"
#include "parakv/core/segment/segment_manager.h"
#include "parallel_hashmap/phmap.h"

namespace parakv {
namespace index {

using segment::Status;

// ---------------------------------------------------------------------------
// In-memory index for ParaKV.
//
// Design (see docs/zh/design/index.md):
//   Map: key (uint64_t)  ->  encoded offset (uint64_t)
//
// The underlying container is phmap::parallel_flat_hash_map with 2^N internal
// sub-maps, each guarded by its own std::mutex.  This gives near-linear
// scalability on multi-core workloads because different keys (hashing to
// different sub-maps) never contend.
//
// The index is paired with a segment::SegmentManager that owns the actual
// key/value bytes:
//
//   Put(k, v):
//     1. active = segment_manager_->GetActiveSegment()
//     2. active->Insert(k, v, &slot_id)
//     3. encoded = OffsetCodec::EncodeSegmentSlot(seg_id, slot_id)
//     4. WAL append Insert/Update
//     5. map_.insert_or_assign(k, encoded)
//     6. if Update, free old slot on the old segment
//
// Crash recovery:
//   Load(snapshot) + Replay(wal) reconstructs the map.  Snapshot uses phmap's
//   BinaryOutputArchive; WAL format is in wal.h.
// ---------------------------------------------------------------------------
class Index {
 public:
  struct Options {
    // Segment storage backing this index.  Must outlive the Index.
    std::shared_ptr<segment::SegmentManager> segment_manager;

    // Path to the WAL file.  If empty, the index runs without WAL (volatile).
    std::string wal_path;

    // Default config describing slot key/value sizes.  Used to size read/write
    // buffers and to sanity-check Put() payloads.
    segment::SegmentConfig segment_config;

    // Rotate the WAL (checkpoint) once it grows beyond this many bytes.
    // Zero disables automatic checkpointing.
    uint64_t wal_checkpoint_bytes = 1ULL << 30;  // 1 GiB

    // Directory for checkpoint / snapshot files.  Snapshot name =
    // <dir>/index.snapshot[.tmp].  Required if wal_path is set.
    std::string snapshot_dir;
  };

  explicit Index(Options opts);
  ~Index();

  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;

  // Open WAL (if configured) and, if a snapshot + WAL exist, recover the
  // in-memory map.  Safe to call once per Index instance.
  Status Open();

  // Flush WAL + close.
  Status Close();

  // ---- Data path ----------------------------------------------------------

  // Write value to an active segment, update index, persist via WAL.
  // |value_size| must equal segment_config.value_size.
  Status Put(uint64_t key, const void* value, size_t value_size);

  // Read value for |key| into |value_out|.  |value_size| must match config.
  Status Get(uint64_t key, void* value_out, size_t value_size);

  // Remove |key| from index and free its slot in the owning segment.
  Status Delete(uint64_t key);

  // Low-level lookup of the encoded offset. Returns kNotFound if absent.
  Status Lookup(uint64_t key, uint64_t* encoded_offset) const;

  // ---- Persistence --------------------------------------------------------

  // Take a consistent snapshot of the map and rotate the WAL so future
  // recovery starts from this snapshot. Thread-safe w.r.t. Put/Delete.
  Status Checkpoint();

  // Write a snapshot file at |path| (without touching the WAL).
  Status DumpSnapshot(const std::string& path) const;

  // Load a snapshot file at |path|; replaces current contents.
  Status LoadSnapshot(const std::string& path);

  // ---- Compaction integration --------------------------------------------

  // Register with the segment manager so that slot migrations update index.
  // Should be called once, after Open().  The callback reads the migrated
  // key out of the destination slot to find the right entry to rewrite.
  void AttachToSegmentManager();

  // Update the encoded offset for |key| from |old_off| to |new_off| iff the
  // current mapping equals |old_off|.  Used by compaction.
  Status RemapIfEquals(uint64_t key, uint64_t old_off, uint64_t new_off);

  // ---- Stats --------------------------------------------------------------

  size_t Size() const;
  uint64_t WalBytes() const;

 private:
  // 16 submaps (N=4) -> 2^4 partitions, each with its own std::mutex.
  // Using parallel_flat_hash_map_m which defaults to std::mutex for internal
  // per-submap locking, enabling safe concurrent Put/Get/Delete without an
  // external lock.
  static constexpr size_t kSubmapLog2 = 4;
  using Map = phmap::parallel_flat_hash_map_m<
      uint64_t, uint64_t, phmap::priv::hash_default_hash<uint64_t>,
      phmap::priv::hash_default_eq<uint64_t>,
      phmap::priv::Allocator<phmap::priv::Pair<const uint64_t, uint64_t>>,
      kSubmapLog2>;

  // Internal: free the slot that |encoded| points to.  Only SegmentSlot
  // entries are reclaimable; other flag types are no-ops here.
  Status FreeSlot(uint64_t encoded);

  // Internal: write value into an active segment, return encoded offset.
  Status WriteToSegment(uint64_t key, const void* value, uint64_t* encoded);

  // Internal: read the key stored at |encoded| (SegmentSlot only).
  Status ReadKeyAtEncoded(uint64_t encoded, uint64_t* key_out);

  // WAL append helper; no-op when WAL is disabled.
  Status AppendWal(WalRecordType t, uint64_t key, uint64_t disk_addr,
                   uint64_t old_disk_addr);

  // Apply a replayed record to the map (used during Open()).
  void ApplyRecord(const WalRecord& rec);

  // Rotate the WAL after a successful snapshot.
  Status RotateWal();

  Options opts_;
  Map map_;
  std::unique_ptr<WalWriter> wal_;
  std::atomic<uint64_t> wal_bytes_;

  // Serialize checkpoint against itself (not against Put/Delete -- those
  // rely on per-submap locking in phmap).
  std::mutex checkpoint_mutex_;
  std::atomic<bool> opened_;
};

}  // namespace index
}  // namespace parakv
