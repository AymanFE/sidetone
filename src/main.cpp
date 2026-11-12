// Sidetone - ultra low latency microphone monitoring
// https://github.com/AymanFE/sidetone  (MIT)
#include "engine.h"
#include "settings.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static EngineConfig toConfig(const Settings& s) {
    EngineConfig c;
    c.input = s.input;
    c.output = s.output;
    c.sampleRate = (uint32_t)s.sampleRate;
    c.periodFrames = (uint32_t)s.periodFrames;
    c.exclusiveIn = s.exclusiveIn;
    c.exclusiveOut = s.exclusiveOut;
    c.direct = s.directEngine;
    c.forward = s.shareMicTo;
    return c;
}

static void pushParams(Engine& e, const Settings& s) {
    e.gainDb = s.gainDb;
    e.muted = s.muted;
    e.pan = s.pan;
    e.chanMode = s.chanMode;
    e.hpfOn = s.hpf;
    e.hpfHz = s.hpfHz;
    e.gateOn = s.gate;
    e.gateDb = s.gateDb;
    e.safetyFrames = (uint32_t)std::max(0, s.safetyFrames);
}

static float toDb(float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; }

// ---------------------------------------------------------------- CLI
static void printUsage() {
    std::printf(
        "Sidetone " APP_VERSION " - ultra low latency mic monitoring\n\n"
        "  Sidetone                 open the GUI\n"
        "  Sidetone --list          list audio backends and devices\n"
        "  Sidetone --headless      run without a window (uses saved settings)\n"
        "     --input \"name\"   --output \"name\"   --period N   --rate N\n"
        "  Sidetone --measure       measure real latency (needs output looped back into the mic)\n"
        "     --share-mic \"output name\"  (send the mic to e.g. VB-Cable so Discord can use it)\n"
        "     --gain dB   --exclusive | --exclusive-in | --exclusive-out   --backend NAME\n");
}

