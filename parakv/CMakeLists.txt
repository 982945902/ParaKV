include(cc_binary)
include(cc_shared_library)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(client)
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(server)


# Generate build-time version header from version.txt.
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PARAKV_BUILD_VERSION_RAW)
string(STRIP "${PARAKV_BUILD_VERSION_RAW}" PARAKV_BUILD_VERSION)
if(PARAKV_BUILD_VERSION MATCHES "^v(.+)$")
  set(PARAKV_BUILD_VERSION "${CMAKE_MATCH_1}")
endif()
if(PARAKV_BUILD_VERSION STREQUAL "")
  set(PARAKV_BUILD_VERSION "unknown")
endif()
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Set warning-as-error for parakv code (third-party headers are marked as SYSTEM)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Werror>
    $<$<COMPILE_LANGUAGE:C>:-Werror>
  )

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-macro-redefined>
      $<$<COMPILE_LANGUAGE:C>:-Wno-macro-redefined>
    )
  endif()
endif()

option(GENERATE_SO "Enable compile to generate .so" OFF)


message(STATUS "build parakv binary and install")
cc_binary(
NAME
    parakv
SRCS
    parakv.cc
DEPS
    glog::glog
    common
    index
    segment
)

# link brpc
target_link_libraries(parakv PRIVATE glog::glog brpc leveldb::leveldb protobuf::libprotobuf)
add_dependencies(parakv brpc-static)


# install parakv
install(TARGETS parakv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install all dependencies for parakv
install(CODE [[
  file(GET_RUNTIME_DEPENDENCIES
        RESOLVED_DEPENDENCIES_VAR DEPENDENCIES
        UNRESOLVED_DEPENDENCIES_VAR UNRESOLVED_DEPENDENCIES
        EXECUTABLES $<TARGET_FILE:parakv>)

  file(INSTALL
        DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
        FILES ${DEPENDENCIES}
        FOLLOW_SYMLINK_CHAIN)

  # This should not be possible, but error out when a dependency cannot
  # be resolved.
  list(LENGTH UNRESOLVED_DEPENDENCIES UNRESOLVED_LENGTH)
  if(${UNRESOLVED_LENGTH} GREATER 0)
      message(FATAL_ERROR "Unresolved dependencies: ${UNRESOLVED_DEPENDENCIES}")
  endif()
]])
