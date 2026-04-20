include(cc_library)
include(cc_test)

cc_library(
    NAME
        index
    HDRS
        offset.h
        wal.h
        index.h
    SRCS
        wal.cc
        index.cc
    DEPS
        :segment
        phmap
        Threads::Threads
)
