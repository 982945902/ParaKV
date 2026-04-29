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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "parakv/core/index/index.h"
#include "parakv/core/kvcache_storage/kvcache_storage_backend.h"
#include "parakv/core/segment/segment_manager.h"

namespace parakv {
namespace kvcache_storage {

// Index-backed KVCache storage backend using Index<Key128>.
// Expected key format: exactly 16 bytes.
class IndexKVCacheStorageBackend final : public KVCacheStorageBackend {
 public:
  struct Options {
    std::string root_dir = "/tmp/parakv-index128-backend";
    uint64_t segment_size = 64ULL * 1024 * 1024;
    uint32_t slot_value_size = 4096;
    uint32_t segment_count = 1;
    uint64_t wal_checkpoint_bytes = 1ULL << 30;
  };

  explicit IndexKVCacheStorageBackend(Options opts);
  ~IndexKVCacheStorageBackend() override;

  WriteResult Put(const std::string& ns, const std::string& key,
                  const std::string& value, const std::string& metadata,
                  const WriteOptions& opts) override;

  ReadResult Get(const std::string& ns, const std::string& key,
                 const ReadOptions& opts) override;

  // Iterate every namespace, dump a snapshot and close its index/WAL.
  // Idempotent: subsequent calls are no-ops because namespaces_ is cleared.
  void Close() override;

 private:
  struct NamespaceContext {
    std::shared_ptr<segment::SegmentManager> segment_manager;
    std::shared_ptr<index::Index128> index;
    std::vector<std::shared_ptr<segment::SegmentBase>> segments;
  };

  static constexpr uint32_t kHeaderBytes = 8;  // value_len(4) + metadata_len(4)

  BackendCode EnsureValidOptions() const;
  BackendCode ParseKey128(const std::string& key, index::Key128* out) const;
  BackendCode EncodePayload(const std::string& value,
                            const std::string& metadata,
                            std::vector<uint8_t>* payload,
                            std::string* err) const;
  BackendCode DecodePayload(const std::vector<uint8_t>& payload,
                            std::string* value, std::string* metadata,
                            std::string* err) const;
  BackendCode ToBackendCode(parakv::Status s) const;

  std::shared_ptr<NamespaceContext> GetOrCreateNamespace(const std::string& ns,
                                                         BackendCode* code,
                                                         std::string* message);
  static std::string SanitizeNs(const std::string& ns);

  Options options_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<NamespaceContext>>
      namespaces_;
};

}  // namespace kvcache_storage
}  // namespace parakv
