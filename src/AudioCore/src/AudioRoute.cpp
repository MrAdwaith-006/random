#include "SheikzAmp/Audio/AudioRoute.h"

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace SheikzAmp::Audio {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        char buffer[64]{};

        sprintf_s(
            buffer,
            " HRESULT=0x%08lX",
            static_cast<unsigned long>(hr));

        throw std::runtime_error(
            std::string(message) + buffer);
    }
}

class ComApartment final {
public:
    ComApartment() {
        const HRESULT hr =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(hr)) {
            throw std::runtime_error(
                "Could not initialize COM");
        }

        initialized_ = true;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

/*
 * The Windows/Voicemod endpoints used by this application can expose
 * WAVE_FORMAT_EXTENSIBLE with a custom SubFormat GUID.
 *
 * For 32-bit interleaved endpoints where:
 *
 *     blockAlign == channels * 4
 *
 * the stream is handled as float32 by this application.
 */
bool isFloat32(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return format.wBitsPerSample == 32 &&
               format.nBlockAlign ==
                   format.nChannels * sizeof(float);
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= 22) {

        if (format.wBitsPerSample == 32 &&
            format.nBlockAlign ==
                format.nChannels * sizeof(float)) {

            return true;
        }
    }

    return false;
}

bool isPcm16(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return format.wBitsPerSample == 16;
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= 22) {

        const auto& ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(
                format);

        return ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
               format.wBitsPerSample == 16;
    }

    return false;
}

bool isPcm24(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return format.wBitsPerSample == 24;
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= 22) {

        const auto& ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(
                format);

        return ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
               format.wBitsPerSample == 24;
    }

    return false;
}

bool isPcm32(const WAVEFORMATEX& format) noexcept {
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return format.wBitsPerSample == 32;
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= 22) {

        const auto& ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(
                format);

        return ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
               format.wBitsPerSample == 32;
    }

    return false;
}

class FloatRingBuffer final {
public:
    FloatRingBuffer(
        std::size_t capacityFrames,
        std::size_t channels)
        : data_(capacityFrames * channels),
          capacityFrames_(capacityFrames),
          channels_(channels) {
    }

    void push(
        const float* samples,
        std::size_t frames) noexcept {

        if (frames == 0 ||
            capacityFrames_ == 0 ||
            channels_ == 0) {
            return;
        }

        if (frames >= capacityFrames_) {
            samples +=
                (frames - capacityFrames_) * channels_;

            frames = capacityFrames_;

            readFrame_ = 0;
            writeFrame_ = 0;
            usedFrames_ = 0;
        }

        for (std::size_t frame = 0;
             frame < frames;
             ++frame) {

            if (usedFrames_ == capacityFrames_) {
                readFrame_ =
                    (readFrame_ + 1) %
                    capacityFrames_;

                --usedFrames_;
            }

            std::memcpy(
                &data_[writeFrame_ * channels_],
                &samples[frame * channels_],
                channels_ * sizeof(float));

            writeFrame_ =
                (writeFrame_ + 1) %
                capacityFrames_;

            ++usedFrames_;
        }
    }

    std::size_t pop(
        float* destination,
        std::size_t frames) noexcept {

        const std::size_t count =
            std::min(frames, usedFrames_);

        for (std::size_t frame = 0;
             frame < count;
             ++frame) {

            std::memcpy(
                &destination[frame * channels_],
                &data_[readFrame_ * channels_],
                channels_ * sizeof(float));

            readFrame_ =
                (readFrame_ + 1) %
                capacityFrames_;
        }

        usedFrames_ -= count;

        return count;
    }

    [[nodiscard]]
    std::size_t availableFrames() const noexcept {
        return usedFrames_;
    }

private:
    std::vector<float> data_;

    std::size_t capacityFrames_{};
    std::size_t channels_{};

    std::size_t readFrame_{};
    std::size_t writeFrame_{};
    std::size_t usedFrames_{};
};

