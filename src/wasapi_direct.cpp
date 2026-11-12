#ifdef _WIN32
#include "wasapi_direct.h"
#include "engine.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdlib>

static const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <class T> static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------------ formats
struct Fmt {
    uint32_t rate = 0, ch = 0, bytes = 4, validBits = 32;
    bool isFloat = true;
};

static Fmt fmtFrom(const WAVEFORMATEX* w) {
    Fmt f;
    f.rate = w->nSamplesPerSec;
    f.ch = w->nChannels;
    f.bytes = w->wBitsPerSample / 8;
    f.validBits = w->wBitsPerSample;
    f.isFloat = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        f.isFloat = IsEqualGUID(x->SubFormat, kSubFloat) != 0;
        if (x->Samples.wValidBitsPerSample) f.validBits = x->Samples.wValidBitsPerSample;
    }
    return f;
}

static bool supported(const Fmt& f) {
    return f.isFloat ? f.bytes == 4 : (f.bytes == 2 || f.bytes == 3 || f.bytes == 4);
}

static void makeWfx(WAVEFORMATEXTENSIBLE& w, const Fmt& f, DWORD mask) {
    std::memset(&w, 0, sizeof w);
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = (WORD)f.ch;
    w.Format.nSamplesPerSec = f.rate;
    w.Format.wBitsPerSample = (WORD)(f.bytes * 8);
    w.Format.nBlockAlign = (WORD)(f.ch * f.bytes);
    w.Format.nAvgBytesPerSec = f.rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (WORD)f.validBits;
    w.dwChannelMask = mask;
    w.SubFormat = f.isFloat ? kSubFloat : kSubPcm;
}

static void toFloat(const BYTE* src, float* dst, size_t samples, const Fmt& f) {
    if (f.isFloat) { std::memcpy(dst, src, samples * 4); return; }
    if (f.bytes == 2) {
        const int16_t* s = (const int16_t*)src;
        for (size_t i = 0; i < samples; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
    } else if (f.bytes == 3) {
        for (size_t i = 0; i < samples; ++i) {
            int32_t v = (int32_t)((uint32_t)src[3 * i] << 8 | (uint32_t)src[3 * i + 1] << 16 | (uint32_t)src[3 * i + 2] << 24);
            dst[i] = (float)v * (1.0f / 2147483648.0f);
        }
    } else {
        const int32_t* s = (const int32_t*)src;
        for (size_t i = 0; i < samples; ++i) dst[i] = (float)s[i] * (1.0f / 2147483648.0f);
    }
}

static void fromFloat(const float* src, BYTE* dst, size_t samples, const Fmt& f) {
    if (f.isFloat) { std::memcpy(dst, src, samples * 4); return; }
    for (size_t i = 0; i < samples; ++i) {
        float x = std::clamp(src[i], -1.0f, 1.0f);
        if (f.bytes == 2) ((int16_t*)dst)[i] = (int16_t)std::lrint(x * 32767.0f);
        else {
            int32_t v = (int32_t)std::llrint((double)x * 2147483647.0);
            if (f.bytes == 4) ((int32_t*)dst)[i] = v;
            else { dst[3 * i] = (BYTE)(v >> 8); dst[3 * i + 1] = (BYTE)(v >> 16); dst[3 * i + 2] = (BYTE)(v >> 24); }
        }
    }
}

static std::string friendlyName(IMMDevice* d) {
    IPropertyStore* ps = nullptr;
    std::string out;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) { out.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, &out[0], n, nullptr, nullptr); }
        }
        PropVariantClear(&v);
        ps->Release();
    }
    return out;
}

// ------------------------------------------------------------------ client setup
struct Side {
    IMMDevice* dev = nullptr;
    IAudioClient* c = nullptr;
    Fmt f;
    bool excl = false, lowLat = false;
    uint32_t period = 0, buffer = 0;
    void release() { safeRelease(c); safeRelease(dev); }
};

