include(cc_library)
include(cc_test)


cc_library(
    NAME
        flags
    HDRS 
        global_gflags.h 
    SRCS 
        global_gflags.cc 
    DEPS 
        gflags::gflags
)