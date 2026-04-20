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

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "parakv/core/segment/segment_base.h"

namespace parakv {
namespace index {

// WAL record for uint64_t keys. Layout (little-endian) matches the table in
// docs/zh/design/index.md (32-byte aligned):
//
//   offset  size  field
//   0       1     magic (kMagic)
//   1       1     type  (WalRecordType)
//   2       8     key
//   10      8     disk_addr (encoded offset)
//   18      8     old_disk_addr (encoded offset, 0 if not applicable)
//   26      4     crc32 of the preceding 26 bytes
//   30      2     padding (zero) -> total 32 bytes
//
// Fixed-length records greatly simplify recovery scans.
enum class WalRecordType : uint8_t {
  kInsert = 0x01,
  kUpdate = 0x02,
  kDelete = 0x03,
};

struct WalRecord {
  static constexpr uint8_t kMagic = 0xA5;
  static constexpr uint32_t kEncodedSize = 32;

  WalRecordType type = WalRecordType::kInsert;
  uint64_t key = 0;
  uint64_t disk_addr = 0;
  uint64_t old_disk_addr = 0;

  // Serialize into a 32-byte buffer. Computes CRC32 and pads to alignment.
  void Encode(uint8_t out[kEncodedSize]) const;

  // Parse from a 32-byte buffer. Returns false if magic or CRC32 mismatch.
  static bool Decode(const uint8_t in[kEncodedSize], WalRecord* out);
};

// Append-only WAL file.
//
// All append operations are serialized by an internal mutex.  Records are
// flushed + fsync'd before return to guarantee durability ordering with the
// in-memory index update:
//
//     WAL.Append(rec)  ->  fsync  ->  map_[key] = rec.disk_addr
//
// Use OpenForAppend() when the caller owns the WAL rotation policy and wants
// to keep writing; use OpenForRead() when replaying during recovery.
class WalWriter {
 public:
  WalWriter();
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Open file in append mode, creating it if necessary.  If |truncate| is
  // true, existing content is discarded (used when rotating after a snapshot).
  Status Open(const std::string& path, bool truncate = false);
  Status Close();

  // Append a single record.  Flushes + fsyncs before returning so that the
  // caller may safely update the in-memory index.
  Status Append(const WalRecord& rec);

  // Append multiple records with a single fsync for batching.
  Status AppendBatch(const WalRecord* recs, size_t count);

  uint64_t BytesWritten() const { return bytes_written_; }

 private:
  Status WriteLocked(const WalRecord& rec);

  std::string path_;
  int fd_;
  uint64_t bytes_written_;
  mutable std::mutex mutex_;
};

// Sequential scanner over a WAL file.  Records with bad magic or CRC32 stop
// the iteration cleanly -- typically indicates a partial tail write after a
// crash.  The byte position of the last valid record is exposed via
// LastGoodOffset() so callers can truncate the log.
class WalReader {
 public:
  WalReader();
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  Status Open(const std::string& path);
  Status Close();

  // Replay every valid record through |fn|, stopping at EOF or first corrupt
  // record. Returns kOk on success (including clean truncation on corruption).
  Status Replay(const std::function<void(const WalRecord&)>& fn);

  uint64_t LastGoodOffset() const { return last_good_offset_; }

 private:
  std::string path_;
  int fd_;
  uint64_t last_good_offset_;
};

// Compute CRC32 over a byte buffer. Exposed for testing.
uint32_t Crc32(const uint8_t* data, size_t len);

}  // namespace index
}  // namespace parakv
