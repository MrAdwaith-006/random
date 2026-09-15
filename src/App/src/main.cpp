#include "SheikzAmp/Audio/AudioRoute.h"
#include "SheikzAmp/Audio/DeviceEnumerator.h"

#include <Windows.h>

#include <iostream>

int wmain() {
    try {
        SheikzAmp::Audio::DeviceEnumerator enumerator;

        const auto microphones =
            enumerator.recordingDevices();

        const auto speakers =
            enumerator.playbackDevices();

        std::wcout << L"SheikzAmp audio engine test\n\n";

        std::wcout << L"Microphones:\n";

        for (std::size_t i = 0; i < microphones.size(); ++i) {
            std::wcout
                << L"[" << i << L"] "
                << (microphones[i].isDefault
                    ? L"[Default] "
                    : L"")
                << microphones[i].name
                << L"\n";
        }

        std::wcout << L"\nSpeakers:\n";

        for (std::size_t i = 0; i < speakers.size(); ++i) {
            std::wcout
                << L"[" << i << L"] "
                << (speakers[i].isDefault
                    ? L"[Default] "
                    : L"")
                << speakers[i].name
                << L"\n";
        }

        if (microphones.empty()) {
            std::wcerr << L"\nNo microphones found.\n";
            return 1;
        }

        if (speakers.empty()) {
            std::wcerr << L"\nNo speakers found.\n";
            return 1;
        }

        std::size_t micIndex = 0;
        std::size_t speakerIndex = 0;

        std::wcout << L"\nSelect input microphone: ";
        std::wcin >> micIndex;

        std::wcout << L"Select self-hear speaker: ";
        std::wcin >> speakerIndex;

        if (micIndex >= microphones.size() ||
            speakerIndex >= speakers.size()) {

            std::wcerr << L"Invalid device selection.\n";
            return 1;
        }

        float gain = 1.0F;

        std::wcout
            << L"Amplification (1.0 - 100.0): ";

        std::wcin >> gain;

        if (gain < 1.0F) {
            gain = 1.0F;
        }

        if (gain > 100.0F) {
            gain = 100.0F;
        }

        SheikzAmp::Audio::AudioRoute route;

        route.startMicrophoneRoute({
            microphones[micIndex].id,
            speakers[speakerIndex].id,
            {
                gain,
                0.98F,
                0.0F
            },
            true
        });

        // Give the audio worker a moment to initialize WASAPI.
        for (int i = 0; i < 50; ++i) {
            if (!route.lastError().empty()) {
                std::wcerr
                    << L"\nAudio engine error:\n"
                    << route.lastError()
                    << L"\n";

                route.stop();
                return 1;
            }

            if (route.isRunning()) {
                break;
            }

            Sleep(10);
        }

        const auto startupError = route.lastError();

        if (!startupError.empty()) {
            std::wcerr
                << L"\nAudio engine error:\n"
                << startupError
                << L"\n";

            route.stop();
            return 1;
        }

        std::wcout
            << L"\nSheikzAmp microphone route RUNNING.\n"
            << L"Gain: "
            << gain
            << L"x\n\n"
            << L"Press ENTER to stop.\n";

        std::wcin.ignore(10000, L'\n');
        std::wcin.get();

        route.stop();

        const auto error = route.lastError();

        if (!error.empty()) {
            std::wcerr
                << L"\nAudio engine error:\n"
                << error
                << L"\n";

            return 1;
        }

        std::wcout
            << L"\nStopped successfully.\n";

        return 0;
    }
    catch (const std::exception& error) {
        std::wcerr
            << L"\nError: "
            << error.what()
            << L"\n";

        return 1;
    }
}