#pragma once

#include <span>

namespace SheikzAmp::Audio {

struct AmplifierSettings {
    float preamp{1.0F};
    float ceiling{0.98F};
    float saturation{0.0F};

    bool maxLoud{true};
};

class SignalProcessor final {
public:
    void setSettings(AmplifierSettings settings) noexcept;
    void processInterleaved(std::span<float> samples) const noexcept;

private:
    AmplifierSettings settings_{};
};

} // namespace SheikzAmp::Audio