static int runCli(int argc, char** argv, Settings& s) {
    bool list = false, headless = false, measure = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--headless") headless = true;
        else if (a == "--measure") measure = headless = true;
        else if (a == "--input") s.input = next();
        else if (a == "--output") s.output = next();
        else if (a == "--period") s.periodFrames = std::atoi(next().c_str());
        else if (a == "--rate") s.sampleRate = std::atoi(next().c_str());
        else if (a == "--gain") s.gainDb = (float)std::atof(next().c_str());
        else if (a == "--backend") s.backend = next();
        else if (a == "--exclusive") s.exclusiveIn = s.exclusiveOut = true;
        else if (a == "--exclusive-in") s.exclusiveIn = true;
        else if (a == "--exclusive-out") s.exclusiveOut = true;
        else if (a == "--no-direct") s.directEngine = false;
        else if (a == "--share-mic") s.shareMicTo = next();
        else { printUsage(); return a == "--help" || a == "-h" ? 0 : 1; }
    }
    auto eng = std::make_unique<Engine>();
    eng->initContext(s.backend);
    if (list) {
        std::printf("Backends:");
        for (auto& b : Engine::backendNames()) std::printf(" %s", b.c_str());
        std::printf("\nActive backend: %s\n\nMicrophones:\n", eng->backendName().c_str());
        for (auto& d : eng->inputs()) std::printf("  %s%s\n", d.name.c_str(), d.isDefault ? "  [default]" : "");
        std::printf("\nOutputs:\n");
        for (auto& d : eng->outputs()) std::printf("  %s%s\n", d.name.c_str(), d.isDefault ? "  [default]" : "");
        if (!headless) return 0;
    }
    pushParams(*eng, s);
    if (!eng->start(toConfig(s))) {
        std::fprintf(stderr, "Error: %s\n", eng->lastError().c_str());
        return 1;
    }
    auto& in = eng->info();
    std::printf("Running [%s]  %s  ->  %s  @ %u Hz\n", in.backend.c_str(), in.inputName.c_str(),
                in.outputName.c_str(), in.sampleRate);
    std::printf("Mic period %u fr%s, output period %u x %u fr%s\n", in.capPeriod, in.capExclusive ? " (exclusive)" : "",
                in.playPeriod, in.playPeriods, in.playExclusive ? " (exclusive)" : "");
    if (!in.note.empty()) std::printf("Note: %s\n", in.note.c_str());
    if (in.playBuffer) std::printf("WASAPI buffers: mic %u fr, out %u fr (rates %u / %u)%s%s\n", in.capBuffer, in.playBuffer,
                                   in.capInternalRate, in.playInternalRate, in.capLowLatency ? " mic-LL" : "", in.playLowLatency ? " out-LL" : "");
    if (!in.forwardName.empty()) std::printf("Sharing mic to: %s\n", in.forwardName.c_str());
    if (measure) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        eng->startLatencyTest();
        while (eng->latencyTestState() == 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (eng->latencyTestState() == 2)
            std::printf("MEASURED mic->ear latency: %.2f ms (min %.2f, max %.2f) | estimate %.2f ms | cushion %u underruns %llu | out margin %u glitches %llu\n",
                        eng->measuredLatencyMs(), eng->measuredMinMs(), eng->measuredMaxMs(), eng->estimatedLatencyMs(),
                        eng->cushionFrames(), (unsigned long long)eng->underruns(), eng->outputMargin(),
                        (unsigned long long)eng->outputGlitches());
        else std::printf("Measurement failed: no loopback detected (put the headphones on the mic or use a loopback cable).\n");
        eng->stop();
        return eng->latencyTestState() == 2 ? 0 : 2;
    }
    std::printf("Press Enter to stop.\n");
    std::fflush(stdout);
    std::atomic<bool> quit{false};
    std::thread([&quit] { std::getchar(); quit = true; }).detach();
    int tick = 0;
    while (!quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (eng->deviceLost()) {
            if (eng->needsQuickRestart()) {
                std::fprintf(stderr, "Exclusive output too slow on this device - restarting with safer settings.\n");
                eng->stop();
                if (!eng->start(toConfig(s))) { std::fprintf(stderr, "Error: %s\n", eng->lastError().c_str()); break; }
                if (!eng->info().note.empty()) std::printf("Note: %s\n", eng->info().note.c_str());
                continue;
            }
            std::fprintf(stderr, "Device disconnected.\n");
            break;
        }
        if (++tick % 20 == 0) {
            std::printf("pipeline %.2f ms | margin %u glitches %llu | est ~%.2f ms | mic block %u | fill %.0f fr | cushion %u | underruns %llu | trimmed %llu fr | mic peak %.0f dB\n",
                        eng->pipelineLatencyMs(), eng->outputMargin(), (unsigned long long)eng->outputGlitches(),
                        eng->estimatedLatencyMs(), eng->micBlockFrames(), eng->avgFillFrames(), eng->cushionFrames(), (unsigned long long)eng->underruns(),
                        (unsigned long long)eng->droppedFrames(), toDb(eng->takeInPeak()));
            std::fflush(stdout);
        }
    }
    eng->stop();
    return 0;
}

// ---------------------------------------------------------------- GUI helpers
static bool deviceCombo(const char* label, std::string& sel, const std::vector<AudioDevice>& devs,
                        const char* emptyLabel = "System default") {
    bool present = sel.empty();
    for (auto& d : devs) if (d.name == sel) present = true;
    std::string preview = sel.empty() ? emptyLabel : (present ? sel : sel + "  (not connected)");
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (ImGui::Selectable(emptyLabel, sel.empty())) { sel.clear(); changed = true; }
        for (size_t i = 0; i < devs.size(); ++i) {
            std::string l = devs[i].name + (devs[i].isDefault ? "  [default]" : "") + "##" + std::to_string(i);
            if (ImGui::Selectable(l.c_str(), sel == devs[i].name)) { sel = devs[i].name; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void meter(const char* label, float db) {
    float frac = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    ImVec4 col = db > -3.0f ? ImVec4(0.95f, 0.30f, 0.30f, 1) : db > -12.0f ? ImVec4(0.95f, 0.80f, 0.25f, 1)
                                                                            : ImVec4(0.30f, 0.85f, 0.50f, 1);
    char buf[48];
    if (db <= -60.0f) std::snprintf(buf, sizeof buf, "%s-inf dB", label);
    else std::snprintf(buf, sizeof buf, "%s%.0f dB", label, db);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac, ImVec2(ImGui::CalcItemWidth(), 0), buf);
    ImGui::PopStyleColor();
}

static void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static void loadFont(float px) {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
#endif
    };
    for (const char* c : candidates) {
        if (FILE* f = std::fopen(c, "rb")) {
            std::fclose(f);
            if (io.Fonts->AddFontFromFileTTF(c, px, nullptr, io.Fonts->GetGlyphRangesDefault())) return;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = px;
    io.Fonts->AddFontDefault(&cfg);
}

static void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 6; s.GrabRounding = 6; s.PopupRounding = 6;
    s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8); s.WindowPadding = ImVec2(16, 14);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.28f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.27f, 0.34f, 1);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.36f, 0.62f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.44f, 0.74f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.32f, 0.56f, 1);
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.19f, 0.24f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.25f, 0.32f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.60f, 0.95f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.70f, 1.0f, 1);
    s.ScaleAllSizes(scale);
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
#ifdef _WIN32
    // GUI-subsystem exe: when run from a terminal, reuse its console (unless output is already redirected)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    bool redirected = hOut && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN;
    if (argc > 1 && !redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::freopen("CONIN$", "r", stdin);
    }
