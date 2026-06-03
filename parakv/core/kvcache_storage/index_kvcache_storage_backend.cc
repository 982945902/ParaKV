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

#include "index_kvcache_storage_backend.h"

#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "backend_registry.h"
#include "parakv/core/segment/segment_file.h"

namespace parakv {
namespace kvcache_storage {

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void WriteU32LE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

bool EnsureDir(const std::string& dir) {
  if (dir.empty()) {
    LOG(ERROR) << "EnsureDir: dir is empty";
    return false;
  }

  if (::access(dir.c_str(), F_OK) == 0) {
    return true;
  }

  std::string current;
  if (dir[0] == '/') {
    current = "/";
  }
  size_t pos = 0;
  while (pos < dir.size()) {
    const size_t next = dir.find('/', pos);
    const std::string part = dir.substr(
        pos, next == std::string::npos ? std::string::npos : next - pos);
    pos = (next == std::string::npos) ? dir.size() : (next + 1);
    if (part.empty()) {
      continue;
    }

    if (!current.empty() && current.back() != '/') {
      current.push_back('/');
    }
    current.append(part);
    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
      return false;
    }
  }

  return true;
}

}  // namespace

IndexKVCacheStorageBackend::IndexKVCacheStorageBackend(Options opts)
    : options_(std::move(opts)) {}

IndexKVCacheStorageBackend::~IndexKVCacheStorageBackend() = default;

BackendCode IndexKVCacheStorageBackend::EnsureValidOptions() const {
  if (options_.slot_value_size <= kHeaderBytes) {
    return BackendCode::kInvalidArgument;
  }

  if (options_.segment_count == 0) {
    return BackendCode::kInvalidArgument;
  }

  return BackendCode::kOk;
}

BackendCode IndexKVCacheStorageBackend::EncodePayload(
    const std::string& value, const std::string& metadata,
    std::vector<uint8_t>* payload, std::string* err) const {
  if (payload == nullptr) {
    return BackendCode::kInvalidArgument;
  }

  const uint32_t max_payload = options_.slot_value_size - kHeaderBytes;
  const uint64_t total = static_cast<uint64_t>(value.size()) + metadata.size();
  if (total > max_payload) {
    if (err != nullptr) {
      *err = "value+metadata exceeds slot_value_size";
    }

    LOG(ERROR) << "Value+metadata exceeds slot_value_size: value.size()="
               << value.size() << " metadata.size()=" << metadata.size()
               << " total=" << total << " max_payload=" << max_payload;

    return BackendCode::kValueTooLarge;
  }

  payload->assign(options_.slot_value_size, 0);
  WriteU32LE(payload->data(), static_cast<uint32_t>(value.size()));
  WriteU32LE(payload->data() + 4, static_cast<uint32_t>(metadata.size()));
  if (!value.empty()) {
    std::memcpy(payload->data() + kHeaderBytes, value.data(), value.size());
  }
  if (!metadata.empty()) {
    std::memcpy(payload->data() + kHeaderBytes + value.size(), metadata.data(),
                metadata.size());
  }

  return BackendCode::kOk;
}

BackendCode IndexKVCacheStorageBackend::DecodePayload(
    const std::vector<uint8_t>& payload, std::string* value,
    std::string* metadata, std::string* err) const {
  if (payload.size() < kHeaderBytes || value == nullptr ||
      metadata == nullptr) {
    return BackendCode::kCorrupted;
  }

  const uint32_t value_len = ReadU32LE(payload.data());
  const uint32_t metadata_len = ReadU32LE(payload.data() + 4);
  const uint64_t total = static_cast<uint64_t>(value_len) + metadata_len;
  if (kHeaderBytes + total > payload.size()) {
    if (err != nullptr) *err = "corrupted payload header";
    return BackendCode::kCorrupted;
  }

  value->assign(reinterpret_cast<const char*>(payload.data() + kHeaderBytes),
                value_len);
  metadata->assign(
      reinterpret_cast<const char*>(payload.data() + kHeaderBytes + value_len),
      metadata_len);

  return BackendCode::kOk;
}

