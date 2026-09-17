#pragma once

#include <span>
#include <atomic>

namespace SheikzAmp::Audio {

struct AmplifierSettings {
    float preamp{1.0F};
    float ceiling{0.98F};
    float saturation{0.0F};

    bool maxLoud{true};
};

class SignalProcessor final {
public:
    void setSettings(const AmplifierSettings& settings) noexcept;
    [[nodiscard]] AmplifierSettings getSettings() const noexcept;
    void processInterleaved(std::span<float> samples) const noexcept;

private:
    std::atomic<float> preamp_{1.0F};
    std::atomic<float> ceiling_{0.98F};
    std::atomic<float> saturation_{0.0F};
    std::atomic<bool> maxLoud_{true};
};

} // namespace SheikzAmp::Audio