#endif
    Settings s;
    s.load();
    if (argc > 1) return runCli(argc, argv, s);

    auto engPtr = std::make_unique<Engine>();
    Engine& eng = *engPtr;
    eng.initContext(s.backend);

    if (!glfwInit()) return 1;
#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* win = glfwCreateWindow(520, 960, "Sidetone", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetWindowAttrib(win, GLFW_FLOATING, s.alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(win, &xs, &ys);
#if defined(__APPLE__)
    float uiScale = 1.0f;  // macOS handles retina via framebuffer scale
#else
    float uiScale = std::max(1.0f, xs);
#endif
    loadFont(17.0f * uiScale);
    applyStyle(uiScale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    bool wantRunning = s.startOnLaunch;
    std::string status = "Stopped";
    double nextRetry = 0, lastSave = 0;
    std::string savedText = s.serialize();
    float inDb = -120, outDb = -120;
    const int periods[] = {16, 32, 48, 64, 96, 128, 192, 256, 512, 1024};
    const int rates[] = {44100, 48000, 88200, 96000, 192000};
    std::vector<std::string> backends = Engine::backendNames();
    backends.insert(backends.begin(), "Auto");

    while (!glfwWindowShouldClose(win)) {
        glfwWaitEventsTimeout(1.0 / 30.0);
        double now = glfwGetTime();

        // ---- engine supervision
        pushParams(eng, s);
        if (eng.running() && eng.deviceLost()) {
            bool quick = eng.needsQuickRestart();
            eng.stop();
            status = quick ? "Output too slow in exclusive mode - adjusting" : "Device disconnected";
            nextRetry = quick ? 0 : now + 1.5;
        }
        if (wantRunning && eng.running() && toConfig(s) != eng.config()) {
            eng.stop();
            nextRetry = 0;
        }
        if (wantRunning && !eng.running() && now >= nextRetry) {
            eng.refreshDevices();
            if (eng.start(toConfig(s))) status = "Running";
            else {
                status = eng.lastError();
                nextRetry = now + 2.0;
                if (!s.autoReconnect) wantRunning = false;
            }
        }
        if (!wantRunning && eng.running()) { eng.stop(); status = "Stopped"; }

        inDb = std::max(toDb(eng.takeInPeak()), inDb - 1.5f);
        outDb = std::max(toDb(eng.takeOutPeak()), outDb - 1.5f);

        if (now - lastSave > 1.0) {
            std::string t = s.serialize();
            if (t != savedText && s.save()) savedText = t;
            lastSave = now;
        }

        // ---- UI
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float colW = 120.0f * uiScale, helpW = 30.0f * uiScale;
        // label on the left, widget fills the rest (leaving room for a (?) tip)
        auto row = [&](const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(colW);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - helpW);
        };

        // Status line
        bool on = eng.running();
        ImGui::TextColored(on ? ImVec4(0.35f, 0.9f, 0.5f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), on ? "LIVE" : "OFF");
        ImGui::SameLine();
        if (on && eng.pipelineLatencyMs() > 0) ImGui::Text("  %.1f ms software latency (measured)", eng.pipelineLatencyMs());
        else if (on) ImGui::Text("  ~%.1f ms software latency (est.)", eng.estimatedLatencyMs());
        else ImGui::TextDisabled("  %s", status.c_str());

        // Big toggle
        ImGui::PushStyleColor(ImGuiCol_Button, wantRunning ? ImVec4(0.65f, 0.22f, 0.25f, 1) : ImVec4(0.20f, 0.55f, 0.35f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, wantRunning ? ImVec4(0.75f, 0.28f, 0.30f, 1) : ImVec4(0.25f, 0.65f, 0.42f, 1));
        if (ImGui::Button(wantRunning ? "Stop sidetone" : "Start sidetone", ImVec2(-1, 44 * uiScale))) {
            wantRunning = !wantRunning;
            nextRetry = 0;
        }
        ImGui::PopStyleColor(2);
        if (!eng.lastError().empty() && !on) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", eng.lastError().c_str());

        ImGui::Spacing();
        ImGui::SeparatorText("Devices");
        row("Microphone");
        deviceCombo("##mic", s.input, eng.inputs());
        row("Mic channel");
        const char* chans[] = {"Mix all channels", "Left / channel 1", "Right / channel 2"};
        ImGui::Combo("##chan", &s.chanMode, chans, 3);
        help("Some USB mics only put sound on one channel - pick it here.");
        row("Output");
        deviceCombo("##out", s.output, eng.outputs());
        row("Audio API");
        ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - helpW) * 0.5f);
        int bi = 0;
        for (size_t i = 0; i < backends.size(); ++i) if (backends[i] == s.backend) bi = (int)i;
        if (ImGui::BeginCombo("##api", backends[bi].c_str())) {
            for (size_t i = 0; i < backends.size(); ++i)
                if (ImGui::Selectable(backends[i].c_str(), (int)i == bi) && (int)i != bi) {
                    s.backend = backends[i];
                    eng.initContext(s.backend);
                    nextRetry = 0;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x - helpW, 0))) eng.refreshDevices();
        help("Auto picks the best API for your OS.\nLinux: JACK (or PipeWire's JACK) gives the lowest latency.\nRefresh after plugging in a new device.");

        ImGui::SeparatorText("Monitor");
        row("Mic level");
        meter("", inDb);
        row("Out level");
        meter("", outDb);
        row("Volume");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - helpW - 80 * uiScale);
        ImGui::SliderFloat("##vol", &s.gainDb, -40.0f, 24.0f, "%+.1f dB");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s.gainDb = 0;
        ImGui::SameLine();
        ImGui::Checkbox("Mute", &s.muted);
        row("Balance");
        ImGui::SliderFloat("##pan", &s.pan, -1.0f, 1.0f, s.pan == 0 ? "Center" : (s.pan < 0 ? "Left %.2f" : "Right %.2f"));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s.pan = 0;
        help("Right-click a slider to reset it.");

        ImGui::SeparatorText("Latency");
        row("Buffer size");
        char pv[48];
        std::snprintf(pv, sizeof pv, "%d frames  (%.2f ms)", s.periodFrames, s.periodFrames * 1000.0 / s.sampleRate);
        if (ImGui::BeginCombo("##period", pv)) {
            for (int p : periods) {
                char l[48];
                std::snprintf(l, sizeof l, "%d frames  (%.2f ms)", p, p * 1000.0 / s.sampleRate);
                if (ImGui::Selectable(l, p == s.periodFrames)) s.periodFrames = p;
            }
            ImGui::EndCombo();
        }
        help("Smaller = lower latency. If you hear crackles, raise it one step.\nThe driver may round it up to its own minimum (shown below when live).");
        row("Sample rate");
        if (ImGui::BeginCombo("##rate", (std::to_string(s.sampleRate) + " Hz").c_str())) {
            for (int r : rates)
                if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), r == s.sampleRate)) s.sampleRate = r;
            ImGui::EndCombo();
        }
        help("Match your devices' native rate (usually 48000 Hz) to avoid resampling.\nHigher rates = shorter buffers in ms if the driver allows.");
        row("Safety buffer");
        ImGui::SliderInt("##safety", &s.safetyFrames, 0, 512, "%d frames");
        help("Fixed cushion on top of the automatic one. 0 = lowest latency.\nRaise it if you hear clicks (underruns going up).");
        ImGui::Checkbox("Exclusive mic", &s.exclusiveIn);
        help("Lowest latency on Windows (WASAPI) / ALSA: Sidetone takes the mic exclusively.\nWARNING: Discord/games can't use that mic while it's on.\nFalls back to shared mode automatically if refused.");
        ImGui::SameLine(0, 24 * uiScale);
        ImGui::Checkbox("Exclusive output", &s.exclusiveOut);
        help("Lowest latency: Sidetone takes the output device exclusively.\nWARNING: no other app can play sound on that device while it's on.\nBest with a dedicated output (e.g. a headset's separate 'chat' endpoint).");