BackendCode IndexKVCacheStorageBackend::ToBackendCode(parakv::Status s) const {
  switch (s) {
    case parakv::Status::kOk:
      return BackendCode::kOk;
    case parakv::Status::kNotFound:
      return BackendCode::kNotFound;
    case parakv::Status::kFull:
    case parakv::Status::kNoSpace:
      return BackendCode::kStorageFull;
    case parakv::Status::kIOError:
      return BackendCode::kIOError;
    case parakv::Status::kInvalidArgument:
      return BackendCode::kInvalidArgument;
    case parakv::Status::kCorruption:
      return BackendCode::kCorrupted;
    default:
      return BackendCode::kUnavailable;
  }
}

std::shared_ptr<IndexKVCacheStorageBackend::Context>
IndexKVCacheStorageBackend::GetOrCreateContext(BackendCode* code,
                                               std::string* message) {
  std::lock_guard<std::mutex> lock(mu_);
  if (context_) {
    return context_;
  }

  if (EnsureValidOptions() != BackendCode::kOk) {
    if (code) {
      *code = BackendCode::kInvalidArgument;
    }
    if (message) {
      *message = "invalid index backend options";
    }
    return nullptr;
  }

  if (!EnsureDir(options_.root_dir)) {
    if (code) {
      *code = BackendCode::kIOError;
    }
    if (message) {
      *message = "failed to create backend directory";
    }
    return nullptr;
  }

  segment::SegmentConfig seg_cfg;
  seg_cfg.key_size = sizeof(index::Key128);
  seg_cfg.value_size = options_.slot_value_size;
  seg_cfg.segment_size = options_.segment_size;

  auto mgr = std::make_shared<segment::SegmentManager>(seg_cfg);
  std::vector<std::shared_ptr<segment::SegmentBase>> opened_segments;
  opened_segments.reserve(options_.segment_count);
  for (uint32_t i = 0; i < options_.segment_count; ++i) {
    std::ostringstream seg_path;
    seg_path << options_.root_dir << "/segment_" << i << ".dat";
    auto seg =
        std::make_shared<segment::SegmentFile>(i, seg_cfg, seg_path.str());
    parakv::Status s = seg->Open();
    if (s != parakv::Status::kOk) {
      if (code) {
        *code = ToBackendCode(s);
      }
      if (message) {
        *message = "failed to open segment file";
      }
      return nullptr;
    }
    s = mgr->AddSegment(seg);
    if (s != parakv::Status::kOk) {
      if (code) {
        *code = ToBackendCode(s);
      }
      if (message) {
        *message = "failed to add segment";
      }
      return nullptr;
    }
    opened_segments.push_back(seg);
  }

  // Restore segment manager state (full-order, etc.) from a previous run.
  const std::string state_path = options_.root_dir + "/segment_manager.state";
  parakv::Status ls = mgr->LoadState(state_path);
  if (ls != parakv::Status::kOk) {
    LOG(WARNING) << "SegmentManager::LoadState returned "
                 << static_cast<int>(ls) << " for " << state_path;
  }

  index::Index128::Options idx_opts;
  idx_opts.segment_manager = mgr;
  idx_opts.segment_config = seg_cfg;
  idx_opts.wal_checkpoint_bytes = options_.wal_checkpoint_bytes;
  idx_opts.wal_path = options_.root_dir + "/index.wal";
  idx_opts.snapshot_dir = options_.root_dir;
  idx_opts.namespace_name = options_.namespace_name;

  auto idx = std::make_shared<index::Index128>(idx_opts);
  parakv::Status s = idx->Open();
  if (s != parakv::Status::kOk) {
    if (code) {
      *code = ToBackendCode(s);
    }
    if (message) {
      *message = "failed to open index";
    }
    return nullptr;
  }
  idx->AttachToSegmentManager();

  auto ctx = std::make_shared<Context>();
  ctx->segment_manager = mgr;
  ctx->index = idx;
  ctx->segments = std::move(opened_segments);
  context_ = ctx;
  if (code) {
    *code = BackendCode::kOk;
  }
  return ctx;
}

