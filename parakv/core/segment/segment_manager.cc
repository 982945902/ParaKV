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

#include "segment_manager.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

namespace parakv {
namespace segment {

namespace {

// Binary state file layout (all little-endian uint32):
//   magic[4]            "SGMS"
//   version[4]          1
//   full_count[4]       N
//   full_ids[4 * N]     ordered, oldest-full first
//   appending_count[4]  M
//   appending_ids[4*M]
//   idle_count[4]       K
//   idle_ids[4 * K]

constexpr uint32_t kStateMagic = 0x534D4753;  // "SGMS" in LE
constexpr uint32_t kStateVersion = 1;

void PutU32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

bool GetU32(const uint8_t*& p, const uint8_t* end, uint32_t* out) {
  if (p + 4 > end) return false;
  *out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
  p += 4;
  return true;
}

std::string JoinIds(const uint32_t* data, size_t n) {
  std::ostringstream oss;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) oss << ", ";
    oss << data[i];
  }
  return oss.str();
}

template <typename Container>
void SerializeIds(std::vector<uint8_t>& buf, const char* label,
                  const Container& ids) {
  PutU32(buf, static_cast<uint32_t>(ids.size()));
  std::vector<uint32_t> tmp(ids.begin(), ids.end());
  for (uint32_t id : tmp) {
    PutU32(buf, id);
  }
  LOG(INFO) << "SegmentManager::SaveState: " << label << "[" << tmp.size()
            << "]: [" << JoinIds(tmp.data(), tmp.size()) << "]";
}

}  // namespace

SegmentManager::SegmentManager(const SegmentConfig& config) : config_(config) {}

SegmentManager::~SegmentManager() = default;

Status SegmentManager::AddSegment(std::shared_ptr<SegmentBase> segment) {
  if (segment == nullptr) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t id = segment->GetSegmentId();
  if (segments_.count(id) != 0) {
    return Status::kInvalidArgument;
  }

  const SegmentState state = segment->GetState();
  segments_[id] = std::move(segment);

  switch (state) {
    case SegmentState::IDLE:
      idle_set_.insert(id);
      break;
    case SegmentState::APPENDING:
      appending_set_.insert(id);
      break;
    case SegmentState::FULL:
      full_set_.insert(id);
      full_order_.push_back(id);
      break;
  }

  return Status::kOk;
}

std::shared_ptr<SegmentBase> SegmentManager::GetActiveSegment() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Prefer an existing APPENDING segment that still has free slots.
  for (const uint32_t id : appending_set_) {
    auto seg = segments_[id];
    if (seg->GetFreeSlots() > 0) {
      return seg;
    } else {
      MarkSegmentFullInternal(id);
    }
  }

  // Otherwise, promote an IDLE segment to APPENDING.
  return PromoteIdleSegment();
}

std::shared_ptr<SegmentBase> SegmentManager::GetSegment(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = segments_.find(segment_id);
  if (it == segments_.end()) {
    return nullptr;
  }

  return it->second;
}

Status SegmentManager::MarkSegmentFull(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  return MarkSegmentFullInternal(segment_id);
}

Status SegmentManager::MarkSegmentFullInternal(uint32_t segment_id) {
  if (segments_.find(segment_id) == segments_.end()) {
    return Status::kNotFound;
  }

  if (full_set_.count(segment_id) != 0) {
    return Status::kOk;
  }

  appending_set_.erase(segment_id);
  idle_set_.erase(segment_id);
  full_set_.insert(segment_id);
  full_order_.push_back(segment_id);

  LOG(INFO) << "SegmentManager::MarkSegmentFull: segment_id=" << segment_id;
  return Status::kOk;
}

Status SegmentManager::ReleaseSegment(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = segments_.find(segment_id);
  if (it == segments_.end()) {
    return Status::kNotFound;
  }

  appending_set_.erase(segment_id);
  full_set_.erase(segment_id);
  full_order_.erase(
      std::remove(full_order_.begin(), full_order_.end(), segment_id),
      full_order_.end());
  hot_segments_.erase(segment_id);
  access_counters_.erase(segment_id);
  idle_set_.insert(segment_id);

  return Status::kOk;
}

std::shared_ptr<SegmentBase> SegmentManager::PromoteIdleSegment() {
  if (idle_set_.empty()) {
    return nullptr;
  }

  const uint32_t id = *idle_set_.begin();
  idle_set_.erase(id);
  appending_set_.insert(id);

  return segments_[id];
}

// ---------------------------------------------------------------------------
// State persistence
// ---------------------------------------------------------------------------

