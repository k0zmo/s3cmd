cmake_minimum_required(VERSION 3.24)
project(s3cmd VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_MODULE_PATH "${s3cmd_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(S3CMD_USING_CONAN)
    # Sadly, conan package uses different naming scheme
    find_package(AWSSDK CONFIG REQUIRED COMPONENTS aws-sdk-cpp-s3)
else()
    find_package(AWSSDK CONFIG REQUIRED COMPONENTS core s3)
endif()

find_package(tomlplusplus CONFIG REQUIRED)

add_library(s3cmd-core STATIC
    src/fsplugin.h
    src/core.hpp    src/core.cpp
    src/s3.cpp      src/s3.hpp
    src/log.cpp     src/log.hpp
)
target_compile_features(s3cmd-core PUBLIC cxx_std_20)
target_compile_definitions(s3cmd-core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(s3cmd-core PUBLIC "${s3cmd_SOURCE_DIR}/src")
target_link_libraries(s3cmd-core PRIVATE tomlplusplus::tomlplusplus)

if(TARGET AWS::aws-sdk-cpp-s3)
    target_link_libraries(s3cmd-core PUBLIC AWS::aws-sdk-cpp-core AWS::aws-sdk-cpp-s3)
else()
    target_link_libraries(s3cmd-core PUBLIC aws-cpp-sdk-core aws-cpp-sdk-s3)
endif()

add_library(s3cmd MODULE
    src/main.cpp
    src/icons.cpp src/icons.hpp
    src/s3cmd.def
)
if(WIN32)
    target_sources(s3cmd PRIVATE src/s3cmd.rc)
endif()
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

    add_executable(s3cmd-test
        tests/aws_test.cpp
        tests/core_test.cpp
    )
    target_link_libraries(s3cmd-test PRIVATE s3cmd-core Catch2::Catch2WithMain)

    add_test(NAME s3cmd-unit COMMAND s3cmd-test "[unit]")
    set_tests_properties(s3cmd-unit PROPERTIES LABELS unit)

    add_test(NAME s3cmd-aws COMMAND s3cmd-test "[integration]")
    set_tests_properties(s3cmd-aws PROPERTIES
        LABELS integration
        SKIP_RETURN_CODE 4
    )
endif()

configure_file(pluginst.inf.in pluginst.inf @ONLY)

install(TARGETS s3cmd
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pluginst.inf"
    README.md
    DESTINATION .
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/README.md"
    RENAME ICONS-LICENSES.md
    DESTINATION .
)

set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "s3cmd-${CMAKE_PROJECT_VERSION}-win64")
include(CPack)
