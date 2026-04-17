include(cc_library)

add_subdirectory(brpc)
add_subdirectory(cpprestsdk)
set(CPPREST_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cpprestsdk/Release/include)
set(CPPREST_LIB ${CMAKE_BINARY_DIR}/third_party/cpprestsdk/Release/Binaries/libcpprest.a)

# add_subdirectory(etcd_cpp_apiv3)
add_subdirectory(xxHash)
# add_subdirectory(spdlog)

# target_include_directories(mooncake_store PUBLIC
#   ${CMAKE_CURRENT_SOURCE_DIR}/Mooncake/mooncake-transfer-engine/include
# )

# target_link_libraries(mooncake_store PUBLIC transfer_engine cachelib_memory_allocator)