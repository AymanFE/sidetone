// Direct WASAPI shared-mode engine (Windows only).
// miniaudio keeps the whole WASAPI render buffer full (often ~2 engine periods = 20+ ms).
// This path instead keeps the render queue at exactly one engine period + a small adaptive
// margin, and writes freshly captured audio to the output immediately on the capture event.
#pragma once
#ifdef _WIN32
#include "miniaudio.h"
#include <atomic>
#include <string>
#include <thread>

class Engine;

class WasapiDirect {
public:
    explicit WasapiDirect(Engine* e) : eng_(e) {}
    ~WasapiDirect() { stop(); }
    // Opens both devices (nullptr id = system default) and starts the audio thread.
    bool start(const ma_device_id* in, const ma_device_id* out, std::string& err);
    void stop();

    // filled after a successful start
    uint32_t capRate = 0, renRate = 0, capCh = 0, renCh = 0;
    uint32_t capPeriod = 0, renPeriod = 0, capBuffer = 0, renBuffer = 0;
    bool capLowLatency = false, renLowLatency = false;
    std::string capName, renName;
    std::atomic<uint32_t> margin{0};        // current render safety margin (frames)
    std::atomic<uint64_t> renderGlitches{0};

private:
    void threadMain(const ma_device_id* in, const ma_device_id* out);
    Engine* eng_;
    std::thread thr_;
    void* stopEvent_ = nullptr;
    std::atomic<int> ready_{0};  // 0 pending, 1 ok, -1 failed
    std::string err_;
};
#endif
