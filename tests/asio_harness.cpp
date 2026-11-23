// asio_harness: a fake ASIO host for the NAM Bridge ASIO driver (docs/WINDOWS_ASIO.md, §7).
// Creates the driver the way hosts do, checks its static properties, then plays a DI file
// through the loopback and compares what comes back with an offline render of the same model.
//
//   asio_harness [--dll PATH] [--model F.nam] [--vectors DIR]
//
// Without --dll the driver is created through the registry (HKLM\SOFTWARE\ASIO\NAM Bridge +
// CoCreateInstance with the CLSID as IID), which also tests registration. With --dll it is
// loaded directly (LoadLibrary + DllGetClassObject), so it can run before regsvr32.
//
// The harness temporarily replaces the driver's settings in
// HKCU\Software\tonex-nam-bridge\NAMBridgeASIO and restores them on exit.
//
// GPLv3: see asio/COPYING.md.

#include <windows.h>
#include <intrin.h>
#include <objbase.h>

#include "iasiodrv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "NamBridgeControl.h"
#include "NamEngine.h"
#include "wav.h"

using namespace nambridge;
namespace fs = std::filesystem;

namespace {

// {0FF6E346-784E-45F1-B522-7B6A486425EA}
constexpr CLSID kClsid = {0x0ff6e346, 0x784e, 0x45f1, {0xb5, 0x22, 0x7b, 0x6a, 0x48, 0x64, 0x25, 0xea}};
constexpr wchar_t kSettingsKey[] = L"Software\\tonex-nam-bridge\\NAMBridgeASIO";
constexpr wchar_t kAsioKey[] = L"SOFTWARE\\ASIO\\NAM Bridge";

int gFails = 0;

bool Check(bool ok, const std::string& what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    fflush(stdout);
    if (!ok)
        gFails++;
    return ok;
}

int64_t Join64(unsigned long hi, unsigned long lo)
{
    return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double Rms(const std::vector<float>& x, size_t from, size_t n)
{
    double e = 0;
    for (size_t i = from; i < from + n && i < x.size(); i++)
        e += static_cast<double>(x[i]) * x[i];
    return std::sqrt(e / static_cast<double>(n));
}

// Windowed-sinc resampler (offline, for the 44.1 kHz level check).
std::vector<float> Resample(const std::vector<float>& x, double from, double to)
{
    const double ratio = to / from;
    const size_t n = static_cast<size_t>(std::floor(static_cast<double>(x.size()) * ratio));
    const double fc = 0.5 * std::min(1.0, ratio) * 0.97; // cycles per input sample
    const int half = 32;
    std::vector<float> y(n);
    for (size_t m = 0; m < n; m++)
    {
        const double t = static_cast<double>(m) / ratio;
        const long long i0 = static_cast<long long>(std::floor(t));
        double acc = 0;
        for (long long k = i0 - half + 1; k <= i0 + half; k++)
        {
            if (k < 0 || k >= static_cast<long long>(x.size()))
                continue;
            const double d = t - static_cast<double>(k);
            const double w = 0.5 * (1 + std::cos(M_PI * d / half));
            const double a = 2 * fc * d;
            const double s = std::abs(a) < 1e-12 ? 2 * fc : 2 * fc * std::sin(M_PI * a) / (M_PI * a);
            acc += x[static_cast<size_t>(k)] * s * w;
        }
        y[m] = static_cast<float>(acc);
    }
    return y;
}

// ---- registry: back up / restore the driver's settings ----------------------------------

struct RegBackup
{
    bool existed = false;
    std::vector<std::tuple<std::wstring, DWORD, std::vector<BYTE>>> values;
};

RegBackup BackupSettings()
{
    RegBackup b;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return b;
    b.existed = true;
    for (DWORD i = 0;; i++)
    {
        wchar_t name[256];
        DWORD nameLen = 256, type = 0, bytes = 0;
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
            break;
        std::vector<BYTE> data(bytes);
        nameLen = 256;
        RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data.data(), &bytes);
        b.values.emplace_back(name, type, data);
    }
    RegCloseKey(key);
    return b;
}

void RestoreSettings(const RegBackup& b)
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsKey);
    if (!b.existed)
        return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    for (const auto& [name, type, data] : b.values)
        RegSetValueExW(key, name.c_str(), 0, type, data.data(), static_cast<DWORD>(data.size()));
    RegCloseKey(key);
}

