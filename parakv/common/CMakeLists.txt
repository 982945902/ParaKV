include(cc_library)
include(cc_test)


cc_library(
    NAME
        flags
    HDRS 
        global_gflags.h 
        status.h
    SRCS 
        global_gflags.cc 
    DEPS 
        gflags::gflags
)

cc_library(
    NAME
        common
    HDRS
        etcd_client.h
        status.h
    SRCS
        etcd_client.cc
    DEPS
        cpprest
        etcd-cpp-api
        nlohmann_json::nlohmann_json
        glog::glog
)

target_include_directories(common PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/etcd-cpp-apiv3
)