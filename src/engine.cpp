#include "engine.h"
#include "wasapi_direct.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

static constexpr float kPi = 3.14159265358979f;

static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

Engine::Engine() {}

Engine::~Engine() {
    stop();
#ifdef _WIN32
    delete direct_;
#endif
    if (ctxInit_) ma_context_uninit(&ctx_);
}

std::vector<std::string> Engine::backendNames() {
    std::vector<std::string> out;
    ma_backend list[MA_BACKEND_COUNT];
    size_t n = 0;
    if (ma_get_enabled_backends(list, MA_BACKEND_COUNT, &n) != MA_SUCCESS) return out;
    for (size_t i = 0; i < n; ++i) {
        if (list[i] == ma_backend_null || list[i] == ma_backend_custom) continue;
        out.push_back(ma_get_backend_name(list[i]));
    }
    return out;
}

bool Engine::initContext(const std::string& backend) {
    stop();
    if (ctxInit_) { ma_context_uninit(&ctx_); ctxInit_ = false; }
    error_.clear();

    ma_context_config cc = ma_context_config_init();
    cc.threadPriority = ma_thread_priority_realtime;

    ma_result r = MA_ERROR;
    bool tried = false;
    if (!backend.empty() && backend != "Auto") {
        ma_backend list[MA_BACKEND_COUNT];
        size_t n = 0;
        ma_get_enabled_backends(list, MA_BACKEND_COUNT, &n);
        for (size_t i = 0; i < n; ++i) {
            if (backend == ma_get_backend_name(list[i])) {
                ma_backend b = list[i];
                r = ma_context_init(&b, 1, &cc, &ctx_);
                tried = true;
                break;
            }
        }
        if (r != MA_SUCCESS) error_ = "Backend '" + backend + "' unavailable, using Auto";
    }
    if (!tried || r != MA_SUCCESS) r = ma_context_init(NULL, 0, &cc, &ctx_);
    if (r != MA_SUCCESS) {
        error_ = std::string("Audio init failed: ") + ma_result_description(r);
        return false;
    }
    ctxInit_ = true;
    refreshDevices();
    return true;
}

std::string Engine::backendName() const {
    return ctxInit_ ? ma_get_backend_name(ctx_.backend) : "none";
}

void Engine::refreshDevices() {
    if (!ctxInit_) return;
    ma_device_info *pPlay = nullptr, *pCap = nullptr;
    ma_uint32 nPlay = 0, nCap = 0;
    if (ma_context_get_devices(&ctx_, &pPlay, &nPlay, &pCap, &nCap) != MA_SUCCESS) return;
    inputs_.clear();
    outputs_.clear();
    for (ma_uint32 i = 0; i < nCap; ++i) inputs_.push_back({pCap[i].name, pCap[i].id, pCap[i].isDefault != 0});
    for (ma_uint32 i = 0; i < nPlay; ++i) outputs_.push_back({pPlay[i].name, pPlay[i].id, pPlay[i].isDefault != 0});
}

const ma_device_id* Engine::findId(const std::vector<AudioDevice>& list, const std::string& name, bool& found) const {
    found = true;
    if (name.empty()) return nullptr;
    for (auto& d : list)
        if (d.name == name) return &d.id;
    found = false;
    return nullptr;
}

static ma_device_config baseConfig(ma_device_type type, const EngineConfig& cfg, void* user) {
    ma_device_config dc = ma_device_config_init(type);
    dc.sampleRate = cfg.sampleRate;
    dc.periodSizeInFrames = cfg.periodFrames;
    dc.periods = 2;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noFixedSizedCallback = MA_TRUE;  // no extra intermediary buffering
    dc.noClip = MA_FALSE;
    dc.pUserData = user;
    dc.resampling.algorithm = ma_resample_algorithm_linear;
    dc.wasapi.usage = ma_wasapi_usage_pro_audio;  // MMCSS "Pro Audio" thread
    dc.wasapi.noAutoConvertSRC = MA_TRUE;
    dc.wasapi.noDefaultQualitySRC = MA_TRUE;
    dc.wasapi.noAutoStreamRouting = MA_FALSE;
    dc.coreaudio.allowNominalSampleRateChange = MA_FALSE;
    dc.pulse.pStreamNamePlayback = "Sidetone";
    dc.pulse.pStreamNameCapture = "Sidetone";
    return dc;
}

