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

#include "segment_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstring>

namespace parakv {
namespace segment {

SegmentFile::SegmentFile(uint32_t segment_id, const SegmentConfig& config,
                         const std::string& file_path)
    : SegmentBase(segment_id, config), file_path_(file_path), fd_(-1) {}

SegmentFile::~SegmentFile() { Close(); }

Status SegmentFile::Open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ >= 0) return Status::kOk;

  struct stat st;
  bool file_exists = (::stat(file_path_.c_str(), &st) == 0);

  int flags = O_RDWR;
  if (!file_exists) {
    flags |= O_CREAT;
  }

  fd_ = ::open(file_path_.c_str(), flags, 0644);
  if (fd_ < 0) return Status::kIOError;

  if (!file_exists) {
    if (::ftruncate(fd_, config_.segment_size) != 0) {
      ::close(fd_);
      fd_ = -1;
      return Status::kIOError;
    }
    ResetBitmap();
    // Write initial empty bitmap to disk
    auto s = FlushBitmap();
    if (s != Status::kOk) {
      ::close(fd_);
      fd_ = -1;
      return s;
    }
  } else {
    auto s = LoadBitmap();
    if (s != Status::kOk) {
      ::close(fd_);
      fd_ = -1;
      return s;
    }
    // Rebuild counters from bitmap
    used_slots_ = 0;
    append_cursor_ = 0;
    for (uint32_t i = 0; i < total_slots_; ++i) {
      if (IsSlotOccupied(i)) {
        ++used_slots_;
        append_cursor_ = i + 1;
      }
    }
    deleted_slots_ = append_cursor_ - used_slots_;
    UpdateState();
  }
  return Status::kOk;
}

Status SegmentFile::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kOk;

  FlushBitmap();
  ::fsync(fd_);
  ::close(fd_);
  fd_ = -1;
  return Status::kOk;
}

Status SegmentFile::Insert(const void* key, const void* value,
                           uint32_t* slot_id) {
  if (!key || !value || !slot_id) return Status::kInvalidArgument;

  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;

  int32_t free_slot = FindFreeSlot();
  if (free_slot < 0) return Status::kFull;

  uint32_t sid = static_cast<uint32_t>(free_slot);
  uint64_t offset = GetSlotOffset(sid);

  // Write key then value (append-only)
  auto s = PWrite(key, config_.key_size, offset);
  if (s != Status::kOk) return s;

  s = PWrite(value, config_.value_size, offset + config_.key_size);
  if (s != Status::kOk) return s;

  // Update bitmap on disk, then in memory
  SetSlotBit(sid);
  s = FlushBitmap();
  if (s != Status::kOk) {
    ClearSlotBit(sid);
    return s;
  }

  ++used_slots_;
  ++append_cursor_;
  *slot_id = sid;
  UpdateState();
  return Status::kOk;
}

Status SegmentFile::BatchInsert(const void* keys, const void* values,
                                uint32_t count, uint32_t* slot_ids) {
  if (!keys || !values || !slot_ids || count == 0) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;

  uint32_t available = total_slots_ - append_cursor_;
  if (count > available) return Status::kFull;

  const auto* key_ptr = static_cast<const uint8_t*>(keys);
  const auto* val_ptr = static_cast<const uint8_t*>(values);

  // Phase 1: write all slot data
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t sid = append_cursor_ + i;
    uint64_t offset = GetSlotOffset(sid);

    auto s = PWrite(key_ptr + static_cast<size_t>(i) * config_.key_size,
                    config_.key_size, offset);
    if (s != Status::kOk) return s;

    s = PWrite(val_ptr + static_cast<size_t>(i) * config_.value_size,
               config_.value_size, offset + config_.key_size);
    if (s != Status::kOk) return s;

    slot_ids[i] = sid;
  }

  // Phase 2: update bitmap for all slots
  for (uint32_t i = 0; i < count; ++i) {
    SetSlotBit(slot_ids[i]);
  }

  auto s = FlushBitmap();
  if (s != Status::kOk) return s;

  used_slots_ += count;
  append_cursor_ += count;
  UpdateState();
  return Status::kOk;
}

Status SegmentFile::Read(uint32_t slot_id, void* key, void* value) {
  if (slot_id >= total_slots_) return Status::kInvalidArgument;

  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;
  if (!IsSlotOccupied(slot_id)) return Status::kNotFound;

  uint64_t offset = GetSlotOffset(slot_id);

  if (key) {
    auto s = PRead(key, config_.key_size, offset);
    if (s != Status::kOk) return s;
  }
  if (value) {
    auto s = PRead(value, config_.value_size, offset + config_.key_size);
    if (s != Status::kOk) return s;
  }
  return Status::kOk;
}

Status SegmentFile::Delete(uint32_t slot_id) {
  if (slot_id >= total_slots_) return Status::kInvalidArgument;

  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;
  if (!IsSlotOccupied(slot_id)) return Status::kNotFound;

  // Clear bitmap bit, then flush
  ClearSlotBit(slot_id);
  auto s = FlushBitmap();
  if (s != Status::kOk) {
    SetSlotBit(slot_id);
    return s;
  }

  assert(used_slots_ > 0);
  --used_slots_;
  ++deleted_slots_;
  UpdateState();
  return Status::kOk;
}

Status SegmentFile::Compact(SegmentBase* target) {
  if (!target) return Status::kInvalidArgument;

  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;

  std::vector<uint8_t> key_buf(config_.key_size);
  std::vector<uint8_t> val_buf(config_.value_size);

  for (uint32_t i = 0; i < append_cursor_; ++i) {
    if (!IsSlotOccupied(i)) continue;

    uint64_t offset = GetSlotOffset(i);
    auto s = PRead(key_buf.data(), config_.key_size, offset);
    if (s != Status::kOk) return s;

    s = PRead(val_buf.data(), config_.value_size, offset + config_.key_size);
    if (s != Status::kOk) return s;

    uint32_t new_slot_id;
    s = target->Insert(key_buf.data(), val_buf.data(), &new_slot_id);
    if (s != Status::kOk) return s;
  }

  // Reset this segment to IDLE
  ResetBitmap();
  auto s = FlushBitmap();
  return s;
}

Status SegmentFile::SyncBitmap() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return Status::kIOError;
  return FlushBitmap();
}

Status SegmentFile::PWrite(const void* buf, size_t count, uint64_t offset) {
  const auto* ptr = static_cast<const char*>(buf);
  size_t remaining = count;
  while (remaining > 0) {
    ssize_t written = ::pwrite(fd_, ptr, remaining, offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return Status::kIOError;
    }
    ptr += written;
    offset += written;
    remaining -= written;
  }
  return Status::kOk;
}

Status SegmentFile::PRead(void* buf, size_t count, uint64_t offset) {
  auto* ptr = static_cast<char*>(buf);
  size_t remaining = count;
  while (remaining > 0) {
    ssize_t nread = ::pread(fd_, ptr, remaining, offset);
    if (nread < 0) {
      if (errno == EINTR) continue;
      return Status::kIOError;
    }
    if (nread == 0) return Status::kCorruption;
    ptr += nread;
    offset += nread;
    remaining -= nread;
  }
  return Status::kOk;
}

Status SegmentFile::LoadBitmap() {
  return PRead(bitmap_.data(), bitmap_size_, GetBitmapOffset());
}

Status SegmentFile::FlushBitmap() {
  return PWrite(bitmap_.data(), bitmap_size_, GetBitmapOffset());
}

}  // namespace segment
}  // namespace parakv
