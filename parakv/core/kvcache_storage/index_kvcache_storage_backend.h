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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "parakv/core/cache/lfuda_cache.h"
#include "parakv/core/index/index.h"
#include "parakv/core/kvcache_storage/kvcache_storage_backend.h"
#include "parakv/core/segment/segment_manager.h"

namespace parakv {
namespace kvcache_storage {

// Index-backed KVCache storage backend using Index<Key128>.
// Each instance serves a single namespace; namespace routing is handled
// by BackendNamespaceManager.
// Expected key format: exactly 16 bytes.
class IndexKVCacheStorageBackend final : public KVCacheStorageBackend {
 public:
  struct Options {
    std::string root_dir = "/tmp/parakv-index128-backend";
    std::string namespace_name;
    uint64_t segment_size = 64ULL * 1024 * 1024;
    uint32_t slot_value_size = 4096;
    uint32_t segment_count = 1;
    uint64_t wal_checkpoint_bytes = 1ULL << 30;
    // Background stats logging interval in seconds. 0 disables the thread.
    uint32_t stats_interval_sec = 10;
    // LFU eviction: capacity = total_segment_slots * lfu_capacity_ratio.
    // 0 disables LFU eviction entirely.
    double lfu_capacity_ratio = 0.9;
    // Dynamic aging interval in seconds for LFU frequency decay.
    uint32_t lfu_age_tick_sec = 60;
  };

  explicit IndexKVCacheStorageBackend(Options opts);
  ~IndexKVCacheStorageBackend() override;

  WriteResult Put(const std::string& key, const std::string& value,
                  const std::string& metadata,
                  const WriteOptions& opts) override;

  ReadResult Get(const std::string& key, const ReadOptions& opts) override;

  WriteResult Delete(const std::string& key) override;

  void Close() override;

 private:
  using LFUCache = lfuda_cache<index::Key128, bool, thread_safe::yes,
                               index::Key128Hash, index::Key128Eq>;

  struct Context {
    std::shared_ptr<segment::SegmentManager> segment_manager;
    std::shared_ptr<index::Index128> index;
    std::vector<std::shared_ptr<segment::SegmentBase>> segments;
    std::unique_ptr<LFUCache> lfu;
  };

  static constexpr uint32_t kHeaderBytes = 8;  // value_len(4) + metadata_len(4)

  BackendCode EnsureValidOptions() const;
  BackendCode EncodePayload(const std::string& value,
                            const std::string& metadata,
                            std::vector<uint8_t>* payload,
                            std::string* err) const;
  BackendCode DecodePayload(const std::vector<uint8_t>& payload,
                            std::string* value, std::string* metadata,
                            std::string* err) const;
  BackendCode ToBackendCode(parakv::Status s) const;

  BackendCode CreateContext();
  std::shared_ptr<Context> GetContext() const;

  void StartStatsThread();
  void StopStatsThread();
  void StatsLoop();

  Options options_;
  mutable std::mutex mu_;
  std::shared_ptr<Context> context_;

  std::thread stats_thread_;
  std::mutex stats_mu_;
  std::condition_variable stats_cv_;
  bool stats_stop_ = false;
};

}  // namespace kvcache_storage
}  // namespace parakv
