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
#include <cstring>

static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <class T> static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool isFloat32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return IsEqualGUID(x->SubFormat, kSubtypeFloat) != 0;
    }
    return false;
}

static std::string friendlyName(IMMDevice* d) {
    IPropertyStore* ps = nullptr;
    std::string out;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            out.resize(n > 0 ? n - 1 : 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, &out[0], n, nullptr, nullptr);
        }
        PropVariantClear(&v);
        ps->Release();
    }
    return out;
}

// Opens a shared-mode event-driven client, preferring IAudioClient3 low-latency periods.
static HRESULT openShared(IMMDevice* dev, HANDLE ev, IAudioClient** outClient, WAVEFORMATEX** outFmt,
                          UINT32* period, bool* lowLat, std::string& err) {
    *outClient = nullptr; *outFmt = nullptr; *lowLat = false;
    IAudioClient3* c3 = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&c3))) {
        WAVEFORMATEX* mf = nullptr;
        if (SUCCEEDED(c3->GetMixFormat(&mf))) {
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mf, &def, &fund, &mn, &mx)) && mn > 0 && mn < def &&
                SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mf, nullptr)) &&
                SUCCEEDED(c3->SetEventHandle(ev))) {
                *outClient = c3; *outFmt = mf; *period = mn; *lowLat = true;
                return S_OK;
            }
            CoTaskMemFree(mf);
        }
        c3->Release();
    }
    IAudioClient* c = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
    if (FAILED(hr)) { err = "Activate failed"; return hr; }
    WAVEFORMATEX* mf = nullptr;
    if (FAILED(hr = c->GetMixFormat(&mf))) { c->Release(); err = "GetMixFormat failed"; return hr; }
    REFERENCE_TIME defP = 0, minP = 0;
    c->GetDevicePeriod(&defP, &minP);
    hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mf, nullptr);
    if (SUCCEEDED(hr)) hr = c->SetEventHandle(ev);
    if (FAILED(hr)) { CoTaskMemFree(mf); c->Release(); err = "Initialize failed"; return hr; }
    *outClient = c; *outFmt = mf;
    *period = (UINT32)((defP * (LONGLONG)mf->nSamplesPerSec + 5000000) / 10000000);
    return S_OK;
}

