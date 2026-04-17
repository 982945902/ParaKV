include(cc_library)
include(cc_test)


cc_library(
    NAME
        parakv_server
    HDRS 
        parakv_server.h 
    SRCS 
        parakv_server.cc 
    DEPS 
        gflags::gflags
)