cmake_minimum_required(VERSION 3.22)

project(FBKSuppressor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FBK_BUILD_PLUGIN "Build the VST3/AU/Standalone plugin (requires JUCE)" ON)
option(FBK_BUILD_TESTS  "Build the DSP unit tests" ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

add_subdirectory(src/dsp)

if(FBK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FBK_BUILD_PLUGIN)
    add_subdirectory(src/plugin)
endif()
