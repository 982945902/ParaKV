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

#include <memory>

#include "core/kvcache_storage/mocked_inmemory_kvcache_storage.h"
#include "service/kvcache_storage_service_impl.h"

DEFINE_int32(port, 8200, "brpc listen port");
DEFINE_int32(idle_timeout_s, 60,
             "Connection idle timeout, in seconds. -1 disables the timeout.");
DEFINE_int32(max_batch_size, 1024, "Max items per batch");
DEFINE_uint64(max_value_bytes, 0, "Per-item value size limit; 0 = no limit");

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  auto backend =
      std::make_shared<parakv::kvcache_storage::MockedInMemoryKVCacheStorage>();

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
  return 0;
}
