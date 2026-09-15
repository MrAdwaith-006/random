#include "SheikzAmp/Audio/SignalProcessor.h"

#include <algorithm>
#include <cmath>

namespace SheikzAmp::Audio {

void SignalProcessor::setSettings(
    AmplifierSettings settings) noexcept {

    settings_.preamp = std::clamp(
        settings.preamp,
        0.0F,
        100.0F);

    settings_.ceiling = std::clamp(
        settings.ceiling,
        0.01F,
        1.0F);

    settings_.saturation = std::clamp(
        settings.saturation,
        0.0F,
        1.0F);

    settings_.maxLoud = settings.maxLoud;
}

void SignalProcessor::processInterleaved(
    std::span<float> samples) const noexcept {

    const float gain = settings_.preamp;
    const float ceiling = settings_.ceiling;

    if (gain <= 0.0F) {
        std::fill(samples.begin(), samples.end(), 0.0F);
        return;
    }

    for (float& sample : samples) {

        float x = sample * gain;

        if (settings_.maxLoud) {

            // Aggressive soft compression.
            const float sign =
                std::copysign(1.0F, x);

            const float magnitude =
                std::abs(x);

            // Strong compression above 1.0.
            float compressed =
                1.0F - std::exp(-magnitude * 1.35F);

            x = sign * compressed;

            // Additional saturation for perceived loudness.
            x = std::tanh(x * 2.2F);

            // Push the result toward the ceiling.
            x *= ceiling / std::tanh(2.2F);

            x = std::clamp(
                x,
                -ceiling,
                ceiling);

            sample = x;
            continue;
        }

        // Normal mode.
        const float magnitude =
            std::abs(x);

        if (magnitude <= ceiling) {
            sample = x;
            continue;
        }

        const float excess =
            magnitude - ceiling;

        const float compressed =
            ceiling +
            (1.0F - ceiling) *
            std::tanh(
                excess /
                (1.0F - ceiling));

        sample =
            std::copysign(
                compressed,
                x);
    }
}

} // namespace SheikzAmp::Audio