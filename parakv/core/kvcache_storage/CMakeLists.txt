include(cc_library)
include(cc_test)

cc_library(
    NAME
        kvcache_storage_backend
    HDRS
        kvcache_storage_backend.h
        mocked_inmemory_kvcache_storage.h
    SRCS
        mocked_inmemory_kvcache_storage.cc
    DEPS
        glog::glog
)
