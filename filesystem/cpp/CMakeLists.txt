cmake_minimum_required(VERSION 3.14)
project(tapefs-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")

set(SOURCES
    src/Crc32.cpp
    src/Ecc.cpp
    src/Directory.cpp
    src/Block.cpp
    src/Filesystem.cpp
    src/DummyMcu.cpp
    src/SerialTransport.cpp
)

# Library
add_library(tapefs_cpp STATIC ${SOURCES})
target_include_directories(tapefs_cpp PUBLIC include)

# Unit tests
add_executable(test_tapefs tests/test_tapefs.cpp)
target_link_libraries(test_tapefs tapefs_cpp)
target_include_directories(test_tapefs PRIVATE include)

# FUSE mount (optional, requires libfuse3-dev)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3)
    if(FUSE3_FOUND)
        add_executable(tapeworm_mount tests/tapeworm_mount.cpp)
        target_link_libraries(tapeworm_mount tapefs_cpp PkgConfig::FUSE3)
        target_include_directories(tapeworm_mount PRIVATE include)
    endif()
endif()
