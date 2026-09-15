#pragma once

#include <span>

namespace SheikzAmp::Audio {

struct AmplifierSettings {
    // Input gain multiplier.
    // 1.0 = 0 dB
    // 2.0 = +6.02 dB
    // 10.0 = +20 dB
    float preamp{1.0F};

    // Final peak ceiling.
    // Keeps the digital signal inside the safe range.
    float ceiling{0.98F};

    // Soft-knee saturation amount.
    // 0 = clean limiting, higher values add more saturation.
    float saturation{0.0F};
};

class SignalProcessor final {
public:
    void setSettings(AmplifierSettings settings) noexcept;

    void processInterleaved(std::span<float> samples) const noexcept;

private:
    AmplifierSettings settings_{};
};

} // namespace SheikzAmp::Audio