void WriteSettings(const std::wstring& modelPath)
{
    HKEY key = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    auto str = [&](const wchar_t* n, const std::wstring& v) {
        RegSetValueExW(key, n, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    str(L"ModelPath", modelPath);
    str(L"InputGainDb", L"0");
    str(L"OutputGainDb", L"0");
    const DWORD sel = 0;
    RegSetValueExW(key, L"InputSelect", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sel), sizeof sel);
    RegCloseKey(key);
}

// ---- creating the driver ----------------------------------------------------------------

HMODULE gDll = nullptr;

IASIO* CreateDriver(const std::wstring& dllPath)
{
    IASIO* d = nullptr;
    if (dllPath.empty())
    {
        // Like a host: find the CLSID under HKLM\SOFTWARE\ASIO, pass it as the IID too.
        wchar_t clsidText[64] = {};
        DWORD bytes = sizeof clsidText;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kAsioKey, L"CLSID", RRF_RT_REG_SZ, nullptr, clsidText, &bytes) !=
            ERROR_SUCCESS)
            return nullptr;
        CLSID clsid;
        if (FAILED(CLSIDFromString(clsidText, &clsid)))
            return nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&d))))
            return nullptr;
        return d;
    }
    if (!gDll)
        gDll = LoadLibraryW(dllPath.c_str());
    if (!gDll)
        return nullptr;
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    auto get = reinterpret_cast<GetClassObject>(GetProcAddress(gDll, "DllGetClassObject"));
    IClassFactory* f = nullptr;
    if (!get || FAILED(get(kClsid, IID_IClassFactory, reinterpret_cast<void**>(&f))))
        return nullptr;
    const HRESULT hr = f->CreateInstance(nullptr, kClsid, reinterpret_cast<void**>(&d));
    f->Release();
    return SUCCEEDED(hr) ? d : nullptr;
}

INamBridgeControl* ControlOf(IASIO* d)
{
    INamBridgeControl* c = nullptr;
    d->QueryInterface(__uuidof(INamBridgeControl), reinterpret_cast<void**>(&c));
    return c;
}

// ---- the fake host's audio side ---------------------------------------------------------

struct Host
{
    ASIOBufferInfo infos[4] = {}; // out 0, out 1, in 0, in 1
    long bufferSize = 0;
    bool timeInfo = true;
    const std::vector<float>* play = nullptr;
    bool loopPlay = false;
    size_t playPos = 0;
    std::vector<float> rec;
    std::atomic<size_t> recPos{0};
    std::vector<int64_t> cbQpc;
    std::vector<int64_t> cbSamplePos;
    std::atomic<size_t> callbacks{0};
    std::atomic<uint64_t> cpuCycles{0}; // QueryThreadCycleTime of the clock thread (cycle-accurate)
    std::atomic<long> resetRequests{0};
} gHost;

void OnSwitch(long index, const ASIOTime* t)
{
    Host& h = gHost;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const size_t c = h.callbacks.load();
    if (c < h.cbQpc.size())
    {
        h.cbQpc[c] = q.QuadPart;
        h.cbSamplePos[c] = t ? Join64(t->timeInfo.samplePosition.hi, t->timeInfo.samplePosition.lo) : -1;
    }
    const long n = h.bufferSize;
    const auto* in0 = static_cast<const float*>(h.infos[2].buffers[index]);
    auto* out0 = static_cast<float*>(h.infos[0].buffers[index]);
    auto* out1 = static_cast<float*>(h.infos[1].buffers[index]);

    size_t pos = h.recPos.load();
    for (long i = 0; i < n && pos < h.rec.size(); i++)
        h.rec[pos++] = in0[i];
    h.recPos.store(pos);

    const auto& p = *h.play;
    for (long i = 0; i < n; i++)
    {
        if (h.loopPlay)
            out0[i] = p[h.playPos++ % p.size()];
        else
            out0[i] = h.playPos < p.size() ? p[h.playPos++] : 0.0f;
        out1[i] = 0.0f; // ToneX-style: signal on output 1 only
    }

    // Not GetThreadTimes: it samples on clock ticks, and a timer-driven thread that wakes right
    // after each tick gets charged whole ticks.
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
        h.cpuCycles.store(cycles);
    h.callbacks.store(c + 1);
}