Status SegmentManager::SaveState(const std::string& path) const {
  std::vector<uint8_t> buf;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t est = 8 + 4 + full_order_.size() * 4 + 4 +
                         appending_set_.size() * 4 + 4 + idle_set_.size() * 4;
    buf.reserve(est);

    PutU32(buf, kStateMagic);
    PutU32(buf, kStateVersion);

    SerializeIds(buf, "full", full_order_);
    SerializeIds(buf, "appending", appending_set_);
    SerializeIds(buf, "idle", idle_set_);
  }

  const std::string tmp_path = path + ".tmp";
  int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    LOG(ERROR) << "SegmentManager::SaveState: open failed: " << tmp_path;
    return Status::kIOError;
  }

  const ssize_t written = ::write(fd, buf.data(), buf.size());
  if (written < 0 || static_cast<size_t>(written) != buf.size()) {
    ::close(fd);
    ::unlink(tmp_path.c_str());
    LOG(ERROR) << "SegmentManager::SaveState: write failed";
    return Status::kIOError;
  }

  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp_path.c_str());
    LOG(ERROR) << "SegmentManager::SaveState: fsync failed";
    return Status::kIOError;
  }
  ::close(fd);

  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    ::unlink(tmp_path.c_str());
    LOG(ERROR) << "SegmentManager::SaveState: rename failed";
    return Status::kIOError;
  }

  LOG(INFO) << "SegmentManager::SaveState: saved " << full_order_.size()
            << " full, " << appending_set_.size() << " appending, "
            << idle_set_.size() << " idle segments to " << path;
  return Status::kOk;
}

Status SegmentManager::LoadState(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    LOG(INFO) << "SegmentManager::LoadState: no state file at " << path
              << ", using default classification";
    return Status::kOk;
  }

  const size_t file_size = static_cast<size_t>(st.st_size);
  if (file_size < 8) {
    LOG(WARNING) << "SegmentManager::LoadState: file too small: " << file_size;
    return Status::kCorruption;
  }

  std::vector<uint8_t> buf(file_size);
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG(ERROR) << "SegmentManager::LoadState: open failed: " << path;
    return Status::kIOError;
  }

  size_t remaining = file_size;
  uint8_t* dst = buf.data();
  while (remaining > 0) {
    const ssize_t n = ::read(fd, dst, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Status::kIOError;
    }
    if (n == 0) break;
    dst += n;
    remaining -= n;
  }
  ::close(fd);

  const uint8_t* p = buf.data();
  const uint8_t* end = buf.data() + buf.size();

  uint32_t magic = 0, version = 0;
  if (!GetU32(p, end, &magic) || magic != kStateMagic) {
    LOG(WARNING) << "SegmentManager::LoadState: bad magic";
    return Status::kCorruption;
  }
  if (!GetU32(p, end, &version) || version != kStateVersion) {
    LOG(WARNING) << "SegmentManager::LoadState: unsupported version "
                 << version;
    return Status::kCorruption;
  }

  auto read_ids = [&](std::vector<uint32_t>& out) -> bool {
    uint32_t count = 0;
    if (!GetU32(p, end, &count)) return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!GetU32(p, end, &out[i])) return false;
    }
    return true;
  };

  std::vector<uint32_t> saved_full, saved_appending, saved_idle;
  if (!read_ids(saved_full) || !read_ids(saved_appending) ||
      !read_ids(saved_idle)) {
    LOG(WARNING) << "SegmentManager::LoadState: truncated state file";
    return Status::kCorruption;
  }

  auto log_ids = [](const char* label, const std::vector<uint32_t>& ids) {
    LOG(INFO) << "SegmentManager::LoadState: file " << label << "["
              << ids.size() << "]: [" << JoinIds(ids.data(), ids.size()) << "]";
  };
  log_ids("full", saved_full);
  log_ids("appending", saved_appending);
  log_ids("idle", saved_idle);

  // Apply the loaded state: reclassify segments that exist in the manager.
  std::lock_guard<std::mutex> lock(mutex_);

  idle_set_.clear();
  appending_set_.clear();
  full_set_.clear();
  full_order_.clear();

  std::unordered_set<uint32_t> classified;

  for (uint32_t id : saved_full) {
    if (segments_.count(id) == 0) {
      LOG(WARNING) << "SegmentManager::LoadState: full segment " << id
                   << " not found, skipping";
      continue;
    }
    full_set_.insert(id);
    full_order_.push_back(id);
    classified.insert(id);
  }

  for (uint32_t id : saved_appending) {
    if (segments_.count(id) == 0 || classified.count(id) != 0) continue;
    appending_set_.insert(id);
    classified.insert(id);
  }

  for (uint32_t id : saved_idle) {
    if (segments_.count(id) == 0 || classified.count(id) != 0) continue;
    idle_set_.insert(id);
    classified.insert(id);
  }

  // Any segments present in the manager but missing from the state file
  // are classified by their current segment-level state.
  for (const auto& [id, seg] : segments_) {
    if (classified.count(id) != 0) continue;
    LOG(INFO) << "SegmentManager::LoadState: segment " << id
              << " not in state file, classifying by segment state";
    switch (seg->GetState()) {
      case SegmentState::IDLE:
        idle_set_.insert(id);
        break;
      case SegmentState::APPENDING:
        appending_set_.insert(id);
        break;
      case SegmentState::FULL:
        full_set_.insert(id);
        full_order_.push_back(id);
        break;
    }
  }

  LOG(INFO) << "SegmentManager::LoadState: restored " << full_order_.size()
            << " full (ordered), " << appending_set_.size() << " appending, "
            << idle_set_.size() << " idle segments";
  return Status::kOk;
}

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