WriteResult IndexKVCacheStorageBackend::Put(const std::string& key,
                                            const std::string& value,
                                            const std::string& metadata,
                                            const WriteOptions& opts) {
  index::Key128 k{};
  if (!index::Key128::FromString(key, &k)) {
    return {BackendCode::kInvalidArgument,
            "key must be exactly 16 bytes (Key128)"};
  }

  BackendCode create_code = BackendCode::kOk;
  std::string create_msg;
  auto ctx = GetOrCreateContext(&create_code, &create_msg);
  if (!ctx) {
    return {create_code, create_msg};
  }

  if (!opts.overwrite) {
    std::vector<uint8_t> probe(options_.slot_value_size, 0);
    parakv::Status s = ctx->index->Get(k, probe.data(), probe.size());
    if (s == parakv::Status::kOk) {
      return {BackendCode::kAlreadyExists, "key already exists"};
    }
    if (s != parakv::Status::kNotFound) {
      return {ToBackendCode(s), "failed to probe existing key"};
    }
  }

  std::vector<uint8_t> payload;
  std::string err;
  BackendCode c = EncodePayload(value, metadata, &payload, &err);
  if (c != BackendCode::kOk) {
    return {c, err};
  }

  parakv::Status s = ctx->index->Put(k, payload.data(), payload.size());
  if (s != parakv::Status::kOk) {
    return {ToBackendCode(s), "index put failed"};
  }
  return {BackendCode::kOk, {}};
}

ReadResult IndexKVCacheStorageBackend::Get(const std::string& key,
                                           const ReadOptions& opts) {
  index::Key128 k{};
  if (!index::Key128::FromString(key, &k)) {
    return {BackendCode::kInvalidArgument,
            "key must be exactly 16 bytes (Key128)",
            {},
            {},
            0};
  }

  LOG(INFO) << "IndexKVCacheStorageBackend::Get: namespace='"
            << options_.namespace_name << "', key='" << k.ToString() << "'";

  BackendCode create_code = BackendCode::kOk;
  std::string create_msg;
  auto ctx = GetOrCreateContext(&create_code, &create_msg);
  if (!ctx) {
    return {create_code, create_msg, {}, {}, 0};
  }

  std::vector<uint8_t> payload(options_.slot_value_size, 0);
  parakv::Status s = ctx->index->Get(k, payload.data(), payload.size());
  if (s != parakv::Status::kOk) {
    return {ToBackendCode(s),
            s == parakv::Status::kNotFound ? "miss" : "index get failed",
            {},
            {},
            0};
  }

  std::string val;
  std::string meta;
  std::string err;
  BackendCode c = DecodePayload(payload, &val, &meta, &err);
  if (c != BackendCode::kOk) {
    return {c, err, {}, {}, 0};
  }

  ReadResult r;
  r.code = BackendCode::kOk;
  r.metadata = meta;
  if (opts.include_value) {
    r.value = val;
  }

  return r;
}

WriteResult IndexKVCacheStorageBackend::Delete(const std::string& key) {
  index::Key128 k{};
  if (!index::Key128::FromString(key, &k)) {
    return {BackendCode::kInvalidArgument,
            "key must be exactly 16 bytes (Key128)"};
  }

  BackendCode create_code = BackendCode::kOk;
  std::string create_msg;
  auto ctx = GetOrCreateContext(&create_code, &create_msg);
  if (!ctx) {
    return {create_code, create_msg};
  }

  parakv::Status s = ctx->index->Delete(k);
  if (s != parakv::Status::kOk) {
    return {ToBackendCode(s), s == parakv::Status::kNotFound
                                  ? "key not found"
                                  : "index delete failed"};
  }
  return {BackendCode::kOk, {}};
}

void IndexKVCacheStorageBackend::Close() {
  std::shared_ptr<Context> ctx;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ctx.swap(context_);
  }

  if (!ctx || !ctx->index) {
    return;
  }

  LOG(INFO) << "IndexKVCacheStorageBackend::Close: checkpointing namespace='"
            << options_.namespace_name << "'";
  parakv::Status s = ctx->index->Checkpoint();
  if (s != parakv::Status::kOk) {
    LOG(WARNING) << "Checkpoint failed for namespace='"
                 << options_.namespace_name
                 << "', status=" << static_cast<int>(s);
  }

  // Persist segment manager state so full-segment order survives restarts.
  if (ctx->segment_manager) {
    const std::string state_path = options_.root_dir + "/segment_manager.state";
    parakv::Status ss = ctx->segment_manager->SaveState(state_path);
    if (ss != parakv::Status::kOk) {
      LOG(WARNING) << "SegmentManager::SaveState failed for namespace='"
                   << options_.namespace_name << "'";
    }
  }

  (void)ctx->index->Close();
}

}  // namespace kvcache_storage
}  // namespace parakv
