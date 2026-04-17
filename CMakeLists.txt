cmake_minimum_required(VERSION 3.26)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_compile_definitions(YLT_ENABLE_IBV)
add_definitions(-DYLT_ENABLE_IBV)
set(YLT_ENABLE_IBV ON)

if(DEVICE_ARCH STREQUAL "ARM")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    # set(RUST_TARGET aarch64-unknown-linux-gnu)
endif()


enable_testing()

if(NOT TARGET all_tests)
  add_custom_target(all_tests)
endif()

if(NOT TARGET export_module)
  add_custom_target(export_module)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
endif()

option(USE_CCACHE "Attempt using CCache to wrap the compilation" ON)
option(USE_CXX11_ABI "Use the new C++-11 ABI, which is not backwards compatible." OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MESSAGE_LOG_LEVEL STATUS)
set(CMAKE_VERBOSE_MAKEFILE ON)

if(POLICY CMP0135)
  # CMP0135: ExternalProject ignores timestamps in archives by default for the URL download method.
  cmake_policy(SET CMP0135 NEW)
endif()

function(parse_make_options options prefix)
  foreach(option ${options})
    string(REGEX REPLACE "(-D|-)" "" option ${option})
    string(REPLACE "=" ";" option ${option})
    list(GET option 0 option_name)
    list(GET option 1 option_value)
    set(${prefix}_${option_name}
        ${option_value}
        PARENT_SCOPE)
  endforeach()
endfunction()

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Build type not set - defaulting to Release")
  set(CMAKE_BUILD_TYPE "Release" 
      CACHE STRING "Choose the type of build from: Debug Release RelWithDebInfo MinSizeRel Coverage." 
      FORCE
  )
endif()

# Convert the bool variable to integer.
if(USE_CXX11_ABI)
  set(USE_CXX11_ABI 1)
  message(STATUS "Using the C++-11 ABI.")
else()
  set(USE_CXX11_ABI 0)
  message(STATUS "Using the pre C++-11 ABI.")
endif()

if(USE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "C compiler launcher")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "CXX compiler launcher")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
    if (DEFINED ENV{CCACHE_DIR})
      message(STATUS "Using CCACHE_DIR: $ENV{CCACHE_DIR}")
    endif()
  else()
    message(WARNING "Could not find ccache. Consider installing ccache to speed up compilation.")
  endif()
endif()

# if defined, create and use the default binary cache for vcpkg
if (DEFINED ENV{VCPKG_DEFAULT_BINARY_CACHE})
  file(MAKE_DIRECTORY $ENV{VCPKG_DEFAULT_BINARY_CACHE})
  message(STATUS "Using VCPKG_DEFAULT_BINARY_CACHE: $ENV{VCPKG_DEFAULT_BINARY_CACHE}")
endif()

if (DEFINED ENV{DEPENDENCES_ROOT})
  message(STATUS "Using DEPENDENCES_ROOT: $ENV{DEPENDENCES_ROOT}")
endif()