bool Engine::start(const EngineConfig& cfg) {
    stop();
    cfg_ = cfg;
    error_.clear();
    info_ = EngineInfo{};
    lost_ = false;
    if (!ctxInit_ && !initContext("Auto")) return false;

    bool inFound, outFound;
    const ma_device_id* inId = findId(inputs_, cfg.input, inFound);
    const ma_device_id* outId = findId(outputs_, cfg.output, outFound);
    if (!inFound) { error_ = "Microphone not found: " + cfg.input; return false; }
    if (!outFound) { error_ = "Output not found: " + cfg.output; return false; }

    // reset state
    ring_.reset();
    hpfCur_ = -1; hz1_ = hz2_ = 0; gateEnv_ = 0; gateGain_ = 1; gateHold_ = 0;
    gainSm_ = muted ? 0.0f : dbToLin(gainDb);
    winMin_ = UINT32_MAX; winFrames_ = 0; dropBudget_ = 0; warm_ = 0; last_ = 0; fillSm_ = 0;
    tState_ = 0; capFrame_ = 0;
    if (testState_ == 1) testState_ = 0;
    cushion_ = 0; stableFrames_ = 0; cushionOut_ = 0; lastCapCount_ = 0; sinceCap_ = 0; capCount_ = 0;
    underruns_ = 0; dropped_ = 0; fillAvg_ = 0; minFill_ = 0;
    sr_ = capSr_ = cfg.sampleRate;

#ifdef _WIN32
    if (cfg.direct && ctx_.backend == ma_backend_wasapi) {
        if (!direct_) direct_ = new WasapiDirect(this);
        stopping_ = false;
        std::string derr;
        DirectOptions o;
        if (inId) { o.in = *inId; o.hasIn = true; }
        if (outId) { o.out = *outId; o.hasOut = true; }
        if (!cfg.forward.empty()) {
            bool f = false;
            const ma_device_id* fid = findId(outputs_, cfg.forward, f);
            if (fid && f) { o.fwd = *fid; o.hasFwd = true; }
            else info_.note += "Share-mic device not found. ";
        }
        if (!cfg.appsFrom.empty()) {
            bool f = false;
            const ma_device_id* lid = findId(outputs_, cfg.appsFrom, f);
            if (lid && f) { o.loop = *lid; o.hasLoop = true; }
            else info_.note += "App-audio device not found. ";
        }
        o.exclusiveIn = cfg.exclusiveIn;
        o.exclusiveOut = cfg.exclusiveOut;
        o.preferredRate = cfg.sampleRate;
        if (direct_->start(o, derr)) {
            auto* d = direct_;
            info_.backend = "WASAPI direct";
            info_.direct = true;
            info_.note += d->note;
            info_.capExclusive = d->capExclusive;
            info_.playExclusive = d->renExclusive;
            if (d->fwdActive) info_.forwardName = d->fwdName;
            if (d->loopActive) info_.appsName = d->loopName;
            info_.inputName = d->capName;
            info_.outputName = d->renName;
            info_.sampleRate = d->renRate;
            info_.capPeriod = d->capPeriod; info_.capPeriods = 1; info_.capChannels = d->capCh;
            info_.capInternalRate = d->capRate; info_.capBuffer = d->capBuffer; info_.capLowLatency = d->capLowLatency;
            info_.playPeriod = d->renPeriod; info_.playPeriods = 1; info_.playChannels = d->renCh;
            info_.playInternalRate = d->renRate; info_.playBuffer = d->renBuffer; info_.playLowLatency = d->renLowLatency;
            running_ = true;
            return true;
        }
        info_.note += "Direct engine unavailable (" + derr + "), using standard. ";
    }
#endif

    // Playback device
    ma_device_config pc = baseConfig(ma_device_type_playback, cfg, this);
    pc.playback.pDeviceID = outId;
    pc.playback.format = ma_format_f32;
    pc.playback.channels = 0;  // native channel count
    pc.playback.shareMode = cfg.exclusiveOut ? ma_share_mode_exclusive : ma_share_mode_shared;
    pc.dataCallback = &Engine::playCb;
    pc.notificationCallback = &Engine::notifyCb;
    ma_result r = ma_device_init(&ctx_, &pc, &play_);
    if (r != MA_SUCCESS && cfg.exclusiveOut) {
        pc.playback.shareMode = ma_share_mode_shared;
        r = ma_device_init(&ctx_, &pc, &play_);
        if (r == MA_SUCCESS) info_.note += "Output: exclusive refused, using shared. ";
    }
    if (r != MA_SUCCESS) { error_ = std::string("Output open failed: ") + ma_result_description(r); return false; }
    playInit_ = true;

    // Capture device
    ma_device_config cc = baseConfig(ma_device_type_capture, cfg, this);
    cc.capture.pDeviceID = inId;
    cc.capture.format = ma_format_f32;
    cc.capture.channels = 0;
    cc.capture.shareMode = cfg.exclusiveIn ? ma_share_mode_exclusive : ma_share_mode_shared;
    cc.dataCallback = &Engine::capCb;
    cc.notificationCallback = &Engine::notifyCb;
    r = ma_device_init(&ctx_, &cc, &cap_);
    if (r != MA_SUCCESS && cfg.exclusiveIn) {
        cc.capture.shareMode = ma_share_mode_shared;
        r = ma_device_init(&ctx_, &cc, &cap_);
        if (r == MA_SUCCESS) info_.note += "Mic: exclusive refused, using shared. ";
    }
    if (r != MA_SUCCESS) {
        error_ = std::string("Microphone open failed: ") + ma_result_description(r);
        stop();
        return false;
    }
    capInit_ = true;

    info_.backend = backendName();
    info_.inputName = cap_.capture.name;
    info_.outputName = play_.playback.name;
    info_.sampleRate = cfg.sampleRate;
    info_.capPeriod = cap_.capture.internalPeriodSizeInFrames;
    info_.capPeriods = cap_.capture.internalPeriods;
    info_.capChannels = cap_.capture.channels;
    info_.capInternalRate = cap_.capture.internalSampleRate;
    info_.capExclusive = cap_.capture.shareMode == ma_share_mode_exclusive;
    info_.playPeriod = play_.playback.internalPeriodSizeInFrames;
    info_.playPeriods = play_.playback.internalPeriods;
    info_.playChannels = play_.playback.channels;
    info_.playInternalRate = play_.playback.internalSampleRate;
    info_.playExclusive = play_.playback.shareMode == ma_share_mode_exclusive;
#if defined(MA_HAS_WASAPI) || defined(_WIN32)
    if (ctx_.backend == ma_backend_wasapi) {
        info_.capBuffer = cap_.wasapi.actualBufferSizeInFramesCapture;
        info_.playBuffer = play_.wasapi.actualBufferSizeInFramesPlayback;
    }
#endif

    stopping_ = false;
    // Start output first so the mic never builds up a backlog.
    if ((r = ma_device_start(&play_)) != MA_SUCCESS || (r = ma_device_start(&cap_)) != MA_SUCCESS) {
        error_ = std::string("Start failed: ") + ma_result_description(r);
        stop();
        return false;
    }
    running_ = true;
    return true;
}

