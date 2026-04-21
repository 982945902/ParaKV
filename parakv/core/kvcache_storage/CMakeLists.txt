include(cc_library)
include(cc_test)

cc_library(
    NAME
        kvcache_storage
    HDRS
        kvcache_storage.h
    SRCS
        kvcache_storage.cc
    DEPS
        glog::glog
)
