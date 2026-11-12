// Direct WASAPI engine (Windows only).
// - Shared mode: keeps the render queue at exactly one engine period + a small adaptive margin
//   (instead of a full buffer) and writes captured audio to the output immediately.
// - Exclusive mode (per side): hardware-minimum periods, event driven.
// - Optional "share mic": forwards the mic to another output (e.g. VB-Cable) so Discord/games
//   can still use the mic while Sidetone holds it exclusively.
// - Measures the real pipeline latency per sample from WASAPI's QPC timestamps.
#pragma once
#ifdef _WIN32
#include "miniaudio.h"
#include <atomic>
#include <string>
#include <thread>

class Engine;

struct DirectOptions {
    bool hasIn = false, hasOut = false, hasFwd = false;
    ma_device_id in{}, out{}, fwd{};
    bool exclusiveIn = false, exclusiveOut = false;
    uint32_t preferredRate = 48000;
};

class WasapiDirect {
public:
    explicit WasapiDirect(Engine* e) : eng_(e) {}
    ~WasapiDirect() { stop(); }
    bool start(const DirectOptions& o, std::string& err);
    void stop();

    // filled after a successful start
    uint32_t capRate = 0, renRate = 0, capCh = 0, renCh = 0;
    uint32_t capPeriod = 0, renPeriod = 0, capBuffer = 0, renBuffer = 0;
    bool capLowLatency = false, renLowLatency = false, capExclusive = false, renExclusive = false;
    bool fwdActive = false;
    std::string capName, renName, fwdName, note;
    std::atomic<uint32_t> margin{0};  // render safety margin (frames, shared mode)
    std::atomic<uint64_t> renderGlitches{0};
    std::atomic<float> pipelineMs{0};  // measured from WASAPI timestamps (capture -> playback)
    // Exclusive-output watchdog: some USB devices run slower than real time at their minimum
    // exclusive period. Level 0 = minimum period, 1 = 2x, 2 = give up and use shared output.
    // Persists across restarts for the same output device.
    std::atomic<int> exclLevel{0};
    std::wstring exclLevelDevice;
    std::atomic<bool> starved{false};

private:
    void threadMain(DirectOptions o);
    Engine* eng_;
    std::thread thr_;
    void* stopEvent_ = nullptr;
    std::atomic<int> ready_{0};  // 0 pending, 1 ok, -1 failed
    std::string err_;
};
#endif