#ifdef _WIN32
        ImGui::Checkbox("Direct WASAPI engine", &s.directEngine);
        help("Sidetone's own WASAPI engine (recommended). Shared mode: keeps the output queue at one\nWindows engine period + an adaptive ~0.25-1 ms margin instead of a full buffer, and uses\nWindows' low-latency periods when the driver supports them. Exclusive: hardware-minimum periods.\nTurn off only if you get problems.");
        if (s.directEngine) {
            row("Share mic to");
            std::string shareSel = s.shareMicTo;
            if (deviceCombo("##share", shareSel, eng.outputs(), "Off")) s.shareMicTo = shareSel;
            help("With 'Exclusive mic' on, other apps can't open your mic.\nPick a virtual cable here (e.g. 'CABLE Input (VB-Audio Virtual Cable)') and set\nDiscord/games to use 'CABLE Output' as their microphone - they keep hearing you\nwhile Sidetone gets the mic at its lowest latency.");
        }
#endif

        if (on) {
            auto& in = eng.info();
            auto ms = [](uint32_t fr, uint32_t rate) { return rate ? fr * 1000.0 / rate : 0.0; };
            ImGui::TextDisabled("%s | mic block %.2f ms%s%s | out period %.2f ms%s%s", in.backend.c_str(),
                                ms(in.capPeriod, in.capInternalRate), in.capExclusive ? " excl" : "",
                                in.capLowLatency ? " LL" : "", ms(in.playPeriod, in.playInternalRate),
                                in.playExclusive ? " excl" : "", in.playLowLatency ? " LL" : "");
            if (in.direct)
                ImGui::TextDisabled("ring %.0f fr | cushion %u fr | out margin %u fr | underruns %llu | out glitches %llu",
                                    eng.avgFillFrames(), eng.cushionFrames(), eng.outputMargin(),
                                    (unsigned long long)eng.underruns(), (unsigned long long)eng.outputGlitches());
            else
                ImGui::TextDisabled("ring %.0f fr | cushion %u fr | underruns %llu | trimmed %llu fr", eng.avgFillFrames(),
                                    eng.cushionFrames(), (unsigned long long)eng.underruns(),
                                    (unsigned long long)eng.droppedFrames());
            if (!in.forwardName.empty()) ImGui::TextDisabled("sharing mic to %s", in.forwardName.c_str());
            if (!in.note.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", in.note.c_str());
        }

        // Real measurement
        int ts = eng.latencyTestState();
        ImGui::BeginDisabled(!on || ts == 1);
        if (ImGui::Button(ts == 1 ? "Measuring..." : "Measure real latency")) eng.startLatencyTest();
        ImGui::EndDisabled();
        help("Measures the true mic-to-ear delay (including the hardware).\n1. Press an earcup / speaker right against the mic, volume up.\n2. Click and stay quiet ~3 s (you'll hear a few beeps).");
        if (ts != 0) ImGui::SameLine();
        if (ts == 1) ImGui::Text("%d / %d", eng.latencyTestRuns(), Engine::kTestRuns);
        else if (ts == 2) ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1), "%.1f ms  (%.1f-%.1f)", eng.measuredLatencyMs(),
                                             eng.measuredMinMs(), eng.measuredMaxMs());
        else if (ts == 3) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No beep heard - put the earcup on the mic");

        ImGui::SeparatorText("Clean-up (zero added latency)");
        ImGui::Checkbox("Low-cut", &s.hpf);
        ImGui::SameLine(colW);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - helpW);
        ImGui::SliderFloat("##hpf", &s.hpfHz, 20.0f, 300.0f, "%.0f Hz");
        help("Removes rumble, breath pops and desk thumps.");
        ImGui::Checkbox("Noise gate", &s.gate);
        ImGui::SameLine(colW);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - helpW);
        ImGui::SliderFloat("##gate", &s.gateDb, -80.0f, -10.0f, "%.0f dB");
        help("Silences the mic below this level (fans, keyboard hiss).");

        ImGui::SeparatorText("Options");
        ImGui::Checkbox("Start sidetone when the app opens", &s.startOnLaunch);
        ImGui::Checkbox("Auto-reconnect when a device is unplugged / replugged", &s.autoReconnect);
        if (ImGui::Checkbox("Always on top", &s.alwaysOnTop))
            glfwSetWindowAttrib(win, GLFW_FLOATING, s.alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);

        ImGui::Spacing();
        ImGui::TextDisabled("Sidetone " APP_VERSION);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings: %s", Settings::path().c_str());
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.09f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    eng.stop();
    s.save();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
