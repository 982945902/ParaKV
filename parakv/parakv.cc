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

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "core/kvcache_storage/backend_registry.h"
#include "service/kvcache_storage_service_impl.h"

DEFINE_int32(port, 9200, "brpc listen port");
DEFINE_int32(idle_timeout_s, 60,
             "Connection idle timeout, in seconds. -1 disables the timeout.");
DEFINE_int32(max_batch_size, 1024, "Max items per batch");
DEFINE_uint64(max_value_bytes, 0, "Per-item value size limit; 0 = no limit");

DEFINE_string(backend, "kvcache_memory",
              "KVCache storage backend name (see RegisteredNames())");
DEFINE_string(backend_opts, "",
              "Comma-separated key=value list passed to the backend factory, "
              "e.g. --backend_opts=max_value_bytes=1048576,default_ttl_ms=0");

namespace {

// Parse "k1=v1,k2=v2" into BackendConfig. Whitespace around keys / values is
// trimmed; empty segments and segments without '=' are skipped with a warning.
parakv::kvcache_storage::BackendConfig ParseBackendOpts(const std::string& s) {
  parakv::kvcache_storage::BackendConfig cfg;
  std::stringstream ss(s);
  std::string segment;
  while (std::getline(ss, segment, ',')) {
    const auto first = segment.find_first_not_of(" \t");
    const auto last = segment.find_last_not_of(" \t");
    if (first == std::string::npos) continue;
    segment = segment.substr(first, last - first + 1);

    const auto eq = segment.find('=');
    if (eq == std::string::npos || eq == 0) {
      LOG(WARNING) << "Ignoring malformed --backend_opts segment: " << segment;
      continue;
    }
    cfg.Set(segment.substr(0, eq), segment.substr(eq + 1));
  }
  return cfg;
}

std::string JoinRegisteredNames() {
  const auto names =
      parakv::kvcache_storage::BackendRegistry::Instance().RegisteredNames();
  std::string joined;
  for (const auto& n : names) {
    if (!joined.empty()) joined.append(", ");
    joined.append(n);
  }
  return joined;
}

}  // namespace

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // parakv::kvcache_storage::RegisterBuiltinBackends();

  auto cfg = ParseBackendOpts(FLAGS_backend_opts);
  auto backend = parakv::kvcache_storage::BackendRegistry::Instance().Create(
      FLAGS_backend, cfg);
  if (!backend) {
    LOG(ERROR) << "Failed to create backend '" << FLAGS_backend
               << "'. Available backends: [" << JoinRegisteredNames() << "]";
    return -1;
  }
  LOG(INFO) << "KVCache backend: " << FLAGS_backend;

  parakv::service::ServiceOptions svc_opts;
  svc_opts.max_batch_size = static_cast<uint32_t>(FLAGS_max_batch_size);
  svc_opts.max_value_bytes = FLAGS_max_value_bytes;
  auto service_impl =
      std::make_unique<parakv::service::KVCacheStorageServiceImpl>(backend,
                                                                   svc_opts);

  brpc::Server server;
  if (server.AddService(service_impl.get(), brpc::SERVER_DOESNT_OWN_SERVICE) !=
      0) {
    LOG(ERROR) << "Failed to add KVCacheStorageService";
    return -1;
  }

  brpc::ServerOptions options;
  options.idle_timeout_sec = FLAGS_idle_timeout_s;
  if (server.Start(FLAGS_port, &options) != 0) {
    LOG(ERROR) << "Failed to start server on port " << FLAGS_port;
    return -1;
  }

  LOG(INFO) << "KVCacheStorageService listening on :" << FLAGS_port;
  server.RunUntilAskedToQuit();

  std::cout << "Press Enter to shutdown..." << std::endl;

  // Graceful shutdown: brpc has stopped serving, but RPC worker bthreads or
  // other shared_ptr holders may still keep the backend alive past the end
  // of main(). Trigger an explicit checkpoint NOW, while glog and the file
  // system are guaranteed to be available, instead of relying on the Index
  // destructor which may run too late (or never, if SIGKILL is delivered).
  LOG(INFO) << "Shutting down KVCache backend...";
  backend->Close();
  LOG(INFO) << "KVCache backend shutdown complete.";

  return 0;
}
