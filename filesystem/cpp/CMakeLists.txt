cmake_minimum_required(VERSION 3.14)
project(tapefs-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources
set(SOURCES
    src/Crc32.cpp
    src/Ecc.cpp
    src/Directory.cpp
    src/Block.cpp
    src/Filesystem.cpp
    src/DummyMcu.cpp
)

# Library
add_library(tapefs_cpp STATIC ${SOURCES})
target_include_directories(tapefs_cpp PUBLIC include)

# Test
add_executable(test_tapefs tests/test_tapefs.cpp)
target_link_libraries(test_tapefs tapefs_cpp)
target_include_directories(test_tapefs PRIVATE include)