# configure vcpkg
# have to set CMAKE_TOOLCHAIN_FILE before first project call.
# if (DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
if (DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file")
  message(STATUS "VCPKG_ROOT found, using vcpkg at $ENV{VCPKG_ROOT}")
else()
  include(FetchContent)
  if (DEFINED ENV{DEPENDENCES_ROOT})
    set(VCPKG_SOURCE_DIR $ENV{DEPENDENCES_ROOT}/vcpkg-src)
  else()
    set(VCPKG_SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vcpkg-src)
  endif()

  if (USE_CXX11_ABI)
    FetchContent_Declare(vcpkg
      GIT_REPOSITORY "https://github.com/microsoft/vcpkg.git"
      GIT_TAG "2024.02.14"
      SOURCE_DIR ${VCPKG_SOURCE_DIR}
    )
  else()
    FetchContent_Declare(vcpkg
      GIT_REPOSITORY "https://gitcode.com/xLLM-AI/vcpkg.git"
      GIT_TAG "ffc42e97c866ce9692f5c441394832b86548422c" #disable cxx11_abi
      SOURCE_DIR ${VCPKG_SOURCE_DIR}
    )
    message(STATUS "Using custom vcpkg with cxx11_abi disabled")
  endif()
  FetchContent_MakeAvailable(vcpkg)

  message(STATUS "Downloading and using vcpkg at ${vcpkg_SOURCE_DIR}")
  set(CMAKE_TOOLCHAIN_FILE ${vcpkg_SOURCE_DIR}/scripts/buildsystems/vcpkg.cmake
      CACHE STRING "Vcpkg toolchain file")
endif()

set(CPPREST_EXCLUDE_WEBSOCKETS ON CACHE BOOL "Exclude websockets functionality." FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-format-truncation")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

project("parakv" LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# ARM architecture optimization (must be after project() so CXX is enabled).
# -march=native lets the compiler use NEON, SVE, LSE atomics, crypto, etc.
# that are available on the local CPU.  Fall back to armv8.2-a if the flag
# is unsupported (cross-compilation scenario).
# armv8.2-a+dotprod+crypto is good for Kunpeng 920, Neoverse N1, etc.
# ---------------------------------------------------------------------------
if(DEVICE_ARCH STREQUAL "ARM")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8.2-a+dotprod+crypto" HAS_MARCH_CUSTOMIZATION)
   if(HAS_MARCH_CUSTOMIZATION)
       add_compile_options(-march=armv8.2-a+dotprod+crypto)
       message(STATUS "ARM: using -march=armv8.2-a+dotprod+crypto for optimal ISA")
   else()
        add_compile_options(-march=native) 
        message(STATUS "ARM: using -march=native (cross-compile fallback)")
   endif()
endif()

# find_package(CUDAToolkit REQUIRED)

# setup CMake module path, defines path for include() and find_package()
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
# enable_language(Rust)
# find_package(Rust REQUIRED)

if(UNIX)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og")
endif()

find_package(Boost REQUIRED)
find_package(Boost REQUIRED COMPONENTS serialization)
find_package(Threads REQUIRED)
# find all dependencies from vcpkg
find_package(glog CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)
find_package(leveldb CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

# Ensure vcpkg protobuf headers are found before PyTorch's bundled older version.
# Use BEFORE SYSTEM so that:
#   1) vcpkg protobuf is first in -isystem list (before torch's -isystem)
#   2) project-local -I paths (e.g. Mooncake fake_include stubs) still win over -isystem
# Protobuf_INCLUDE_DIRS may be empty in CONFIG mode; fall back to the imported target.
if(NOT Protobuf_INCLUDE_DIRS AND TARGET protobuf::libprotobuf)
  get_target_property(Protobuf_INCLUDE_DIRS protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
endif()
if(Protobuf_INCLUDE_DIRS)
  include_directories(BEFORE SYSTEM ${Protobuf_INCLUDE_DIRS})
endif()
find_package(gRPC CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Python COMPONENTS Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

if (USE_CXX11_ABI)
  # only use jemalloc if using the new C++-11 ABI
  find_package(Jemalloc)
  if(Jemalloc_FOUND)
    link_libraries(Jemalloc::jemalloc)
  endif()
endif()


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DC10_USE_GLOG")

message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "CMAKE_CXX_FLAGS_DEBUG: ${CMAKE_CXX_FLAGS_DEBUG}")

# enable testing in this directory so we can do a top-level `make test`.
# this also includes the BUILD_TESTING option, which is on by default.
include(CTest)
include(GoogleTest)
option(BUILD_TESTING "Build the testing tree." ON)

# include current path
list(APPEND COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
# Note: third_party is not added to COMMON_INCLUDE_DIRS to avoid warnings
# Individual third_party directories are added as SYSTEM headers below
list(APPEND COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/etcd_cpp_apiv3)

# brpc - mark as system headers to suppress warnings from third-party code
set(BRPC_OUTPUT_DIR ${CMAKE_BINARY_DIR}/third_party/brpc/output)
include_directories(SYSTEM ${BRPC_OUTPUT_DIR}/include)
link_directories(${BRPC_OUTPUT_DIR}/lib)

# Mooncake - mark as system headers to suppress warnings from third-party code
# Add third_party as SYSTEM to support #include <Mooncake/...> style includes
# Use absolute path to ensure it works correctly
# include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Mooncake/mooncake-common/include)
# include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Mooncake/mooncake-transfer-engine/include)
# include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Mooncake/mooncake-store/include)


# add subdirectories
# Configure third_party first so parakv can link against real CMake targets.
add_subdirectory(third_party)
if(TARGET brpc-static AND NOT TARGET brpc)
  # Expose a canonical target name used across parakv/CMakeLists.
  add_library(brpc ALIAS brpc-static)
endif()
add_subdirectory(parakv)
