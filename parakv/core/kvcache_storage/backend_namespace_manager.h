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

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "parakv/core/kvcache_storage/kvcache_storage_backend.h"

namespace parakv {
namespace kvcache_storage {

// Thread-safe manager that routes requests to pre-registered per-namespace
// backend instances.  All backends must be registered via Register() before
// the service starts accepting traffic; runtime creation is not supported.
class BackendNamespaceManager {
 public:
  BackendNamespaceManager();
  ~BackendNamespaceManager();

  BackendNamespaceManager(const BackendNamespaceManager&) = delete;
  BackendNamespaceManager& operator=(const BackendNamespaceManager&) = delete;

  // Pre-register a backend for the given namespace.
  // Returns false if `ns` is empty, `backend` is null, or `ns` is already
  // registered (existing registration is kept).
  bool Register(const std::string& ns,
                std::shared_ptr<KVCacheStorageBackend> backend);

  // Write one entry.
  WriteResult Put(const std::string& ns, const std::string& key,
                  const std::string& value, const std::string& metadata,
                  const WriteOptions& opts);

  // Read one entry.
  ReadResult Get(const std::string& ns, const std::string& key,
                 const ReadOptions& opts);

  // Delete one entry.
  WriteResult Delete(const std::string& ns, const std::string& key);

  void Close();

  // Replace characters unsafe for filesystem paths with '_'.
  static std::string SanitizeNs(const std::string& ns);

 private:
  std::shared_ptr<KVCacheStorageBackend> GetBackend(
      const std::string& ns) const;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<KVCacheStorageBackend>>
      backends_;
};

}  // namespace kvcache_storage
}  // namespace parakv
