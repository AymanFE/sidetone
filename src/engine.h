// Sidetone - ultra low latency microphone monitoring
// Audio engine: capture device -> lock-free ring -> playback device.
#pragma once

#include "miniaudio.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct AudioDevice {
    std::string name;
    ma_device_id id;
    bool isDefault = false;
};

struct EngineConfig {
    std::string input;   // "" = system default
    std::string output;  // "" = system default
    uint32_t sampleRate = 48000;
    uint32_t periodFrames = 64;
    bool exclusiveIn = false;
    bool exclusiveOut = false;
    bool direct = true;  // Windows: use the direct low-latency WASAPI engine for shared mode

    bool operator==(const EngineConfig& o) const {
        return input == o.input && output == o.output && sampleRate == o.sampleRate &&
               periodFrames == o.periodFrames && exclusiveIn == o.exclusiveIn &&
               exclusiveOut == o.exclusiveOut && direct == o.direct;
    }
    bool operator!=(const EngineConfig& o) const { return !(*this == o); }
};

struct EngineInfo {
    std::string inputName, outputName, backend, note;
    uint32_t sampleRate = 0;
    uint32_t capPeriod = 0, capPeriods = 0, capChannels = 0, capInternalRate = 0;
    uint32_t playPeriod = 0, playPeriods = 0, playChannels = 0, playInternalRate = 0;
    bool capExclusive = false, playExclusive = false, direct = false;
    bool capLowLatency = false, playLowLatency = false;
    uint32_t capBuffer = 0, playBuffer = 0;  // actual driver buffer sizes (WASAPI)
    uint32_t playMargin = 0;
};

// Single-producer / single-consumer lock-free mono float ring buffer.
class SpscRing {
public:
    static constexpr uint32_t kSize = 1u << 16;  // power of two
    void reset() { w_.store(0); r_.store(0); }
    uint32_t readAvailable() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed); }
    // returns number of samples that did not fit (dropped)
    uint32_t push(const float* s, uint32_t n) {
        uint32_t w = w_.load(std::memory_order_relaxed);
        uint32_t r = r_.load(std::memory_order_acquire);
        uint32_t freeSpace = kSize - (w - r);
        uint32_t dropped = 0;
        if (n > freeSpace) { dropped = n - freeSpace; n = freeSpace; }
        for (uint32_t i = 0; i < n; ++i) buf_[(w + i) & (kSize - 1)] = s[i];
        w_.store(w + n, std::memory_order_release);
        return dropped;
    }
    uint32_t pop(float* d, uint32_t n) {
        uint32_t r = r_.load(std::memory_order_relaxed);
        uint32_t avail = w_.load(std::memory_order_acquire) - r;
        if (n > avail) n = avail;
        for (uint32_t i = 0; i < n; ++i) d[i] = buf_[(r + i) & (kSize - 1)];
        r_.store(r + n, std::memory_order_release);
        return n;
    }
    void skip(uint32_t n) {
        uint32_t r = r_.load(std::memory_order_relaxed);
        uint32_t avail = w_.load(std::memory_order_acquire) - r;
        if (n > avail) n = avail;
        r_.store(r + n, std::memory_order_release);
    }

private:
    float buf_[kSize] = {};
    alignas(64) std::atomic<uint32_t> w_{0};
    alignas(64) std::atomic<uint32_t> r_{0};
};

class WasapiDirect;

class Engine {
    friend class WasapiDirect;
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static std::vector<std::string> backendNames();  // compiled-in backends
    bool initContext(const std::string& backend);    // "Auto" or a backend name
    std::string backendName() const;
    void refreshDevices();
    const std::vector<AudioDevice>& inputs() const { return inputs_; }
    const std::vector<AudioDevice>& outputs() const { return outputs_; }

    bool start(const EngineConfig& cfg);
    void stop();
    bool running() const { return running_; }
    bool deviceLost() const { return lost_.load(); }
    const EngineInfo& info() const { return info_; }
    const EngineConfig& config() const { return cfg_; }
    const std::string& lastError() const { return error_; }

    // Live parameters (GUI thread writes, audio threads read)
    std::atomic<float> gainDb{0.0f};
    std::atomic<float> pan{0.0f};  // -1 left .. +1 right
    std::atomic<bool> muted{false};
    std::atomic<int> chanMode{0};  // 0 mix, 1 left, 2 right
    std::atomic<bool> hpfOn{true};
    std::atomic<float> hpfHz{80.0f};
    std::atomic<bool> gateOn{false};
    std::atomic<float> gateDb{-50.0f};
    std::atomic<uint32_t> safetyFrames{16};

