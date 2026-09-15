#pragma once

#include <string>

namespace SheikzAmp::Audio {

enum class DeviceFlow { Playback, Recording };

struct AudioDevice {
    std::wstring id;
    std::wstring name;
    DeviceFlow flow{};
    bool isDefault{};
};

} // namespace SheikzAmp::Audio