void CbBufferSwitch(long index, ASIOBool) { OnSwitch(index, nullptr); }
void CbSampleRateDidChange(ASIOSampleRate) {}
ASIOTime* CbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool)
{
    OnSwitch(index, params);
    return nullptr;
}
long CbAsioMessage(long selector, long value, void*, double*)
{
    switch (selector)
    {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion || value == kAsioResetRequest ||
                (value == kAsioSupportsTimeInfo && gHost.timeInfo))
                   ? 1
                   : 0;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
        gHost.resetRequests++;
        return 1;
    case kAsioSupportsTimeInfo:
        return gHost.timeInfo ? 1 : 0;
    }
    return 0;
}

ASIOCallbacks gCallbacks = {CbBufferSwitch, CbSampleRateDidChange, CbAsioMessage, CbBufferSwitchTimeInfo};

// Rate of the counter behind QueryThreadCycleTime (the invariant TSC), measured against QPC.
double CyclesPerSecond()
{
    static const double hz = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const unsigned long long c0 = __rdtsc();
        do
            QueryPerformanceCounter(&b);
        while (b.QuadPart - a.QuadPart < f.QuadPart / 5);
        const unsigned long long c1 = __rdtsc();
        return static_cast<double>(c1 - c0) * static_cast<double>(f.QuadPart) /
               static_cast<double>(b.QuadPart - a.QuadPart);
    }();
    return hz;
}

struct RunStats
{
    bool ok = false;
    size_t callbacks = 0;
    double meanPeriod = 0, nominal = 0, maxJitter = 0, steadyJitter = 0, wall = 0, cpu = 0, process = 0;
    int longPeriods = 0;
    bool positionsOk = true;
};

