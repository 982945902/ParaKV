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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include "parakv/core/index/key.h"
#include "parakv/core/index/offset.h"
#include "parakv/core/index/wal.h"
#include "parakv/core/segment/segment_base.h"
#include "parakv/core/segment/segment_manager.h"
#include "parallel_hashmap/phmap.h"

namespace parakv {
namespace index {

// ---------------------------------------------------------------------------
// In-memory index for ParaKV.
//
// The class is parameterised on the user key type:
//
//   Index<Key64>   -- parameter-server-style workloads (64-bit id / sign)
//   Index<Key128>  -- LLM KVCache workloads (128-bit prefix hash)
//
// |KeyT| must be trivially copyable so that the raw bytes can be written
// into segment slots and WAL records without any serialization step.
//
// Design (see docs/zh/design/index.md):
//   Map: KeyT  ->  encoded offset (uint64_t)
//
// The underlying container is phmap::parallel_flat_hash_map_m with 2^N
// internal sub-maps, each guarded by its own std::mutex.  This gives
// near-linear scalability on multi-core workloads because different keys
// (hashing to different sub-maps) never contend.
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
//   Load(snapshot) + Replay(wal) reconstructs the map.
//
// Raw API:
//   PutRaw / GetRaw / DeleteRaw accept (const void*, size_t) and are the
//   recommended entry point for RPC handlers that do not statically know
//   the key type.  |key_size| is validated against sizeof(KeyT) at runtime.
// ---------------------------------------------------------------------------
template <typename KeyT = Key64,
          typename Hash = phmap::priv::hash_default_hash<KeyT>,
          typename Eq = phmap::priv::hash_default_eq<KeyT>>
class Index : public std::enable_shared_from_this<Index<KeyT, Hash, Eq>> {
 public:
  static_assert(std::is_trivially_copyable<KeyT>::value,
                "Index KeyT must be trivially copyable");

  using key_type = KeyT;

  struct Options {
    // Segment storage backing this index.  Must outlive the Index.
    std::shared_ptr<segment::SegmentManager> segment_manager;

    // Path to the WAL file.  If empty, the index runs without WAL (volatile).
    std::string wal_path;

    // Config describing slot key/value sizes.  |segment_config.key_size|
    // must equal sizeof(KeyT); this is checked at Open() time.
    segment::SegmentConfig segment_config;

    // Rotate the WAL (checkpoint) once it grows beyond this many bytes.
    // Zero disables automatic checkpointing.
    uint64_t wal_checkpoint_bytes = 1ULL << 30;  // 1 GiB

    // Directory for checkpoint / snapshot files.  Snapshot name =
    // <dir>/index.snapshot[.tmp].  Required if wal_path is set.
    std::string snapshot_dir;

    // Namespace name for the index.
    std::string namespace_name;
  };

  explicit Index(Options opts);
  ~Index();

  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;

  // Open WAL (if configured) and, if a snapshot + WAL exist, recover the
  // in-memory map.  Safe to call once per Index instance.
  Status Open();

  // Flush WAL and close the writer.
  Status Close();

  // ---- Typed data path ----------------------------------------------------

  // Write |value| to an active segment, update the index, persist via WAL.
  Status Put(const KeyT& key, const void* value, size_t value_size);

  // Read value for |key| into |value_out|.  |value_size| must match config.
  Status Get(const KeyT& key, void* value_out, size_t value_size);

  // Check if |key| is present in the index.
  bool Contains(const KeyT& key) const;

  // Remove |key| from index and free its slot in the owning segment.
  Status Delete(const KeyT& key);

  // Low-level lookup of the encoded offset. Returns kNotFound if absent.
  Status Lookup(const KeyT& key, uint64_t* encoded_offset) const;

  // ---- Type-erased data path ----------------------------------------------
  //
  // These overloads are meant for RPC handlers and other dynamic dispatch
  // sites that do not know |KeyT| at compile time.  |key_size| must equal
  // sizeof(KeyT); a mismatch yields kInvalidArgument.
  //
  // The |key_input| buffer is memcpy'd into a KeyT value, so callers do not
  // need to retain the buffer after the call returns.

  Status PutRaw(const void* key_input, size_t key_size, const void* value,
                size_t value_size);

  Status GetRaw(const void* key_input, size_t key_size, void* value_out,
                size_t value_size);

  Status DeleteRaw(const void* key_input, size_t key_size);

  Status LookupRaw(const void* key_input, size_t key_size,
                   uint64_t* encoded_offset) const;

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
  Status RemapIfEquals(const KeyT& key, uint64_t old_off, uint64_t new_off);

  // ---- Enumeration ---------------------------------------------------------

  // Invoke |callback| for every key currently in the index.  The callback
  // receives a const reference and must not mutate the index.  Iteration
  // order is unspecified.  Internally locks each phmap submap in turn.
  void ForEachKey(const std::function<void(const KeyT&)>& callback) const;

  // ---- Stats --------------------------------------------------------------

  size_t Size() const;
  uint64_t WalBytes() const;

  // Size in bytes of the user key, exposed so that raw-API callers can size
  // their buffers correctly.
  static constexpr size_t key_size() { return sizeof(KeyT); }

 private:
  // 16 submaps (N=4) -> 2^4 partitions, each with its own std::mutex.
  static constexpr size_t kSubmapLog2 = 4;
  using Map = phmap::parallel_flat_hash_map_m<
      KeyT, uint64_t, Hash, Eq,
      phmap::priv::Allocator<phmap::priv::Pair<const KeyT, uint64_t>>,
      kSubmapLog2>;

  // Free the slot that |encoded| points to.  Only SegmentSlot entries are
  // reclaimable; other flag types are no-ops here.
  Status FreeSlot(uint64_t encoded);

  // Write |value| into an active segment and return the encoded offset.
  Status WriteToSegment(const KeyT& key, const void* value, uint64_t* encoded);

  // Read the key stored at the slot identified by |encoded| (SegmentSlot only).
  Status ReadKeyAtEncoded(uint64_t encoded, KeyT* key_out);

  // WAL append helper; no-op when WAL is disabled.
  Status AppendWal(WalRecordType type, const KeyT& key, uint64_t disk_addr,
                   uint64_t old_disk_addr);

  // Apply a replayed record to the map (used during Open()).
  void ApplyRecord(const WalRecord<KeyT>& rec);

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

// Convenient type aliases for the two supported key widths.
using Index64 = Index<Key64>;
using Index128 = Index<Key128, Key128Hash, Key128Eq>;

}  // namespace index
}  // namespace parakv
