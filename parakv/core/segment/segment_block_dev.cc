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

#include "segment_block_dev.h"

#include <cassert>
#include <cstring>
#include <vector>

namespace parakv {
namespace segment {

SegmentBlockDev::SegmentBlockDev(uint32_t segment_id,
                                 const SegmentConfig& config, BlockDeviceIO* io,
                                 uint64_t base_lba_offset)
    : SegmentBase(segment_id, config),
      io_(io),
      base_lba_offset_(base_lba_offset),
      opened_(false) {
  assert(io_ != nullptr);
}

SegmentBlockDev::~SegmentBlockDev() { Close(); }

Status SegmentBlockDev::Open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_) {
    return Status::kOk;
  }

  const Status s = LoadBitmap();
  if (s != Status::kOk) {
    return s;
  }

  // Rebuild counters from the on-disk bitmap.
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
  opened_ = true;

  return Status::kOk;
}

Status SegmentBlockDev::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kOk;
  }

  Status s = FlushBitmap();
  if (s != Status::kOk) {
    return s;
  }

  s = io_->Sync();
  if (s != Status::kOk) {
    return s;
  }
  opened_ = false;

  return Status::kOk;
}

Status SegmentBlockDev::Insert(const void* key, const void* value,
                               uint32_t* slot_id) {
  if (key == nullptr || value == nullptr || slot_id == nullptr) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  const int32_t free_slot = FindFreeSlot();
  if (free_slot < 0) {
    return Status::kFull;
  }

  const uint32_t sid = static_cast<uint32_t>(free_slot);
  const uint64_t offset = AbsoluteOffset(GetSlotOffset(sid));

  Status s = io_->Write(key, config_.key_size, offset);
  if (s != Status::kOk) {
    return s;
  }

  s = io_->Write(value, config_.value_size, offset + config_.key_size);
  if (s != Status::kOk) {
    return s;
  }

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

Status SegmentBlockDev::BatchInsert(const void* keys, const void* values,
                                    uint32_t count, uint32_t* slot_ids) {
  if (keys == nullptr || values == nullptr || slot_ids == nullptr ||
      count == 0) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  const uint32_t available = total_slots_ - append_cursor_;
  if (count > available) {
    return Status::kFull;
  }

  const auto* key_ptr = static_cast<const uint8_t*>(keys);
  const auto* val_ptr = static_cast<const uint8_t*>(values);

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t sid = append_cursor_ + i;
    const uint64_t offset = AbsoluteOffset(GetSlotOffset(sid));

    Status s = io_->Write(key_ptr + static_cast<size_t>(i) * config_.key_size,
                          config_.key_size, offset);
    if (s != Status::kOk) {
      return s;
    }

    s = io_->Write(val_ptr + static_cast<size_t>(i) * config_.value_size,
                   config_.value_size, offset + config_.key_size);
    if (s != Status::kOk) {
      return s;
    }

    slot_ids[i] = sid;
  }

  for (uint32_t i = 0; i < count; ++i) {
    SetSlotBit(slot_ids[i]);
  }

  const Status s = FlushBitmap();
  if (s != Status::kOk) {
    return s;
  }

  used_slots_ += count;
  append_cursor_ += count;
  UpdateState();

  return Status::kOk;
}

Status SegmentBlockDev::Read(uint32_t slot_id, void* key, void* value) {
  if (slot_id >= total_slots_) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  if (!IsSlotOccupied(slot_id)) {
    return Status::kNotFound;
  }

  const uint64_t offset = AbsoluteOffset(GetSlotOffset(slot_id));

  if (key != nullptr) {
    const Status s = io_->Read(key, config_.key_size, offset);
    if (s != Status::kOk) {
      return s;
    }
  }

  if (value != nullptr) {
    const Status s =
        io_->Read(value, config_.value_size, offset + config_.key_size);
    if (s != Status::kOk) {
      return s;
    }
  }

  return Status::kOk;
}

Status SegmentBlockDev::Delete(uint32_t slot_id) {
  if (slot_id >= total_slots_) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  if (!IsSlotOccupied(slot_id)) {
    return Status::kNotFound;
  }

  ClearSlotBit(slot_id);
  const Status s = FlushBitmap();
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

Status SegmentBlockDev::Compact(SegmentBase* target,
                                const SlotMoveCallback& on_slot_moved) {
  if (target == nullptr) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  std::vector<uint8_t> key_buf(config_.key_size);
  std::vector<uint8_t> val_buf(config_.value_size);

  for (uint32_t i = 0; i < append_cursor_; ++i) {
    if (!IsSlotOccupied(i)) {
      continue;
    }

    const uint64_t offset = AbsoluteOffset(GetSlotOffset(i));
    Status s = io_->Read(key_buf.data(), config_.key_size, offset);
    if (s != Status::kOk) {
      return s;
    }

    s = io_->Read(val_buf.data(), config_.value_size,
                  offset + config_.key_size);
    if (s != Status::kOk) {
      return s;
    }

    uint32_t new_slot_id = 0;
    s = target->Insert(key_buf.data(), val_buf.data(), &new_slot_id);
    if (s != Status::kOk) {
      return s;
    }
    if (on_slot_moved) {
      on_slot_moved(key_buf.data(), segment_id_, i, target->GetSegmentId(),
                    new_slot_id);
    }
  }

  ResetBitmap();

  return FlushBitmap();
}

Status SegmentBlockDev::SyncBitmap() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::kIOError;
  }

  return FlushBitmap();
}

Status SegmentBlockDev::LoadBitmap() {
  return io_->Read(bitmap_.data(), bitmap_size_,
                   AbsoluteOffset(GetBitmapOffset()));
}

Status SegmentBlockDev::FlushBitmap() {
  return io_->Write(bitmap_.data(), bitmap_size_,
                    AbsoluteOffset(GetBitmapOffset()));
}

}  // namespace segment
}  // namespace parakv