void Engine::stop() {
    stopping_ = true;
#ifdef _WIN32
    if (direct_) direct_->stop();
#endif
    if (rsOn_) { ma_linear_resampler_uninit(&rs_, nullptr); rsOn_ = false; }
    if (capInit_) { ma_device_uninit(&cap_); capInit_ = false; }
    if (playInit_) { ma_device_uninit(&play_); playInit_ = false; }
    running_ = false;
}

void Engine::directPrepare(uint32_t capRate, uint32_t renRate) {
    capSr_ = capRate;
    sr_ = renRate;
    hpfCur_ = -1;
    if (rsOn_) { ma_linear_resampler_uninit(&rs_, nullptr); rsOn_ = false; }
    if (capRate != renRate) {
        ma_linear_resampler_config rc = ma_linear_resampler_config_init(ma_format_f32, 1, capRate, renRate);
        rsOn_ = ma_linear_resampler_init(&rc, nullptr, &rs_) == MA_SUCCESS;
    }
}

void Engine::pushRing(const float* s, uint32_t n) {
    if (!rsOn_) { dropped_ += ring_.push(s, n); return; }
    while (n > 0) {
        ma_uint64 inF = std::min<uint32_t>(n, 1024), outF = sizeof(rsBuf_) / sizeof(float);
        ma_linear_resampler_process_pcm_frames(&rs_, s, &inF, rsBuf_, &outF);
        dropped_ += ring_.push(rsBuf_, (uint32_t)outF);
        if (inF == 0) break;
        s += inF;
        n -= (uint32_t)inF;
    }
}

