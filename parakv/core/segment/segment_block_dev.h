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
#include <string>

#include "segment_base.h"

namespace parakv {
namespace segment {

// Abstract I/O interface for block device backends (e.g., SPDK).
// Concrete implementations should be provided by the chosen block I/O layer.
class BlockDeviceIO {
 public:
  virtual ~BlockDeviceIO() = default;
  virtual Status Write(const void* buf, size_t count, uint64_t offset) = 0;
  virtual Status Read(void* buf, size_t count, uint64_t offset) = 0;
  virtual Status Sync() = 0;
};

class SegmentBlockDev : public SegmentBase {
 public:
  // Each segment occupies [base_lba_offset, base_lba_offset + segment_size)
  // on the block device.
  SegmentBlockDev(uint32_t segment_id, const SegmentConfig& config,
                  BlockDeviceIO* io, uint64_t base_lba_offset);
  ~SegmentBlockDev() override;

  Status Open() override;
  Status Close() override;

  Status Insert(const void* key, const void* value, uint32_t* slot_id) override;

  Status BatchInsert(const void* keys, const void* values, uint32_t count,
                     uint32_t* slot_ids) override;

  Status Read(uint32_t slot_id, void* key, void* value) override;
  Status Delete(uint32_t slot_id) override;
  Status Compact(SegmentBase* target,
                 const SlotMoveCallback& on_slot_moved) override;
  Status SyncBitmap() override;

 private:
  Status LoadBitmap();
  Status FlushBitmap();

  uint64_t AbsoluteOffset(uint64_t relative) const {
    return base_lba_offset_ + relative;
  }

  BlockDeviceIO* io_;  // not owned
  uint64_t base_lba_offset_;
  bool opened_;
};

}  // namespace segment
}  // namespace parakv
