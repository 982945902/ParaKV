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

#include "parakv/core/index/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace parakv {
namespace index {

using segment::Status;

namespace {

// Bytes [0, kCrcPayloadLen) of a record are covered by CRC32. The CRC value
// itself sits at byte offset kCrcOffset (== 26); bytes [30, 32) are zero
// padding for 32-byte alignment.
constexpr size_t kCrcOffset = 26;
constexpr size_t kCrcPayloadLen = 26;

// Pre-computed lookup table for the reflected IEEE 802.3 CRC32 polynomial
// (0xEDB88320). Wrapping the table in a function-local const static leverages
// C++11 thread-safe "magic static" initialization and avoids the
// non-const-static-variable pitfall called out in the Google C++ style guide.
const std::array<uint32_t, 256>& Crc32Table() {
  static const std::array<uint32_t, 256> kTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    return table;
  }();
  return kTable;
}

inline uint32_t Crc32Step(uint32_t crc, uint8_t b) {
  return Crc32Table()[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

void WriteLE64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(v >> (i * 8));
  }
}

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (i * 8);
  }
  return v;
}

void WriteLE32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(v >> (i * 8));
  }
}

uint32_t ReadLE32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(p[i]) << (i * 8);
  }
  return v;
}

// Write |count| bytes at |offset| with EINTR retry, returning kIOError on
// any unrecoverable error.
Status PWriteAll(int fd, const void* buf, size_t count, uint64_t offset) {
  const auto* p = static_cast<const char*>(buf);
  while (count > 0) {
    ssize_t n = ::pwrite(fd, p, count, offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::kIOError;
    }
    p += n;
    offset += n;
    count -= n;
  }
  return Status::kOk;
}

// Read exactly |count| bytes into |buf| from the current file cursor.
// Sets |*eof| to true and returns kOk if EOF is reached before any data is
// read; partial reads past EOF return kIOError to flag corruption.
Status ReadAll(int fd, void* buf, size_t count, bool* eof) {
  auto* p = static_cast<char*>(buf);
  *eof = false;
  size_t total_read = 0;
  while (count > 0) {
    ssize_t n = ::read(fd, p, count);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::kIOError;
    }
    if (n == 0) {
      if (total_read == 0) {
        *eof = true;
        return Status::kOk;
      }
      // Partial record at EOF; caller treats this as corruption and stops.
      return Status::kCorruption;
    }
    p += n;
    count -= n;
    total_read += n;
  }
  return Status::kOk;
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = Crc32Step(crc, data[i]);
  }
  return crc ^ 0xFFFFFFFFu;
}

void WalRecord::Encode(uint8_t out[kEncodedSize]) const {
  std::memset(out, 0, kEncodedSize);
  out[0] = kMagic;
  out[1] = static_cast<uint8_t>(type);
  WriteLE64(out + 2, key);
  WriteLE64(out + 10, disk_addr);
  WriteLE64(out + 18, old_disk_addr);

  const uint32_t crc = Crc32(out, kCrcPayloadLen);
  WriteLE32(out + kCrcOffset, crc);
  // bytes [30, 32) remain zero padding for alignment.
}

bool WalRecord::Decode(const uint8_t in[kEncodedSize], WalRecord* out) {
  if (in[0] != kMagic) {
    return false;
  }
  const uint32_t crc_expected = ReadLE32(in + kCrcOffset);
  const uint32_t crc_actual = Crc32(in, kCrcPayloadLen);
  if (crc_expected != crc_actual) {
    return false;
  }

  out->type = static_cast<WalRecordType>(in[1]);
  out->key = ReadLE64(in + 2);
  out->disk_addr = ReadLE64(in + 10);
  out->old_disk_addr = ReadLE64(in + 18);
  return true;
}

// -------------------------- WalWriter ---------------------------------------

WalWriter::WalWriter() : fd_(-1), bytes_written_(0) {}

WalWriter::~WalWriter() { Close(); }

Status WalWriter::Open(const std::string& path, bool truncate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ >= 0) {
    return Status::kOk;
  }

  int flags = O_RDWR | O_CREAT;
  if (truncate) {
    flags |= O_TRUNC;
  }
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) {
    return Status::kIOError;
  }

  struct stat st{};
  if (::fstat(fd_, &st) != 0) {
    ::close(fd_);
    fd_ = -1;
    return Status::kIOError;
  }
  path_ = path;
  bytes_written_ = static_cast<uint64_t>(st.st_size);
  return Status::kOk;
}

Status WalWriter::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) {
    return Status::kOk;
  }
  ::fsync(fd_);
  ::close(fd_);
  fd_ = -1;
  return Status::kOk;
}

Status WalWriter::WriteLocked(const WalRecord& rec) {
  if (fd_ < 0) {
    return Status::kIOError;
  }
  uint8_t buf[WalRecord::kEncodedSize];
  rec.Encode(buf);
  const auto s = PWriteAll(fd_, buf, WalRecord::kEncodedSize, bytes_written_);
  if (s != Status::kOk) {
    return s;
  }
  bytes_written_ += WalRecord::kEncodedSize;
  return Status::kOk;
}

Status WalWriter::Append(const WalRecord& rec) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto s = WriteLocked(rec);
  if (s != Status::kOk) {
    return s;
  }
  if (::fsync(fd_) != 0) {
    return Status::kIOError;
  }
  return Status::kOk;
}

Status WalWriter::AppendBatch(const WalRecord* recs, size_t count) {
  if (recs == nullptr || count == 0) {
    return Status::kInvalidArgument;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < count; ++i) {
    const auto s = WriteLocked(recs[i]);
    if (s != Status::kOk) {
      return s;
    }
  }
  if (::fsync(fd_) != 0) {
    return Status::kIOError;
  }
  return Status::kOk;
}

// -------------------------- WalReader ---------------------------------------

WalReader::WalReader() : fd_(-1), last_good_offset_(0) {}

WalReader::~WalReader() { Close(); }

Status WalReader::Open(const std::string& path) {
  if (fd_ >= 0) {
    return Status::kOk;
  }
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) {
    return Status::kIOError;
  }
  path_ = path;
  last_good_offset_ = 0;
  if (::lseek(fd_, 0, SEEK_SET) < 0) {
    ::close(fd_);
    fd_ = -1;
    return Status::kIOError;
  }
  return Status::kOk;
}

Status WalReader::Close() {
  if (fd_ < 0) {
    return Status::kOk;
  }
  ::close(fd_);
  fd_ = -1;
  return Status::kOk;
}

Status WalReader::Replay(const std::function<void(const WalRecord&)>& fn) {
  if (fd_ < 0) {
    return Status::kIOError;
  }

  uint8_t buf[WalRecord::kEncodedSize];
  while (true) {
    bool eof = false;
    const auto s = ReadAll(fd_, buf, WalRecord::kEncodedSize, &eof);
    if (s == Status::kCorruption) {
      // Partial tail record (torn write after a crash). Stop cleanly so the
      // caller can truncate the log at last_good_offset_.
      break;
    }
    if (s != Status::kOk) {
      return s;
    }
    if (eof) {
      break;
    }

    WalRecord rec;
    if (!WalRecord::Decode(buf, &rec)) {
      // Magic/CRC mismatch -- same torn-write semantics as above.
      break;
    }
    if (fn) {
      fn(rec);
    }
    last_good_offset_ += WalRecord::kEncodedSize;
  }
  return Status::kOk;
}

}  // namespace index
}  // namespace parakv
