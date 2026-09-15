# The Windows App SDK supplies WinUI 3. Keep the real-time audio engine independent
# of it, so the engine can be built and tested before the UI package is installed.
find_package(Microsoft.WindowsAppSDK CONFIG QUIET)

if(NOT Microsoft.WindowsAppSDK_FOUND)
    message(FATAL_ERROR
        "Windows App SDK was not found. Install it with Visual Studio's Windows App SDK "
        "workload/component, or configure with -DSHEIKZAMP_ENABLE_WINUI=OFF.")
endif()

message(STATUS "Windows App SDK found; WinUI host integration is enabled.")
