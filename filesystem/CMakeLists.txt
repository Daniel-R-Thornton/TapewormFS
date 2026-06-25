cmake_minimum_required(VERSION 3.10)
project(tapefs LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Sources
set(TAPEFS_SRCS
    src/block.c
    src/crc32.c
    src/directory.c
    src/ecc.c
    src/tapefs.c
)

set(TAPEFS_HDRS
    include/tapefs.h
)

# Library target
add_library(tapefs STATIC ${TAPEFS_SRCS} ${TAPEFS_HDRS})
target_include_directories(tapefs PUBLIC include)

# Test executable (optional, not built by default)
option(BUILD_TESTS "Build the test suite" OFF)
if(BUILD_TESTS)
    add_executable(test_tapefs src/test_tapefs.c)
    target_link_libraries(test_tapefs tapefs)
    target_include_directories(test_tapefs PRIVATE include)
endif()
