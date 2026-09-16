#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "SheikzAmp/Audio/AudioRoute.h"
#include "SheikzAmp/Audio/DeviceEnumerator.h"

#include <Windows.h>
#include <commctrl.h>

#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

using namespace SheikzAmp::Audio;

namespace {

constexpr int IDC_MIC = 1001;
constexpr int IDC_SPEAKER = 1003;
constexpr int IDC_GAIN = 1004;
constexpr int IDC_GAIN_LABEL = 1005;
constexpr int IDC_MAX_LOUD = 1006;
constexpr int IDC_SELF_HEAR = 1007;
constexpr int IDC_START = 1008;
constexpr int IDC_STATUS = 1009;

HWND g_window = nullptr;
HWND g_micCombo = nullptr;
HWND g_speakerCombo = nullptr;
HWND g_gainSlider = nullptr;
HWND g_gainLabel = nullptr;
HWND g_maxLoud = nullptr;
HWND g_selfHear = nullptr;
HWND g_startButton = nullptr;
HWND g_status = nullptr;

std::vector<AudioDevice> g_microphones;
std::vector<AudioDevice> g_outputRoutes;
std::vector<AudioDevice> g_speakers;

std::unique_ptr<AudioRoute> g_route;
bool g_running = false;

HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;

void setText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void updateGainLabel() {
    const int gain = static_cast<int>(
        SendMessageW(
            g_gainSlider,
            TBM_GETPOS,
            0,
            0));

    setText(
        g_gainLabel,
        std::to_wstring(gain) + L"x");
}

void setStatus(const std::wstring& text) {
    setText(g_status, L"Status: " + text);
}

void populateDevices() {

    g_microphones =
        DeviceEnumerator().recordingDevices();

    g_outputRoutes =
        DeviceEnumerator().playbackDevices();

    g_speakers =
        DeviceEnumerator().playbackDevices();

    SendMessageW(
        g_micCombo,
        CB_RESETCONTENT,
        0,
        0);


    SendMessageW(
        g_speakerCombo,
        CB_RESETCONTENT,
        0,
        0);

    for (const auto& mic : g_microphones) {
        SendMessageW(
            g_micCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                mic.name.c_str()));
    }


    for (const auto& speaker : g_speakers) {
        SendMessageW(
            g_speakerCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(
                speaker.name.c_str()));
    }

    int defaultMic = 0;
    int defaultSpeaker = 0;

    for (std::size_t i = 0;
         i < g_microphones.size();
         ++i) {

        if (g_microphones[i].isDefault) {
            defaultMic =
                static_cast<int>(i);
            break;
        }
    }


    for (std::size_t i = 0;
         i < g_speakers.size();
         ++i) {

        if (g_speakers[i].isDefault) {
            defaultSpeaker =
                static_cast<int>(i);
            break;
        }
    }

    SendMessageW(
        g_micCombo,
        CB_SETCURSEL,
        defaultMic,
        0);


    SendMessageW(
        g_speakerCombo,
        CB_SETCURSEL,
        defaultSpeaker,
        0);
}