bool WasapiDirect::start(const ma_device_id* in, const ma_device_id* out, std::string& err) {
    stop();
    ready_ = 0;
    err_.clear();
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // copy ids: the caller's device list may be refreshed while we run
    struct Ids { ma_device_id in, out; bool hasIn, hasOut; };
    Ids ids{};
    if (in) { ids.in = *in; ids.hasIn = true; }
    if (out) { ids.out = *out; ids.hasOut = true; }
    thr_ = std::thread([this, ids]() {
        threadMain(ids.hasIn ? &ids.in : nullptr, ids.hasOut ? &ids.out : nullptr);
    });
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

void WasapiDirect::threadMain(const ma_device_id* inId, const ma_device_id* outId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice *capDev = nullptr, *renDev = nullptr;
    IAudioClient *capC = nullptr, *renC = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    WAVEFORMATEX *capF = nullptr, *renF = nullptr;
    HANDLE capEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE avrt = nullptr;
    std::string e;

    auto fail = [&](const std::string& msg) { err_ = msg; ready_ = -1; };

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    (void**)&en))) { fail("MMDeviceEnumerator failed"); break; }
        hr = inId ? en->GetDevice((LPCWSTR)inId->wasapi, &capDev) : en->GetDefaultAudioEndpoint(eCapture, eConsole, &capDev);
        if (FAILED(hr)) { fail("Microphone not available"); break; }
        hr = outId ? en->GetDevice((LPCWSTR)outId->wasapi, &renDev) : en->GetDefaultAudioEndpoint(eRender, eConsole, &renDev);
        if (FAILED(hr)) { fail("Output not available"); break; }

        // Render first so the mic never builds a backlog.
        if (FAILED(openShared(renDev, renEv, &renC, &renF, &renPeriod, &renLowLatency, e))) { fail("Output: " + e); break; }
        if (FAILED(openShared(capDev, capEv, &capC, &capF, &capPeriod, &capLowLatency, e))) { fail("Microphone: " + e); break; }
        if (!isFloat32(renF) || !isFloat32(capF)) { fail("Unsupported mix format"); break; }
        if (FAILED(renC->GetService(__uuidof(IAudioRenderClient), (void**)&ren)) ||
            FAILED(capC->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) { fail("GetService failed"); break; }
        UINT32 rb = 0, cb = 0;
        renC->GetBufferSize(&rb);
        capC->GetBufferSize(&cb);
        renBuffer = rb; capBuffer = cb;
        capRate = capF->nSamplesPerSec; renRate = renF->nSamplesPerSec;
        capCh = capF->nChannels; renCh = renF->nChannels;
        capName = friendlyName(capDev); renName = friendlyName(renDev);
    } while (false);

    if (ready_ == 0) {
        eng_->directPrepare(capRate, renRate);
        DWORD taskIdx = 0;
        avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
        if (avrt) AvSetMmThreadPriority(avrt, AVRT_PRIORITY_CRITICAL);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        const uint32_t ms = renRate / 1000;
        uint32_t mg = std::max<uint32_t>(ms * 3 / 2, 16);  // start with 1.5 ms of margin
        const uint32_t maxMargin = std::max<uint32_t>(renPeriod, ms * 10);
        margin = mg;
        renderGlitches = 0;

        // prime with silence = one period + margin, then go
        BYTE* buf = nullptr;
        uint32_t prime = std::min<uint32_t>(renPeriod + mg, renBuffer);
        if (SUCCEEDED(ren->GetBuffer(prime, &buf))) ren->ReleaseBuffer(prime, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(renC->Start()) || FAILED(capC->Start())) fail("Start failed");
        else ready_ = 1;

        HANDLE evs[3] = {(HANDLE)stopEvent_, capEv, renEv};
        uint64_t renderTicks = 0, cleanTicks = 0;
        int timeouts = 0;
        while (ready_ == 1) {
            DWORD w = WaitForMultipleObjects(3, evs, FALSE, 250);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) {
                if (++timeouts >= 8) { eng_->directLost(); break; }  // 2 s without any event
                continue;
            }
            timeouts = 0;

            // 1) drain all captured packets into the ring
            UINT32 pkt = 0;
            bool lost = false;
            while (true) {
                hr = cap->GetNextPacketSize(&pkt);
                if (FAILED(hr)) { lost = true; break; }
                if (pkt == 0) break;
                BYTE* data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                hr = cap->GetBuffer(&data, &n, &flags, nullptr, nullptr);
                if (FAILED(hr)) { lost = true; break; }
                if (n) eng_->directCapture((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : (const float*)data, n, capCh);
                cap->ReleaseBuffer(n);
            }
            if (lost) { eng_->directLost(); break; }

            // 2) render: keep the queue at exactly one engine period + margin
            UINT32 pad = 0;
            if (FAILED(renC->GetCurrentPadding(&pad))) { eng_->directLost(); break; }
            const bool renderTick = (w == WAIT_OBJECT_0 + 2);
            if (renderTick) {
                ++renderTicks;
                if (pad == 0 && renderTicks > 20) {  // engine ran dry -> grow margin
                    renderGlitches++;
                    mg = std::min<uint32_t>(mg + ms, maxMargin);
                    cleanTicks = 0;
                } else if (++cleanTicks * renPeriod >= renRate * 10ull && mg > ms / 2) {  // 10 s clean -> shrink
                    mg -= std::max<uint32_t>(1, mg / 16);
                    cleanTicks = 0;
                }
                margin = mg;
            }
            uint32_t target = std::min<uint32_t>(renPeriod + mg, renBuffer);
            uint32_t need = pad < target ? target - pad : 0;
            uint32_t n = renderTick ? need : std::min<uint32_t>(need, eng_->directAvailable());
            if (n > 0 && SUCCEEDED(ren->GetBuffer(n, &buf))) {
                eng_->directRender((float*)buf, n, renCh);
                ren->ReleaseBuffer(n, 0);
            }
        }
        renC->Stop();
        capC->Stop();
    }

    if (avrt) AvRevertMmThreadCharacteristics(avrt);
    safeRelease(cap); safeRelease(ren); safeRelease(capC); safeRelease(renC);
    safeRelease(capDev); safeRelease(renDev); safeRelease(en);
    if (capF) CoTaskMemFree(capF);
    if (renF) CoTaskMemFree(renF);
    CloseHandle(capEv);
    CloseHandle(renEv);
    if (comInit) CoUninitialize();
    if (ready_ == 0) ready_ = -1;
}
#endif
