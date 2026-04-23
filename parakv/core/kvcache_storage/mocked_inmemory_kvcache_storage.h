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
#include <mutex>
#include <string>
#include <unordered_map>

#include "kvcache_storage_backend.h"

namespace parakv {
namespace kvcache_storage {

// Reference backend intended for unit tests, smoke tests and demos. Not
// production-grade: all data lives in a flat in-memory hash map protected
// by a single mutex and there is no eviction / on-disk persistence.
class MockedInMemoryKVCacheStorage final : public KVCacheStorageBackend {
 public:
  struct Options {
    uint64_t max_value_bytes = 0;  // 0 means unlimited.
    uint64_t default_ttl_ms = 0;   // 0 means never expire.
  };

  MockedInMemoryKVCacheStorage();
  explicit MockedInMemoryKVCacheStorage(Options opts);
  ~MockedInMemoryKVCacheStorage() override;

  WriteResult Put(const std::string& ns, const std::string& key,
                  const std::string& value, const std::string& metadata,
                  const WriteOptions& opts) override;

  ReadResult Get(const std::string& ns, const std::string& key,
                 const ReadOptions& opts) override;

  size_t size() const;

 private:
  struct Entry {
    std::string value;
    std::string metadata;
    uint64_t expiration_ms = 0;  // 0 means never expire.
  };

  static uint64_t NowMs();
  static std::string MakeKey(const std::string& ns, const std::string& key);

  Options options_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> store_;  // key = ns + '\0' + key
};

}  // namespace kvcache_storage
}  // namespace parakv
