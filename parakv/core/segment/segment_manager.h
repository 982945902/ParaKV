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
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "segment_base.h"

namespace parakv {
namespace segment {

struct HotSegmentInfo {
  uint32_t segment_id;
  uint64_t access_count;
  std::vector<uint8_t> cached_data;
};

// Callback to notify the index layer when a slot moves during compaction.
// Parameters: old_segment_id, old_slot_id, new_segment_id, new_slot_id
using SlotMoveCallback =
    std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)>;

class SegmentManager {
 public:
  explicit SegmentManager(const SegmentConfig& config);
  ~SegmentManager();

  SegmentManager(const SegmentManager&) = delete;
  SegmentManager& operator=(const SegmentManager&) = delete;

  // Register an externally created segment.
  Status AddSegment(std::unique_ptr<SegmentBase> segment);

  // Returns an APPENDING segment that has free slots.
  // If none exists, promotes an IDLE segment.
  SegmentBase* GetActiveSegment();

  // Return segment by id.
  SegmentBase* GetSegment(uint32_t segment_id);

  // Release a segment to IDLE pool (after compaction).
  Status ReleaseSegment(uint32_t segment_id);

  // ---- Compaction ----

  void SetSlotMoveCallback(SlotMoveCallback cb) {
    slot_move_cb_ = std::move(cb);
  }

  // Scan all FULL segments and compact those exceeding the deletion threshold.
  Status RunCompaction();

  // ---- Hot / Cold management ----

  void RecordAccess(uint32_t segment_id);

  // Promote / demote segments based on accumulated access counts.
  // hot_threshold: min access count to promote.
  // max_hot_segments: memory budget expressed as max number of hot segments.
  Status EvaluateHotCold(uint64_t hot_threshold, uint32_t max_hot_segments);

  bool IsHotSegment(uint32_t segment_id) const;

  // ---- Queries ----

  uint32_t GetTotalSegments() const;
  uint32_t GetIdleSegments() const;
  uint32_t GetFullSegments() const;

 private:
  SegmentBase* PromoteIdleSegment();

  SegmentConfig config_;

  mutable std::mutex mutex_;

  std::unordered_map<uint32_t, std::unique_ptr<SegmentBase>> segments_;

  std::unordered_set<uint32_t> idle_set_;
  std::unordered_set<uint32_t> appending_set_;
  std::unordered_set<uint32_t> full_set_;

  // Hot segment tracking
  std::unordered_map<uint32_t, uint64_t> access_counters_;
  std::unordered_set<uint32_t> hot_segments_;

  SlotMoveCallback slot_move_cb_;
};

}  // namespace segment
}  // namespace parakv
