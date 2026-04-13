include(cc_library)
include(cc_test)


cc_library(
    NAME
        flags
    HDRS 
        global_flags.h 
    SRCS 
        global_flags.cc 
    DEPS 
        gflags::gflags
)