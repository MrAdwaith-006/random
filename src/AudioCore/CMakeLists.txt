add_library(SheikzAmpAudioCore STATIC
    src/AudioRoute.cpp
    src/DeviceEnumerator.cpp
    src/SignalProcessor.cpp
)

target_include_directories(SheikzAmpAudioCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(SheikzAmpAudioCore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SheikzAmpAudioCore PRIVATE /W4 /permissive-)
target_link_libraries(SheikzAmpAudioCore PUBLIC ole32 uuid avrt)
