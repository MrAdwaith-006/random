cmake_minimum_required(VERSION 3.24)

project(SheikzAmp VERSION 0.1.0 LANGUAGES CXX)

option(SHEIKZAMP_ENABLE_WINUI "Enable the Windows App SDK / WinUI host when the SDK is installed" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "SheikzAmp targets Windows only.")
endif()

if(SHEIKZAMP_ENABLE_WINUI)
    include(cmake/WindowsAppSDK.cmake)
endif()

add_subdirectory(src/AudioCore)
add_subdirectory(src/App)
