#include "SheikzAmp/Audio/SignalProcessor.h"

#include <algorithm>
#include <cmath>

namespace SheikzAmp::Audio {

void SignalProcessor::setSettings(
    const AmplifierSettings& settings) noexcept {

    preamp_.store(
        std::clamp(settings.preamp, 1.0F, 100.0F),
        std::memory_order_relaxed);

    ceiling_.store(
        std::clamp(settings.ceiling, 0.01F, 1.0F),
        std::memory_order_relaxed);

    saturation_.store(
        std::clamp(settings.saturation, 0.0F, 1.0F),
        std::memory_order_relaxed);

    maxLoud_.store(
        settings.maxLoud,
        std::memory_order_relaxed);
}

AmplifierSettings SignalProcessor::getSettings() const noexcept {
    return {
        preamp_.load(std::memory_order_relaxed),
        ceiling_.load(std::memory_order_relaxed),
        saturation_.load(std::memory_order_relaxed),
        maxLoud_.load(std::memory_order_relaxed)
    };
}

void SignalProcessor::processInterleaved(
    std::span<float> samples) const noexcept {

    if (samples.empty()) {
        return;
    }

    const float gain = preamp_.load(std::memory_order_relaxed);
    const float ceiling = ceiling_.load(std::memory_order_relaxed);
    const bool maxLoud = maxLoud_.load(std::memory_order_relaxed);

    if (!maxLoud) {

        // Clean amplification with only peak protection.
        for (float& sample : samples) {

            float x = sample * gain;

            if (std::abs(x) <= ceiling) {
                sample = x;
                continue;
            }

            const float sign =
                std::copysign(1.0F, x);

            const float excess =
                std::abs(x) - ceiling;

            const float range =
                1.0F - ceiling;

            const float compressed =
                ceiling +
                range *
                    (1.0F -
                     std::exp(
                         -excess /
                         std::max(
                             range,
                             0.001F)));

            sample =
                sign *
                std::min(
                    compressed,
                    0.9999F);
        }

        return;
    }

    /*
     * MAX LOUD
     *
     * High-quality loudness processing:
     *
     * 1. Clean gain
     * 2. Gentle dynamic compression
     * 3. Transparent soft limiting
     *
     * Avoids the previous aggressive tanh stages
     * which caused audible distortion on music/radio.
     */

    constexpr float compressorThreshold = 0.18F;
    constexpr float compressorRatio = 4.0F;
    constexpr float limiterThreshold = 0.94F;

    constexpr float noiseThreshold = 0.012F;
    constexpr float gateFloor = 0.18F;

    for (float& sample : samples) {

        float input = sample;
        float magnitude = std::abs(input);

        // Gentle reduction of very quiet microphone noise.
        if (magnitude < noiseThreshold) {
            const float ratio =
                magnitude / noiseThreshold;

            const float gainReduction =
                gateFloor +
                (1.0F - gateFloor) * ratio;

            input *= gainReduction;
        }

        float x = input * gain;

        const float sign =
            std::copysign(1.0F, x);

        magnitude = std::abs(x);

        // Gentle compression above threshold.
        if (magnitude > compressorThreshold) {

            const float excess =
                magnitude -
                compressorThreshold;

            magnitude =
                compressorThreshold +
                excess / compressorRatio;
        }

        // Transparent soft limiter.
        if (magnitude > limiterThreshold) {

            const float excess =
                magnitude -
                limiterThreshold;

            const float range =
                1.0F -
                limiterThreshold;

            magnitude =
                limiterThreshold +
                range *
                    std::tanh(
                        excess /
                        std::max(
                            range,
                            0.001F));
        }

        sample =
            sign *
            std::min(
                magnitude,
                ceiling);
    }
}

} // namespace SheikzAmp::Audio