void toFloat(
    const BYTE* source,
    std::size_t frames,
    std::size_t channels,
    const WAVEFORMATEX& format,
    std::vector<float>& destination) {

    const std::size_t sampleCount =
        frames * channels;

    destination.resize(sampleCount);

    if (isFloat32(format)) {
        std::memcpy(
            destination.data(),
            source,
            sampleCount * sizeof(float));

        return;
    }

    if (isPcm16(format)) {
        const auto* pcm =
            reinterpret_cast<const int16_t*>(source);

        constexpr float scale =
            1.0F / 32768.0F;

        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            destination[i] =
                static_cast<float>(pcm[i]) *
                scale;
        }

        return;
    }

    if (isPcm24(format)) {
        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            const BYTE* sample =
                source + i * 3;

            int32_t value =
                static_cast<int32_t>(sample[0]) |
                (static_cast<int32_t>(sample[1]) << 8) |
                (static_cast<int32_t>(sample[2]) << 16);

            if ((value & 0x00800000) != 0) {
                value |=
                    static_cast<int32_t>(0xFF000000);
            }

            destination[i] =
                static_cast<float>(value) /
                8388608.0F;
        }

        return;
    }

    if (isPcm32(format)) {
        const auto* pcm =
            reinterpret_cast<const int32_t*>(source);

        constexpr float scale =
            1.0F / 2147483648.0F;

        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            destination[i] =
                static_cast<float>(pcm[i]) *
                scale;
        }

        return;
    }

    throw std::runtime_error(
        "Unsupported microphone format");
}

void fromFloat(
    const float* source,
    std::size_t frames,
    std::size_t channels,
    const WAVEFORMATEX& format,
    BYTE* destination) {

    const std::size_t sampleCount =
        frames * channels;

    if (isFloat32(format)) {
        std::memcpy(
            destination,
            source,
            sampleCount * sizeof(float));

        return;
    }

    if (isPcm16(format)) {
        auto* pcm =
            reinterpret_cast<int16_t*>(destination);

        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            const float value =
                std::clamp(
                    source[i],
                    -1.0F,
                    0.999969F);

            pcm[i] =
                static_cast<int16_t>(
                    std::lrint(value * 32767.0F));
        }

        return;
    }

    if (isPcm24(format)) {
        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            const float value =
                std::clamp(
                    source[i],
                    -1.0F,
                    0.999999F);

            const int32_t pcm =
                static_cast<int32_t>(
                    std::lrint(value * 8388607.0F));

            BYTE* sample =
                destination + i * 3;

            sample[0] =
                static_cast<BYTE>(pcm & 0xFF);

            sample[1] =
                static_cast<BYTE>((pcm >> 8) & 0xFF);

            sample[2] =
                static_cast<BYTE>((pcm >> 16) & 0xFF);
        }

        return;
    }

    if (isPcm32(format)) {
        auto* pcm =
            reinterpret_cast<int32_t*>(destination);

        for (std::size_t i = 0;
             i < sampleCount;
             ++i) {

            const float value =
                std::clamp(
                    source[i],
                    -1.0F,
                    0.999999F);

            pcm[i] =
                static_cast<int32_t>(
                    std::lrint(
                        value * 2147483647.0F));
        }

        return;
    }

    /*
     * Some Windows endpoints report 32-bit extensible
     * audio using a non-standard SubFormat. Treat the
     * 32-bit interleaved stream as float32.
     */
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.wBitsPerSample == 32 &&
        format.nBlockAlign ==
            format.nChannels * sizeof(float)) {

        std::memcpy(
            destination,
            source,
            sampleCount * sizeof(float));

        return;
    }

    throw std::runtime_error(
        "Unsupported speaker format");
}

