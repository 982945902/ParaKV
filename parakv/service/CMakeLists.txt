include(cc_library)
include(cc_binary)

cc_library(
  NAME
    kvcache_storage_service
  HDRS
    kvcache_storage_service_impl.h
  SRCS
    kvcache_storage_service_impl.cc
  DEPS
    :kvcache_storage_backend
    proto::parakv_proto
    brpc
    glog::glog
    protobuf::libprotobuf
)