float Engine::takeAppsPeak() {
#ifdef _WIN32
    if (direct_) return direct_->appsPeak.exchange(0.0f);
#endif
    return 0.0f;
}

bool Engine::needsQuickRestart() const {
#ifdef _WIN32
    return direct_ && direct_->starved.load();
#else
    return false;
#endif
}

float Engine::pipelineLatencyMs() const {
#ifdef _WIN32
    if (running_ && info_.direct && direct_) return direct_->pipelineMs.load();
#endif
    return 0.0f;
}

uint32_t Engine::outputMargin() const {
#ifdef _WIN32
    if (info_.direct && direct_) return direct_->margin.load();
#endif
    return 0;
}

uint64_t Engine::outputGlitches() const {
#ifdef _WIN32
    if (info_.direct && direct_) return direct_->renderGlitches.load();
#endif
    return 0;
}

double Engine::estimatedLatencyMs() const {
    if (!running_ || sr_ == 0) return 0.0;
    if (info_.direct) {
        double capMs = capSr_ ? info_.capPeriod * 1000.0 / capSr_ : 0.0;
        return capMs + (minFill_.load() + info_.playPeriod + outputMargin()) * 1000.0 / sr_;
    }
    // capture period (wait for a block) + ring fill + output buffer queue
    auto toLocal = [&](uint32_t frames, uint32_t rate) { return rate ? (double)frames * sr_ / rate : (double)frames; };
    // miniaudio keeps the whole output buffer full
    uint32_t outQ = info_.playBuffer ? info_.playBuffer : info_.playPeriod * std::max(1u, info_.playPeriods);
    double frames = toLocal(info_.capPeriod, info_.capInternalRate) + minFill_.load() +
                    toLocal(outQ, info_.playInternalRate);
    return frames * 1000.0 / sr_;
}

void Engine::notifyCb(const ma_device_notification* n) {
    Engine* e = static_cast<Engine*>(n->pDevice->pUserData);
    if (n->type == ma_device_notification_type_stopped && !e->stopping_) e->lost_ = true;
}

void Engine::capCb(ma_device* d, void*, const void* in, ma_uint32 frames) {
    static_cast<Engine*>(d->pUserData)->onCapture(static_cast<const float*>(in), frames, d->capture.channels);
}

void Engine::playCb(ma_device* d, void* out, const void*, ma_uint32 frames) {
    static_cast<Engine*>(d->pUserData)->onPlayback(static_cast<float*>(out), frames, d->playback.channels);
}