    // Stats
    float takeInPeak() { return inPeak_.exchange(0.0f); }
    float takeOutPeak() { return outPeak_.exchange(0.0f); }
    float avgFillFrames() const { return fillAvg_.load(); }
    uint64_t underruns() const { return underruns_.load(); }
    uint64_t droppedFrames() const { return dropped_.load(); }
    uint32_t cushionFrames() const { return cushionOut_.load(); }
    uint32_t micBlockFrames() const { return lastCapBlock_.load(); }
    double estimatedLatencyMs() const;
    uint32_t outputMargin() const;
    uint64_t outputGlitches() const;

    // Measured latency test: loop the output back into the mic (headphone earcup on the mic,
    // or a loopback cable) and call startLatencyTest(). state: 0 idle, 1 running, 2 done, 3 failed
    static constexpr int kTestRuns = 7;
    void startLatencyTest() { testState_ = 1; testRuns_ = 0; testReq_ = true; }
    int latencyTestState() const { return testState_.load(); }
    int latencyTestRuns() const { return testRuns_.load(); }
    float measuredLatencyMs() const { return testResult_.load(); }
    float measuredMinMs() const { return testMin_.load(); }
    float measuredMaxMs() const { return testMax_.load(); }

private:
    static void capCb(ma_device* d, void* out, const void* in, ma_uint32 frames);
    static void playCb(ma_device* d, void* out, const void* in, ma_uint32 frames);
    static void notifyCb(const ma_device_notification* n);
    void onCapture(const float* in, uint32_t frames, uint32_t ch);
    void onPlayback(float* out, uint32_t frames, uint32_t ch);
    float testStep(float absIn);
    void pushRing(const float* s, uint32_t n);
    // hooks for the direct WASAPI engine
    void directPrepare(uint32_t capRate, uint32_t renRate);
    void directCapture(const float* in, uint32_t frames, uint32_t ch) { onCapture(in, frames, ch); }
    void directRender(float* out, uint32_t frames, uint32_t ch) { onPlayback(out, frames, ch); }
    uint32_t directAvailable() const { return ring_.readAvailable(); }
    void directLost() { lost_ = true; }
    WasapiDirect* direct_ = nullptr;
    uint32_t capSr_ = 48000;
    bool rsOn_ = false;
    ma_linear_resampler rs_{};
    float rsBuf_[1024 * 5 + 64] = {};
    void playChunk(float* out, uint32_t frames, uint32_t ch);
    const ma_device_id* findId(const std::vector<AudioDevice>& list, const std::string& name, bool& found) const;

    ma_context ctx_{};
    bool ctxInit_ = false;
    ma_device cap_{}, play_{};
    bool capInit_ = false, playInit_ = false, running_ = false;
    std::atomic<bool> stopping_{false}, lost_{false};
    std::vector<AudioDevice> inputs_, outputs_;
    EngineConfig cfg_;
    EngineInfo info_;
    std::string error_;
    SpscRing ring_;
    uint32_t sr_ = 48000;

    // capture DSP state (capture thread only)
    float capBuf_[1024] = {};
    float hb0_ = 1, hb1_ = 0, hb2_ = 0, ha1_ = 0, ha2_ = 0, hz1_ = 0, hz2_ = 0, hpfCur_ = -1;
    float gateEnv_ = 0, gateGain_ = 1;
    uint32_t gateHold_ = 0;
    float gainSm_ = 0;
    uint64_t capFrame_ = 0, tFire_ = 0;
    int tState_ = 0, tRuns_ = 0, tFails_ = 0;
    uint32_t tCnt_ = 0;
    float tNoise_ = 0, tThr_ = 0, tRes_[16] = {};
    std::atomic<bool> testReq_{false};
    std::atomic<int> testState_{0}, testRuns_{0};
    std::atomic<float> testResult_{0}, testMin_{0}, testMax_{0};

    // playback state (playback thread only)
    float tmp_[8192 + 1024] = {};
    float mono_[8192] = {};
    uint32_t winMin_ = UINT32_MAX, winFrames_ = 0, dropBudget_ = 0, warm_ = 0;
    float last_ = 0, fillSm_ = 0;
    uint32_t cushion_ = 0, stableFrames_ = 0, lastCapCount_ = 0, sinceCap_ = 0;

    // shared stats
    std::atomic<float> inPeak_{0}, outPeak_{0}, fillAvg_{0};
    std::atomic<uint32_t> minFill_{0};
    std::atomic<uint64_t> underruns_{0}, dropped_{0};
    std::atomic<uint32_t> cushionOut_{0}, lastCapBlock_{0}, capCount_{0};
};
