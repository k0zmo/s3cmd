# CPack reads this at packaging time, so the revision is not cached by CMake.
set(package_version "${CPACK_PACKAGE_VERSION}")
if(package_version MATCHES "-dev$")
    set(revision "nogit")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=7 "--exclude=*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
            OUTPUT_VARIABLE git_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE git_result
            ERROR_QUIET
        )
        if(git_result EQUAL 0)
            set(revision "g${git_revision}")
        endif()
    endif()
    string(APPEND package_version "-${revision}")
endif()
set(CPACK_PACKAGE_FILE_NAME "s3cmd-${package_version}-win64")