void Engine::onCapture(const float* in, uint32_t frames, uint32_t ch) {
    if (ch == 0) return;
    if (testReq_.exchange(false)) {
        tState_ = 1; tCnt_ = 0; tNoise_ = 0; tRuns_ = 0; tFails_ = 0;
        testState_ = 1; testRuns_ = 0;
    }
    lastCapBlock_.store(frames, std::memory_order_relaxed);
    capCount_.fetch_add(1, std::memory_order_relaxed);
    const float fs = (float)capSr_;
    const float target = muted ? 0.0f : dbToLin(gainDb);
    const int mode = chanMode;
    const bool hpf = hpfOn;
    const bool gate = gateOn;
    const float gateOpen = dbToLin(gateDb), gateClose = gateOpen * 0.5f;
    const float gainCoef = 1.0f - std::exp(-1.0f / (0.005f * fs));
    const float envRel = std::exp(-1.0f / (0.05f * fs));
    const float gAtk = 1.0f - std::exp(-1.0f / (0.001f * fs));
    const float gRel = 1.0f - std::exp(-1.0f / (0.08f * fs));
    const uint32_t holdFrames = (uint32_t)(0.05f * fs);

    if (hpf) {
        float f = std::clamp(hpfHz.load(), 20.0f, 400.0f);
        if (f != hpfCur_) {  // RBJ high-pass biquad, Q = 0.707
            float w0 = 2.0f * kPi * f / fs, c = std::cos(w0), al = std::sin(w0) / (2.0f * 0.7071f);
            float a0 = 1.0f + al;
            hb0_ = (1.0f + c) / 2.0f / a0; hb1_ = -(1.0f + c) / a0; hb2_ = hb0_;
            ha1_ = -2.0f * c / a0; ha2_ = (1.0f - al) / a0;
            hpfCur_ = f;
        }
    }

    float inPk = 0.0f;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = std::min<uint32_t>(frames - done, 1024);
        static const float kZero[8] = {};
        const float* src = in ? in + (size_t)done * ch : nullptr;
        for (uint32_t i = 0; i < n; ++i) {
            const float* fr = src ? src + (size_t)i * ch : kZero;
            float s;
            if (ch > 8 && !src) s = 0.0f;
            else if (mode == 1 || ch == 1) s = fr[0];
            else if (mode == 2) s = fr[1];
            else { s = 0.0f; for (uint32_t c = 0; c < ch; ++c) s += fr[c]; s /= (float)ch; }
            float a = std::fabs(s);
            if (a > inPk) inPk = a;
            ++capFrame_;
            if (tState_ != 0) {  // latency test owns the signal path
                capBuf_[i] = testStep(a);
                continue;
            }
            if (hpf) {
                float y = hb0_ * s + hz1_;
                hz1_ = hb1_ * s - ha1_ * y + hz2_;
                hz2_ = hb2_ * s - ha2_ * y;
                s = y;
            }
            if (gate) {
                gateEnv_ = a > gateEnv_ ? a : gateEnv_ * envRel;
                float t;
                if (gateEnv_ > gateOpen) { t = 1.0f; gateHold_ = holdFrames; }
                else if (gateHold_ > 0) { --gateHold_; t = 1.0f; }
                else t = (gateEnv_ < gateClose) ? 0.0f : (gateGain_ > 0.5f ? 1.0f : 0.0f);
                gateGain_ += (t - gateGain_) * (t > gateGain_ ? gAtk : gRel);
                s *= gateGain_;
            }
            gainSm_ += (target - gainSm_) * gainCoef;
            capBuf_[i] = s * gainSm_;
        }
        pushRing(capBuf_, n);
        done += n;
    }
    float prev = inPeak_.load(std::memory_order_relaxed);
    if (inPk > prev) inPeak_.store(inPk, std::memory_order_relaxed);
}

