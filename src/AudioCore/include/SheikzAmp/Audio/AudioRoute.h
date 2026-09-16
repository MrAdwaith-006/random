#pragma once

#include "SheikzAmp/Audio/SignalProcessor.h"

#include <memory>
#include <string>

namespace SheikzAmp::Audio {

struct MicrophoneRouteSettings {
    std::wstring inputDeviceId;
    std::wstring outputDeviceId;
    std::wstring speakerDeviceId;

    AmplifierSettings amplifier{};

    bool selfHear{false};
};

class AudioRoute final {
public:
    AudioRoute();
    ~AudioRoute();

    AudioRoute(const AudioRoute&) = delete;
    AudioRoute& operator=(const AudioRoute&) = delete;

    void startMicrophoneRoute(const MicrophoneRouteSettings& settings);

    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] std::wstring lastError() const;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace SheikzAmp::Audio