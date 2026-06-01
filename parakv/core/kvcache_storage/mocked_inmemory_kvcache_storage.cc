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

#include "mocked_inmemory_kvcache_storage.h"

#include <glog/logging.h>

#include <chrono>
#include <memory>
#include <utility>

#include "backend_registry.h"

namespace parakv {
namespace kvcache_storage {

MockedInMemoryKVCacheStorage::MockedInMemoryKVCacheStorage()
    : MockedInMemoryKVCacheStorage(Options{}) {}

MockedInMemoryKVCacheStorage::MockedInMemoryKVCacheStorage(Options opts)
    : options_(opts) {}

MockedInMemoryKVCacheStorage::~MockedInMemoryKVCacheStorage() = default;

uint64_t MockedInMemoryKVCacheStorage::NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

WriteResult MockedInMemoryKVCacheStorage::Put(const std::string& key,
                                              const std::string& value,
                                              const std::string& metadata,
                                              const WriteOptions& opts) {
  if (options_.max_value_bytes > 0 && value.size() > options_.max_value_bytes) {
    return {BackendCode::kValueTooLarge, "value exceeds backend limit"};
  }

  const uint64_t ttl_ms =
      opts.ttl_ms > 0 ? opts.ttl_ms : options_.default_ttl_ms;
  Entry entry;
  entry.value = value;
  entry.metadata = metadata;
  entry.expiration_ms = ttl_ms > 0 ? (NowMs() + ttl_ms) : 0;

  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(key);
  if (it != store_.end()) {
    const bool expired =
        it->second.expiration_ms > 0 && it->second.expiration_ms <= NowMs();
    if (!opts.overwrite && !expired) {
      return {BackendCode::kAlreadyExists, "key already exists"};
    }
    it->second = std::move(entry);
  } else {
    store_.emplace(key, std::move(entry));
  }
  return {BackendCode::kOk, {}};
}

ReadResult MockedInMemoryKVCacheStorage::Get(const std::string& key,
                                             const ReadOptions& opts) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(key);
  if (it == store_.end()) {
    return {BackendCode::kNotFound, "miss", {}, {}, 0};
  }

  const uint64_t now_ms = NowMs();
  if (it->second.expiration_ms > 0 && it->second.expiration_ms <= now_ms) {
    store_.erase(it);
    return {BackendCode::kNotFound, "expired", {}, {}, 0};
  }

  if (opts.touch_on_hit && it->second.expiration_ms > 0 &&
      options_.default_ttl_ms > 0) {
    it->second.expiration_ms = now_ms + options_.default_ttl_ms;
  }

  ReadResult r;
  r.code = BackendCode::kOk;
  r.expiration_ms = it->second.expiration_ms;
  r.metadata = it->second.metadata;
  if (opts.include_value) {
    r.value = it->second.value;
  }
  return r;
}

size_t MockedInMemoryKVCacheStorage::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_.size();
}

}  // namespace kvcache_storage
}  // namespace parakv