// Real round-trip measurement. A short tone burst is injected into the sidetone path in place
// of the mic signal at capture frame N; it travels ring -> output -> (cable/air) -> mic and is
// detected at capture frame M. (M - N) / rate is exactly the mic-to-ear sidetone latency,
// measured on a single clock.
float Engine::testStep(float a) {
    const uint32_t fs = capSr_;
    const uint32_t burstLen = fs * 3 / 1000, ramp = std::max<uint32_t>(1, fs / 4000);
    float out = 0.0f;
    switch (tState_) {
    case 1:  // measure background noise
        tNoise_ = std::max(tNoise_, a);
        if (++tCnt_ >= fs * 3 / 10) {
            tThr_ = std::max(0.02f, tNoise_ * 4.0f);
            if (tThr_ > 0.5f) { tState_ = 0; testState_ = 3; break; }  // too noisy
            tState_ = 2; tCnt_ = 0; tFire_ = capFrame_;
        }
        break;
    case 2:  // fire + listen
        if (tCnt_ < burstLen) {
            float env = std::min({1.0f, (float)tCnt_ / ramp, (float)(burstLen - tCnt_) / ramp});
            out = 0.7f * env * std::sin(2.0f * kPi * 2000.0f * (float)tCnt_ / (float)fs);
        }
        if (a > tThr_ && tCnt_ > 0) {
            tRes_[tRuns_++] = (float)((double)(capFrame_ - tFire_) * 1000.0 / fs);
            testRuns_ = tRuns_;
            tState_ = 3; tCnt_ = 0;
        } else if (++tCnt_ > fs / 2) {  // nothing came back within 500 ms
            tFails_++;
            tState_ = 3; tCnt_ = 0;
        }
        break;
    case 3:  // let it die down
        if (++tCnt_ >= fs / 4) {
            if (tRuns_ >= kTestRuns || tFails_ >= 3) {
                if (tRuns_ >= 3) {
                    std::sort(tRes_, tRes_ + tRuns_);
                    testResult_ = tRes_[tRuns_ / 2];
                    testMin_ = tRes_[0];
                    testMax_ = tRes_[tRuns_ - 1];
                    testState_ = 2;
                } else testState_ = 3;
                tState_ = 0;
            } else { tState_ = 2; tCnt_ = 0; tFire_ = capFrame_; }
        }
        break;
    }
    return out;
}

void Engine::onPlayback(float* out, uint32_t frames, uint32_t ch) {
    if (!out || ch == 0) return;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = std::min<uint32_t>(frames - done, 4096);
        playChunk(out + (size_t)done * ch, n, ch);
        done += n;
    }
}

