cmake_minimum_required(VERSION 3.14)
project(tapefs-firmware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/Motor.cpp
    src/TapeHead.cpp
    src/Modem.cpp
    src/TapeDeck.cpp
    src/DummyMcu.cpp
)

set(HEADERS
    include/tapefs/firmware/Motor.hpp
    include/tapefs/firmware/TapeHead.hpp
    include/tapefs/firmware/Modem.hpp
    include/tapefs/firmware/TapeDeck.hpp
    include/tapefs/firmware/DummyMcu.hpp
)

add_library(tapefs_firmware STATIC ${SOURCES} ${HEADERS})
target_include_directories(tapefs_firmware PUBLIC include)

add_executable(test_firmware tests/test_firmware.cpp)
target_link_libraries(test_firmware tapefs_firmware)
target_include_directories(test_firmware PRIVATE include)
