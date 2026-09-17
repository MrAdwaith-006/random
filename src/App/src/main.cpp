#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "SheikzAmp/Audio/AudioRoute.h"
#include "SheikzAmp/Audio/DeviceEnumerator.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

using namespace Gdiplus;
using namespace SheikzAmp::Audio;

namespace {

// Application state
HWND g_hwnd = nullptr;
ULONG_PTR g_gdiplusToken = 0;
std::unique_ptr<Gdiplus::Bitmap> g_bgImage;
std::unique_ptr<Gdiplus::Bitmap> g_logoImage;

std::vector<AudioDevice> g_microphones;
std::vector<AudioDevice> g_speakers;
std::vector<AudioDevice> g_playbackDevices;

int g_selectedMic = 0;
int g_selectedSpeaker = 0;

int g_gain = 50; // 1x to 100x
bool g_maxLoud = true;
bool g_selfHear = true;

bool g_running = false;
std::wstring g_statusText = L"STOPPED";
bool g_statusIsError = false;

std::unique_ptr<AudioRoute> g_route;

// Window dimensions
constexpr int DEFAULT_WIDTH = 620;
constexpr int DEFAULT_HEIGHT = 780;
constexpr int MIN_WIDTH = 480;
constexpr int MIN_HEIGHT = 620;

// Dynamic Interactive Rectangles (computed on resize)
RECT g_rectBtnMin{};
RECT g_rectBtnMax{};
RECT g_rectBtnClose{};

RECT g_rectCardMic{};
RECT g_rectDropdownMic{};

RECT g_rectCardSpeaker{};
RECT g_rectDropdownSpeaker{};

RECT g_rectCardAmp{};
RECT g_rectSlider{};

RECT g_rectBoxMaxLoud{};
RECT g_rectBoxSelfHear{};

RECT g_rectBtnAction{};
RECT g_rectStatusBar{};

// Interaction State
bool g_isDraggingSlider = false;
int g_hoverState = 0; // 1: close, 2: min, 3: max, 4: micDrop, 5: spkDrop, 6: maxLoud, 7: selfHear, 8: actionBtn, 9: slider

// Double buffering persistent surfaces
std::unique_ptr<Gdiplus::Bitmap> g_backBuffer;
std::unique_ptr<Gdiplus::Graphics> g_backGraphics;
int g_bufWidth = 0;
int g_bufHeight = 0;

// ----------------------------------------------------------------------------
// Animation Engine State
// ----------------------------------------------------------------------------
struct Particle {
    float x{0.0f};
    float y{0.0f};
    float size{2.0f};
    float speedY{22.0f};
    float speedX{8.0f};
    float baseAlpha{50.0f};
    float phase{0.0f};
};

std::vector<Particle> g_particles;
float g_animTime = 0.0f;
auto g_lastFrameTime = std::chrono::steady_clock::now();

struct ControlAnim {
    float hoverClose = 0.0f;
    float hoverMin = 0.0f;
    float hoverMax = 0.0f;
    float hoverMicCard = 0.0f;
    float hoverMicDrop = 0.0f;
    float hoverSpkCard = 0.0f;
    float hoverSpkDrop = 0.0f;
    float hoverAmpCard = 0.0f;
    float hoverSlider = 0.0f;
    float hoverMaxLoud = 0.0f;
    float hoverSelfHear = 0.0f;
    float hoverActionBtn = 0.0f;
    float actionBtnPress = 0.0f;
    float runningGlow = 0.0f;
    float checkMaxLoud = 1.0f;
    float checkSelfHear = 1.0f;
    float visualGain = 50.0f;
    float sliderGlow = 0.0f;
} g_anim;

struct VisualizerData {
    float inLevel = 0.0f;
    float outLevel = 0.0f;
    float smoothIn = 0.0f;
    float smoothOut = 0.0f;
    float bands[16] = {0.0f};
    float smoothBands[16] = {0.0f};
    float peakBands[16] = {0.0f};
    float peakDropSpeed[16] = {0.0f};
} g_viz;

void initParticles(int width, int height) {
    g_particles.clear();
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> distX(0.0f, static_cast<float>(std::max(width, DEFAULT_WIDTH)));
    std::uniform_real_distribution<float> distY(0.0f, static_cast<float>(std::max(height, DEFAULT_HEIGHT)));
    std::uniform_real_distribution<float> distS(1.5f, 3.5f);
    std::uniform_real_distribution<float> distSpd(15.0f, 35.0f);
    std::uniform_real_distribution<float> distDrift(4.0f, 15.0f);
    std::uniform_real_distribution<float> distA(30.0f, 80.0f);
    std::uniform_real_distribution<float> distPh(0.0f, 6.28318f);

    for (int i = 0; i < 35; ++i) {
        Particle p;
        p.x = distX(rng);
        p.y = distY(rng);
        p.size = distS(rng);
        p.speedY = distSpd(rng);
        p.speedX = distDrift(rng);
        p.baseAlpha = distA(rng);
        p.phase = distPh(rng);
        g_particles.push_back(p);
    }
}

// Helper functions for path resolution
std::wstring getExecutableDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring strPath(path);
    size_t lastSlash = strPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return strPath.substr(0, lastSlash);
    }
    return L".";
}

void loadAssets() {
    std::wstring exeDir = getExecutableDir();
    std::vector<std::wstring> candidates = {
        exeDir + L"\\assets\\background.jpg",
        exeDir + L"\\..\\assets\\background.jpg",
        L"assets\\background.jpg",
        L"src\\App\\assets\\background.jpg",
        L"d:\\SheikzAmp\\src\\App\\assets\\background.jpg"
    };

    for (const auto& path : candidates) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_bgImage = std::make_unique<Gdiplus::Bitmap>(path.c_str());
            if (g_bgImage && g_bgImage->GetLastStatus() == Gdiplus::Ok) {
                break;
            }
        }
    }

    std::vector<std::wstring> logoCandidates = {
        exeDir + L"\\assets\\sheikzamp_logo.png",
        exeDir + L"\\..\\assets\\sheikzamp_logo.png",
        L"assets\\sheikzamp_logo.png",
        L"src\\App\\assets\\sheikzamp_logo.png",
        L"d:\\SheikzAmp\\src\\App\\assets\\sheikzamp_logo.png",
        exeDir + L"\\assets\\sheikzamp_logo.jpg",
        exeDir + L"\\..\\assets\\sheikzamp_logo.jpg",
        L"assets\\sheikzamp_logo.jpg",
        L"src\\App\\assets\\sheikzamp_logo.jpg",
        L"d:\\SheikzAmp\\src\\App\\assets\\sheikzamp_logo.jpg"
    };

    for (const auto& path : logoCandidates) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_logoImage = std::make_unique<Gdiplus::Bitmap>(path.c_str());
            if (g_logoImage && g_logoImage->GetLastStatus() == Gdiplus::Ok) {
                break;
            }
        }
    }
}

void computeLayout(int width, int height) {
    // Title bar controls (Windows 11 proportions)
    g_rectBtnClose = { width - 44, 6, width - 10, 32 };
    g_rectBtnMax   = { width - 80, 6, width - 46, 32 };
    g_rectBtnMin   = { width - 116, 6, width - 82, 32 };

    int cardMarginX = std::clamp(static_cast<int>(width * 0.075f), 24, 60);
    int cardLeft = cardMarginX;
    int cardRight = width - cardMarginX;
    int cardWidth = cardRight - cardLeft;

    // Proportional Y positioning matching reference photo
    float contentTop = 152.0f;
    float footerReserved = 52.0f;
    float availableH = static_cast<float>(height) - contentTop - footerReserved;

    // Card heights matching reference photo
    int cardMicH = std::clamp(static_cast<int>(availableH * 0.165f), 76, 92);
    int cardSpkH = cardMicH;
    int cardAmpH = std::clamp(static_cast<int>(availableH * 0.485f), 226, 290);
    int statusH = std::clamp(static_cast<int>(availableH * 0.082f), 36, 44);

    int gap = std::max(6, static_cast<int>((availableH - (cardMicH + cardSpkH + cardAmpH + statusH)) / 3.0f));

    // Card 1: Microphone
    int yMic = static_cast<int>(contentTop);
    g_rectCardMic = { cardLeft, yMic, cardRight, yMic + cardMicH };
    g_rectDropdownMic = { cardLeft + 80, yMic + cardMicH - 42, cardRight - 60, yMic + cardMicH - 12 };

    // Card 2: Speaker
    int ySpk = yMic + cardMicH + gap;
    g_rectCardSpeaker = { cardLeft, ySpk, cardRight, ySpk + cardSpkH };
    g_rectDropdownSpeaker = { cardLeft + 80, ySpk + cardSpkH - 42, cardRight - 60, ySpk + cardSpkH - 12 };

    // Card 3: Amplification
    int yAmp = ySpk + cardSpkH + gap;
    g_rectCardAmp = { cardLeft, yAmp, cardRight, yAmp + cardAmpH };
    g_rectSlider = { cardLeft + 80, yAmp + 40, cardRight - 20, yAmp + 54 };

    int boxW = (cardWidth - 36) / 2;
    int boxH = std::clamp(static_cast<int>(cardAmpH * 0.20f), 42, 52);
    int boxY = yAmp + 82;
    g_rectBoxMaxLoud = { cardLeft + 14, boxY, cardLeft + 14 + boxW, boxY + boxH };
    g_rectBoxSelfHear = { cardRight - 14 - boxW, boxY, cardRight - 14, boxY + boxH };

    int btnH = std::clamp(static_cast<int>(cardAmpH * 0.21f), 44, 52);
    int btnY = yAmp + cardAmpH - btnH - 14;
    g_rectBtnAction = { cardLeft + 18, btnY, cardRight - 18, btnY + btnH };

    // Status Bar
    int yStatus = yAmp + cardAmpH + gap;
    g_rectStatusBar = { cardLeft, yStatus, cardRight, yStatus + statusH };
}