// Plays `play` on output 0 at `rate` and records input 0 until `recLen` samples arrived.
// `during` (optional) runs on another thread while audio runs.
RunStats Run(IASIO* d, double rate, long bufferSize, const std::vector<float>& play, bool loopPlay, bool timeInfo,
             size_t recLen, const std::function<void()>& during = {})
{
    RunStats st;
    Host& h = gHost;
    h.bufferSize = bufferSize;
    h.timeInfo = timeInfo;
    h.play = &play;
    h.loopPlay = loopPlay;
    h.playPos = 0;
    h.rec.assign(recLen, 0.0f);
    h.recPos.store(0);
    h.cbQpc.assign(recLen / static_cast<size_t>(bufferSize) + 64, 0);
    h.cbSamplePos.assign(h.cbQpc.size(), -1);
    h.callbacks.store(0);
    h.cpuCycles.store(0);
    for (int i = 0; i < 4; i++)
    {
        h.infos[i] = {};
        h.infos[i].isInput = i >= 2 ? ASIOTrue : ASIOFalse;
        h.infos[i].channelNum = i % 2;
    }

    char what[128];
    snprintf(what, sizeof what, "setSampleRate(%.0f)", rate);
    if (!Check(d->setSampleRate(rate) == ASE_OK, what))
        return st;
    snprintf(what, sizeof what, "createBuffers(2 in + 2 out, %ld frames)", bufferSize);
    if (!Check(d->createBuffers(h.infos, 4, bufferSize, &gCallbacks) == ASE_OK, what))
        return st;
    long inLat = -1, outLat = -1;
    d->getLatencies(&inLat, &outLat);
    snprintf(what, sizeof what, "latencies: input %ld, output %ld (= buffer)", inLat, outLat);
    Check(inLat == 0 && outLat == bufferSize, what);

    INamBridgeControl* ctl = ControlOf(d);
    if (!Check(d->start() == ASE_OK, "start()"))
    {
        d->disposeBuffers();
        return st;
    }
    std::thread side;
    if (during)
        side = std::thread(during);
    const double timeout = static_cast<double>(recLen) / rate * 3 + 5;
    const ULONGLONG t0 = GetTickCount64();
    while (h.recPos.load() < recLen && static_cast<double>(GetTickCount64() - t0) / 1000.0 < timeout)
        Sleep(10);
    if (side.joinable())
        side.join();
    while (h.recPos.load() < recLen && static_cast<double>(GetTickCount64() - t0) / 1000.0 < timeout)
        Sleep(10);
    NamBridgeStatus status = {};
    if (ctl)
    {
        ctl->GetStatus(&status);
        ctl->Release();
    }
    st.process = status.processSeconds;
    ASIOSamples sp;
    ASIOTimeStamp ts;
    Check(d->getSamplePosition(&sp, &ts) == ASE_OK && Join64(sp.hi, sp.lo) > 0, "getSamplePosition advances");
    Check(d->stop() == ASE_OK, "stop()");
    Check(d->getSamplePosition(&sp, &ts) == ASE_SPNotAdvancing, "getSamplePosition after stop -> not advancing");
    Check(d->disposeBuffers() == ASE_OK, "disposeBuffers()");
    if (!Check(h.recPos.load() >= recLen, "recorded the whole signal (no stall)"))
        return st;

    // Timing.
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const size_t n = std::min(h.callbacks.load(), h.cbQpc.size());
    st.callbacks = n;
    st.nominal = static_cast<double>(bufferSize) / rate;
    st.wall = static_cast<double>(h.cbQpc[n - 1] - h.cbQpc[0]) / static_cast<double>(f.QuadPart);
    st.meanPeriod = st.wall / static_cast<double>(n - 1);
    for (size_t i = 1; i < n; i++)
    {
        const double p = static_cast<double>(h.cbQpc[i] - h.cbQpc[i - 1]) / static_cast<double>(f.QuadPart);
        st.maxJitter = std::max(st.maxJitter, std::abs(p - st.nominal));
        if (i > 4) // after the engine's re-prime on the first processed buffer
            st.steadyJitter = std::max(st.steadyJitter, std::abs(p - st.nominal));
        if (p > 2 * st.nominal)
            st.longPeriods++;
    }
    for (size_t i = 0; i < n && timeInfo; i++)
        if (h.cbSamplePos[i] != static_cast<int64_t>(i) * bufferSize)
            st.positionsOk = false;
    st.cpu = static_cast<double>(h.cpuCycles.load()) / CyclesPerSecond();
    st.ok = true;
    return st;
}

