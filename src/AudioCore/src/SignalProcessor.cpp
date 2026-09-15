#include "SheikzAmp/Audio/SignalProcessor.h"

#include <algorithm>
#include <cmath>

namespace SheikzAmp::Audio {

void SignalProcessor::setSettings(
    AmplifierSettings settings) noexcept {

    settings_.preamp = std::clamp(
        settings.preamp,
        1.0F,
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

    if (samples.empty()) {
        return;
    }

    const float gain = settings_.preamp;
    const float ceiling = settings_.ceiling;

    /*
     * Build 2:
     * Basic speech-focused processing.
     *
     * Very quiet signals are attenuated before amplification.
     * Speech-level signals are amplified normally.
     * Strong signals are compressed to keep speech loud without
     * allowing peaks to dominate.
     */

    constexpr float noiseFloor = 0.008F;
    constexpr float speechStart = 0.025F;

    for (float& sample : samples) {

        const float input = sample;
        const float magnitude = std::abs(input);

        // Basic noise gate.
        if (magnitude < noiseFloor) {
            sample = 0.0F;
            continue;
        }

        float x = input * gain;

        if (settings_.maxLoud) {


            /*
             * Gentle expansion around speech level.
             * Keeps normal speech prominent while avoiding
             * unnecessary amplification of very small noise.
             */
            if (magnitude < speechStart) {
                x *= 0.55F;
            }

            /*
             * Strong dynamic compression.
             * Quiet speech remains boosted while loud peaks
             * are brought under control.
             */
            const float compressedMagnitude =
                1.0F -
                std::exp(
                    -std::abs(x) * 1.15F);

            x =
                std::copysign(
                    compressedMagnitude,
                    x);

            /*
             * Additional soft saturation gives speech
             * more perceived loudness.
             */
            x = std::tanh(x * 2.0F);

            /*
             * Final peak limiter.
             */
            x = std::clamp(
                x,
                -ceiling,
                ceiling);

            sample = x;
            continue;
        }

        // Normal processing.
        if (std::abs(x) <= ceiling) {
            sample = x;
            continue;
        }

        const float excess =
            std::abs(x) - ceiling;

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