void convertChannels(
    const std::vector<float>& input,
    std::size_t inputChannels,
    std::vector<float>& output,
    std::size_t outputChannels) {

    if (inputChannels == outputChannels) {
        output = input;
        return;
    }

    if (inputChannels == 0 ||
        outputChannels == 0) {

        output.clear();
        return;
    }

    const std::size_t inputFrames =
        input.size() / inputChannels;

    output.resize(
        inputFrames * outputChannels);

    for (std::size_t frame = 0;
         frame < inputFrames;
         ++frame) {

        if (inputChannels == 1) {
            const float value =
                input[frame];

            for (std::size_t channel = 0;
                 channel < outputChannels;
                 ++channel) {

                output[
                    frame * outputChannels +
                    channel] = value;
            }

            continue;
        }

        if (outputChannels == 1) {
            float sum = 0.0F;

            for (std::size_t channel = 0;
                 channel < inputChannels;
                 ++channel) {

                sum +=
                    input[
                        frame * inputChannels +
                        channel];
            }

            output[frame] =
                sum /
                static_cast<float>(inputChannels);

            continue;
        }

        for (std::size_t channel = 0;
             channel < outputChannels;
             ++channel) {

            const std::size_t sourceChannel =
                std::min(
                    channel,
                    inputChannels - 1);

            output[
                frame * outputChannels +
                channel] =
                input[
                    frame * inputChannels +
                    sourceChannel];
        }
    }
}

class LinearResampler final {
public:
    LinearResampler(
        std::size_t channels,
        double inputRate,
        double outputRate)
        : channels_(channels),
          step_(inputRate / outputRate) {
    }

    void reset() {
        buffer_.clear();
        position_ = 0.0;
    }

    void process(
        const std::vector<float>& input,
        std::vector<float>& output) {

        output.clear();

        if (channels_ == 0 ||
            input.empty()) {
            return;
        }

        const std::size_t inputFrames =
            input.size() / channels_;

        if (inputFrames == 0) {
            return;
        }

        if (buffer_.empty()) {
            buffer_ = input;
        }
        else {
            const std::size_t oldFrames =
                buffer_.size() / channels_;

            buffer_.reserve(
                buffer_.size() + input.size());

            buffer_.insert(
                buffer_.end(),
                input.begin(),
                input.end());

            (void)oldFrames;
        }

        const std::size_t availableFrames =
            buffer_.size() / channels_;

        if (availableFrames < 2) {
            return;
        }

        while (position_ + 1.0 <
               static_cast<double>(availableFrames)) {

            const std::size_t index =
                static_cast<std::size_t>(
                    position_);

            const double fraction =
                position_ -
                static_cast<double>(index);

            for (std::size_t channel = 0;
                 channel < channels_;
                 ++channel) {

                const float a =
                    buffer_[
                        index * channels_ +
                        channel];

                const float b =
                    buffer_[
                        (index + 1) * channels_ +
                        channel];

                output.push_back(
                    a +
                    static_cast<float>(
                        (b - a) * fraction));
            }

            position_ += step_;
        }

        const std::size_t consumedFrames =
            static_cast<std::size_t>(
                position_);

        if (consumedFrames > 0) {
            const std::size_t consumedSamples =
                consumedFrames * channels_;

            if (consumedSamples <
                buffer_.size()) {

                std::move(
                    buffer_.begin() +
                        static_cast<std::ptrdiff_t>(
                            consumedSamples),
                    buffer_.end(),
                    buffer_.begin());
            }

            buffer_.resize(
                buffer_.size() -
                std::min(
                    consumedSamples,
                    buffer_.size()));

            position_ -=
                static_cast<double>(
                    consumedFrames);
        }
    }

private:
    std::size_t channels_{};
    double step_{1.0};
    double position_{};

    std::vector<float> buffer_;
};

} // namespace

class AudioRoute::Implementation final {
public:
    ~Implementation() {
        stop();
    }

