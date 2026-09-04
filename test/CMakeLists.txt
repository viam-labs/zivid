include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Extra sources after the first are compiled into the same test executable; the
# executable is named after the first.
function(zivid_add_gtest SOURCE_FILE_NAME)
    get_filename_component(TEST_EXECUTABLE_NAME ${SOURCE_FILE_NAME} NAME_WE)

    add_executable(${TEST_EXECUTABLE_NAME} ${SOURCE_FILE_NAME} ${ARGN})
    target_include_directories(${TEST_EXECUTABLE_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    target_link_libraries(${TEST_EXECUTABLE_NAME}
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            GTest::gmock
    )

    # DISCOVERY_MODE PRE_TEST defers test discovery until `ctest` runs, so
    # shared libs from Conan's runenv are on the loader path.
    gtest_discover_tests(${TEST_EXECUTABLE_NAME}
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endfunction()

zivid_add_gtest(sanity_test.cpp)
zivid_add_gtest(zivid_locks_test.cpp zivid_locks_other_tu.cpp)