Status SegmentManager::RunCompaction() {
  LOG(INFO) << "SegmentManager::RunCompaction: starting compaction";

  std::vector<uint32_t> candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO) << "SegmentManager::RunCompaction: full_set_=" << full_set_.size()
              << " appending_set_=" << appending_set_.size()
              << " idle_set_=" << idle_set_.size();
    for (const uint32_t id : full_set_) {
      LOG(INFO) << "==== SegmentManager::RunCompaction: segment " << id
                << " is full, deleted ratio: "
                << segments_[id]->GetDeletedRatio();
      if (hot_segments_.count(id) != 0) {
        continue;
      }
      auto seg = segments_[id];
      if (seg->NeedsCompaction()) {
        LOG(INFO) << "SegmentManager::RunCompaction: segment " << id
                  << " needs compaction, deleted ratio: "
                  << seg->GetDeletedRatio();
        candidates.push_back(id);
      }
    }
  }

  LOG(INFO) << "SegmentManager::RunCompaction: found " << candidates.size()
            << " candidates for compaction";

  for (const uint32_t src_id : candidates) {
    auto active = GetActiveSegment();
    if (active == nullptr) {
      return Status::kNoSpace;
    }

    std::shared_ptr<SegmentBase> src;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = segments_.find(src_id);
      if (it == segments_.end()) {
        continue;
      }
      src = it->second;
    }

    LOG(INFO) << "==== SegmentManager::RunCompaction: src_id=" << src_id;
    const Status s = src->Compact(
        active.get(), [this](void* key, uint32_t old_seg, uint32_t old_slot,
                             uint32_t new_seg, uint32_t new_slot) {
          if (slot_move_cb_) {
            slot_move_cb_(key, old_seg, old_slot, new_seg, new_slot);
          }
        });
    if (s != Status::kOk) {
      return s;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      full_set_.erase(src_id);
      full_order_.erase(
          std::remove(full_order_.begin(), full_order_.end(), src_id),
          full_order_.end());
      idle_set_.insert(src_id);

      const uint32_t target_id = active->GetSegmentId();
      const SegmentState new_state = active->GetState();
      if (new_state == SegmentState::FULL) {
        appending_set_.erase(target_id);
        full_set_.insert(target_id);
        full_order_.push_back(target_id);
      }
    }
  }

  return Status::kOk;
}

// ---------------------------------------------------------------------------
// Hot / Cold management
// ---------------------------------------------------------------------------

void SegmentManager::RecordAccess(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  access_counters_[segment_id]++;
}

Status SegmentManager::EvaluateHotCold(uint64_t hot_threshold,
                                       uint32_t max_hot_segments) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto& [id, count] : access_counters_) {
    if (count >= hot_threshold && hot_segments_.count(id) == 0 &&
        hot_segments_.size() < max_hot_segments) {
      hot_segments_.insert(id);
    }
  }

  std::vector<uint32_t> to_demote;
  for (const uint32_t id : hot_segments_) {
    auto it = access_counters_.find(id);
    if (it == access_counters_.end() || it->second < hot_threshold) {
      to_demote.push_back(id);
    }
  }
  for (const uint32_t id : to_demote) {
    hot_segments_.erase(id);
  }

  while (hot_segments_.size() > max_hot_segments) {
    uint32_t victim = 0;
    uint64_t min_count = UINT64_MAX;
    for (const uint32_t id : hot_segments_) {
      const uint64_t c = access_counters_[id];
      if (c < min_count) {
        min_count = c;
        victim = id;
      }
    }
    hot_segments_.erase(victim);
  }

  access_counters_.clear();

  return Status::kOk;
}

bool SegmentManager::IsHotSegment(uint32_t segment_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hot_segments_.count(segment_id) > 0;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

uint32_t SegmentManager::GetTotalSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(segments_.size());
}

uint32_t SegmentManager::GetIdleSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(idle_set_.size());
}

uint32_t SegmentManager::GetFullSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(full_set_.size());
}

std::vector<uint32_t> SegmentManager::GetFullSegmentOrder() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return full_order_;
}

}  // namespace segment
}  // namespace parakv