    void start(
        const MicrophoneRouteSettings& settings) {

        if (settings.inputDeviceId.empty()) {
            throw std::runtime_error(
                "Select an input microphone");
        }

        if (settings.selfHear &&
            settings.speakerDeviceId.empty()) {

            throw std::runtime_error(
                "Select a speaker for self hear");
        }

        if (running_) {
            throw std::runtime_error(
                "Audio route is already running");
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        {
            std::lock_guard lock(errorMutex_);
            lastError_.clear();
        }

        {
            std::lock_guard lock(startupMutex_);
            startupComplete_ = false;
            startupSuccess_ = false;
        }

        processor_.setSettings(
            settings.amplifier);

        running_ = true;

        try {
            worker_ = std::thread(
                &Implementation::run,
                this,
                settings.inputDeviceId,
                settings.speakerDeviceId,
                settings.selfHear);
        }
        catch (...) {
            running_ = false;
            throw;
        }

        {
            std::unique_lock lock(startupMutex_);

            startupCondition_.wait_for(
                lock,
                std::chrono::seconds(3),
                [this] {
                    return startupComplete_;
                });
        }

        const auto error = lastError();

        if (!error.empty()) {
            running_ = false;

            if (worker_.joinable()) {
                worker_.join();
            }

            throw std::runtime_error(
            "Could not initialize microphone: " +
            std::string(
                error.begin(),
                error.end()));
        }

        if (!startupSuccess_) {
            running_ = false;

            if (worker_.joinable()) {
                worker_.join();
            }

            throw std::runtime_error(
                "Audio engine startup timed out");
        }
    }

    void stop() noexcept {
        running_ = false;

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]]
    bool isRunning() const noexcept {
        return running_;
    }

    [[nodiscard]]
    std::wstring lastError() const {
        std::lock_guard lock(errorMutex_);
        return lastError_;
    }

private:
    void setError(
        const char* message) noexcept {

        {
            std::lock_guard lock(errorMutex_);

            lastError_.clear();

            for (const char* p = message;
                *p != '\0';
                ++p) {

                lastError_.push_back(
                    static_cast<unsigned char>(*p));
            }
        }

        {
            std::lock_guard lock(startupMutex_);
            startupComplete_ = true;
            startupSuccess_ = false;
        }

        startupCondition_.notify_all();
    }

    void signalStartupSuccess() noexcept {
        {
            std::lock_guard lock(startupMutex_);
            startupComplete_ = true;
            startupSuccess_ = true;
        }

        startupCondition_.notify_all();
    }

