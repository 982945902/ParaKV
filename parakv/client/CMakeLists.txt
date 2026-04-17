include(cc_library)
include(cc_test)


cc_library(
    NAME
        parakv_client
    HDRS 
        parakv_client.h 
    SRCS 
        parakv_client.cc 
    DEPS 
        gflags::gflags
)