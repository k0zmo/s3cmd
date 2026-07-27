include_guard(GLOBAL)

set(CMAKE_CXX_COMPILER "g++" CACHE STRING "CXX Compiler")

set(CXX_FLAGS
    -Wall
    -Wextra
    -Wpedantic
)
set(CXX_FLAGS_DEBUG
    -O0
    -g
    # Consider adding -gsplit-dwarf
)
set(CXX_FLAGS_RELEASE
    -O3
    -DNDEBUG
)
set(CXX_FLAGS_COVERAGE ${CXX_FLAGS_DEBUG}
    --coverage
    -fprofile-update=atomic # Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68080
)
set(CXX_FLAGS_ASAN
    -O1
    -g
    -fsanitize=address
    -fno-omit-frame-pointer
    -DNDEBUG
)
set(CXX_FLAGS_TSAN
    -O1
    -g
    -fsanitize=thread
    -DNDEBUG
)
set(CXX_FLAGS_UBSAN
    -O1
    -g
    -fsanitize=undefined
    -fno-omit-frame-pointer
    -DNDEBUG
)
set(LINKER_FLAGS
    -Wl,--exclude-libs=ALL
    -Wl,--as-needed
    -Wl,-z,defs
)
set(LINKER_FLAGS_DEBUG
    # Consider adding -Wl,--gdb-index (requires gold, lld or mold)
)
set(LINKER_FLAGS_RELEASE)
set(LINKER_FLAGS_COVERAGE ${LINKER_FLAGS_DEBUG})
set(LINKER_FLAGS_ASAN)
set(LINKER_FLAGS_TSAN)
set(LINKER_FLAGS_UBSAN)

include(${CMAKE_CURRENT_LIST_DIR}/SetFlags.cmake)
