#pragma once

#include "SheikzAmp/Audio/AudioDevice.h"

#include <vector>

namespace SheikzAmp::Audio {

class DeviceEnumerator final {
public:
    [[nodiscard]] std::vector<AudioDevice> playbackDevices() const;
    [[nodiscard]] std::vector<AudioDevice> recordingDevices() const;
};

} // namespace SheikzAmp::Audio