static bool openShared(Side& s, HANDLE ev, std::string& err) {
    IAudioClient3* c3 = nullptr;
    if (SUCCEEDED(s.dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&c3))) {
        WAVEFORMATEX* mf = nullptr;
        if (SUCCEEDED(c3->GetMixFormat(&mf))) {
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mf, &def, &fund, &mn, &mx)) && mn > 0 && mn < def &&
                SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mf, nullptr)) &&
                SUCCEEDED(c3->SetEventHandle(ev))) {
                s.c = c3; s.f = fmtFrom(mf); s.period = mn; s.lowLat = true;
                CoTaskMemFree(mf);
                c3->GetBufferSize(&s.buffer);
                return true;
            }
            CoTaskMemFree(mf);
        }
        c3->Release();
    }
    IAudioClient* c = nullptr;
    if (FAILED(s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) { err = "activate failed"; return false; }
    WAVEFORMATEX* mf = nullptr;
    if (FAILED(c->GetMixFormat(&mf))) { c->Release(); err = "no mix format"; return false; }
    REFERENCE_TIME defP = 0, minP = 0;
    c->GetDevicePeriod(&defP, &minP);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mf, nullptr);
    if (SUCCEEDED(hr)) hr = c->SetEventHandle(ev);
    if (FAILED(hr)) { CoTaskMemFree(mf); c->Release(); err = "initialize failed"; return false; }
    s.c = c; s.f = fmtFrom(mf);
    s.period = (uint32_t)((defP * (LONGLONG)mf->nSamplesPerSec + 5000000) / 10000000);
    CoTaskMemFree(mf);
    c->GetBufferSize(&s.buffer);
    return true;
}

static bool openExclusive(Side& s, HANDLE ev, uint32_t prefRate, int mult, std::string& err) {
    IAudioClient* c = nullptr;
    if (FAILED(s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) { err = "activate failed"; return false; }
    WAVEFORMATEX* mf = nullptr;
    c->GetMixFormat(&mf);
    DWORD mask = 0;
    uint32_t mixCh = 2, mixRate = 48000;
    if (mf) {
        mixCh = mf->nChannels; mixRate = mf->nSamplesPerSec;
        if (mf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mf)->dwChannelMask;
        CoTaskMemFree(mf);
    }
    REFERENCE_TIME defP = 0, minP = 0;
    c->GetDevicePeriod(&defP, &minP);

    WAVEFORMATEXTENSIBLE w{};
    Fmt chosen;
    bool found = false;

    // 1) the device's own format (Sound control panel "Default Format") - what drivers are tested with
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(s.dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            const WAVEFORMATEX* df = (const WAVEFORMATEX*)v.blob.pBlobData;
            Fmt t = fmtFrom(df);
            DWORD m = df->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((const WAVEFORMATEXTENSIBLE*)df)->dwChannelMask : mask;
            if (supported(t)) {
                makeWfx(w, t, m);
                if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) { chosen = t; found = true; }
            }
        }
        PropVariantClear(&v);
        ps->Release();
    }

    // 2) otherwise search common formats
    const uint32_t rates[] = {prefRate, mixRate, 48000, 44100, 96000};
    const Fmt types[] = {{0, 0, 4, 32, true}, {0, 0, 4, 32, false}, {0, 0, 4, 24, false}, {0, 0, 3, 24, false}, {0, 0, 2, 16, false}};
    const uint32_t chans[] = {mixCh, 2, 1};
    for (uint32_t r : rates) {
        if (found) break;
        for (uint32_t ch : chans) {
            for (Fmt t : types) {
                t.rate = r; t.ch = ch;
                DWORD m = ch == mixCh ? mask : (ch == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
                makeWfx(w, t, m);
                if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) { chosen = t; found = true; break; }
            }
            if (found) break;
        }
        if (found) break;
    }
    if (!found) { c->Release(); err = "no exclusive format"; return false; }

    REFERENCE_TIME per = minP * std::max(1, mult);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, &w.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr = 0;
        c->GetBufferSize(&fr);
        c->Release();
        c = nullptr;
        per = (REFERENCE_TIME)(10000000.0 * fr / chosen.rate + 0.5);
        if (FAILED(s.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) { err = "activate failed"; return false; }
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, &w.Format, nullptr);
    }
    if (SUCCEEDED(hr)) hr = c->SetEventHandle(ev);
    if (FAILED(hr)) {
        c->Release();
        err = hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ? "exclusive mode not allowed for this device"
            : hr == AUDCLNT_E_DEVICE_IN_USE ? "device in use" : "exclusive init failed";
        return false;
    }
    s.c = c; s.f = chosen; s.excl = true;
    c->GetBufferSize(&s.buffer);
    s.period = s.buffer;
    return true;
}

