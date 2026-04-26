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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvcache_storage_backend.h"

namespace parakv {
namespace kvcache_storage {

// Generic key/value configuration passed to a backend factory. Values are
// stored as strings so the same object can be populated from gflags, a YAML
// file, an etcd node, or tests.
//
// Typed helpers (GetUint / GetInt / GetBool) return the provided default
// when the key is missing or unparseable; they never throw.
class BackendConfig {
 public:
  BackendConfig() = default;

  void Set(std::string key, std::string value);

  bool Has(const std::string& key) const;
  const std::string& Get(const std::string& key,
                         const std::string& default_value = kEmpty) const;
  int64_t GetInt(const std::string& key, int64_t default_value = 0) const;
  uint64_t GetUint(const std::string& key, uint64_t default_value = 0) const;
  bool GetBool(const std::string& key, bool default_value = false) const;

  const std::unordered_map<std::string, std::string>& entries() const {
    return entries_;
  }

 private:
  static const std::string kEmpty;
  std::unordered_map<std::string, std::string> entries_;
};

// Factory callable owned by the registry. Implementations should return
// nullptr (and LOG the reason) when the config is invalid.
using BackendFactory =
    std::function<std::shared_ptr<KVCacheStorageBackend>(const BackendConfig&)>;

// Process-wide registry for KVCache storage backends. Meyers singleton,
// thread-safe for registration and lookup.
class BackendRegistry {
 public:
  static BackendRegistry& Instance();

  // Returns false if `name` is empty, `factory` is null, or the name is
  // already registered (the existing registration is kept).
  bool Register(std::string name, BackendFactory factory);

  // Returns nullptr if `name` is unknown or the factory itself returns
  // nullptr. The caller is responsible for logging the final status.
  std::shared_ptr<KVCacheStorageBackend> Create(
      const std::string& name, const BackendConfig& config) const;

  bool IsRegistered(const std::string& name) const;
  std::vector<std::string> RegisteredNames() const;

 private:
  BackendRegistry() = default;
  BackendRegistry(const BackendRegistry&) = delete;
  BackendRegistry& operator=(const BackendRegistry&) = delete;

  mutable std::mutex mu_;
  std::unordered_map<std::string, BackendFactory> factories_;
};

// Register every backend shipped inside libkvcache_storage_backend.a.
//
// Self-registration via PARAKV_REGISTER_KVCACHE_BACKEND works when the
// defining translation unit is pulled in by the linker, but with static
// libraries an unreferenced TU is typically discarded. Call this once from
// `main()` (or a test fixture) to guarantee built-in backends are available
// regardless of link order.
void RegisterBuiltinBackends();

}  // namespace kvcache_storage
}  // namespace parakv

// ---------------------------------------------------------------------------
// Self-registration macro
//
// Usage (at file scope, inside the .cc of the backend):
//
//   PARAKV_REGISTER_KVCACHE_BACKEND("memory",
//       [](const parakv::kvcache_storage::BackendConfig& cfg) {
//         MockedInMemoryKVCacheStorage::Options opts;
//         opts.max_value_bytes = cfg.GetUint("max_value_bytes");
//         return std::make_shared<MockedInMemoryKVCacheStorage>(opts);
//       });
//
// The macro creates a file-scope object whose constructor performs the
// registration at static-init time. See note on RegisterBuiltinBackends()
// for the static-library caveat.
// ---------------------------------------------------------------------------

#define PARAKV_KV_BACKEND_CONCAT_IMPL(x, y) x##y
#define PARAKV_KV_BACKEND_CONCAT(x, y) PARAKV_KV_BACKEND_CONCAT_IMPL(x, y)

#define PARAKV_REGISTER_KVCACHE_BACKEND(name_str, factory_expr)          \
  namespace {                                                            \
  struct PARAKV_KV_BACKEND_CONCAT(KvCacheBackendRegistrar_, __LINE__) {  \
    PARAKV_KV_BACKEND_CONCAT(KvCacheBackendRegistrar_, __LINE__)() {     \
      ::parakv::kvcache_storage::BackendRegistry::Instance().Register(   \
          (name_str), (factory_expr));                                   \
    }                                                                    \
  };                                                                     \
  static PARAKV_KV_BACKEND_CONCAT(KvCacheBackendRegistrar_, __LINE__)    \
      PARAKV_KV_BACKEND_CONCAT(g_kv_cache_backend_registrar_, __LINE__); \
  }  // namespace
