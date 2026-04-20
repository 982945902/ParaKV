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

#include "segment_base.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace parakv {
namespace segment {

SegmentBase::SegmentBase(uint32_t segment_id, const SegmentConfig& config)
    : segment_id_(segment_id),
      config_(config),
      state_(SegmentState::IDLE),
      total_slots_(0),
      used_slots_(0),
      deleted_slots_(0),
      append_cursor_(0),
      slot_size_(0),
      bitmap_size_(0),
      slot_data_offset_(0) {
  CalculateLayout();
}

SegmentBase::~SegmentBase() = default;

void SegmentBase::CalculateLayout() {
  slot_size_ = config_.key_size + config_.value_size;
  assert(slot_size_ > 0);

  // total_slots = floor((segment_size * 8) / (8 * slot_size + 1))
  // (each slot costs 8*slot_size data bits + 1 bitmap bit).
  total_slots_ = static_cast<uint32_t>((config_.segment_size * 8) /
                                       (8ULL * slot_size_ + 1));

  bitmap_size_ = (total_slots_ + 7) / 8;

  // Align the bitmap area to the configured boundary for direct-I/O.
  const uint64_t align = config_.bitmap_alignment;
  if (align > 0) {
    bitmap_size_ = (bitmap_size_ + align - 1) / align * align;
  }

  slot_data_offset_ = bitmap_size_;

  // Recompute total_slots to fit within the remaining data area.
  const uint64_t data_area = config_.segment_size - slot_data_offset_;
  const uint32_t max_slots = static_cast<uint32_t>(data_area / slot_size_);
  total_slots_ = std::min(total_slots_, max_slots);

  bitmap_.resize(bitmap_size_, 0);
}

SegmentState SegmentBase::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

uint32_t SegmentBase::GetUsedSlots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return used_slots_;
}

uint32_t SegmentBase::GetFreeSlots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_slots_ - append_cursor_ + deleted_slots_;
}

uint32_t SegmentBase::GetDeletedSlots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deleted_slots_;
}

double SegmentBase::GetDeletedRatio() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (append_cursor_ == 0) {
    return 0.0;
  }
  return static_cast<double>(deleted_slots_) / append_cursor_;
}

bool SegmentBase::NeedsCompaction() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == SegmentState::FULL && append_cursor_ > 0 &&
         static_cast<double>(deleted_slots_) / append_cursor_ >=
             config_.compaction_threshold;
}

uint64_t SegmentBase::GetSlotOffset(uint32_t slot_id) const {
  assert(slot_id < total_slots_);
  return slot_data_offset_ + static_cast<uint64_t>(slot_id) * slot_size_;
}

void SegmentBase::SetSlotBit(uint32_t slot_id) {
  assert(slot_id < total_slots_);
  bitmap_[slot_id / 8] |= (1u << (slot_id % 8));
}

void SegmentBase::ClearSlotBit(uint32_t slot_id) {
  assert(slot_id < total_slots_);
  bitmap_[slot_id / 8] &= ~(1u << (slot_id % 8));
}

bool SegmentBase::IsSlotOccupied(uint32_t slot_id) const {
  assert(slot_id < total_slots_);
  return (bitmap_[slot_id / 8] & (1u << (slot_id % 8))) != 0;
}

int32_t SegmentBase::FindFreeSlot() const {
  if (append_cursor_ < total_slots_) {
    return static_cast<int32_t>(append_cursor_);
  }
  return -1;
}

void SegmentBase::UpdateState() {
  if (used_slots_ == 0 && append_cursor_ == 0) {
    state_ = SegmentState::IDLE;
  } else if (append_cursor_ >= total_slots_) {
    state_ = SegmentState::FULL;
  } else {
    state_ = SegmentState::APPENDING;
  }
}

void SegmentBase::ResetBitmap() {
  std::fill(bitmap_.begin(), bitmap_.end(), 0);
  used_slots_ = 0;
  deleted_slots_ = 0;
  append_cursor_ = 0;
  state_ = SegmentState::IDLE;
}

}  // namespace segment
}  // namespace parakv
