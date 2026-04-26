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

std::string MockedInMemoryKVCacheStorage::MakeKey(const std::string& ns,
                                                  const std::string& key) {
  std::string out;
  out.reserve(ns.size() + 1 + key.size());
  out.append(ns);
  out.push_back('\0');
  out.append(key);
  return out;
}

WriteResult MockedInMemoryKVCacheStorage::Put(const std::string& ns,
                                              const std::string& key,
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

  const std::string full_key = MakeKey(ns, key);
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(full_key);
  if (it != store_.end()) {
    const bool expired =
        it->second.expiration_ms > 0 && it->second.expiration_ms <= NowMs();
    if (!opts.overwrite && !expired) {
      return {BackendCode::kAlreadyExists, "key already exists"};
    }
    it->second = std::move(entry);
  } else {
    store_.emplace(full_key, std::move(entry));
  }
  return {BackendCode::kOk, {}};
}

ReadResult MockedInMemoryKVCacheStorage::Get(const std::string& ns,
                                             const std::string& key,
                                             const ReadOptions& opts) {
  const std::string full_key = MakeKey(ns, key);
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(full_key);
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

// Self-registration. Redundant with RegisterBuiltinBackends() on purpose:
// the explicit bootstrap guarantees the backend is reachable even when this
// TU is dropped by the static linker; the macro keeps the "where is this
// name bound?" answer co-located with the implementation.
PARAKV_REGISTER_KVCACHE_BACKEND(
    "memory",
    [](const ::parakv::kvcache_storage::BackendConfig& cfg)
        -> std::shared_ptr<::parakv::kvcache_storage::KVCacheStorageBackend> {
      ::parakv::kvcache_storage::MockedInMemoryKVCacheStorage::Options opts;
      opts.max_value_bytes = cfg.GetUint("max_value_bytes", 0);
      opts.default_ttl_ms = cfg.GetUint("default_ttl_ms", 0);
      return std::make_shared<
          ::parakv::kvcache_storage::MockedInMemoryKVCacheStorage>(opts);
    });