    void run(
        std::wstring inputId,
        std::wstring speakerId,
        bool selfHear) noexcept {

        ComPtr<IAudioClient> inputClient;
        ComPtr<IAudioClient> speakerClient;

        try {
            ComApartment com;

            ComPtr<IMMDeviceEnumerator> enumerator;

            check(
                CoCreateInstance(
                    __uuidof(MMDeviceEnumerator),
                    nullptr,
                    CLSCTX_ALL,
                    IID_PPV_ARGS(&enumerator)),
                "Could not create audio enumerator");

            ComPtr<IMMDevice> inputDevice;

            check(
                enumerator->GetDevice(
                    inputId.c_str(),
                    &inputDevice),
                "Could not open input microphone");

            check(
                inputDevice->Activate(
                    __uuidof(IAudioClient),
                    CLSCTX_ALL,
                    nullptr,
                    reinterpret_cast<void**>(
                        inputClient.GetAddressOf())),
                "Could not activate microphone");

            WAVEFORMATEX* inputRaw = nullptr;

            check(
                inputClient->GetMixFormat(
                    &inputRaw),
                "Could not read microphone format");

            WAVEFORMATEX inputFormat{};
                inputFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                inputFormat.nChannels = 2;
                inputFormat.nSamplesPerSec = 48000;
                inputFormat.wBitsPerSample = 32;
                inputFormat.nBlockAlign =
                    inputFormat.nChannels *
                    (inputFormat.wBitsPerSample / 8);
                inputFormat.nAvgBytesPerSec =
                    inputFormat.nSamplesPerSec *
                    inputFormat.nBlockAlign;
                inputFormat.cbSize = 0;

                WAVEFORMATEX* closestFormat = nullptr;

                HRESULT formatResult =
                    inputClient->IsFormatSupported(
                        AUDCLNT_SHAREMODE_SHARED,
                        &inputFormat,
                        &closestFormat);

                if (closestFormat != nullptr) {
                    CoTaskMemFree(closestFormat);
                    closestFormat = nullptr;
                }

                if (FAILED(formatResult)) {
                    CoTaskMemFree(inputRaw);

                    throw std::runtime_error(
                        "Voicemod does not accept 48kHz float32 microphone format");
                }

                ComPtr<IAudioCaptureClient> capture;

                check(
                    inputClient->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        0,
                        0,
                        0,
                        &inputFormat,
                        nullptr),
                    "Could not initialize microphone");

                CoTaskMemFree(inputRaw);

            check(
                inputClient->GetService(
                    IID_PPV_ARGS(&capture)),
                "Could not open microphone capture");

            WAVEFORMATEX speakerFormat{};
            std::size_t speakerChannels = 0;
            UINT32 speakerBufferFrames = 0;

            ComPtr<IAudioRenderClient> render;

            if (selfHear) {
                ComPtr<IMMDevice> speakerDevice;

                check(
                    enumerator->GetDevice(
                        speakerId.c_str(),
                        &speakerDevice),
                    "Could not open speaker");

                check(
                    speakerDevice->Activate(
                        __uuidof(IAudioClient),
                        CLSCTX_ALL,
                        nullptr,
                        reinterpret_cast<void**>(
                            speakerClient.GetAddressOf())),
                    "Could not activate speaker");

                WAVEFORMATEX* speakerRaw = nullptr;

                check(
                    speakerClient->GetMixFormat(
                        &speakerRaw),
                    "Could not read speaker format");

                speakerFormat =
                    *speakerRaw;

                const bool speakerSupported =
                    isFloat32(speakerFormat) ||
                    isPcm16(speakerFormat) ||
                    isPcm24(speakerFormat) ||
                    isPcm32(speakerFormat);

                if (!speakerSupported) {
                    CoTaskMemFree(speakerRaw);

                    throw std::runtime_error(
                        "Speaker format is not supported");
                }

                speakerChannels =
                    speakerFormat.nChannels;

                WAVEFORMATEX speakerInitFormat{};
                    speakerInitFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                    speakerInitFormat.nChannels = speakerFormat.nChannels;
                    speakerInitFormat.nSamplesPerSec = speakerFormat.nSamplesPerSec;
                    speakerInitFormat.wBitsPerSample = 32;
                    speakerInitFormat.nBlockAlign =
                        speakerInitFormat.nChannels *
                        sizeof(float);
                    speakerInitFormat.nAvgBytesPerSec =
                        speakerInitFormat.nSamplesPerSec *
                        speakerInitFormat.nBlockAlign;
                    speakerInitFormat.cbSize = 0;

                    WAVEFORMATEX* speakerClosest = nullptr;

                    HRESULT speakerFormatResult =
                        speakerClient->IsFormatSupported(
                            AUDCLNT_SHAREMODE_SHARED,
                            &speakerInitFormat,
                            &speakerClosest);

                    if (speakerClosest != nullptr) {
                        CoTaskMemFree(speakerClosest);
                        speakerClosest = nullptr;
                    }

                    if (FAILED(speakerFormatResult)) {
                        CoTaskMemFree(speakerRaw);

                        throw std::runtime_error(
                            "Speaker does not accept float32 mix format");
                    }

                    speakerFormat = speakerInitFormat;

                    check(
                        speakerClient->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            0,
                            0,
                            0,
                            &speakerFormat,
                            nullptr),
                        "Could not initialize speaker");

                CoTaskMemFree(speakerRaw);

                check(
                    speakerClient->GetService(
                        IID_PPV_ARGS(&render)),
                    "Could not open speaker renderer");

                check(
                    speakerClient->GetBufferSize(
                        &speakerBufferFrames),
                    "Could not read speaker buffer size");
            }

            /*
             * We process internally at the microphone sample rate,
             * convert channels, then resample to the speaker rate.
             */
            const double inputRate =
                static_cast<double>(
                    inputFormat.nSamplesPerSec);

            const double outputRate =
                selfHear
                    ? static_cast<double>(
                          speakerFormat.nSamplesPerSec)
                    : inputRate;

            FloatRingBuffer queue(
                std::max<std::size_t>(
                    static_cast<std::size_t>(
                        speakerBufferFrames) * 6,
                    4096),
                selfHear
                    ? speakerChannels
                    : inputFormat.nChannels);

            LinearResampler resampler(
                selfHear
                    ? speakerChannels
                    : inputFormat.nChannels,
                inputRate,
                outputRate);

            std::vector<float> captured;
            std::vector<float> channelConverted;
            std::vector<float> resampled;