void Engine::playChunk(float* out, uint32_t frames, uint32_t ch) {
    if (resetReq_.exchange(false)) {
        cushion_ = 0; stableFrames_ = 0; winMin_ = UINT32_MAX; winFrames_ = 0; dropBudget_ = 0;
        warm_ = 0;  // ignore underruns for a moment while the new stream settles
        underruns_ = 0;
    }
    uint32_t avail = ring_.readAvailable();
    // average data left waiting in the ring after this read = added latency
    fillSm_ = fillSm_ * 0.99f + (float)(avail > frames ? avail - frames : 0) * 0.01f;
    fillAvg_.store(fillSm_, std::memory_order_relaxed);
    if (warm_ < sr_) warm_ += frames;

    // Is the mic actually delivering? (unplugged / muted endpoints deliver nothing)
    uint32_t cc = capCount_.load(std::memory_order_relaxed);
    if (cc != lastCapCount_) { lastCapCount_ = cc; sinceCap_ = 0; } else sinceCap_ += frames;
    const bool inputAlive = sinceCap_ < sr_ / 10;

    // Adaptive jitter cushion: grows on underruns, shrinks back while stable.
    if (avail < frames && warm_ >= sr_ / 4 && inputAlive) {
        // grow gently: at most ~2 ms per underrun, so one stray hiccup can't add a big permanent delay
        uint32_t maxCushion = 2 * std::max(lastCapBlock_.load(std::memory_order_relaxed), frames) + 64;
        uint32_t step = std::min<uint32_t>(frames - avail, sr_ / 500) + 8;
        cushion_ = std::min<uint32_t>(cushion_ + step, maxCushion);
        stableFrames_ = 0;
        underruns_++;
    } else {
        stableFrames_ += frames;
        if (stableFrames_ >= sr_ * 3 && cushion_ > 0) {  // after 3 s clean: shrink ~10% every 0.5 s
            cushion_ -= std::max<uint32_t>(1, cushion_ / 10);
            stableFrames_ = sr_ * 5 / 2;
        }
    }
    cushionOut_.store(cushion_, std::memory_order_relaxed);
    const uint32_t target = safetyFrames + cushion_;

    // Drift / backlog control: track the minimum leftover (fill after this callback) over a
    // short window. Anything permanently above the target is pure added latency -> remove it.
    uint32_t leftover = avail > frames ? avail - frames : 0;
    if (leftover < winMin_) winMin_ = leftover;
    winFrames_ += frames;
    if (winFrames_ >= sr_ / 5) {
        if (winMin_ != UINT32_MAX) minFill_.store(winMin_, std::memory_order_relaxed);
        dropBudget_ = (winMin_ != UINT32_MAX && winMin_ > target + 2) ? winMin_ - target - 1 : 0;
        winMin_ = UINT32_MAX;
        winFrames_ = 0;
    }
    if (dropBudget_ > sr_ / 50) {  // >20 ms backlog: jump straight to live
        uint32_t keep = target + frames;
        if (avail > keep) {
            ring_.skip(avail - keep);
            dropped_ += avail - keep;
            avail = keep;
        }
        dropBudget_ = 0;
    }
    uint32_t extra = 0;
    if (dropBudget_ > 0 && frames > 1) {  // gently time-compress (inaudible)
        extra = std::min(dropBudget_, std::max<uint32_t>(1, frames / 16));
        if (avail < frames + extra) extra = avail > frames ? avail - frames : 0;
    }

    uint32_t want = frames + extra;
    uint32_t got = ring_.pop(tmp_, want);
    if (extra > 0 && got == want) {
        float step = (float)(want - 1) / (float)(frames - 1);
        for (uint32_t i = 0; i < frames; ++i) {
            float pos = i * step;
            uint32_t k = (uint32_t)pos;
            float fr = pos - (float)k;
            float b = (k + 1 < want) ? tmp_[k + 1] : tmp_[k];
            mono_[i] = tmp_[k] + (b - tmp_[k]) * fr;
        }
        dropBudget_ -= extra;
        dropped_ += extra;
    } else {
        uint32_t n = std::min(got, frames);
        std::memcpy(mono_, tmp_, n * sizeof(float));
        if (n < frames) {  // underrun: fade from last sample instead of a hard click
            float v = n ? mono_[n - 1] : last_;
            for (uint32_t i = n; i < frames; ++i) { v *= 0.98f; mono_[i] = v; }
        }
    }
    last_ = mono_[frames - 1];

    float p = std::clamp(pan.load(), -1.0f, 1.0f);
    float gl = std::min(1.0f, 1.0f - p), gr = std::min(1.0f, 1.0f + p);
    float outPk = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float s = mono_[i];
        float* fr = out + (size_t)i * ch;
        if (ch == 1) fr[0] = s;
        else {
            fr[0] = s * gl;
            fr[1] = s * gr;
            for (uint32_t c = 2; c < ch; ++c) fr[c] = 0.0f;
        }
        float a = std::fabs(s);
        if (a > outPk) outPk = a;
    }
    float prev = outPeak_.load(std::memory_order_relaxed);
    if (outPk > prev) outPeak_.store(outPk, std::memory_order_relaxed);
}
