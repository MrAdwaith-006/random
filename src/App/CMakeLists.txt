add_executable(SheikzAmp
    src/main.cpp
)

target_link_libraries(SheikzAmp PRIVATE SheikzAmpAudioCore comctl32 uxtheme)
target_compile_definitions(SheikzAmp PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SheikzAmp PRIVATE /W4 /permissive-)
