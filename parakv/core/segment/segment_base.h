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
#include <vector>

#include "common/status.h"

namespace parakv {
namespace segment {

enum class SegmentState {
  IDLE = 0,
  APPENDING,
  FULL,
};

struct SegmentConfig {
  bool is_need_compaction = true;
  uint32_t key_size = 8;
  uint32_t value_size = 512;
  uint64_t segment_size = 256ULL * 1024 * 1024;
  float compaction_threshold = 0.75;
  uint64_t bitmap_alignment = 4096;
};

using SlotMoveCallback =
    std::function<void(void* key, uint32_t old_segment_id, uint32_t old_slot_id,
                       uint32_t new_segment_id, uint32_t new_slot_id)>;

class SegmentBase {
 public:
  SegmentBase(uint32_t segment_id, const SegmentConfig& config);
  virtual ~SegmentBase();

  SegmentBase(const SegmentBase&) = delete;
  SegmentBase& operator=(const SegmentBase&) = delete;

  virtual Status Open() = 0;
  virtual Status Close() = 0;

  virtual Status Insert(const void* key, const void* value,
                        uint32_t* slot_id) = 0;

  virtual Status BatchInsert(const void* keys, const void* values,
                             uint32_t count, uint32_t* slot_ids) = 0;

  virtual Status Read(uint32_t slot_id, void* key, void* value) = 0;

  virtual Status Delete(uint32_t slot_id) = 0;

  virtual Status Compact(SegmentBase* target,
                         const SlotMoveCallback& on_slot_moved) = 0;

  virtual Status SyncBitmap() = 0;

  uint32_t GetSegmentId() const { return segment_id_; }
  SegmentState GetState() const;
  uint32_t GetTotalSlots() const { return total_slots_; }
  uint32_t GetUsedSlots() const;
  uint32_t GetFreeSlots() const;
  uint32_t GetDeletedSlots() const;
  float GetDeletedRatio() const;
  bool NeedsCompaction() const;
  uint32_t GetSlotSize() const { return slot_size_; }

  uint64_t GetBitmapOffset() const { return 0; }
  uint64_t GetBitmapSize() const { return bitmap_size_; }
  uint64_t GetSlotDataAreaOffset() const { return slot_data_offset_; }
  uint64_t GetSlotOffset(uint32_t slot_id) const;

 protected:
  void SetSlotBit(uint32_t slot_id);
  void ClearSlotBit(uint32_t slot_id);
  bool IsSlotOccupied(uint32_t slot_id) const;
  int32_t FindFreeSlot() const;

  void CalculateLayout();
  void UpdateState();
  void ResetBitmap();

  uint32_t segment_id_;
  SegmentConfig config_;
  SegmentState state_;

  uint32_t total_slots_;
  uint32_t used_slots_;
  uint32_t deleted_slots_;
  uint32_t append_cursor_;

  uint32_t slot_size_;
  uint64_t bitmap_size_;
  uint64_t slot_data_offset_;

  std::vector<uint8_t> bitmap_;
  mutable std::mutex mutex_;
};

}  // namespace segment
}  // namespace parakv
