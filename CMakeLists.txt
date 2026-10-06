cmake_minimum_required(VERSION 3.10)

project(FAST-RPC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

enable_testing()

find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED)
find_package(GTest REQUIRED)

message(STATUS "Boost ${Boost_VERSION}, Protobuf ${Protobuf_VERSION}")
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/inc)
include_directories(${GTEST_INCLUDE_DIRS})

protobuf_generate_cpp(IMPL_PROTO_SRCS IMPL_PROTO_HDRS proto/fast_impl.proto)

file(GLOB FAST_SRC "src-common/*.cc" "src-resource/*.cc" "src-endpoint/*.cc")
add_library(fastrpc STATIC ${FAST_SRC} ${IMPL_PROTO_SRCS})
target_link_libraries(fastrpc "-libverbs -lrdmacm -lpthread" ${Boost_LIBRARIES} protobuf::libprotobuf)

add_subdirectory(src-bthread)
target_link_libraries(fastrpc fast_bthread)
add_subdirectory(test/logging)

protobuf_generate_cpp(TEST_PROTO_SRCS TEST_PROTO_HDRS test/test.proto)

add_executable(client test/client.cc ${TEST_PROTO_SRCS})
add_executable(server test/server.cc ${TEST_PROTO_SRCS})
target_link_libraries(client fastrpc)
target_link_libraries(server fastrpc)

file(GLOB UNIT_TEST_SOURCES test/unit/*.cc)
add_executable(unit_tests ${UNIT_TEST_SOURCES} ${IMPL_PROTO_SRCS})
target_link_libraries(unit_tests fastrpc fast_bthread ${GTEST_LIBRARIES} pthread)
add_test(NAME UnitTests COMMAND unit_tests)

# Linux-only queue tests intercept sched_yield to control the UNCONNECTED
# window deterministically. The production library has no test hooks.
add_executable(rdma_write_queue_tests test/endpoint_write_queue_test.cc)
target_link_libraries(rdma_write_queue_tests fastrpc ${GTEST_LIBRARIES} pthread
                      "-Wl,--wrap=sched_yield")
add_test(NAME RdmaWriteQueueTests COMMAND rdma_write_queue_tests)
set_tests_properties(RdmaWriteQueueTests PROPERTIES TIMEOUT 30)

# Pause at the real butex_wait entry to reproduce predicate-check/wait races.
# The wrapper is linked only into this test executable.
add_executable(rdma_writable_tests test/endpoint_writable_test.cc)
target_link_libraries(rdma_writable_tests fastrpc ${GTEST_LIBRARIES} pthread
                      "-Wl,--wrap=_ZN4fast10butex_waitEPviPK8timespecb")
add_test(NAME RdmaWritableTests COMMAND rdma_writable_tests)
set_tests_properties(RdmaWritableTests PROPERTIES TIMEOUT 30)

# Exercise the port independently of the RPC/RDMA library. Startup checks run
# in subprocesses so that each case sees an uninitialized scheduler.
add_executable(bthread_runtime_tests test/bthread_runtime_test.cc)
target_link_libraries(bthread_runtime_tests fast_bthread ${GTEST_LIBRARIES} pthread)
add_test(NAME BthreadRuntimeTests COMMAND bthread_runtime_tests)
set_tests_properties(BthreadRuntimeTests PROPERTIES TIMEOUT 30)
