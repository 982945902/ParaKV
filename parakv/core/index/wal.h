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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>

#include "parakv/core/index/key.h"
#include "parakv/core/segment/segment_base.h"

namespace parakv {
namespace index {

// WAL record types -- kept stable across key widths so that snapshots /
// WALs from different deployments can be inspected with the same tooling.
enum class WalRecordType : uint8_t {
  kInsert = 0x01,
  kUpdate = 0x02,
  kDelete = 0x03,
};

// WAL record, parameterised on the user key type.  |KeyT| must be trivially
// copyable so that the in-memory struct can be memcpy'd to/from disk.
//
// Layout (little-endian, see docs/zh/design/index.md for the 64-bit case):
//
//   offset            size         field
//   0                 1            magic (kMagic)
//   1                 1            type  (WalRecordType)
//   2                 sizeof(KeyT) key (memcpy of KeyT)
//   2 + K             8            disk_addr (encoded offset)
//   10 + K            8            old_disk_addr (0 if not applicable)
//   18 + K            4            crc32 of bytes [0, 18 + K)
//   22 + K            pad          zeros up to 8-byte alignment
//
// For KeyT = uint64_t the record is 32 bytes (matches the original spec).
// For KeyT = Key128   the record is 40 bytes.
template <typename KeyT>
struct WalRecord {
  static_assert(std::is_trivially_copyable<KeyT>::value,
                "WalRecord key type must be trivially copyable");

  static constexpr uint8_t kMagic = 0xA5;
  static constexpr size_t kKeySize = sizeof(KeyT);
  static constexpr size_t kCrcOffset = 2 + kKeySize + 16;
  static constexpr size_t kCrcPayloadLen = kCrcOffset;
  static constexpr size_t kRawSize = kCrcOffset + 4;
  // Round kRawSize up to an 8-byte boundary so every record is naturally
  // aligned on disk (friendlier for direct-I/O backends).
  static constexpr size_t kEncodedSize = (kRawSize + 7) & ~size_t{7};

  WalRecordType type = WalRecordType::kInsert;
  KeyT key{};
  uint64_t disk_addr = 0;
  uint64_t old_disk_addr = 0;

  // Serialize into |out[0 .. kEncodedSize)|.  |out| must point to at least
  // kEncodedSize bytes of writable memory.
  void Encode(uint8_t* out) const;

  // Parse |in[0 .. kEncodedSize)|.  Returns false on magic or CRC mismatch.
  static bool Decode(const uint8_t* in, WalRecord<KeyT>* out);
};

// Common WAL writer: byte-oriented so it can back any WalRecord<KeyT>.
//
// Durability contract: Append/AppendBatch do not return until the bytes are
// fsync'd.  The in-memory index should be updated only after a successful
// Append, which ensures that a crash never produces an index entry whose
// WAL record is missing.
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

  // Append |n| bytes and fsync before returning.  Callers are expected to
  // pass a buffer produced by WalRecord<KeyT>::Encode.
  Status Append(const uint8_t* buf, size_t n);

  // Append |count| fixed-size records of |record_size| bytes each, with a
  // single fsync at the end.  |buf| must hold record_size * count bytes.
  Status AppendBatch(const uint8_t* buf, size_t record_size, size_t count);

  // Convenience overload for typed records.
  template <typename KeyT>
  Status Append(const WalRecord<KeyT>& rec);

  uint64_t BytesWritten() const { return bytes_written_; }

 private:
  Status WriteLocked(const uint8_t* buf, size_t n);

  std::string path_;
  int fd_;
  uint64_t bytes_written_;
  mutable std::mutex mutex_;
};

// Sequential WAL scanner.
//
// Magic / CRC mismatches stop the replay cleanly (torn-write semantics).
// LastGoodOffset() returns the byte position just past the last valid
// record so callers can truncate the log if necessary.
class WalReader {
 public:
  WalReader();
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  Status Open(const std::string& path);
  Status Close();

  // Replay fixed-size records, stopping at EOF or the first corrupt record.
  // |fn| is invoked for every decoded record byte-buffer; it should attempt
  // to parse and return true on success, false to terminate the scan.
  Status Replay(size_t record_size,
                const std::function<bool(const uint8_t* buf)>& fn);

  // Typed convenience helper: decodes into WalRecord<KeyT> and forwards.
  template <typename KeyT>
  Status Replay(const std::function<void(const WalRecord<KeyT>&)>& fn);

  uint64_t LastGoodOffset() const { return last_good_offset_; }

 private:
  std::string path_;
  int fd_;
  uint64_t last_good_offset_;
};

// Compute CRC32 (IEEE 802.3, reflected) over a byte buffer. Exposed for
// testing and for any caller implementing a custom record format.
uint32_t Crc32(const uint8_t* data, size_t len);

// -----------------------------------------------------------------------
// Template member definitions for the typed convenience overloads.  These
// are header-inline because they're small wrappers around the byte-oriented
// API.  The heavy lifting (Encode/Decode) lives in wal.cc and is explicitly
// instantiated for the supported KeyT.
// -----------------------------------------------------------------------

template <typename KeyT>
inline Status WalWriter::Append(const WalRecord<KeyT>& rec) {
  uint8_t buf[WalRecord<KeyT>::kEncodedSize];
  rec.Encode(buf);
  return Append(buf, WalRecord<KeyT>::kEncodedSize);
}

template <typename KeyT>
inline Status WalReader::Replay(
    const std::function<void(const WalRecord<KeyT>&)>& fn) {
  return Replay(WalRecord<KeyT>::kEncodedSize,
                [&fn](const uint8_t* buf) -> bool {
                  WalRecord<KeyT> rec;
                  if (!WalRecord<KeyT>::Decode(buf, &rec)) {
                    return false;
                  }
                  if (fn) {
                    fn(rec);
                  }
                  return true;
                });
}

}  // namespace index
}  // namespace parakv
