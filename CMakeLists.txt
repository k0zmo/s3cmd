cmake_minimum_required(VERSION 3.24)
project(s3cmd LANGUAGES C CXX)

set(CMAKE_MODULE_PATH "${s3cmd_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(AWSSDK CONFIG REQUIRED COMPONENTS aws-sdk-cpp-s3)

add_library(s3cmd-core STATIC
    src/s3.cpp
    src/s3.hpp
    src/fsplugin.h
)
target_compile_features(s3cmd-core PUBLIC cxx_std_20)
target_compile_definitions(s3cmd-core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(s3cmd-core PUBLIC "${s3cmd_SOURCE_DIR}/src")
target_link_libraries(s3cmd-core PUBLIC AWS::aws-sdk-cpp-s3)

add_library(s3cmd MODULE
    src/main.cpp
    src/s3cmd.def
)
target_compile_features(s3cmd PRIVATE cxx_std_20)
target_compile_definitions(s3cmd PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(s3cmd PRIVATE s3cmd-core)
set_target_properties(s3cmd PROPERTIES
    PREFIX ""
    SUFFIX ".wfx64"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 CONFIG REQUIRED)
    option(S3CMD_ENABLE_AWS_TESTS "Register tests that use real AWS credentials" OFF)

    add_executable(s3cmd-test EXCLUDE_FROM_ALL
        tests/aws_test.cpp
        tests/core_test.cpp
    )
    target_link_libraries(s3cmd-test PRIVATE s3cmd-core Catch2::Catch2WithMain)

    add_test(NAME s3cmd-unit COMMAND s3cmd-test "[unit]")
    set_tests_properties(s3cmd-unit PROPERTIES LABELS unit)

    if(S3CMD_ENABLE_AWS_TESTS)
        add_test(NAME s3cmd-aws COMMAND s3cmd-test "[integration]")
        set_tests_properties(s3cmd-aws PROPERTIES
            LABELS integration
            SKIP_RETURN_CODE 4
        )
    endif()
endif()
