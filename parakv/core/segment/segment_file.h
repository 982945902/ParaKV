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

#include <string>

#include "segment_base.h"

namespace parakv {
namespace segment {

class SegmentFile : public SegmentBase {
 public:
  SegmentFile(uint32_t segment_id, const SegmentConfig& config,
              const std::string& file_path);
  ~SegmentFile() override;

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

  const std::string& GetFilePath() const { return file_path_; }

 private:
  Status PWrite(const void* buf, size_t count, uint64_t offset);
  Status PRead(void* buf, size_t count, uint64_t offset);
  Status LoadBitmap();
  Status FlushBitmap();

  std::string file_path_;
  int fd_;
};

}  // namespace segment
}  // namespace parakv