            check(
                inputClient->Start(),
                "Could not start microphone");

            if (selfHear) {
                check(
                    speakerClient->Start(),
                    "Could not start speaker");
            }

            signalStartupSuccess();

            while (running_) {
                UINT32 packetFrames = 0;

                check(
                    capture->GetNextPacketSize(
                        &packetFrames),
                    "Could not query microphone");

                while (packetFrames != 0 &&
                       running_) {

                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    check(
                        capture->GetBuffer(
                            &data,
                            &frames,
                            &flags,
                            nullptr,
                            nullptr),
                        "Could not capture microphone");

                    if ((flags &
                         AUDCLNT_BUFFERFLAGS_SILENT) != 0) {

                        captured.assign(
                            static_cast<std::size_t>(
                                frames) *
                                inputFormat.nChannels,
                            0.0F);
                    }
                    else {
                        toFloat(
                            data,
                            frames,
                            inputFormat.nChannels,
                            inputFormat,
                            captured);
                    }

                    processor_.processInterleaved(
                        std::span<float>(
                            captured.data(),
                            captured.size()));

                    if (selfHear) {
                        convertChannels(
                            captured,
                            inputFormat.nChannels,
                            channelConverted,
                            speakerChannels);

                        resampler.process(
                            channelConverted,
                            resampled);

                        if (!resampled.empty()) {
                            queue.push(
                                resampled.data(),
                                resampled.size() /
                                    speakerChannels);
                        }
                    }

                    check(
                        capture->ReleaseBuffer(
                            frames),
                        "Could not release microphone");

                    check(
                        capture->GetNextPacketSize(
                            &packetFrames),
                        "Could not query microphone");
                }

                if (selfHear) {
                    UINT32 padding = 0;

                    check(
                        speakerClient->GetCurrentPadding(
                            &padding),
                        "Could not query speaker");

                    const UINT32 writable =
                        speakerBufferFrames > padding
                            ? speakerBufferFrames - padding
                            : 0;

                    if (writable != 0) {
                        BYTE* output = nullptr;

                        check(
                            render->GetBuffer(
                                writable,
                                &output),
                            "Could not acquire speaker buffer");

                        std::vector<float> renderSamples(
                            static_cast<std::size_t>(
                                writable) *
                            speakerChannels,
                            0.0F);

                        const std::size_t available =
                            queue.availableFrames();

                        const std::size_t framesToWrite =
                            std::min(
                                static_cast<std::size_t>(
                                    writable),
                                available);

                        if (framesToWrite > 0) {
                            queue.pop(
                                renderSamples.data(),
                                framesToWrite);
                        }

                        /*
                         * Anything not yet available from the
                         * microphone is silence.
                         */
                        fromFloat(
                            renderSamples.data(),
                            writable,
                            speakerChannels,
                            speakerFormat,
                            output);

                        check(
                            render->ReleaseBuffer(
                                writable,
                                0),
                            "Could not release speaker buffer");
                    }
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }

            inputClient->Stop();

            if (selfHear) {
                speakerClient->Stop();
            }
        }
        catch (const std::exception& error) {
            setError(error.what());
        }
        catch (...) {
            setError(
                "Unknown Windows audio error");
        }

        running_ = false;
    }

    std::atomic_bool running_{false};

    std::thread worker_;

    SignalProcessor processor_;

    mutable std::mutex errorMutex_;
    std::wstring lastError_;

    mutable std::mutex startupMutex_;
    std::condition_variable startupCondition_;

    bool startupComplete_{false};
    bool startupSuccess_{false};
};

AudioRoute::AudioRoute()
    : implementation_(
          std::make_unique<Implementation>()) {
}

AudioRoute::~AudioRoute() = default;

void AudioRoute::startMicrophoneRoute(
    const MicrophoneRouteSettings& settings) {

    implementation_->start(settings);
}

void AudioRoute::stop() noexcept {
    implementation_->stop();
}

bool AudioRoute::isRunning() const noexcept {
    return implementation_->isRunning();
}

std::wstring AudioRoute::lastError() const {
    return implementation_->lastError();
}

} // namespace SheikzAmp::Audio