void ReportTiming(const RunStats& st, double rate)
{
    char what[256];
    const double errPct = 100.0 * (st.meanPeriod - st.nominal) / st.nominal;
    snprintf(what, sizeof what, "mean period %.4f ms vs nominal %.4f ms (%+.3f%%, %zu callbacks)",
             st.meanPeriod * 1e3, st.nominal * 1e3, errPct, st.callbacks);
    Check(std::abs(errPct) < 0.5, what);
    printf("         max jitter %.3f ms (%.3f ms after the first 4 callbacks); periods over 2x nominal: %d\n",
           st.maxJitter * 1e3, st.steadyJitter * 1e3, st.longPeriods);
    printf("         clock thread CPU %.3f s over %.2f s (%.1f%% of one core, incl. host callback)\n", st.cpu,
           st.wall, 100.0 * st.cpu / std::max(st.wall, 1e-9));
    printf("         of which in NamEngine::Process: %.3f s (DSP load %.1f%%)\n", st.process,
           100.0 * st.process / std::max(st.wall, 1e-9));
    (void)rate;
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring dllPath;
    fs::path model, vectors = "tests/vectors";
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--dll" && i + 1 < argc)
            dllPath = fs::absolute(argv[++i]).wstring();
        else if (a == "--model" && i + 1 < argc)
            model = argv[++i];
        else if (a == "--vectors" && i + 1 < argc)
            vectors = argv[++i];
        else
        {
            fprintf(stderr, "usage: asio_harness [--dll PATH] [--model F.nam] [--vectors DIR]\n");
            return 2;
        }
    }
    if (model.empty() && fs::is_directory("testdata"))
        for (const auto& e : fs::directory_iterator("testdata"))
            if (e.path().extension() == ".nam")
            {
                model = e.path();
                break;
            }
    if (model.empty() || !fs::exists(model))
    {
        fprintf(stderr, "No model: pass --model F.nam (or run from the repo root with testdata/*.nam)\n");
        return 2;
    }
    model = fs::absolute(model);

    std::vector<float> di, expected;
    double diRate = 0, expRate = 0;
    if (dsp::wav::Load((vectors / "di-48k.wav").string().c_str(), di, diRate) != dsp::wav::LoadReturnCode::SUCCESS ||
        dsp::wav::Load((vectors / "expected-clubman-ssr-od-lo-48k.wav").string().c_str(), expected, expRate) !=
            dsp::wav::LoadReturnCode::SUCCESS)
    {
        fprintf(stderr, "Cannot read the test vectors in %s\n", vectors.string().c_str());
        return 2;
    }
    const std::string json = ReadFile(model);

    printf("NAM Bridge ASIO harness (%s)\n  model: %s\n", dllPath.empty() ? "via registry" : "via --dll",
           model.string().c_str());
    CoInitialize(nullptr);
    const RegBackup backup = BackupSettings();
    struct Restore
    {
        const RegBackup& b;
        ~Restore() { RestoreSettings(b); }
    } restore{backup};

    // ---- 1. creation, init robustness --------------------------------------------------
    printf("Creation / identity\n");
    if (dllPath.empty())
    {
        wchar_t text[64] = {};
        DWORD bytes = sizeof text;
        CLSID c = {};
        const bool found = RegGetValueW(HKEY_LOCAL_MACHINE, kAsioKey, L"CLSID", RRF_RT_REG_SZ, nullptr, text,
                                        &bytes) == ERROR_SUCCESS &&
                           SUCCEEDED(CLSIDFromString(text, &c)) && c == kClsid;
        if (!Check(found, "HKLM\\SOFTWARE\\ASIO\\NAM Bridge has the driver CLSID"))
            return 1;
    }
    {
        WriteSettings(L"C:\\does-not-exist\\missing.nam");
        IASIO* d = CreateDriver(dllPath);
        if (!Check(d != nullptr, "driver created (CLSID passed as IID)"))
            return 1;
        char err[124] = {};
        Check(d->init(nullptr) == ASIOTrue, "init() succeeds with a missing model file");
        d->getErrorMessage(err);
        Check(strlen(err) > 0, std::string("  ... and reports it: \"") + err + "\"");
        INamBridgeControl* ctl = ControlOf(d);
        NamBridgeStatus s = {};
        Check(ctl && SUCCEEDED(ctl->GetStatus(&s)) && !s.modelLoaded, "  ... and runs with no model");
        if (ctl)
            ctl->Release();
        d->Release();
    }

    WriteSettings(model.wstring());
    IASIO* d = CreateDriver(dllPath);
    if (!Check(d != nullptr, "driver created with the test model in settings"))
        return 1;
    Check(d->init(GetConsoleWindow()) == ASIOTrue, "init()");
    INamBridgeControl* ctl = ControlOf(d);
    if (!Check(ctl != nullptr, "QueryInterface(INamBridgeControl)"))
        return 1;
    {
        NamBridgeStatus s = {};
        ctl->GetStatus(&s);
        char err[124] = {};
        d->getErrorMessage(err);
        Check(s.modelLoaded && err[0] == '\0', "model from settings loaded by init()");
    }

    // ---- 2. static properties -------------------------------------------------------------
    {
        char name[32] = {};
        d->getDriverName(name);
        Check(strcmp(name, "NAM Bridge") == 0, std::string("getDriverName = \"") + name + "\"");
        Check(d->getDriverVersion() == 1, "getDriverVersion = 1");
        long ins = 0, outs = 0;
        d->getChannels(&ins, &outs);
        Check(ins == 2 && outs == 2, "2 inputs, 2 outputs");
        long mn = 0, mx = 0, pref = 0, gran = 0;
        d->getBufferSize(&mn, &mx, &pref, &gran);
        Check(mn == 64 && mx == 2048 && pref == 256 && gran == -1, "buffer sizes 64..2048, preferred 256, pow2");
        bool rates = true;
        for (double r : {44100.0, 48000.0, 88200.0, 96000.0})
            rates = rates && d->canSampleRate(r) == ASE_OK;
        Check(rates, "accepts 44100, 48000, 88200, 96000");
        bool others = true;
        for (double r : {8000.0, 22050.0, 32000.0, 176400.0, 192000.0})
            others = others && d->canSampleRate(r) == ASE_NoClock;
        Check(others && d->setSampleRate(22050) == ASE_NoClock, "rejects other rates (ASE_NoClock)");
        ASIOSampleRate cur = 0;
        d->getSampleRate(&cur);
        Check(cur == 48000.0, "default rate 48000");
        bool types = true;
        for (int input = 0; input < 2; input++)
            for (long c = 0; c < 2; c++)
            {
                ASIOChannelInfo ci = {};
                ci.channel = c;
                ci.isInput = input;
                types = types && d->getChannelInfo(&ci) == ASE_OK && ci.type == ASIOSTFloat32LSB &&
                        ci.isActive == ASIOFalse && ci.channelGroup == 0;
                printf("         %s %ld: \"%s\"\n", input ? "in " : "out", c, ci.name);
            }
        ASIOChannelInfo bad = {};
        bad.channel = 2;
        Check(types && d->getChannelInfo(&bad) == ASE_InvalidParameter, "channels are float32 LSB, inactive, group 0");
        ASIOClockSource clocks[4] = {};
        long nClocks = 4;
        d->getClockSources(clocks, &nClocks);
        Check(nClocks == 1 && clocks[0].isCurrentSource && strcmp(clocks[0].name, "Internal") == 0,
              "one clock source: Internal (current)");
        Check(d->future(kAsioCanTimeInfo, nullptr) == ASE_SUCCESS &&
                  d->future(kAsioCanTimeCode, nullptr) == ASE_InvalidParameter,
              "future: CanTimeInfo only");
        Check(d->outputReady() == ASE_NotPresent, "outputReady -> ASE_NotPresent");
        ASIOBufferInfo bi[2] = {};
        bi[0].isInput = ASIOTrue;
        bi[1].isInput = ASIOFalse;
        Check(d->createBuffers(bi, 2, 100, &gCallbacks) != ASE_OK, "createBuffers rejects 100 frames");
        bi[1].channelNum = 2;
        Check(d->createBuffers(bi, 2, 256, &gCallbacks) != ASE_OK, "createBuffers rejects channel 2");
        Check(d->start() != ASE_OK, "start() without buffers fails");
    }

    // ---- 3. loopback exactness at 48 kHz, buffer 256 ---------------------------------------
    const long B = 256;
    printf("Loopback at 48000 Hz, buffer %ld (bufferSwitchTimeInfo)\n", B);
    RunStats st48 = Run(d, 48000, B, di, false, true, di.size() + 8 * static_cast<size_t>(B));
    std::vector<float> rec48 = gHost.rec;
    if (st48.ok)
    {
        // Offline render through the same engine build and block size, primed like the driver.
        NamEngine ref;
        ref.SetHostRate(48000);
        std::string err;
        ref.LoadFromJson(json, model.stem().string(), err);
        ref.ResetState();
        std::vector<float> y(di.size());
        for (size_t i = 0; i < di.size(); i += B)
            ref.Process(di.data() + i, y.data() + i, static_cast<int>(std::min<size_t>(B, di.size() - i)));

        const bool silentFirst = std::all_of(rec48.begin(), rec48.begin() + B, [](float v) { return v == 0.0f; });
        Check(silentFirst, "first buffer of input is silence");
        double maxDiff = 0;
        for (size_t i = 0; i < di.size(); i++)
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(rec48[i + B] - y[i])));
        char what[256];
        snprintf(what, sizeof what, "recorded[n + %ld] == offline engine render (max diff %.3g)", B, maxDiff);
        Check(maxDiff < 1e-6, what);

        if (expected.size() == di.size())
        {
            double eErr = 0, eRef = 0;
            for (size_t i = 0; i < di.size(); i++)
            {
                const double dd = static_cast<double>(rec48[i + B]) - expected[i];
                eErr += dd * dd;
                eRef += static_cast<double>(expected[i]) * expected[i];
            }
            const double errDb = eErr > 0 ? 10 * std::log10(eErr / eRef) : -300;
            snprintf(what, sizeof what, "aligned recording vs NAM reference render: %.1f dB (<= -80)", errDb);
            Check(errDb <= -80, what);
        }
        else
            Check(false, "reference render has the DI's length");
        ReportTiming(st48, 48000);
        Check(st48.positionsOk, "ASIOTime samplePosition == k * bufferSize on every callback");
    }

    // ---- 4. 44.1 kHz (resampling), plain bufferSwitch --------------------------------------
    printf("Loopback at 44100 Hz, buffer %ld (bufferSwitch, resampling)\n", B);
    const std::vector<float> di44 = Resample(di, 48000, 44100);
    RunStats st44 = Run(d, 44100, B, di44, false, false, di44.size() + 8 * static_cast<size_t>(B));
    if (st44.ok)
    {
        NamBridgeStatus s = {};
        ctl->GetStatus(&s);
        Check(s.resampling && s.sampleRate == 44100.0, "engine resamples at 44100 Hz");
        const double r48 = Rms(rec48, B, di.size()), r44 = Rms(gHost.rec, B, di44.size());
        const double diffDb = 20 * std::log10(r44 / r48);
        char what[256];
        snprintf(what, sizeof what, "RMS 48k %.4f vs 44.1k %.4f (%+.2f dB, within 0.5 dB)", r48, r44, diffDb);
        Check(std::abs(diffDb) < 0.5, what);
        ReportTiming(st44, 44100);
    }

    // ---- 5. hot swap while running -------------------------------------------------------
    printf("Hot swap while running (48000 Hz)\n");
    std::atomic<bool> swapsOk{true};
    std::atomic<size_t> unloadedAt{0}, silentUntil{0}, reloadedAt{0};
    const std::wstring modelW = model.wstring();
    RunStats stSwap = Run(d, 48000, B, di, true, true, static_cast<size_t>(48000 * 4), [&] {
        Sleep(300);
        for (int k = 0; k < 3; k++)
            swapsOk = swapsOk && SUCCEEDED(ctl->LoadModel(modelW.c_str()));
        ctl->UnloadModel();
        unloadedAt = gHost.recPos.load();
        Sleep(500);
        silentUntil = gHost.recPos.load();
        swapsOk = swapsOk && SUCCEEDED(ctl->LoadModel(modelW.c_str()));
        reloadedAt = gHost.recPos.load();
    });
    if (stSwap.ok)
    {
        Check(swapsOk.load(), "3 reloads + unload + reload from another thread, no crash");
        const auto& r = gHost.rec;
        const size_t a = unloadedAt + 2 * static_cast<size_t>(B), b = silentUntil;
        bool silent = b > a + 4 * static_cast<size_t>(B);
        for (size_t i = a; i < b && i < r.size(); i++)
            silent = silent && r[i] == 0.0f;
        char what[256];
        snprintf(what, sizeof what, "silence while unloaded (%zu samples checked)", b > a ? b - a : 0);
        Check(silent, what);
        const double after = Rms(r, reloadedAt + 2 * static_cast<size_t>(B),
                                 r.size() - std::min(r.size(), reloadedAt + 2 * static_cast<size_t>(B)));
        Check(after > 1e-3, "signal returns after reload");
        NamBridgeStatus s = {};
        ctl->GetStatus(&s);
        printf("         driver clock overruns so far: %llu\n", s.overruns);
    }

    ctl->Release();
    d->Release();
    if (gDll)
    {
        using CanUnload = HRESULT(STDAPICALLTYPE*)();
        auto canUnload = reinterpret_cast<CanUnload>(GetProcAddress(gDll, "DllCanUnloadNow"));
        Check(canUnload && canUnload() == S_OK, "DllCanUnloadNow == S_OK after release");
    }
    CoUninitialize();
    printf(gFails ? "%d check(s) failed\n" : "All checks passed\n", gFails);
    return gFails ? 1 : 0;
}
