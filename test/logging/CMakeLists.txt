# Header-only logging tests: independent of fast_bthread and RDMA hardware.
# Exercise all DCHECK modes even when the outer build uses a different mode.
foreach(mode IN ITEMS debug release forced)
    set(target fast_log_test_${mode})
    add_executable(${target} test_fast_log.cc)
    target_link_libraries(${target} PRIVATE GTest::GTest GTest::Main pthread)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(mode STREQUAL "debug")
        target_compile_options(${target} PRIVATE -UNDEBUG -UDCHECK_ALWAYS_ON)
    elseif(mode STREQUAL "release")
        target_compile_options(${target} PRIVATE -O3 -DNDEBUG -UDCHECK_ALWAYS_ON)
    else()
        target_compile_options(${target} PRIVATE -O3 -DNDEBUG -DDCHECK_ALWAYS_ON)
    endif()
    # A deterministic clock tests exact interval boundaries and concurrent CAS.
    set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS
                 " -Wl,--wrap=gettimeofday")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
endforeach()