void startAmplifier() {

    if (g_running) {
        return;
    }

    const int micIndex = static_cast<int>(
    SendMessageW(
        g_micCombo,
        CB_GETCURSEL,
        0,
        0));

    const int speakerIndex = static_cast<int>(
        SendMessageW(
            g_speakerCombo,
            CB_GETCURSEL,
            0,
            0));

    const bool selfHear =
        SendMessageW(
            g_selfHear,
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;

    int cableInputIndex = -1;
    for (std::size_t i = 0; i < g_outputRoutes.size(); ++i) {
        if (g_outputRoutes[i].name.find(L"CABLE Input") != std::wstring::npos) {
            cableInputIndex = static_cast<int>(i);
            break;
        }
    }

    if (micIndex < 0 ||
        micIndex >= static_cast<int>(g_microphones.size()) ||
        cableInputIndex < 0 ||
        (selfHear &&
        (speakerIndex < 0 ||
        speakerIndex >= static_cast<int>(g_speakers.size())))) {

        MessageBoxW(
            g_window,
            L"Please select the required audio devices.",
            L"SheikzAmp",
            MB_ICONWARNING);

        return;
    }

    const float gain = static_cast<float>(
        SendMessageW(
            g_gainSlider,
            TBM_GETPOS,
            0,
            0));

    const bool maxLoud =
        SendMessageW(
            g_maxLoud,
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;

    try {

        g_route =
            std::make_unique<AudioRoute>();

        g_route->startMicrophoneRoute({
            g_microphones[micIndex].id,
            g_outputRoutes[cableInputIndex].id,
            selfHear
                ? g_speakers[speakerIndex].id
                : L"",
            {
                gain,
                0.98F,
                0.0F,
                maxLoud
            },
            selfHear
        });

        for (int i = 0; i < 50; ++i) {

            if (!g_route->lastError().empty()) {
                throw std::runtime_error(
                    std::string(
                        g_route->lastError().begin(),
                        g_route->lastError().end()));
            }

            if (g_route->isRunning()) {
                break;
            }

            Sleep(10);
        }

        if (!g_route->lastError().empty()) {
            throw std::runtime_error(
                std::string(
                    g_route->lastError().begin(),
                    g_route->lastError().end()));
        }

        if (!g_route->isRunning()) {
            throw std::runtime_error(
                "Audio engine failed to start.");
        }

        g_running = true;

        setText(
            g_startButton,
            L"STOP AMPLIFIER");

        setStatus(
            L"RUNNING");

    }
    catch (const std::exception& error) {

        if (g_route) {
            g_route->stop();
            g_route.reset();
        }

        std::wstring message(
            error.what(),
            error.what() + strlen(error.what()));

        MessageBoxW(
            g_window,
            message.c_str(),
            L"SheikzAmp Audio Error",
            MB_ICONERROR);

        setStatus(
            L"ERROR");
    }
}

void stopAmplifier() {

    if (!g_route) {
        return;
    }

    g_route->stop();
    g_route.reset();

    g_running = false;

    setText(
        g_startButton,
        L"START AMPLIFIER");

    setStatus(
        L"STOPPED");
}

HWND createLabel(
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height) {

    HWND label = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    SendMessageW(
        label,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(g_font),
        TRUE);

    return label;
}

HWND createCombo(
    HWND parent,
    int id,
    int x,
    int y,
    int width,
    int height) {

    HWND combo = CreateWindowExW(
        0,
        WC_COMBOBOXW,
        L"",
        WS_CHILD |
        WS_VISIBLE |
        WS_TABSTOP |
        CBS_DROPDOWNLIST,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);

    SendMessageW(
        combo,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(g_font),
        TRUE);

    return combo;
}

LRESULT CALLBACK windowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    switch (message) {

    case WM_CREATE: {

        g_font =
            CreateFontW(
                18,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH,
                L"Segoe UI");

        g_titleFont =
            CreateFontW(
                30,
                0,
                0,
                0,
                FW_BOLD,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH,
                L"Segoe UI");

        createLabel(
            hwnd,
            L"SHEIKZAMP",
            35,
            25,
            400,
            45);

        HWND title =
            FindWindowExW(
                hwnd,
                nullptr,
                L"STATIC",
                L"SHEIKZAMP");

        if (title) {
            SendMessageW(
                title,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(g_titleFont),
                TRUE);
        }

        createLabel(
            hwnd,
            L"Input Microphone",
            35,
            90,
            200,
            25);

        g_micCombo =
            createCombo(
                hwnd,
                IDC_MIC,
                35,
                118,
                410,
                180);

        createLabel(
            hwnd,
            L"Output: VB-CABLE Input (automatic)",
            35,
            155,
            410,
            25);

createLabel(
            hwnd,
            L"Self-Hear Speaker",
            35,
            165,
            200,
            25);

        g_speakerCombo =
            createCombo(
                hwnd,
                IDC_SPEAKER,
                35,
                193,
                410,
                180);

        createLabel(
            hwnd,
            L"Amplification",
            35,
            240,
            200,
            25);

        g_gainSlider =
            CreateWindowExW(
                0,
                TRACKBAR_CLASSW,
                L"",
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                TBS_AUTOTICKS,
                35,
                270,
                350,
                35,
                hwnd,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(IDC_GAIN)),
                GetModuleHandleW(nullptr),
                nullptr);

        SendMessageW(
            g_gainSlider,
            TBM_SETRANGE,
            TRUE,
            MAKELONG(1, 100));

        SendMessageW(
            g_gainSlider,
            TBM_SETPOS,
            TRUE,
            50);

        g_gainLabel =
            createLabel(
                hwnd,
                L"50x",
                395,
                270,
                60,
                35);

        g_maxLoud =
            CreateWindowExW(
                0,
                L"BUTTON",
                L"MAX LOUD",
                WS_CHILD |
                WS_VISIBLE |
                BS_AUTOCHECKBOX,
                35,
                320,
                180,
                30,
                hwnd,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(IDC_MAX_LOUD)),
                GetModuleHandleW(nullptr),
                nullptr);

        SendMessageW(
            g_maxLoud,
            BM_SETCHECK,
            BST_CHECKED,
            0);

        g_selfHear =
            CreateWindowExW(
                0,
                L"BUTTON",
                L"Self Hear",
                WS_CHILD |
                WS_VISIBLE |
                BS_AUTOCHECKBOX,
                230,
                320,
                150,
                30,
                hwnd,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(IDC_SELF_HEAR)),
                GetModuleHandleW(nullptr),
                nullptr);

        SendMessageW(
            g_selfHear,
            BM_SETCHECK,
            BST_CHECKED,
            0);

        g_startButton =
            CreateWindowExW(
                0,
                L"BUTTON",
                L"START AMPLIFIER",
                WS_CHILD |
                WS_VISIBLE |
                WS_TABSTOP |
                BS_PUSHBUTTON,
                35,
                375,
                410,
                50,
                hwnd,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(IDC_START)),
                GetModuleHandleW(nullptr),
                nullptr);

        g_status =
            createLabel(
                hwnd,
                L"Status: STOPPED",
                35,
                440,
                410,
                30);

        populateDevices();

        EnableWindow(
            g_speakerCombo,
            TRUE);

        return 0;
    }

    case WM_HSCROLL:

        if (reinterpret_cast<HWND>(lParam) ==
            g_gainSlider) {

            updateGainLabel();
        }

        return 0;

           case WM_COMMAND: {

        const int id =
            LOWORD(wParam);

        if (id == IDC_SELF_HEAR &&
            HIWORD(wParam) == BN_CLICKED) {

            const bool enabled =
                SendMessageW(
                    g_selfHear,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;

            EnableWindow(
                g_speakerCombo,
                enabled ? TRUE : FALSE);

            return 0;
        }

        if (id == IDC_START &&
            HIWORD(wParam) == BN_CLICKED) {

            if (g_running) {
                stopAmplifier();
            }
            else {
                startAmplifier();
            }

            return 0;
        }

        return 0;
    }

    case WM_CLOSE:

        stopAmplifier();

        DestroyWindow(hwnd);

        return 0;

    case WM_DESTROY:

        if (g_font) {
            DeleteObject(g_font);
            g_font = nullptr;
        }

        if (g_titleFont) {
            DeleteObject(g_titleFont);
            g_titleFont = nullptr;
        }

        PostQuitMessage(0);

        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int showCommand) {

    INITCOMMONCONTROLSEX controls{
        sizeof(INITCOMMONCONTROLSEX),
        ICC_BAR_CLASSES
    };

    InitCommonControlsEx(&controls);

    const wchar_t* className =
        L"SheikzAmpWindow";

    WNDCLASSW windowClass{};

    windowClass.lpfnWndProc =
        windowProc;

    windowClass.hInstance =
        instance;

    windowClass.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);

    windowClass.lpszClassName =
        className;

    RegisterClassW(&windowClass);

    g_window =
        CreateWindowExW(
            0,
            className,
            L"SheikzAmp",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            500,
            540,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_window) {
        return 1;
    }

    ShowWindow(
        g_window,
        showCommand);

    UpdateWindow(g_window);

    MSG message{};

    while (
        GetMessageW(
            &message,
            nullptr,
            0,
            0) > 0) {

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(
        message.wParam);
}