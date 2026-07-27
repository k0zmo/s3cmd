cmake_minimum_required(VERSION 3.24)
project(s3cmd LANGUAGES C CXX)

set(CMAKE_MODULE_PATH "${s3cmd_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(AWSSDK CONFIG REQUIRED COMPONENTS aws-sdk-cpp-s3)

add_library(s3cmd MODULE
    src/main.cpp
    src/fsplugin.h
    src/s3cmd.def
)
target_compile_features(s3cmd PRIVATE cxx_std_20)
target_compile_definitions(s3cmd PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(s3cmd PRIVATE AWS::aws-sdk-cpp-s3)
set_target_properties(s3cmd PROPERTIES
    PREFIX ""
    SUFFIX ".wfx64"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(s3cmd-check EXCLUDE_FROM_ALL
        src/main.cpp
        src/fsplugin.h
    )
    target_compile_features(s3cmd-check PRIVATE cxx_std_20)
    target_compile_definitions(s3cmd-check PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        S3CMD_SELF_CHECK
    )
    target_link_libraries(s3cmd-check PRIVATE AWS::aws-sdk-cpp-s3)
    add_test(NAME s3cmd-check COMMAND s3cmd-check)
endif()
