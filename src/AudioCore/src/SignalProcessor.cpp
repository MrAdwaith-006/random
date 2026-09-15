#include "SheikzAmp/Audio/SignalProcessor.h"

#include <algorithm>
#include <cmath>

namespace SheikzAmp::Audio {

void SignalProcessor::setSettings(AmplifierSettings settings) noexcept {
    settings_.preamp = std::max(0.0F, settings.preamp);
    settings_.ceiling = std::clamp(settings.ceiling, 0.01F, 1.0F);
    settings_.saturation = std::clamp(settings.saturation, 0.0F, 1.0F);
}

void SignalProcessor::processInterleaved(std::span<float> samples) const noexcept {
    const float gain = settings_.preamp;
    const float ceiling = settings_.ceiling;
    const float saturation = settings_.saturation;

    for (float& sample : samples) {
        const float amplified = sample * gain;

        // At zero saturation, use a clean soft limiter.
        if (saturation <= 0.0001F) {
            const float magnitude = std::abs(amplified);

            if (magnitude <= ceiling) {
                sample = amplified;
            } else {
                // Smoothly approaches the ceiling instead of hard clipping.
                const float excess = magnitude - ceiling;
                const float compressed = ceiling +
                    (1.0F - ceiling) * std::tanh(excess / (1.0F - ceiling));

                sample = std::copysign(compressed, amplified);
            }

            continue;
        }

        // Optional saturation for extreme amplification.
        const float drive = 1.0F + saturation * 8.0F;
        const float saturated = std::tanh(amplified * drive);

        // Blend clean and saturated signal.
        const float mixed =
            amplified * (1.0F - saturation) +
            saturated * saturation;

        const float magnitude = std::abs(mixed);

        if (magnitude <= ceiling) {
            sample = mixed;
        } else {
            const float excess = magnitude - ceiling;
            const float compressed = ceiling +
                (1.0F - ceiling) * std::tanh(excess / (1.0F - ceiling));

            sample = std::copysign(compressed, mixed);
        }
    }
}

} // namespace SheikzAmp::Audio