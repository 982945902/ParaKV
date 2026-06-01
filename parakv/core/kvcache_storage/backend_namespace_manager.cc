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

#include "backend_namespace_manager.h"

#include <glog/logging.h>

#include <utility>

namespace parakv {
namespace kvcache_storage {

BackendNamespaceManager::BackendNamespaceManager() = default;

BackendNamespaceManager::~BackendNamespaceManager() = default;

std::string BackendNamespaceManager::SanitizeNs(const std::string& ns) {
  std::string out;
  out.reserve(ns.size());
  for (char c : ns) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) {
    out = "default";
  }
  return out;
}

bool BackendNamespaceManager::Register(
    const std::string& ns, std::shared_ptr<KVCacheStorageBackend> backend) {
  if (ns.empty()) {
    LOG(ERROR) << "BackendNamespaceManager::Register: empty namespace";
    return false;
  }
  if (!backend) {
    LOG(ERROR) << "BackendNamespaceManager::Register: null backend for "
                  "namespace='"
               << ns << "'";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto [it, inserted] = backends_.emplace(ns, std::move(backend));
  if (!inserted) {
    LOG(WARNING) << "BackendNamespaceManager::Register: namespace='" << ns
                 << "' already registered; keeping existing backend";
    return false;
  }

  LOG(INFO) << "BackendNamespaceManager::Register: registered namespace='" << ns
            << "'";
  return true;
}

std::shared_ptr<KVCacheStorageBackend> BackendNamespaceManager::GetBackend(
    const std::string& ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = backends_.find(ns);
  if (it != backends_.end()) {
    return it->second;
  }
  return nullptr;
}

WriteResult BackendNamespaceManager::Put(const std::string& ns,
                                         const std::string& key,
                                         const std::string& value,
                                         const std::string& metadata,
                                         const WriteOptions& opts) {
  auto backend = GetBackend(ns);
  if (!backend) {
    return {BackendCode::kInvalidArgument,
            "namespace '" + ns + "' is not registered"};
  }
  return backend->Put(key, value, metadata, opts);
}

ReadResult BackendNamespaceManager::Get(const std::string& ns,
                                        const std::string& key,
                                        const ReadOptions& opts) {
  auto backend = GetBackend(ns);
  if (!backend) {
    return {BackendCode::kInvalidArgument,
            "namespace '" + ns + "' is not registered",
            {},
            {},
            0};
  }
  return backend->Get(key, opts);
}

void BackendNamespaceManager::Close() {
  std::unordered_map<std::string, std::shared_ptr<KVCacheStorageBackend>> taken;
  {
    std::lock_guard<std::mutex> lock(mu_);
    taken.swap(backends_);
  }

  LOG(INFO) << "BackendNamespaceManager::Close: closing " << taken.size()
            << " namespace backends";

  for (auto& [ns, backend] : taken) {
    if (!backend) continue;
    LOG(INFO) << "BackendNamespaceManager::Close: closing namespace='" << ns
              << "'";
    backend->Close();
  }
}

}  // namespace kvcache_storage
}  // namespace parakv
