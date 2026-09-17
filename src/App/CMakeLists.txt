add_executable(SheikzAmp WIN32
    src/main.cpp
    src/SheikzAmp.rc
)

target_link_libraries(SheikzAmp PRIVATE SheikzAmpAudioCore comctl32 uxtheme gdiplus msimg32 dwmapi)
target_compile_definitions(SheikzAmp PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(SheikzAmp PRIVATE /W4 /permissive-)
else()
    target_compile_options(SheikzAmp PRIVATE -Wall -Wextra)
endif()

if(MINGW)
    target_link_options(SheikzAmp PRIVATE -municode)
endif()

add_custom_command(TARGET SheikzAmp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:SheikzAmp>/assets
)