// ------------------------------------------------------------------ control
bool WasapiDirect::start(const DirectOptions& o, std::string& err) {
    stop();
    ready_ = 0;
    err_.clear();
    note.clear();
    fwdActive = false;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thr_ = std::thread([this, o]() { threadMain(o); });
    while (ready_ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (ready_ < 0) {
        err = err_;
        if (thr_.joinable()) thr_.join();
        CloseHandle((HANDLE)stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }
    return true;
}

void WasapiDirect::stop() {
    if (stopEvent_) SetEvent((HANDLE)stopEvent_);
    if (thr_.joinable()) thr_.join();
    if (stopEvent_) { CloseHandle((HANDLE)stopEvent_); stopEvent_ = nullptr; }
}

// ------------------------------------------------------------------ audio thread
void WasapiDirect::threadMain(DirectOptions o) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);
    IMMDeviceEnumerator* en = nullptr;
    Side cs, rs, fs;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient *ren = nullptr, *fwd = nullptr;
    IAudioClock* clock = nullptr;
    HANDLE capEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE fwdEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE avrt = nullptr;
    ma_linear_resampler fwdRs{};
    bool fwdRsOn = false;
    std::string e;
    auto fail = [&](const std::string& m) { err_ = m; ready_ = -1; };

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
            fail("MMDeviceEnumerator failed"); break;
        }
        hr = o.hasIn ? en->GetDevice((LPCWSTR)o.in.wasapi, &cs.dev) : en->GetDefaultAudioEndpoint(eCapture, eConsole, &cs.dev);
        if (FAILED(hr)) { fail("Microphone not available"); break; }
        hr = o.hasOut ? en->GetDevice((LPCWSTR)o.out.wasapi, &rs.dev) : en->GetDefaultAudioEndpoint(eRender, eConsole, &rs.dev);
        if (FAILED(hr)) { fail("Output not available"); break; }

        // watchdog level is per output device
        std::wstring outKey = o.hasOut ? std::wstring((const wchar_t*)o.out.wasapi) : L"<default>";
        if (outKey != exclLevelDevice) { exclLevelDevice = outKey; exclLevel = 0; }
        starved = false;

        // output first so the mic never builds a backlog
        if (o.exclusiveOut && exclLevel >= 2) note += "Output exclusive unstable on this device -> shared. ";
        else if (o.exclusiveOut && !openExclusive(rs, renEv, o.preferredRate, exclLevel + 1, e)) { note += "Output exclusive: " + e + " -> shared. "; }
        else if (o.exclusiveOut && exclLevel == 1) note += "Output exclusive: using 2x period for stability. ";
        if (!rs.c && !openShared(rs, renEv, e)) { fail("Output: " + e); break; }
        if (o.exclusiveIn && !openExclusive(cs, capEv, o.preferredRate, 1, e)) { note += "Mic exclusive: " + e + " -> shared. "; }
        if (!cs.c && !openShared(cs, capEv, e)) { fail("Microphone: " + e); break; }
        if (!supported(rs.f) || !supported(cs.f)) { fail("Unsupported sample format"); break; }
        if (FAILED(rs.c->GetService(__uuidof(IAudioRenderClient), (void**)&ren)) ||
            FAILED(cs.c->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) { fail("GetService failed"); break; }
        rs.c->GetService(__uuidof(IAudioClock), (void**)&clock);

        if (o.hasFwd) {  // share the mic with other apps through another output (e.g. VB-Cable)
            if (SUCCEEDED(en->GetDevice((LPCWSTR)o.fwd.wasapi, &fs.dev)) && openShared(fs, fwdEv, e) && supported(fs.f) &&
                SUCCEEDED(fs.c->GetService(__uuidof(IAudioRenderClient), (void**)&fwd))) {
                fwdActive = true;
                fwdName = friendlyName(fs.dev);
            } else note += "Share-mic output failed. ";
        }

        capRate = cs.f.rate; renRate = rs.f.rate; capCh = cs.f.ch; renCh = rs.f.ch;
        capPeriod = cs.period; renPeriod = rs.period; capBuffer = cs.buffer; renBuffer = rs.buffer;
        capLowLatency = cs.lowLat; renLowLatency = rs.lowLat; capExclusive = cs.excl; renExclusive = rs.excl;
        capName = friendlyName(cs.dev); renName = friendlyName(rs.dev);
    } while (false);

    if (ready_ == 0) {
        eng_->directPrepare(capRate, renRate);
        if (fwdActive && fs.f.rate != capRate) {
            ma_linear_resampler_config rc = ma_linear_resampler_config_init(ma_format_f32, 1, capRate, fs.f.rate);
            fwdRsOn = ma_linear_resampler_init(&rc, nullptr, &fwdRs) == MA_SUCCESS;
        }
        DWORD taskIdx = 0;
        avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
        if (avrt) AvSetMmThreadPriority(avrt, AVRT_PRIORITY_CRITICAL);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        std::vector<float> capF((size_t)std::max<uint32_t>(cs.buffer, 4096) * cs.f.ch + 64);
        std::vector<float> renF((size_t)std::max<uint32_t>(rs.buffer, 4096) * rs.f.ch + 64);
        std::vector<float> mono(8192 * 2), fwdOut((size_t)8192 * std::max<uint32_t>(1, fs.f.ch));

        const uint32_t ms = std::max<uint32_t>(1, renRate / 1000);
        uint32_t mg = ms;  // shared-mode margin starts at 1 ms, adapts 0.25 .. 10 ms
        const uint32_t minMargin = std::max<uint32_t>(ms / 4, 8), maxMargin = std::max<uint32_t>(renPeriod, ms * 10);
        margin = rs.excl ? 0 : mg;
        renderGlitches = 0;
        pipelineMs = 0;

        // latency bookkeeping: capture packet timestamps keyed by ring write index
        struct Pkt { uint32_t k; uint64_t qpc; };
        Pkt pk[256] = {};
        volatile uint32_t pkN = 0;  // written by the mic loop, read by the render side (benign race)
        uint64_t written = 0;
        UINT64 clockFreq = 0;
        if (clock) clock->GetFrequency(&clockFreq);
        float latSm = 0.0f;
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        LONGLONG lastClockQ = 0;

        BYTE* buf = nullptr;
        // prime output with silence
        uint32_t prime = rs.excl ? rs.buffer : std::min<uint32_t>(renPeriod + mg, rs.buffer);
        if (SUCCEEDED(ren->GetBuffer(prime, &buf))) { ren->ReleaseBuffer(prime, AUDCLNT_BUFFERFLAGS_SILENT); written += prime; }
        if (fwdActive) fs.c->Start();
        if (FAILED(rs.c->Start()) || FAILED(cs.c->Start())) fail("Start failed");
        else ready_ = 1;

        uint64_t dbgTicks = 0, dbgFail = 0, dbgFrames = 0;
        HRESULT dbgHr = S_OK;
        auto writeRender = [&](uint32_t n) {
            if (n == 0) return;
            HRESULT gh = ren->GetBuffer(n, &buf);
            if (FAILED(gh)) { dbgFail++; dbgHr = gh; return; }
            dbgFrames += n;
            const uint32_t R = eng_->ring_.readIndex();  // ring index of the first sample written now
            float* dst = rs.f.isFloat ? (float*)buf : renF.data();
            eng_->directRender(dst, n, rs.f.ch);
            if (!rs.f.isFloat) fromFloat(renF.data(), buf, (size_t)n * rs.f.ch, rs.f);
            ren->ReleaseBuffer(n, 0);
            // IAudioClock::GetPosition can take milliseconds on USB devices (it may ask the
            // hardware), so only sample the latency ~10x per second.
            LARGE_INTEGER nowQ;
            QueryPerformanceCounter(&nowQ);
            bool sample = nowQ.QuadPart - lastClockQ >= qpf.QuadPart / 10;
            if (sample && clock && clockFreq && pkN) {
                lastClockQ = nowQ.QuadPart;
                UINT64 pos = 0, qpc = 0;
                if (SUCCEEDED(clock->GetPosition(&pos, &qpc))) {
                    for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)pkN, 256u); ++i) {
                        const Pkt& p = pk[(pkN - 1 - i) & 255];
                        int32_t d = (int32_t)(R - p.k);
                        if (d >= 0) {
                            double capT = (double)p.qpc + d * 1e7 / renRate;  // 100 ns units
                            double playT = (double)qpc + ((double)written / renRate - (double)pos / clockFreq) * 1e7;
                            float l = (float)((playT - capT) / 1e4);
                            if (l > 0.0f && l < 1000.0f) {
                                latSm = latSm == 0.0f ? l : latSm * 0.8f + l * 0.2f;
                                pipelineMs = latSm;
                            }
                            break;
                        }
                    }
                }
            }
            written += n;
        };

        // Exclusive output: dedicated thread that answers every device event immediately.
        std::thread renThr;
        if (rs.excl && ready_ == 1) {
            renThr = std::thread([&]() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                DWORD ti = 0;
                HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &ti);
                if (av) AvSetMmThreadPriority(av, AVRT_PRIORITY_CRITICAL);
                HANDLE rev[2] = {(HANDLE)stopEvent_, renEv};
                LARGE_INTEGER t0, tn;
                QueryPerformanceCounter(&t0);
                uint64_t winFrames = 0;
                LONGLONG winStart = 0;
                while (true) {
                    DWORD r = WaitForMultipleObjects(2, rev, FALSE, 250);
                    if (r == WAIT_OBJECT_0) break;
                    if (r == WAIT_OBJECT_0 + 1) { dbgTicks++; writeRender(rs.buffer); winFrames += rs.buffer; }
                    // watchdog: the device must consume ~real time. Check 1 s windows after 1 s warm-up.
                    QueryPerformanceCounter(&tn);
                    if (tn.QuadPart - t0.QuadPart < qpf.QuadPart) { winStart = tn.QuadPart; winFrames = 0; continue; }
                    if (tn.QuadPart - winStart >= qpf.QuadPart) {
                        double secs = (double)(tn.QuadPart - winStart) / qpf.QuadPart;
                        if (winFrames < 0.85 * renRate * secs) {
                            exclLevel = exclLevel + 1;
                            starved = true;
                            eng_->directLost();  // the engine restarts us with the next level
                            break;
                        }
                        winStart = tn.QuadPart;
                        winFrames = 0;
                    }
                }
                if (av) AvRevertMmThreadCharacteristics(av);
                CoUninitialize();
            });
        }
        // render has priority: its deadline is hard, the mic can wait a moment
        HANDLE evs[3] = {(HANDLE)stopEvent_, renEv, capEv};
        const DWORD nEv = rs.excl ? 3 : 3;
        uint64_t renderTicks = 0, cleanFrames = 0;
        int timeouts = 0;
        if (rs.excl) evs[1] = capEv;  // main loop only services the mic (index 2 duplicate is harmless)
        while (ready_ == 1) {
            DWORD w = WaitForMultipleObjects(rs.excl ? 2 : nEv, evs, FALSE, 250);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) {
                if (++timeouts >= 8) { eng_->directLost(); break; }
                continue;
            }
            timeouts = 0;

            // 1) drain the mic
            bool lost = false;
            while (true) {
                UINT32 pkt = 0;
                if (FAILED(cap->GetNextPacketSize(&pkt))) { lost = true; break; }
                if (pkt == 0) break;
                BYTE* data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpc = 0;
                if (FAILED(cap->GetBuffer(&data, &n, &flags, &devPos, &qpc))) { lost = true; break; }
                if (n) {
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    const float* in = nullptr;
                    if (!silent) {
                        if (cs.f.isFloat) in = (const float*)data;
                        else {
                            if (capF.size() < (size_t)n * cs.f.ch) capF.resize((size_t)n * cs.f.ch);
                            toFloat(data, capF.data(), (size_t)n * cs.f.ch, cs.f);
                            in = capF.data();
                        }
                    }
                    pk[pkN & 255] = {eng_->ring_.writeIndex(), qpc};
                    pkN = pkN + 1;
                    eng_->directCapture(in, n, cs.f.ch);

                    if (fwdActive) {  // share the raw mic with other apps
                        uint32_t done = 0;
                        while (done < n) {
                            uint32_t c = std::min<uint32_t>(n - done, 4096);
                            for (uint32_t i = 0; i < c; ++i) {
                                float s = 0.0f;
                                if (in) {
                                    const float* fr = in + (size_t)(done + i) * cs.f.ch;
                                    for (uint32_t k = 0; k < cs.f.ch; ++k) s += fr[k];
                                    s /= (float)cs.f.ch;
                                }
                                mono[i] = s;
                            }
                            const float* src = mono.data();
                            ma_uint64 outN = c;
                            if (fwdRsOn) {
                                ma_uint64 inN = c;
                                outN = 8192;
                                ma_linear_resampler_process_pcm_frames(&fwdRs, mono.data(), &inN, mono.data() + 8192, &outN);
                                src = mono.data() + 8192;
                            }
                            UINT32 fpad = 0;
                            if (SUCCEEDED(fs.c->GetCurrentPadding(&fpad))) {
                                uint32_t room = fs.buffer > fpad ? fs.buffer - fpad : 0;
                                uint32_t wn = (uint32_t)std::min<ma_uint64>(outN, room);
                                BYTE* fb = nullptr;
                                if (wn && SUCCEEDED(fwd->GetBuffer(wn, &fb))) {
                                    for (uint32_t i = 0; i < wn; ++i)
                                        for (uint32_t k = 0; k < fs.f.ch; ++k) fwdOut[(size_t)i * fs.f.ch + k] = src[i];
                                    fromFloat(fwdOut.data(), fb, (size_t)wn * fs.f.ch, fs.f);
                                    fwd->ReleaseBuffer(wn, 0);
                                }
                            }
                            done += c;
                        }
                    }
                }
                cap->ReleaseBuffer(n);
            }
            if (lost) { eng_->directLost(); break; }

            const bool renderTick = (w == WAIT_OBJECT_0 + 1);
            if (rs.excl) continue;  // exclusive render runs on its own thread

            // 2) shared: keep the queue at one engine period + margin
            UINT32 pad = 0;
            if (FAILED(rs.c->GetCurrentPadding(&pad))) { eng_->directLost(); break; }
            if (renderTick) {
                ++renderTicks;
                if (pad == 0 && renderTicks > 20) {  // engine ran dry -> grow margin
                    renderGlitches++;
                    mg = std::min<uint32_t>(mg + ms, maxMargin);
                    cleanFrames = 0;
                } else if ((cleanFrames += renPeriod) >= renRate * 5ull && mg > minMargin) {  // 5 s clean -> shrink
                    mg = std::max<uint32_t>(minMargin, mg - std::max<uint32_t>(1, mg / 8));
                    cleanFrames = 0;
                }
                margin = mg;
            }
            uint32_t target = std::min<uint32_t>(renPeriod + mg, rs.buffer);
            uint32_t need = pad < target ? target - pad : 0;
            uint32_t n = renderTick ? need : std::min<uint32_t>(need, eng_->directAvailable());
            writeRender(n);
        }
        if (std::getenv("SIDETONE_DEBUG")) {
            UINT32 pd = 0; rs.c->GetCurrentPadding(&pd);
            fprintf(stderr, "[direct] renderTicks=%llu fails=%llu lastHr=0x%08lx frames=%llu buffer=%u pad=%u fmt=%u/%uch/%ub/%s\n",
                    (unsigned long long)dbgTicks, (unsigned long long)dbgFail, (unsigned long)dbgHr, (unsigned long long)dbgFrames,
                    rs.buffer, pd, rs.f.rate, rs.f.ch, rs.f.bytes * 8, rs.f.isFloat ? "f" : "i");
        }
        if (renThr.joinable()) { SetEvent((HANDLE)stopEvent_); renThr.join(); }
        rs.c->Stop();
        cs.c->Stop();
        if (fwdActive) fs.c->Stop();
    }

    if (fwdRsOn) ma_linear_resampler_uninit(&fwdRs, nullptr);
    if (avrt) AvRevertMmThreadCharacteristics(avrt);
    safeRelease(clock); safeRelease(cap); safeRelease(ren); safeRelease(fwd);
    cs.release(); rs.release(); fs.release();
    safeRelease(en);
    CloseHandle(capEv); CloseHandle(renEv); CloseHandle(fwdEv);
    if (comInit) CoUninitialize();
    if (ready_ == 0) ready_ = -1;
}
#endif