void syncAmplifierSettings() {
    if (g_running && g_route) {
        g_route->updateSettings({
            static_cast<float>(g_gain),
            0.98F,
            0.0F,
            g_maxLoud
        });
    }
}

void refreshDevices() {
    try {
        DeviceEnumerator enumerator;
        g_microphones = enumerator.recordingDevices();
        g_speakers = enumerator.playbackDevices();
        g_playbackDevices = enumerator.playbackDevices();

        g_selectedMic = 0;
        for (size_t i = 0; i < g_microphones.size(); ++i) {
            if (g_microphones[i].isDefault) {
                g_selectedMic = static_cast<int>(i);
                break;
            }
        }

        g_selectedSpeaker = 0;
        for (size_t i = 0; i < g_speakers.size(); ++i) {
            if (g_speakers[i].isDefault) {
                g_selectedSpeaker = static_cast<int>(i);
                break;
            }
        }
    }
    catch (...) {
        if (g_microphones.empty()) {
            g_microphones.push_back({ L"mic1", L"Microphone (Default)", DeviceFlow::Recording, true });
        }
        if (g_speakers.empty()) {
            g_speakers.push_back({ L"spk1", L"Headphones (Default)", DeviceFlow::Playback, true });
        }
    }
}

void updateStatus(const std::wstring& text, bool isError = false) {
    g_statusText = text;
    g_statusIsError = isError;
    if (g_hwnd) {
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

void stopAmplifier() {
    if (!g_running) return;
    try {
        if (g_route) {
            g_route->stop();
            g_route.reset();
        }
    }
    catch (...) {}
    g_running = false;
    updateStatus(L"STOPPED", false);
}

void startAmplifier() {
    if (g_running) return;

    if (g_microphones.empty() || g_selectedMic < 0 || g_selectedMic >= static_cast<int>(g_microphones.size())) {
        MessageBoxW(g_hwnd, L"Please select a valid microphone.", L"SheikzAmp", MB_ICONWARNING);
        return;
    }

    if (g_selfHear && (g_speakers.empty() || g_selectedSpeaker < 0 || g_selectedSpeaker >= static_cast<int>(g_speakers.size()))) {
        MessageBoxW(g_hwnd, L"Please select a valid speaker for Self Hear.", L"SheikzAmp", MB_ICONWARNING);
        return;
    }

    int cableInputIndex = -1;
    for (size_t i = 0; i < g_playbackDevices.size(); ++i) {
        if (g_playbackDevices[i].name.find(L"CABLE Input") != std::wstring::npos) {
            cableInputIndex = static_cast<int>(i);
            break;
        }
    }

    std::wstring outputDeviceId;
    if (cableInputIndex >= 0) {
        outputDeviceId = g_playbackDevices[cableInputIndex].id;
    } else {
        outputDeviceId = g_speakers[g_selectedSpeaker].id;
    }

    try {
        g_route = std::make_unique<AudioRoute>();
        g_route->startMicrophoneRoute({
            g_microphones[g_selectedMic].id,
            outputDeviceId,
            g_selfHear ? g_speakers[g_selectedSpeaker].id : L"",
            {
                static_cast<float>(g_gain),
                0.98F,
                0.0F,
                g_maxLoud
            },
            g_selfHear
        });

        g_running = true;
        updateStatus(L"RUNNING (" + std::to_wstring(g_gain) + L"x)", false);
    }
    catch (const std::exception& ex) {
        g_running = false;
        std::string err = ex.what();
        std::wstring wErr(err.begin(), err.end());
        updateStatus(L"ERROR: " + wErr, true);
        MessageBoxW(g_hwnd, (L"Could not start amplifier:\n" + wErr).c_str(), L"SheikzAmp Audio Error", MB_ICONERROR);
    }
}

// ----------------------------------------------------------------------------
// Drawing Utilities
// ----------------------------------------------------------------------------
void addRoundedRect(GraphicsPath& path, const RectF& rect, float radius) {
    float diameter = radius * 2.0f;
    float right = rect.X + rect.Width;
    float bottom = rect.Y + rect.Height;

    if (diameter > rect.Width) diameter = rect.Width;
    if (diameter > rect.Height) diameter = rect.Height;

    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(right - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void drawGlassCard(Graphics& g, const RectF& rect, float radius, float hoverAlpha = 0.0f, bool isHighlighted = false) {
    GraphicsPath path;
    addRoundedRect(path, rect, radius);

    // Deep dark translucent glass fill matching reference photo
    int baseBgA = static_cast<int>(225.0f + hoverAlpha * 20.0f);
    SolidBrush bgBrush(Color(baseBgA, 5, 14, 24));
    g.FillPath(&bgBrush, &path);

    // Inner vertical gradient
    int gradTopA = static_cast<int>(32.0f + hoverAlpha * 30.0f + (isHighlighted ? 40.0f : 0.0f));
    LinearGradientBrush gradBrush(rect, Color(gradTopA, 0, 229, 255), Color(6, 0, 80, 140), LinearGradientModeVertical);
    g.FillPath(&gradBrush, &path);

    // Outer glow & border matching reference photo
    int glowAlpha = static_cast<int>(40.0f + hoverAlpha * 70.0f + (isHighlighted ? 60.0f : 0.0f));
    Pen glowPen(Color(glowAlpha, 0, 229, 255), 3.5f);
    g.DrawPath(&glowPen, &path);

    int borderAlpha = static_cast<int>(160.0f + hoverAlpha * 95.0f + (isHighlighted ? 90.0f : 0.0f));
    Pen borderPen(Color(std::min(255, borderAlpha), 0, 229, 255), 1.3f);
    g.DrawPath(&borderPen, &path);
}

void drawMicrophoneIcon(Graphics& g, float x, float y, float size, float glow = 0.0f) {
    int alpha = static_cast<int>(220.0f + glow * 35.0f);
    Pen pen(Color(alpha, 0, 235, 255), 1.8f);

    float cx = x + size * 0.5f;
    float cy = y + size * 0.40f;
    float mw = size * 0.34f;
    float mh = size * 0.54f;

    // Mic Capsule
    GraphicsPath micPath;
    addRoundedRect(micPath, RectF(cx - mw * 0.5f, cy - mh * 0.5f, mw, mh), mw * 0.5f);
    g.DrawPath(&pen, &micPath);

    // Stand Arc
    g.DrawArc(&pen, cx - mw * 0.85f, cy - mh * 0.2f, mw * 1.7f, mh * 0.85f, 0.0f, 180.0f);

    // Vertical Stand
    g.DrawLine(&pen, cx, cy + mh * 0.65f, cx, y + size * 0.88f);

    // Base line
    g.DrawLine(&pen, cx - mw * 0.7f, y + size * 0.88f, cx + mw * 0.7f, y + size * 0.88f);
}

void drawHeadphonesIcon(Graphics& g, float x, float y, float size, float glow = 0.0f) {
    int alpha = static_cast<int>(220.0f + glow * 35.0f);
    Pen pen(Color(alpha, 0, 235, 255), 1.8f);
    SolidBrush brush(Color(alpha, 0, 235, 255));

    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float w = size * 0.68f;
    float h = size * 0.68f;

    // Headband
    g.DrawArc(&pen, cx - w * 0.5f, cy - h * 0.55f, w, h * 0.9f, 180.0f, 180.0f);

    // Left Ear Cup
    GraphicsPath leftCup;
    addRoundedRect(leftCup, RectF(cx - w * 0.52f, cy - h * 0.05f, w * 0.25f, h * 0.55f), w * 0.1f);
    g.FillPath(&brush, &leftCup);

    // Right Ear Cup
    GraphicsPath rightCup;
    addRoundedRect(rightCup, RectF(cx + w * 0.27f, cy - h * 0.05f, w * 0.25f, h * 0.55f), w * 0.1f);
    g.FillPath(&brush, &rightCup);
}

void drawWaveIcon(Graphics& g, float x, float y, float size, float animPhase = 0.0f) {
    Pen pen(Color(255, 0, 235, 255), 1.8f);

    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float w = size * 0.72f;

    float p1 = sinf(animPhase) * 3.5f;
    float p2 = cosf(animPhase * 1.3f) * 4.5f;

    PointF points[9] = {
        PointF(cx - w * 0.5f, cy),
        PointF(cx - w * 0.35f, cy),
        PointF(cx - w * 0.25f, cy - size * 0.28f + p1),
        PointF(cx - w * 0.12f, cy + size * 0.32f - p2),
        PointF(cx, cy - size * 0.38f + p2),
        PointF(cx + w * 0.12f, cy + size * 0.32f - p1),
        PointF(cx + w * 0.25f, cy - size * 0.28f + p2),
        PointF(cx + w * 0.35f, cy),
        PointF(cx + w * 0.5f, cy)
    };

    g.DrawLines(&pen, points, 9);
}

void drawCrownIcon(Graphics& g, float x, float y, float size, float glowPhase = 0.0f) {
    GraphicsPath path;
    float cx = x + size * 0.5f;
    float top = y + size * 0.15f;
    float bottom = y + size * 0.85f;
    float left = x + size * 0.15f;
    float right = x + size * 0.85f;

    PointF pts[6] = {
        PointF(left, bottom),
        PointF(left, top + size * 0.2f),
        PointF(cx - size * 0.18f, top + size * 0.42f),
        PointF(cx, top),
        PointF(cx + size * 0.18f, top + size * 0.42f),
        PointF(right, top + size * 0.2f)
    };

    path.AddLines(pts, 6);
    path.AddLine(right, bottom, left, bottom);
    path.CloseFigure();

    int glowA = static_cast<int>(90.0f + 50.0f * sinf(glowPhase));
    Pen glowPen(Color(glowA, 0, 235, 255), 3.0f);
    g.DrawPath(&glowPen, &path);

    SolidBrush brush(Color(255, 0, 235, 255));
    g.FillPath(&brush, &path);
}

// ----------------------------------------------------------------------------
// Animation Update Step (Called every 16ms timer tick)
// ----------------------------------------------------------------------------
void updateAnimations(float dt) {
    g_animTime += dt;

    // 1. Lerp Hover & Interaction States
    auto lerpTo = [dt](float& current, float target, float speed = 14.0f) {
        current += (target - current) * std::clamp(dt * speed, 0.0f, 1.0f);
    };

    lerpTo(g_anim.hoverClose, (g_hoverState == 1) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverMin, (g_hoverState == 2) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverMax, (g_hoverState == 3) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverMicDrop, (g_hoverState == 4) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverSpkDrop, (g_hoverState == 5) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverMaxLoud, (g_hoverState == 6) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverSelfHear, (g_hoverState == 7) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverActionBtn, (g_hoverState == 8) ? 1.0f : 0.0f);
    lerpTo(g_anim.hoverSlider, (g_hoverState == 9 || g_isDraggingSlider) ? 1.0f : 0.0f);

    lerpTo(g_anim.runningGlow, g_running ? 1.0f : 0.0f, 8.0f);
    lerpTo(g_anim.checkMaxLoud, g_maxLoud ? 1.0f : 0.0f, 16.0f);
    lerpTo(g_anim.checkSelfHear, g_selfHear ? 1.0f : 0.0f, 16.0f);
    lerpTo(g_anim.visualGain, static_cast<float>(g_gain), 22.0f);
    lerpTo(g_anim.sliderGlow, (g_isDraggingSlider || g_hoverState == 9) ? 1.0f : 0.0f, 12.0f);

    // 2. Audio Visualizer Levels & Frequency Bands
    float liveIn = 0.0f;
    float liveOut = 0.0f;
    float rawBands[16] = {0.0f};

    if (g_running && g_route) {
        g_route->getAudioLevels(liveIn, liveOut, rawBands, 16);
        float gainScale = std::clamp(std::sqrt(static_cast<float>(g_gain)), 1.0f, 7.0f);
        liveIn = std::clamp(liveIn * 3.0f, 0.0f, 1.0f);
        liveOut = std::clamp(liveOut * gainScale * 2.2f, 0.0f, 1.0f);

        for (int i = 0; i < 16; ++i) {
            rawBands[i] = std::clamp(rawBands[i] * gainScale * 3.0f, 0.02f, 1.0f);
        }
    } else {
        for (int i = 0; i < 16; ++i) {
            float wave = sinf(g_animTime * 2.8f + i * 0.42f) * 0.5f + 0.5f;
            rawBands[i] = 0.08f + wave * 0.14f;
        }
        liveIn = 0.05f;
        liveOut = 0.08f;
    }

    g_viz.inLevel = liveIn;
    g_viz.outLevel = liveOut;
    lerpTo(g_viz.smoothIn, liveIn, 16.0f);
    lerpTo(g_viz.smoothOut, liveOut, 16.0f);

    for (int i = 0; i < 16; ++i) {
        float targetVal = rawBands[i];
        if (targetVal > g_viz.smoothBands[i]) {
            g_viz.smoothBands[i] = targetVal;
        } else {
            g_viz.smoothBands[i] += (targetVal - g_viz.smoothBands[i]) * std::clamp(dt * 12.0f, 0.0f, 1.0f);
        }

        if (g_viz.smoothBands[i] >= g_viz.peakBands[i]) {
            g_viz.peakBands[i] = g_viz.smoothBands[i];
            g_viz.peakDropSpeed[i] = 0.0f;
        } else {
            g_viz.peakDropSpeed[i] += dt * 1.8f;
            g_viz.peakBands[i] -= g_viz.peakDropSpeed[i] * dt;
            if (g_viz.peakBands[i] < g_viz.smoothBands[i]) {
                g_viz.peakBands[i] = g_viz.smoothBands[i];
            }
        }
    }

    // 3. Update Background Particles
    float audioBoost = 1.0f + g_viz.smoothOut * 2.2f;
    for (auto& p : g_particles) {
        p.y -= p.speedY * dt * audioBoost;
        p.x += sinf(p.phase + g_animTime * 1.5f) * p.speedX * dt;

        if (p.y < -15.0f) {
            p.y = static_cast<float>(DEFAULT_HEIGHT + 10);
            p.x = static_cast<float>(rand() % std::max(DEFAULT_WIDTH, 600));
        }
    }
}

// ----------------------------------------------------------------------------
// Main UI Render Pipeline (60 FPS Double Buffered)
// ----------------------------------------------------------------------------
void renderUI(HDC hdc, int width, int height) {
    if (width <= 0 || height <= 0) return;
    computeLayout(width, height);

    // Initialize or resize persistent backbuffer
    if (!g_backBuffer || g_bufWidth != width || g_bufHeight != height) {
        g_backBuffer = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
        g_backGraphics = std::make_unique<Gdiplus::Graphics>(g_backBuffer.get());
        g_bufWidth = width;
        g_bufHeight = height;

        if (g_particles.empty()) {
            initParticles(width, height);
        }
    }

    Graphics& g = *g_backGraphics;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // ------------------------------------------------------------------------
    // 1. Clean Cyberpunk Dark Background Backdrop
    // ------------------------------------------------------------------------
    if (g_bgImage && g_bgImage->GetLastStatus() == Gdiplus::Ok) {
        g.DrawImage(g_bgImage.get(), RectF(0, 0, static_cast<float>(width), static_cast<float>(height)));
        LinearGradientBrush darkOverlay(RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
            Color(140, 4, 10, 18), Color(165, 2, 6, 12), LinearGradientModeVertical);
        g.FillRectangle(&darkOverlay, 0, 0, width, height);
    } else {
        LinearGradientBrush bgGrad(RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
            Color(255, 3, 8, 16), Color(255, 1, 4, 10), LinearGradientModeVertical);
        g.FillRectangle(&bgGrad, 0, 0, width, height);
    }

    // Dynamic Ambient Nebula Glows
    float pulse1 = sinf(g_animTime * 1.8f) * 15.0f;
    float pulse2 = cosf(g_animTime * 2.2f) * 18.0f;

    GraphicsPath glow1;
    glow1.AddEllipse(RectF(-80 + pulse1, 120 + pulse2, 360, 420));
    PathGradientBrush pgb1(&glow1);
    pgb1.SetCenterColor(Color(static_cast<int>(40 + g_viz.smoothOut * 45.0f), 0, 200, 255));
    Color colors1[] = { Color(0, 0, 0, 0) };
    int count1 = 1;
    pgb1.SetSurroundColors(colors1, &count1);
    g.FillPath(&pgb1, &glow1);

    GraphicsPath glow2;
    glow2.AddEllipse(RectF(width - 260 - pulse1, 160 - pulse2, 360, 420));
    PathGradientBrush pgb2(&glow2);
    pgb2.SetCenterColor(Color(static_cast<int>(35 + g_anim.runningGlow * 35.0f), 0, 229, 255));
    Color colors2[] = { Color(0, 0, 0, 0) };
    int count2 = 1;
    pgb2.SetSurroundColors(colors2, &count2);
    g.FillPath(&pgb2, &glow2);

    // ------------------------------------------------------------------------
    // 2. Animated Floating Neon Particles
    // ------------------------------------------------------------------------
    for (const auto& p : g_particles) {
        float alpha = p.baseAlpha + sinf(p.phase + g_animTime * 3.0f) * 20.0f + g_viz.smoothOut * 50.0f;
        alpha = std::clamp(alpha, 10.0f, 255.0f);
        SolidBrush pBrush(Color(static_cast<int>(alpha), 0, 229, 255));
        g.FillEllipse(&pBrush, p.x, p.y, p.size, p.size);
    }

    // ------------------------------------------------------------------------
    // 3. Custom Modern Title Bar matching reference photo
    // ------------------------------------------------------------------------
    {
        // Custom App Icon Badge on Title Bar (SheikzAmp Logo)
        RectF iconBadge(14.0f, 6.0f, 24.0f, 24.0f);
        GraphicsPath badgePath;
        addRoundedRect(badgePath, iconBadge, 6.0f);

        if (g_logoImage && g_logoImage->GetLastStatus() == Gdiplus::Ok) {
            GraphicsState state = g.Save();
            g.SetClip(&badgePath, CombineModeReplace);
            g.DrawImage(g_logoImage.get(), iconBadge);
            g.Restore(state);
            Pen badgeBorder(Color(255, 0, 229, 255), 1.2f);
            g.DrawPath(&badgeBorder, &badgePath);
        } else {
            SolidBrush badgeBg(Color(180, 0, 40, 70));
            g.FillPath(&badgeBg, &badgePath);
            Pen badgeBorder(Color(255, 0, 229, 255), 1.2f);
            g.DrawPath(&badgeBorder, &badgePath);

            // "S" logo inside badge
            Font sFont(L"Arial Black", 10.0f, FontStyleBold, UnitPoint);
            SolidBrush sBrush(Color(255, 0, 229, 255));
            StringFormat sfCenter;
            sfCenter.SetAlignment(StringAlignmentCenter);
            g.DrawString(L"S", -1, &sFont, PointF(iconBadge.X + iconBadge.Width * 0.5f, iconBadge.Y + 1.0f), &sfCenter, &sBrush);
        }

        // App Title
        SolidBrush titleTextBrush(Color(240, 255, 255, 255));
        Font titleFont(L"Segoe UI", 9.2f, FontStyleBold, UnitPoint);
        g.DrawString(L"SheikzAmp", -1, &titleFont, PointF(46, 9), &titleTextBrush);

        // Minimize
        if (g_anim.hoverMin > 0.01f) {
            SolidBrush minBg(Color(static_cast<int>(g_anim.hoverMin * 45.0f), 0, 229, 255));
            g.FillRectangle(&minBg, static_cast<float>(g_rectBtnMin.left), static_cast<float>(g_rectBtnMin.top),
                static_cast<float>(g_rectBtnMin.right - g_rectBtnMin.left), static_cast<float>(g_rectBtnMin.bottom - g_rectBtnMin.top));
        }
        Pen minPen(Color(static_cast<int>(180 + g_anim.hoverMin * 75.0f), 0, 229, 255), 2.0f);
        g.DrawLine(&minPen, static_cast<float>(g_rectBtnMin.left + 8), static_cast<float>(g_rectBtnMin.bottom - 8), static_cast<float>(g_rectBtnMin.right - 8), static_cast<float>(g_rectBtnMin.bottom - 8));

        // Maximize / Restore
        if (g_anim.hoverMax > 0.01f) {
            SolidBrush maxBg(Color(static_cast<int>(g_anim.hoverMax * 45.0f), 0, 229, 255));
            g.FillRectangle(&maxBg, static_cast<float>(g_rectBtnMax.left), static_cast<float>(g_rectBtnMax.top),
                static_cast<float>(g_rectBtnMax.right - g_rectBtnMax.left), static_cast<float>(g_rectBtnMax.bottom - g_rectBtnMax.top));
        }
        Pen maxPen(Color(static_cast<int>(180 + g_anim.hoverMax * 75.0f), 0, 229, 255), 1.8f);
        if (IsZoomed(g_hwnd)) {
            g.DrawRectangle(&maxPen, static_cast<float>(g_rectBtnMax.left + 8), static_cast<float>(g_rectBtnMax.top + 6), 8.0f, 8.0f);
            g.DrawRectangle(&maxPen, static_cast<float>(g_rectBtnMax.left + 5), static_cast<float>(g_rectBtnMax.top + 9), 8.0f, 8.0f);
        } else {
            g.DrawRectangle(&maxPen, static_cast<float>(g_rectBtnMax.left + 6), static_cast<float>(g_rectBtnMax.top + 7), 11.0f, 11.0f);
        }

        // Close
        if (g_anim.hoverClose > 0.01f) {
            SolidBrush closeBg(Color(static_cast<int>(g_anim.hoverClose * 210.0f), 235, 45, 65));
            g.FillRectangle(&closeBg, static_cast<float>(g_rectBtnClose.left), static_cast<float>(g_rectBtnClose.top),
                static_cast<float>(g_rectBtnClose.right - g_rectBtnClose.left), static_cast<float>(g_rectBtnClose.bottom - g_rectBtnClose.top));
        }
        Pen closePen(Color(static_cast<int>(180 + g_anim.hoverClose * 75.0f), 255, 255, 255), 2.0f);
        g.DrawLine(&closePen, static_cast<float>(g_rectBtnClose.left + 8), static_cast<float>(g_rectBtnClose.top + 7), static_cast<float>(g_rectBtnClose.right - 8), static_cast<float>(g_rectBtnClose.bottom - 7));
        g.DrawLine(&closePen, static_cast<float>(g_rectBtnClose.right - 8), static_cast<float>(g_rectBtnClose.top + 7), static_cast<float>(g_rectBtnClose.left + 8), static_cast<float>(g_rectBtnClose.bottom - 7));
    }

    // ------------------------------------------------------------------------
    // 4. Hero Header Section
    // ------------------------------------------------------------------------
    {
        // Left Slogan: Vertical cyan bar + "SOUND \n BEYOND \n LIMITS"
        Pen cyanBar(Color(255, 0, 229, 255), 1.8f);
        g.DrawLine(&cyanBar, 30.0f, 68.0f, 30.0f, 110.0f);
        Font sloganFont(L"Segoe UI", 7.0f, FontStyleBold, UnitPoint);
        SolidBrush sloganBrush(Color(200, 160, 200, 230));
        StringFormat sfLeft;
        g.DrawString(L"SOUND\nBEYOND\nLIMITS", -1, &sloganFont, PointF(38, 68), &sfLeft, &sloganBrush);

        // Right Slogan: Vertical cyan bar + "HEAR \n FEEL \n DOMINATE"
        g.DrawLine(&cyanBar, width - 82.0f, 68.0f, width - 82.0f, 110.0f);
        StringFormat sfRightSlogan;
        g.DrawString(L"HEAR\nFEEL\nDOMINATE", -1, &sloganFont, PointF(width - 74.0f, 68), &sfRightSlogan, &sloganBrush);

        if (g_logoImage && g_logoImage->GetLastStatus() == Gdiplus::Ok) {
            // Glowing cyan aura behind the logo that pulses smoothly with animation & audio
            float auraPulse = sinf(g_animTime * 2.5f) * 6.0f + g_viz.smoothOut * 12.0f;
            float logoSize = 104.0f;
            float logoX = (width - logoSize) * 0.5f;
            float logoY = 36.0f;

            GraphicsPath auraPath;
            auraPath.AddEllipse(RectF(width * 0.5f - 56.0f - auraPulse * 0.5f, 36.0f + 52.0f - 56.0f - auraPulse * 0.5f, 112.0f + auraPulse, 112.0f + auraPulse));
            PathGradientBrush auraBrush(&auraPath);
            int auraAlpha = static_cast<int>(50 + sinf(g_animTime * 2.5f) * 20.0f + g_viz.smoothOut * 60.0f);
            auraBrush.SetCenterColor(Color(std::clamp(auraAlpha, 20, 180), 0, 229, 255));
            Color surroundColors[] = { Color(0, 0, 0, 0) };
            int count = 1;
            auraBrush.SetSurroundColors(surroundColors, &count);
            g.FillPath(&auraBrush, &auraPath);

            // Draw High-Resolution SheikzAmp Logo Badge
            RectF logoRect(logoX, logoY, logoSize, logoSize);
            g.DrawImage(g_logoImage.get(), logoRect);
        } else {
            // Center Glowing Crown Icon fallback
            drawCrownIcon(g, width * 0.5f - 18.0f, 38.0f, 36.0f, g_animTime * 3.0f);

            // Stylized "SHEIKZAMP" Gaming Logo with intense cyan outline glow
            Font brandFont(L"Arial Black", 24.0f, FontStyleBold, UnitPoint);
            StringFormat sfCenter;
            sfCenter.SetAlignment(StringAlignmentCenter);

            float shimmer = sinf(g_animTime * 2.5f) * 0.5f + 0.5f;
            int glowA = static_cast<int>(60 + shimmer * 45.0f + g_viz.smoothOut * 70.0f);
            SolidBrush glowBrush(Color(std::min(255, glowA), 0, 229, 255));

            for (int dx = -3; dx <= 3; ++dx) {
                for (int dy = -3; dy <= 3; ++dy) {
                    if (dx != 0 || dy != 0) {
                        g.DrawString(L"SHEIKZAMP", -1, &brandFont, PointF(width * 0.5f + dx, 76.0f + dy), &sfCenter, &glowBrush);
                    }
                }
            }

            // Inner title: Pristine electric cyan / white gradient
            SolidBrush titleBrush(Color(255, 245, 252, 255));
            g.DrawString(L"SHEIKZAMP", -1, &brandFont, PointF(width * 0.5f, 76.0f), &sfCenter, &titleBrush);

            // Subtitle: "A M P L I F Y   E V E R Y T H I N G"
            Font subFont(L"Segoe UI", 7.8f, FontStyleBold, UnitPoint);
            SolidBrush subBrush(Color(220, 180, 220, 245));
            g.DrawString(L"A M P L I F Y   E V E R Y T H I N G", -1, &subFont, PointF(width * 0.5f, 122.0f), &sfCenter, &subBrush);
        }
    }

    // ------------------------------------------------------------------------
    // 5. Card 1: INPUT MICROPHONE
    // ------------------------------------------------------------------------
    {
        RectF cardRect(static_cast<float>(g_rectCardMic.left), static_cast<float>(g_rectCardMic.top),
            static_cast<float>(g_rectCardMic.right - g_rectCardMic.left), static_cast<float>(g_rectCardMic.bottom - g_rectCardMic.top));
        drawGlassCard(g, cardRect, 12.0f, g_anim.hoverMicDrop);

        // Left Icon Badge
        float iconBoxH = cardRect.Height - 20.0f;
        RectF iconBox(cardRect.X + 10.0f, cardRect.Y + 10.0f, iconBoxH, iconBoxH);
        drawGlassCard(g, iconBox, 8.0f, g_anim.hoverMicDrop, true);
        drawMicrophoneIcon(g, iconBox.X + (iconBoxH - 32.0f) * 0.5f, iconBox.Y + (iconBoxH - 32.0f) * 0.5f, 32.0f, g_viz.smoothIn);

        // Header: "INPUT " (White) + "MICROPHONE" (Cyan)
        Font boldFont(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush cyanBrush(Color(255, 0, 229, 255));

        g.DrawString(L"INPUT ", -1, &boldFont, PointF(cardRect.X + iconBoxH + 20.0f, cardRect.Y + 11.0f), &whiteBrush);
        g.DrawString(L"MICROPHONE", -1, &boldFont, PointF(cardRect.X + iconBoxH + 68.0f, cardRect.Y + 11.0f), &cyanBrush);

        // Right label "PICK \n YOUR \n VOICE" with vertical separator
        Pen sepPen(Color(100, 0, 229, 255), 1.0f);
        float sepX = cardRect.GetRight() - 54.0f;
        g.DrawLine(&sepPen, sepX, cardRect.Y + 12.0f, sepX, cardRect.GetBottom() - 12.0f);

        Font tagFont(L"Segoe UI", 6.2f, FontStyleBold, UnitPoint);
        SolidBrush tagBrush(Color(180, 130, 190, 220));
        StringFormat sfFar;
        sfFar.SetAlignment(StringAlignmentFar);
        g.DrawString(L"PICK\nYOUR\nVOICE", -1, &tagFont, PointF(cardRect.GetRight() - 10.0f, cardRect.Y + 14.0f), &sfFar, &tagBrush);

        // Dropdown box
        RectF dropRect(static_cast<float>(g_rectDropdownMic.left), static_cast<float>(g_rectDropdownMic.top),
            static_cast<float>(g_rectDropdownMic.right - g_rectDropdownMic.left), static_cast<float>(g_rectDropdownMic.bottom - g_rectDropdownMic.top));
        drawGlassCard(g, dropRect, 7.0f, g_anim.hoverMicDrop);

        std::wstring micName = L"No Microphone Found";
        if (g_selectedMic >= 0 && g_selectedMic < static_cast<int>(g_microphones.size())) {
            micName = g_microphones[g_selectedMic].name;
        }

        Font dropFont(L"Segoe UI", 8.8f, FontStyleRegular, UnitPoint);
        SolidBrush textBrush(Color(255, 230, 245, 255));
        StringFormat sfDrop;
        sfDrop.SetTrimming(StringTrimmingEllipsisCharacter);
        sfDrop.SetFormatFlags(StringFormatFlagsNoWrap);

        RectF textLayout(dropRect.X + 12.0f, dropRect.Y + (dropRect.Height - 18.0f) * 0.5f, dropRect.Width - 32.0f, 20.0f);
        g.DrawString(micName.c_str(), -1, &dropFont, textLayout, &sfDrop, &textBrush);

        // Chevron Down Arrow
        Pen chevPen(Color(255, 0, 229, 255), 1.8f);
        float ax = (dropRect.X + dropRect.Width) - 16.0f;
        float ay = dropRect.Y + dropRect.Height * 0.5f;
        g.DrawLine(&chevPen, ax - 4, ay - 2, ax, ay + 2);
        g.DrawLine(&chevPen, ax, ay + 2, ax + 4, ay - 2);
    }

    // ------------------------------------------------------------------------
    // 6. Card 2: SELF-HEAR SPEAKER
    // ------------------------------------------------------------------------
    {
        RectF cardRect(static_cast<float>(g_rectCardSpeaker.left), static_cast<float>(g_rectCardSpeaker.top),
            static_cast<float>(g_rectCardSpeaker.right - g_rectCardSpeaker.left), static_cast<float>(g_rectCardSpeaker.bottom - g_rectCardSpeaker.top));
        drawGlassCard(g, cardRect, 12.0f, g_anim.hoverSpkDrop);

        // Left Icon Badge
        float iconBoxH = cardRect.Height - 20.0f;
        RectF iconBox(cardRect.X + 10.0f, cardRect.Y + 10.0f, iconBoxH, iconBoxH);
        drawGlassCard(g, iconBox, 8.0f, g_anim.hoverSpkDrop, true);
        drawHeadphonesIcon(g, iconBox.X + (iconBoxH - 32.0f) * 0.5f, iconBox.Y + (iconBoxH - 32.0f) * 0.5f, 32.0f, g_viz.smoothOut);

        // Header: "SELF-HEAR " (White) + "SPEAKER" (Cyan)
        Font boldFont(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush cyanBrush(Color(255, 0, 229, 255));

        g.DrawString(L"SELF-HEAR ", -1, &boldFont, PointF(cardRect.X + iconBoxH + 20.0f, cardRect.Y + 11.0f), &whiteBrush);
        g.DrawString(L"SPEAKER", -1, &boldFont, PointF(cardRect.X + iconBoxH + 98.0f, cardRect.Y + 11.0f), &cyanBrush);

        // Right label "HEAR \n YOUR \n POWER" with vertical separator
        Pen sepPen(Color(100, 0, 229, 255), 1.0f);
        float sepX = cardRect.GetRight() - 54.0f;
        g.DrawLine(&sepPen, sepX, cardRect.Y + 12.0f, sepX, cardRect.GetBottom() - 12.0f);

        Font tagFont(L"Segoe UI", 6.2f, FontStyleBold, UnitPoint);
        SolidBrush tagBrush(Color(180, 130, 190, 220));
        StringFormat sfFar;
        sfFar.SetAlignment(StringAlignmentFar);
        g.DrawString(L"HEAR\nYOUR\nPOWER", -1, &tagFont, PointF(cardRect.GetRight() - 10.0f, cardRect.Y + 14.0f), &sfFar, &tagBrush);

        // Dropdown box
        RectF dropRect(static_cast<float>(g_rectDropdownSpeaker.left), static_cast<float>(g_rectDropdownSpeaker.top),
            static_cast<float>(g_rectDropdownSpeaker.right - g_rectDropdownSpeaker.left), static_cast<float>(g_rectDropdownSpeaker.bottom - g_rectDropdownSpeaker.top));
        drawGlassCard(g, dropRect, 7.0f, g_anim.hoverSpkDrop);

        std::wstring spkName = L"No Speaker Found";
        if (g_selectedSpeaker >= 0 && g_selectedSpeaker < static_cast<int>(g_speakers.size())) {
            spkName = g_speakers[g_selectedSpeaker].name;
        }

        Font dropFont(L"Segoe UI", 8.8f, FontStyleRegular, UnitPoint);
        SolidBrush textBrush(Color(255, 230, 245, 255));
        StringFormat sfDrop;
        sfDrop.SetTrimming(StringTrimmingEllipsisCharacter);
        sfDrop.SetFormatFlags(StringFormatFlagsNoWrap);

        RectF textLayout(dropRect.X + 12.0f, dropRect.Y + (dropRect.Height - 18.0f) * 0.5f, dropRect.Width - 32.0f, 20.0f);
        g.DrawString(spkName.c_str(), -1, &dropFont, textLayout, &sfDrop, &textBrush);

        // Chevron Down Arrow
        Pen chevPen(Color(255, 0, 229, 255), 1.8f);
        float ax = (dropRect.X + dropRect.Width) - 16.0f;
        float ay = dropRect.Y + dropRect.Height * 0.5f;
        g.DrawLine(&chevPen, ax - 4, ay - 2, ax, ay + 2);
        g.DrawLine(&chevPen, ax, ay + 2, ax + 4, ay - 2);
    }

    // ------------------------------------------------------------------------
    // 7. Card 3: AMPLIFICATION & CONTROLS matching reference photo
    // ------------------------------------------------------------------------
    {
        RectF cardRect(static_cast<float>(g_rectCardAmp.left), static_cast<float>(g_rectCardAmp.top),
            static_cast<float>(g_rectCardAmp.right - g_rectCardAmp.left), static_cast<float>(g_rectCardAmp.bottom - g_rectCardAmp.top));
        drawGlassCard(g, cardRect, 12.0f, g_anim.hoverAmpCard);

        // Left Icon Badge
        RectF iconBox(cardRect.X + 12.0f, cardRect.Y + 12.0f, 48.0f, 48.0f);
        drawGlassCard(g, iconBox, 8.0f, g_anim.hoverAmpCard, true);
        drawWaveIcon(g, iconBox.X + 6.0f, iconBox.Y + 6.0f, 36.0f, g_animTime * 4.0f);

        // Header: "AMPLIFICATION" (White)
        Font boldFont(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        g.DrawString(L"AMPLIFICATION", -1, &boldFont, PointF(cardRect.X + 70.0f, cardRect.Y + 14.0f), &whiteBrush);

        // Large Dynamic Display: "50x"
        Font bigAmpFont(L"Segoe UI", 18.0f, FontStyleBold, UnitPoint);
        SolidBrush cyanBrush(Color(255, 0, 229, 255));
        StringFormat sfRight;
        sfRight.SetAlignment(StringAlignmentFar);
        std::wstring gainStr = std::to_wstring(static_cast<int>(std::round(g_anim.visualGain))) + L"x";
        g.DrawString(gainStr.c_str(), -1, &bigAmpFont, PointF(cardRect.GetRight() - 16.0f, cardRect.Y + 8.0f), &sfRight, &cyanBrush);

        // Slider Track
        float trackLeft = static_cast<float>(g_rectSlider.left);
        float trackRight = static_cast<float>(g_rectSlider.right);
        float trackY = static_cast<float>(g_rectSlider.top + 6);
        float trackWidth = trackRight - trackLeft;

        float thumbRatio = (g_anim.visualGain - 1.0f) / 99.0f;
        thumbRatio = std::clamp(thumbRatio, 0.0f, 1.0f);
        float thumbX = trackLeft + thumbRatio * trackWidth;

        // Background groove
        Pen groovePen(Color(180, 10, 26, 45), 4.5f);
        groovePen.SetStartCap(LineCapRound);
        groovePen.SetEndCap(LineCapRound);
        g.DrawLine(&groovePen, trackLeft, trackY, trackRight, trackY);

        // Active cyan glow groove
        Pen activeGroovePen(Color(255, 0, 229, 255), 4.5f);
        activeGroovePen.SetStartCap(LineCapRound);
        activeGroovePen.SetEndCap(LineCapRound);
        g.DrawLine(&activeGroovePen, trackLeft, trackY, thumbX, trackY);

        // Ticks beneath track
        Pen tickPen(Color(110, 0, 180, 230), 1.0f);
        for (int i = 0; i <= 24; ++i) {
            float tx = trackLeft + (trackWidth / 24.0f) * i;
            float th = (i % 6 == 0) ? 4.5f : 2.5f;
            g.DrawLine(&tickPen, tx, trackY + 8.0f, tx, trackY + 8.0f + th);
        }

        // 1x and 100x labels
        Font labelFont(L"Segoe UI", 7.5f, FontStyleBold, UnitPoint);
        SolidBrush labelBrush(Color(160, 140, 180, 200));
        g.DrawString(L"1x", -1, &labelFont, PointF(trackLeft, trackY + 14.0f), &labelBrush);
        StringFormat sfEnd;
        sfEnd.SetAlignment(StringAlignmentFar);
        g.DrawString(L"100x", -1, &labelFont, PointF(trackRight, trackY + 14.0f), &sfEnd, &labelBrush);

        // Glowing Thumb Knob (Cyan halo + White center core)
        float glowSize = 22.0f + g_anim.sliderGlow * 6.0f + sinf(g_animTime * 5.0f) * 1.5f;
        GraphicsPath thumbGlow;
        thumbGlow.AddEllipse(thumbX - glowSize * 0.5f, trackY - glowSize * 0.5f, glowSize, glowSize);
        SolidBrush thumbGlowBrush(Color(static_cast<int>(90 + g_anim.sliderGlow * 120.0f), 0, 229, 255));
        g.FillPath(&thumbGlowBrush, &thumbGlow);

        SolidBrush thumbFill(Color(255, 0, 229, 255));
        g.FillEllipse(&thumbFill, thumbX - 8.5f, trackY - 8.5f, 17.0f, 17.0f);

        SolidBrush thumbCore(Color(255, 255, 255, 255));
        g.FillEllipse(&thumbCore, thumbX - 4.0f, trackY - 4.0f, 8.0f, 8.0f);

        // --------------------------------------------------------------------
        // Checkbox 1: MAX LOUD
        // --------------------------------------------------------------------
        {
            RectF boxRect(static_cast<float>(g_rectBoxMaxLoud.left), static_cast<float>(g_rectBoxMaxLoud.top),
                static_cast<float>(g_rectBoxMaxLoud.right - g_rectBoxMaxLoud.left), static_cast<float>(g_rectBoxMaxLoud.bottom - g_rectBoxMaxLoud.top));
            drawGlassCard(g, boxRect, 8.0f, g_anim.hoverMaxLoud);

            // Checkbox glyph
            RectF cbRect(boxRect.X + 10.0f, boxRect.Y + (boxRect.Height - 22.0f) * 0.5f, 22.0f, 22.0f);
            drawGlassCard(g, cbRect, 5.0f, g_anim.checkMaxLoud);

            if (g_anim.checkMaxLoud > 0.05f) {
                SolidBrush cbFill(Color(static_cast<int>(g_anim.checkMaxLoud * 255.0f), 0, 229, 255));
                GraphicsPath cbPath;
                addRoundedRect(cbPath, cbRect, 5.0f);
                g.FillPath(&cbFill, &cbPath);

                // Dark Checkmark
                Pen checkPen(Color(static_cast<int>(g_anim.checkMaxLoud * 255.0f), 0, 18, 32), 2.0f);
                g.DrawLine(&checkPen, cbRect.X + 5, cbRect.Y + 11, cbRect.X + 9, cbRect.Y + 15);
                g.DrawLine(&checkPen, cbRect.X + 9, cbRect.Y + 15, cbRect.X + 16, cbRect.Y + 6);
            }

            // Labels
            Font cbTitleFont(L"Segoe UI", 8.8f, FontStyleBold, UnitPoint);
            Font cbSubFont(L"Segoe UI", 6.8f, FontStyleRegular, UnitPoint);
            SolidBrush cbTitleBrush(Color(255, 255, 255, 255));
            SolidBrush cbSubBrush(Color(180, 0, 200, 240));

            g.DrawString(L"MAX LOUD", -1, &cbTitleFont, PointF(boxRect.X + 38.0f, boxRect.Y + 7.0f), &cbTitleBrush);
            g.DrawString(L"Push to the limit", -1, &cbSubFont, PointF(boxRect.X + 38.0f, boxRect.Y + 23.0f), &cbSubBrush);
        }

        // --------------------------------------------------------------------
        // Checkbox 2: Self Hear
        // --------------------------------------------------------------------
        {
            RectF boxRect(static_cast<float>(g_rectBoxSelfHear.left), static_cast<float>(g_rectBoxSelfHear.top),
                static_cast<float>(g_rectBoxSelfHear.right - g_rectBoxSelfHear.left), static_cast<float>(g_rectBoxSelfHear.bottom - g_rectBoxSelfHear.top));
            drawGlassCard(g, boxRect, 8.0f, g_anim.hoverSelfHear);

            // Checkbox glyph
            RectF cbRect(boxRect.X + 10.0f, boxRect.Y + (boxRect.Height - 22.0f) * 0.5f, 22.0f, 22.0f);
            drawGlassCard(g, cbRect, 5.0f, g_anim.checkSelfHear);

            if (g_anim.checkSelfHear > 0.05f) {
                SolidBrush cbFill(Color(static_cast<int>(g_anim.checkSelfHear * 255.0f), 0, 229, 255));
                GraphicsPath cbPath;
                addRoundedRect(cbPath, cbRect, 5.0f);
                g.FillPath(&cbFill, &cbPath);

                // Dark Checkmark
                Pen checkPen(Color(static_cast<int>(g_anim.checkSelfHear * 255.0f), 0, 18, 32), 2.0f);
                g.DrawLine(&checkPen, cbRect.X + 5, cbRect.Y + 11, cbRect.X + 9, cbRect.Y + 15);
                g.DrawLine(&checkPen, cbRect.X + 9, cbRect.Y + 15, cbRect.X + 16, cbRect.Y + 6);
            }

            // Labels
            Font cbTitleFont(L"Segoe UI", 8.8f, FontStyleBold, UnitPoint);
            Font cbSubFont(L"Segoe UI", 6.8f, FontStyleRegular, UnitPoint);
            SolidBrush cbTitleBrush(Color(255, 255, 255, 255));
            SolidBrush cbSubBrush(Color(180, 0, 200, 240));

            g.DrawString(L"Self Hear", -1, &cbTitleFont, PointF(boxRect.X + 38.0f, boxRect.Y + 7.0f), &cbTitleBrush);
            g.DrawString(L"Listen in real time", -1, &cbSubFont, PointF(boxRect.X + 38.0f, boxRect.Y + 23.0f), &cbSubBrush);
        }

        // --------------------------------------------------------------------
        // Hero Action Button: START / STOP AMPLIFIER matching reference photo
        // --------------------------------------------------------------------
        {
            RectF btnRect(static_cast<float>(g_rectBtnAction.left), static_cast<float>(g_rectBtnAction.top),
                static_cast<float>(g_rectBtnAction.right - g_rectBtnAction.left), static_cast<float>(g_rectBtnAction.bottom - g_rectBtnAction.top));

            GraphicsPath btnPath;
            addRoundedRect(btnPath, btnRect, 10.0f);

            // Dynamic Pulsing Halo
            float btnPulse = sinf(g_animTime * (g_running ? 6.0f : 3.0f)) * 0.5f + 0.5f;
            int haloA = static_cast<int>(70.0f + g_anim.hoverActionBtn * 90.0f + btnPulse * 50.0f);

            int haloR = g_running ? 255 : 0;
            int haloG = g_running ? 60 : 229;
            int haloB = g_running ? 80 : 255;
            Pen glowPen(Color(std::min(255, haloA), haloR, haloG, haloB), 4.5f);
            g.DrawPath(&glowPen, &btnPath);

            // Fill gradient with smooth state transition
            Color cTop = g_running
                ? Color(255, static_cast<int>(180 + g_anim.hoverActionBtn * 40.0f), 20, 40)
                : Color(255, 0, static_cast<int>(150 + g_anim.hoverActionBtn * 79.0f), 220);

            Color cBot = g_running
                ? Color(255, 110, 10, 20)
                : Color(255, 0, 70, 130);

            LinearGradientBrush btnGrad(btnRect, cTop, cBot, LinearGradientModeVertical);
            g.FillPath(&btnGrad, &btnPath);

            Pen borderPen(Color(255, 255, 255, 255), 1.2f);
            g.DrawPath(&borderPen, &btnPath);

            // Icon + Label
            Font btnFont(L"Segoe UI", 11.0f, FontStyleBold, UnitPoint);
            SolidBrush btnTextBrush(Color(255, 255, 255, 255));

            float btnCx = btnRect.X + btnRect.Width * 0.5f;
            float btnCy = btnRect.Y + btnRect.Height * 0.5f;

            if (!g_running) {
                // Play triangle
                PointF playPts[3] = {
                    PointF(btnCx - 76.0f, btnCy - 9.0f),
                    PointF(btnCx - 60.0f, btnCy),
                    PointF(btnCx - 76.0f, btnCy + 9.0f)
                };
                SolidBrush playBrush(Color(255, 0, 229, 255));
                g.FillPolygon(&playBrush, playPts, 3);
                Pen playBorder(Color(255, 255, 255, 255), 1.0f);
                g.DrawPolygon(&playBorder, playPts, 3);

                g.DrawString(L"START AMPLIFIER", -1, &btnFont, PointF(btnCx - 48.0f, btnCy - 9.5f), &btnTextBrush);
            } else {
                // Stop square
                SolidBrush stopBrush(Color(255, 255, 255, 255));
                g.FillRectangle(&stopBrush, btnCx - 74.0f, btnCy - 7.0f, 14.0f, 14.0f);

                g.DrawString(L"STOP AMPLIFIER", -1, &btnFont, PointF(btnCx - 48.0f, btnCy - 9.5f), &btnTextBrush);
            }
        }
    }

    // ------------------------------------------------------------------------
    // 8. Status Bar Capsule with Pulsing Radar Rings
    // ------------------------------------------------------------------------
    {
        RectF statusRect(static_cast<float>(g_rectStatusBar.left), static_cast<float>(g_rectStatusBar.top),
            static_cast<float>(g_rectStatusBar.right - g_rectStatusBar.left), static_cast<float>(g_rectStatusBar.bottom - g_rectStatusBar.top));
        drawGlassCard(g, statusRect, 8.0f, 0.0f);

        // Status LED
        float ledX = statusRect.X + 18.0f;
        float ledY = statusRect.Y + statusRect.Height * 0.5f;

        Color ledColor = g_statusIsError
            ? Color(255, 255, 50, 50)
            : (g_running ? Color(255, 0, 240, 180) : Color(255, 255, 255, 255));

        // Pulsing radar ripple rings when running
        if (g_running) {
            float ringPhase = fmodf(g_animTime * 2.2f, 1.0f);
            float ringRadius = 5.0f + ringPhase * 12.0f;
            int ringAlpha = static_cast<int>((1.0f - ringPhase) * 160.0f);
            Pen radarRing(Color(ringAlpha, 0, 240, 180), 1.5f);
            g.DrawEllipse(&radarRing, ledX - ringRadius, ledY - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f);
        }

        SolidBrush ledBrush(ledColor);
        g.FillEllipse(&ledBrush, ledX - 4.5f, ledY - 4.5f, 9.0f, 9.0f);

        Pen ledRing(Color(140, ledColor.GetR(), ledColor.GetG(), ledColor.GetB()), 1.8f);
        g.DrawEllipse(&ledRing, ledX - 6.5f, ledY - 6.5f, 13.0f, 13.0f);

        // Status Text
        Font statusFont(L"Segoe UI", 8.5f, FontStyleBold, UnitPoint);
        SolidBrush statTitleBrush(Color(255, 255, 255, 255));
        g.DrawString(L"Status:", -1, &statusFont, PointF(ledX + 16.0f, ledY - 7.5f), &statTitleBrush);

        SolidBrush statValBrush(Color(255, 0, 229, 255));
        g.DrawString(g_statusText.c_str(), -1, &statusFont, PointF(ledX + 60.0f, ledY - 7.5f), &statValBrush);

        // Right tagline: "READY TO AMPLIFY ///"
        Font readyFont(L"Segoe UI", 7.0f, FontStyleBold, UnitPoint);
        SolidBrush readyBrush(Color(180, 0, 229, 255));
        StringFormat sfFar;
        sfFar.SetAlignment(StringAlignmentFar);
        const wchar_t* tag = g_running ? L"AMPLIFYING NOW  ///" : L"READY TO AMPLIFY  ///";
        g.DrawString(tag, -1, &readyFont, PointF((statusRect.X + statusRect.Width) - 12.0f, ledY - 6.0f), &sfFar, &readyBrush);
    }

    // ------------------------------------------------------------------------
    // 9. Footer Info matching reference photo
    // ------------------------------------------------------------------------
    {
        Font footerFont(L"Segoe UI", 7.0f, FontStyleRegular, UnitPoint);
        SolidBrush footerBrush(Color(120, 140, 180, 200));

        g.DrawString(L"v 1.0.0", -1, &footerFont, PointF(20.0f, height - 18.0f), &footerBrush);

        StringFormat sfFar;
        sfFar.SetAlignment(StringAlignmentFar);
        g.DrawString(L"MADE BY SHEIKY  ///", -1, &footerFont, PointF(width - 20.0f, height - 18.0f), &sfFar, &footerBrush);
    }

    // ------------------------------------------------------------------------
    // 10. Outer Frame Border Glow
    // ------------------------------------------------------------------------
    if (!IsZoomed(g_hwnd)) {
        GraphicsPath framePath;
        RectF frameRect(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
        addRoundedRect(framePath, frameRect, 10.0f);
        Pen framePen(Color(100, 0, 229, 255), 1.2f);
        g.DrawPath(&framePen, &framePath);
    }

    // Blit backbuffer directly to screen DC in a single ultra-fast operation
    Gdiplus::Graphics screenG(hdc);
    screenG.DrawImage(g_backBuffer.get(), 0, 0);
}

void showDeviceMenu(HWND hwnd, bool isMicrophone, const RECT& buttonRect) {
    HMENU hMenu = CreatePopupMenu();
    const auto& list = isMicrophone ? g_microphones : g_speakers;
    int selected = isMicrophone ? g_selectedMic : g_selectedSpeaker;

    for (size_t i = 0; i < list.size(); ++i) {
        UINT flags = MF_STRING;
        if (static_cast<int>(i) == selected) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(hMenu, flags, 2000 + i, list[i].name.c_str());
    }

    POINT pt = { buttonRect.left, buttonRect.bottom };
    ClientToScreen(hwnd, &pt);

    int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd >= 2000) {
        int index = cmd - 2000;
        if (isMicrophone) {
            g_selectedMic = index;
        } else {
            g_selectedSpeaker = index;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        refreshDevices();
        // Start 60 FPS animation timer (~16ms)
        SetTimer(hwnd, 1, 16, nullptr);
        g_lastFrameTime = std::chrono::steady_clock::now();
        break;
    }

    case WM_TIMER: {
        if (wParam == 1) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - g_lastFrameTime).count();
            g_lastFrameTime = now;
            if (dt > 0.1f) dt = 0.016f; // prevent large time steps when window is moved

            updateAnimations(dt);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = MIN_WIDTH;
        mmi->ptMinTrackSize.y = MIN_HEIGHT;
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetWindowRect(hwnd, &rc);

        constexpr int BORDER = 8;

        bool left = (pt.x >= rc.left && pt.x < rc.left + BORDER);
        bool right = (pt.x >= rc.right - BORDER && pt.x < rc.right);
        bool top = (pt.y >= rc.top && pt.y < rc.top + BORDER);
        bool bottom = (pt.y >= rc.bottom - BORDER && pt.y < rc.bottom);

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        POINT clientPt = pt;
        ScreenToClient(hwnd, &clientPt);

        // Check if cursor is over window control buttons
        if (PtInRect(&g_rectBtnClose, clientPt) ||
            PtInRect(&g_rectBtnMax, clientPt) ||
            PtInRect(&g_rectBtnMin, clientPt)) {
            return HTCLIENT;
        }

        // Draggable Title / Header region
        if (clientPt.y < 140) {
            return HTCAPTION;
        }

        return HTCLIENT;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        computeLayout(w, h);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        renderUI(hdc, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // Prevent flickering

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        POINT pt = { x, y };

        if (g_isDraggingSlider) {
            float trackLeft = static_cast<float>(g_rectSlider.left);
            float trackRight = static_cast<float>(g_rectSlider.right);
            float ratio = (static_cast<float>(x) - trackLeft) / (trackRight - trackLeft);
            ratio = std::clamp(ratio, 0.0f, 1.0f);
            int newGain = static_cast<int>(std::round(1.0f + ratio * 99.0f));
            if (newGain != g_gain) {
                g_gain = newGain;
                syncAmplifierSettings();
                if (g_running) {
                    g_statusText = L"RUNNING (" + std::to_wstring(g_gain) + L"x)";
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        int prevHover = g_hoverState;

        RECT expandedSlider = g_rectSlider;
        expandedSlider.top -= 10;
        expandedSlider.bottom += 10;

        if (PtInRect(&g_rectBtnClose, pt)) g_hoverState = 1;
        else if (PtInRect(&g_rectBtnMin, pt)) g_hoverState = 2;
        else if (PtInRect(&g_rectBtnMax, pt)) g_hoverState = 3;
        else if (PtInRect(&g_rectDropdownMic, pt)) g_hoverState = 4;
        else if (PtInRect(&g_rectDropdownSpeaker, pt)) g_hoverState = 5;
        else if (PtInRect(&g_rectBoxMaxLoud, pt)) g_hoverState = 6;
        else if (PtInRect(&g_rectBoxSelfHear, pt)) g_hoverState = 7;
        else if (PtInRect(&g_rectBtnAction, pt)) g_hoverState = 8;
        else if (PtInRect(&expandedSlider, pt)) g_hoverState = 9;
        else g_hoverState = 0;

        if (prevHover != g_hoverState) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (g_hoverState != 0) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
        } else {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        POINT pt = { x, y };

        if (PtInRect(&g_rectBtnClose, pt)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }

        if (PtInRect(&g_rectBtnMin, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }

        if (PtInRect(&g_rectBtnMax, pt)) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }

        if (PtInRect(&g_rectDropdownMic, pt)) {
            showDeviceMenu(hwnd, true, g_rectDropdownMic);
            return 0;
        }

        if (PtInRect(&g_rectDropdownSpeaker, pt)) {
            showDeviceMenu(hwnd, false, g_rectDropdownSpeaker);
            return 0;
        }

        if (PtInRect(&g_rectBoxMaxLoud, pt)) {
            g_maxLoud = !g_maxLoud;
            syncAmplifierSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (PtInRect(&g_rectBoxSelfHear, pt)) {
            g_selfHear = !g_selfHear;
            if (g_running) {
                // Restart to dynamically rebind Self-Hear endpoint
                stopAmplifier();
                startAmplifier();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (PtInRect(&g_rectBtnAction, pt)) {
            if (g_running) {
                stopAmplifier();
            } else {
                startAmplifier();
            }
            return 0;
        }

        // Slider Seek / Drag
        RECT expandedSlider = g_rectSlider;
        expandedSlider.top -= 12;
        expandedSlider.bottom += 12;
        if (PtInRect(&expandedSlider, pt)) {
            g_isDraggingSlider = true;
            SetCapture(hwnd);

            float trackLeft = static_cast<float>(g_rectSlider.left);
            float trackRight = static_cast<float>(g_rectSlider.right);
            float ratio = (static_cast<float>(x) - trackLeft) / (trackRight - trackLeft);
            ratio = std::clamp(ratio, 0.0f, 1.0f);
            g_gain = static_cast<int>(std::round(1.0f + ratio * 99.0f));
            syncAmplifierSettings();
            if (g_running) {
                g_statusText = L"RUNNING (" + std::to_wstring(g_gain) + L"x)";
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (g_isDraggingSlider) {
            g_isDraggingSlider = false;
            ReleaseCapture();
        }
        break;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        stopAmplifier();
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    loadAssets();

    // Load application icon from embedded resource or file
    HICON hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!hIcon) {
        hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    }
    if (!hIcon) {
        std::wstring icoPath = getExecutableDir() + L"\\assets\\sheikzamp.ico";
        hIcon = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    HICON hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_SHARED);
    if (!hIconSm) {
        hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_SHARED);
    }
    if (!hIconSm) {
        std::wstring icoPath = getExecutableDir() + L"\\assets\\sheikzamp.ico";
        hIconSm = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SheikzAmpModernUI";

    RegisterClassExW(&wc);

    // Center window on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - DEFAULT_WIDTH) / 2;
    int posY = (screenH - DEFAULT_HEIGHT) / 2;

    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"SheikzAmpModernUI",
        L"SheikzAmp",
        WS_THICKFRAME | WS_POPUP | WS_VISIBLE | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        posX,
        posY,
        DEFAULT_WIDTH,
        DEFAULT_HEIGHT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hwnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    if (hIcon) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
    }
    if (hIconSm) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSm));
    }

    // Enable Windows 11 rounded corners & immersive dark mode
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Modern DWM Window Shadow
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_backGraphics.reset();
    g_backBuffer.reset();
    g_bgImage.reset();
    g_logoImage.reset();
    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(msg.wParam);
}