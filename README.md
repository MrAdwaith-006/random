# SheikzAmp

Native Windows audio-amplifier foundation for clean, controlled loudness.

## Current milestone

- C++20 / CMake project
- WASAPI endpoint discovery for playback and microphone devices
- Separate real-time audio-core library
- Safe initial gain stage with soft saturation and a configurable ceiling
- Windows application host that displays the available input and output devices

## Planned signal paths

- **Playback:** selected Windows output / app audio -> DSP -> chosen physical output
- **Microphone:** physical microphone or Voicemod virtual microphone -> DSP -> selectable virtual-microphone endpoint

Discord, WhatsApp, BlueStacks / Free Fire, and Voicemod are handled as normal Windows audio endpoints. No app-specific hooks are required. A virtual microphone output requires a separately installed virtual-audio driver; this app will not install or ship a kernel driver.

## Build

Open the folder in Visual Studio 2022 with the Desktop development with C++ workload, then configure CMake for x64:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
```

Run `build\\src\\App\\Debug\\SheikzAmp.exe`. The first scaffold window lists the active playback and microphone devices.

To add the WinUI 3 shell, install the Windows App SDK and configure with `-DSHEIKZAMP_ENABLE_WINUI=ON`. The audio core stays independent so it remains testable and real-time safe.
