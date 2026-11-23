// nam-bridge: command-line companion for the NAM -> ToneX bridge.
//
//   nam-bridge devices                         list CoreAudio devices
//   nam-bridge probe  --device NAME            live per-channel input meters (find ToneX's output)
//   nam-bridge bridge --device NAME --model F  prototype: run NAM between two channels of one
//                     [--in 1] [--out 3]       loopback device (e.g. BlackHole 16ch)
//   nam-bridge render MODEL IN.wav OUT.wav     offline render through the same engine
//   nam-bridge driver status|load F|unload|set [--in-gain dB] [--out-gain dB] [--input L|R|mix]
//   nam-bridge selftest MODEL                  engine + loopback checks (no audio devices)
//
// Only `render` and `selftest` exist on Windows; everything else is CoreAudio (macOS).
//   nam-bridge loopcheck --model F             play/record through a device like ToneX would

// CoreAudio commands are macOS-only. Define NB_PORTABLE_ONLY to compile just the portable
// subset (render, selftest) on a Mac, the way Windows builds it.
#if defined(__APPLE__) && !defined(NB_PORTABLE_ONLY)
#define NB_COREAUDIO 1
#else
#define NB_COREAUDIO 0
#endif

#if NB_COREAUDIO
#include <CoreAudio/CoreAudio.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if NB_COREAUDIO
#include "CFUtil.h"
#include "DriverProtocol.h"
#endif
#include "Loopback.h"
#include "NamEngine.h"
#include "wav.h"

using namespace nambridge;

namespace {

std::atomic<bool> gStop{false};
void OnSignal(int) { gStop.store(true); }

// ---------------------------------------------------------------------------------------
// Arguments

struct Args
{
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;

    bool Has(const std::string& k) const { return opts.count(k) > 0; }
    std::string Get(const std::string& k, const std::string& def = "") const
    {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
    double Num(const std::string& k, double def) const { return Has(k) ? std::stod(Get(k)) : def; }
};

Args Parse(int argc, char** argv, int start)
{
    Args a;
    for (int i = start; i < argc; i++)
    {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0)
        {
            std::string key = s.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                a.opts[key] = argv[++i];
            else
                a.opts[key] = "1";
        }
        else
            a.positional.push_back(s);
    }
    return a;
}

std::string Db(float lin)
{
    if (lin <= 1e-9f)
        return "  -inf";
    char buf[16];
    snprintf(buf, sizeof buf, "%6.1f", 20.0f * std::log10(lin));
    return buf;
}

void PrintInfo(const ModelInfo& info)
{
    printf("Model: %s  [%s, trained @ %.0f Hz]\n", info.name.c_str(), info.architecture.c_str(), info.modelRate);
    if (info.resampling)
        printf("  Host runs at %.0f Hz -> resampling around the model (+%d frames latency)\n", info.hostRate,
               info.resamplerLatency);
    if (info.hasLoudness)
        printf("  Model loudness metadata: %.1f dB\n", info.loudnessDb);
    if (info.hasInputLevel)
        printf("  Calibrated input level: %.1f dBu\n", info.inputLevelDbu);
}

#if NB_COREAUDIO
// ---------------------------------------------------------------------------------------
// CoreAudio helpers

AudioObjectPropertyAddress Addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {sel, scope, kAudioObjectPropertyElementMain};
}

std::vector<AudioObjectID> AllDevices()
{
    auto addr = Addr(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data());
    return ids;
}

std::string StringProp(AudioObjectID id, AudioObjectPropertySelector sel)
{
    auto addr = Addr(sel);
    CFStringRef s = nullptr;
    UInt32 size = sizeof s;
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &s) != noErr || !s)
        return {};
    std::string out = cf::ToString(s);
    CFRelease(s);
    return out;
}

int Channels(AudioObjectID id, AudioObjectPropertyScope scope)
{
    auto addr = Addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, list) != noErr)
        return 0;
    int n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++)
        n += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return n;
}

double NominalRate(AudioObjectID id)
{
    auto addr = Addr(kAudioDevicePropertyNominalSampleRate);
    Float64 r = 0;
    UInt32 size = sizeof r;
    AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &r);
    return r;
}

bool SetNominalRate(AudioObjectID id, double rate)
{
    auto addr = Addr(kAudioDevicePropertyNominalSampleRate);
    Float64 r = rate;
    if (AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof r, &r) != noErr)
        return false;
    for (int i = 0; i < 50 && std::abs(NominalRate(id) - rate) > 0.5; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return std::abs(NominalRate(id) - rate) < 0.5;
}

AudioObjectID FindDevice(const std::string& query)
{
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    AudioObjectID partial = kAudioObjectUnknown;
    for (auto id : AllDevices())
    {
        if (StringProp(id, kAudioDevicePropertyDeviceUID) == query)
            return id;
        std::string name = StringProp(id, kAudioObjectPropertyName);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name == q)
            return id;
        if (partial == kAudioObjectUnknown && name.find(q) != std::string::npos)
            partial = id;
    }
    return partial;
}

// Locate global channel `ch` (0-based) inside a buffer list.
bool LocateChannel(const AudioBufferList* list, int ch, UInt32& buffer, UInt32& offset, UInt32& stride)
{
    if (!list)
        return false;
    for (UInt32 b = 0; b < list->mNumberBuffers; b++)
    {
        const int n = static_cast<int>(list->mBuffers[b].mNumberChannels);
        if (ch < n)
        {
            buffer = b;
            offset = static_cast<UInt32>(ch);
            stride = static_cast<UInt32>(n);
            return true;
        }
        ch -= n;
    }
    return false;
}

#endif // NB_COREAUDIO

std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteWavFloat(const std::string& path, const std::vector<float>& x, double rate)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataSize = static_cast<uint32_t>(x.size() * 4);
    out.write("RIFF", 4);
    u32(36 + dataSize);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(1);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * 4);
    u16(4);
    u16(32);
    out.write("data", 4);
    u32(dataSize);
    out.write(reinterpret_cast<const char*>(x.data()), dataSize);
    return out.good();
}

#if NB_COREAUDIO
// ---------------------------------------------------------------------------------------
// devices

int CmdDevices()
{
    printf("%-6s %-34s %-5s %-5s %-8s %s\n", "ID", "Name", "In", "Out", "Rate", "UID");
    for (auto id : AllDevices())
    {
        printf("%-6u %-34s %-5d %-5d %-8.0f %s\n", id, StringProp(id, kAudioObjectPropertyName).c_str(),
               Channels(id, kAudioObjectPropertyScopeInput), Channels(id, kAudioObjectPropertyScopeOutput),
               NominalRate(id), StringProp(id, kAudioDevicePropertyDeviceUID).c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------------------
// probe: per-channel input meters

struct ProbeCtx
{
    int channels = 0;
    std::vector<std::atomic<float>> peaks;
    explicit ProbeCtx(int n) : channels(n), peaks(static_cast<size_t>(n)) {}
};

OSStatus ProbeProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                   AudioBufferList*, const AudioTimeStamp*, void* user)
{
    auto* ctx = static_cast<ProbeCtx*>(user);
    if (!in)
        return noErr;
    int ch = 0;
    for (UInt32 b = 0; b < in->mNumberBuffers; b++)
    {
        const auto& buf = in->mBuffers[b];
        const UInt32 n = buf.mNumberChannels;
        if (n == 0 || !buf.mData)
            continue;
        const UInt32 frames = buf.mDataByteSize / (sizeof(float) * n);
        const auto* d = static_cast<const float*>(buf.mData);
        for (UInt32 c = 0; c < n && ch + static_cast<int>(c) < ctx->channels; c++)
        {
            float p = 0;
            for (UInt32 i = 0; i < frames; i++)
                p = std::max(p, std::abs(d[i * n + c]));
            auto& slot = ctx->peaks[static_cast<size_t>(ch) + c];
            float cur = slot.load();
            while (p > cur && !slot.compare_exchange_weak(cur, p)) {}
        }
        ch += static_cast<int>(n);
    }
    return noErr;
}

int CmdProbe(const Args& a)
{
    const AudioObjectID dev = FindDevice(a.Get("device", "BlackHole 16ch"));
    if (dev == kAudioObjectUnknown)
    {
        fprintf(stderr, "Device not found. Run `nam-bridge devices`.\n");
        return 1;
    }
    const int n = Channels(dev, kAudioObjectPropertyScopeInput);
    ProbeCtx ctx(n);
    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(dev, ProbeProc, &ctx, &procId) != noErr || AudioDeviceStart(dev, procId) != noErr)
    {
        fprintf(stderr, "Could not start the device.\n");
        return 1;
    }
    printf("Probing %s (%d inputs @ %.0f Hz). Channels with signal are shown; Ctrl-C to stop.\n",
           StringProp(dev, kAudioObjectPropertyName).c_str(), n, NominalRate(dev));
    printf("If every channel reads -inf while audio is playing, grant this terminal Microphone access\n"
           "(System Settings > Privacy & Security > Microphone).\n");
    const double seconds = a.Num("seconds", 0);
    const auto start = std::chrono::steady_clock::now();
    while (!gStop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string line;
        for (int c = 0; c < n; c++)
        {
            const float p = ctx.peaks[static_cast<size_t>(c)].exchange(0.0f);
            if (p > 1e-5f)
                line += " ch" + std::to_string(c + 1) + ":" + Db(p);
        }
        printf("%s\n", line.empty() ? " (silence)" : line.c_str());
        fflush(stdout);
        if (seconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::duration<double>(seconds))
            break;
    }
    AudioDeviceStop(dev, procId);
    AudioDeviceDestroyIOProcID(dev, procId);
    return 0;
}

// ---------------------------------------------------------------------------------------
// bridge: the BlackHole prototype

struct BridgeCtx
{
    NamEngine* engine = nullptr;
    int inCh = 0, outCh = 2;
    std::vector<float> mono, processed;
    std::atomic<uint32_t> lastFrames{0};
    std::atomic<uint32_t> mismatches{0};
};

OSStatus BridgeProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                    AudioBufferList* out, const AudioTimeStamp*, void* user)
{
    auto* ctx = static_cast<BridgeCtx*>(user);
    if (!out)
        return noErr;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++)
        if (out->mBuffers[b].mData)
            memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);

    UInt32 ib, io, is, ob, oo, os;
    if (!LocateChannel(in, ctx->inCh, ib, io, is) || !LocateChannel(out, ctx->outCh, ob, oo, os))
        return noErr;
    const auto& ibuf = in->mBuffers[ib];
    auto& obuf = out->mBuffers[ob];
    if (!ibuf.mData || !obuf.mData)
        return noErr;
    UInt32 frames = ibuf.mDataByteSize / (sizeof(float) * is);
    const UInt32 outFrames = obuf.mDataByteSize / (sizeof(float) * os);
    if (frames != outFrames)
        ctx->mismatches.fetch_add(1);
    frames = std::min({frames, outFrames, static_cast<UInt32>(ctx->mono.size())});
    ctx->lastFrames.store(frames);

    const auto* src = static_cast<const float*>(ibuf.mData);
    for (UInt32 i = 0; i < frames; i++)
        ctx->mono[i] = src[i * is + io];
    ctx->engine->Process(ctx->mono.data(), ctx->processed.data(), static_cast<int>(frames));
    auto* dst = static_cast<float*>(obuf.mData);
    for (UInt32 i = 0; i < frames; i++)
        dst[i * os + oo] = ctx->processed[i];
    return noErr;
}


int CmdBridge(const Args& a)
{
    if (!a.Has("model"))
    {
        fprintf(stderr, "bridge needs --model FILE.nam\n");
        return 1;
    }
    const AudioObjectID dev = FindDevice(a.Get("device", "BlackHole 16ch"));
    if (dev == kAudioObjectUnknown)
    {
        fprintf(stderr, "Device not found. Run `nam-bridge devices`.\n");
        return 1;
    }
    if (a.Has("rate") && !SetNominalRate(dev, a.Num("rate", 48000)))
        fprintf(stderr, "Warning: could not set device rate to %s\n", a.Get("rate").c_str());

    NamEngine engine;
    engine.SetHostRate(NominalRate(dev));
    engine.SetInputGainDb(static_cast<float>(a.Num("in-gain", 0)));
    engine.SetOutputGainDb(static_cast<float>(a.Num("out-gain", 0)));
    std::string err;
    if (!engine.LoadFromFile(a.Get("model"), err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    BridgeCtx ctx;
    ctx.engine = &engine;
    ctx.inCh = static_cast<int>(a.Num("in", 1)) - 1;
    ctx.outCh = static_cast<int>(a.Num("out", 3)) - 1;
    ctx.mono.resize(16384);
    ctx.processed.resize(16384);
    const int nIn = Channels(dev, kAudioObjectPropertyScopeInput);
    const int nOut = Channels(dev, kAudioObjectPropertyScopeOutput);
    if (ctx.inCh < 0 || ctx.inCh >= nIn || ctx.outCh < 0 || ctx.outCh >= nOut)
    {
        fprintf(stderr, "Channel out of range (device has %d in / %d out)\n", nIn, nOut);
        return 1;
    }
    if (ctx.inCh == ctx.outCh)
        fprintf(stderr, "Warning: --in and --out are the same channel; the NAM output will feed back.\n");

    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(dev, BridgeProc, &ctx, &procId) != noErr ||
        AudioDeviceStart(dev, procId) != noErr)
    {
        fprintf(stderr, "Could not start the device.\n");
        return 1;
    }

    PrintInfo(engine.Info());
    printf("Bridge running on %s @ %.0f Hz: ch%d -> NAM -> ch%d. Ctrl-C to stop.\n",
           StringProp(dev, kAudioObjectPropertyName).c_str(), NominalRate(dev), ctx.inCh + 1, ctx.outCh + 1);
    printf("In ToneX: output = %s ch%d, input = %s ch%d.\n", StringProp(dev, kAudioObjectPropertyName).c_str(),
           ctx.inCh + 1, StringProp(dev, kAudioObjectPropertyName).c_str(), ctx.outCh + 1);

    const double seconds = a.Num("seconds", 0);
    const auto start = std::chrono::steady_clock::now();
    while (!gStop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        float ip, op;
        engine.TakePeaks(ip, op);
        printf("in %s dBFS  out %s dBFS  (buffer %u)%s\n", Db(ip).c_str(), Db(op).c_str(), ctx.lastFrames.load(),
               op > 1.0f ? "  CLIPPING: lower --out-gain" : "");
        fflush(stdout);
        if (seconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::duration<double>(seconds))
            break;
    }
    AudioDeviceStop(dev, procId);
    AudioDeviceDestroyIOProcID(dev, procId);
    if (ctx.mismatches.load())
        printf("Note: %u cycles had different input/output buffer sizes.\n", ctx.mismatches.load());
    return 0;
}

#endif // NB_COREAUDIO

// ---------------------------------------------------------------------------------------
// render

int CmdRender(const Args& a)
{
    if (a.positional.size() < 3)
    {
        fprintf(stderr, "usage: nam-bridge render MODEL.nam IN.wav OUT.wav [--in-gain dB] [--out-gain dB]\n");
        return 1;
    }
    std::vector<float> x;
    double rate = 0;
    auto rc = dsp::wav::Load(a.positional[1].c_str(), x, rate);
    if (rc != dsp::wav::LoadReturnCode::SUCCESS)
    {
        fprintf(stderr, "Cannot read %s: %s\n", a.positional[1].c_str(), dsp::wav::GetMsgForLoadReturnCode(rc).c_str());
        return 1;
    }
    NamEngine engine;
    engine.SetHostRate(rate);
    engine.SetInputGainDb(static_cast<float>(a.Num("in-gain", 0)));
    engine.SetOutputGainDb(static_cast<float>(a.Num("out-gain", 0)));
    std::string err;
    if (!engine.LoadFromFile(a.positional[0], err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    PrintInfo(engine.Info());
    std::vector<float> y(x.size());
    const auto t0 = std::chrono::steady_clock::now();
    const int block = static_cast<int>(a.Num("block", 512));
    for (size_t i = 0; i < x.size(); i += static_cast<size_t>(block))
    {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), x.size() - i));
        engine.Process(x.data() + i, y.data() + i, n);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    float ip, op;
    engine.TakePeaks(ip, op);
    if (!WriteWavFloat(a.positional[2], y, rate))
    {
        fprintf(stderr, "Cannot write %s\n", a.positional[2].c_str());
        return 1;
    }
    const double audio = static_cast<double>(x.size()) / rate;
    printf("Rendered %.2f s of audio in %.2f s (%.1fx realtime). Peak in %s / out %s dBFS -> %s\n", audio, secs,
           audio / secs, Db(ip).c_str(), Db(op).c_str(), a.positional[2].c_str());
    return 0;
}

#if NB_COREAUDIO
// ---------------------------------------------------------------------------------------
// driver control

AudioObjectID FindBridgeDevice()
{
    for (auto id : AllDevices())
        if (StringProp(id, kAudioDevicePropertyDeviceUID) == protocol::kDeviceUID)
            return id;
    return kAudioObjectUnknown;
}

bool SetDict(AudioObjectID dev, AudioObjectPropertySelector sel, CFDictionaryRef d)
{
    auto addr = Addr(sel);
    CFPropertyListRef v = d;
    const OSStatus st = AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof v, &v);
    if (st != noErr)
        fprintf(stderr, "Setting property failed (OSStatus %d)\n", static_cast<int>(st));
    return st == noErr;
}

CFDictionaryRef GetDict(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    auto addr = Addr(sel);
    CFPropertyListRef v = nullptr;
    UInt32 size = sizeof v;
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &v) != noErr || !v)
        return nullptr;
    if (CFGetTypeID(v) != CFDictionaryGetTypeID())
    {
        CFRelease(v);
        return nullptr;
    }
    return static_cast<CFDictionaryRef>(v);
}

int PrintStatus(AudioObjectID dev)
{
    CFDictionaryRef st = GetDict(dev, protocol::kPropStatus);
    if (!st)
    {
        fprintf(stderr, "Driver did not return status.\n");
        return 1;
    }
    std::string name, arch, error, version;
    double loaded = 0, modelRate = 0, hostRate = 0, resampling = 0, inG = 0, outG = 0, sel = 0, inPk = -200,
           outPk = -200, running = 0;
    cf::GetNumber(st, "loaded", loaded);
    cf::GetString(st, "modelName", name);
    cf::GetString(st, "architecture", arch);
    cf::GetString(st, "lastError", error);
    cf::GetString(st, "version", version);
    cf::GetNumber(st, "modelRate", modelRate);
    cf::GetNumber(st, "hostRate", hostRate);
    cf::GetNumber(st, "resampling", resampling);
    cf::GetNumber(st, "inputGainDb", inG);
    cf::GetNumber(st, "outputGainDb", outG);
    cf::GetNumber(st, "inputSelect", sel);
    cf::GetNumber(st, "inPeakDb", inPk);
    cf::GetNumber(st, "outPeakDb", outPk);
    cf::GetNumber(st, "running", running);
    CFRelease(st);
    static const char* sels[] = {"left", "right", "mix"};
    printf("NAM Bridge driver %s  (%s, device @ %.0f Hz)\n", version.c_str(), running ? "running" : "idle", hostRate);
    if (loaded)
        printf("  model: %s [%s @ %.0f Hz]%s\n", name.c_str(), arch.c_str(), modelRate,
               resampling ? "  (resampling)" : "");
    else
        printf("  model: none loaded (input is silent)\n");
    printf("  input gain %.1f dB, output gain %.1f dB, source channel: %s\n", inG, outG,
           sels[std::clamp(static_cast<int>(sel), 0, 2)]);
    printf("  peaks: in %.1f dBFS, out %.1f dBFS\n", inPk, outPk);
    if (!error.empty())
        printf("  last error: %s\n", error.c_str());
    return 0;
}

int CmdDriver(const Args& a)
{
    const AudioObjectID dev = FindBridgeDevice();
    if (dev == kAudioObjectUnknown)
    {
        fprintf(stderr, "NAM Bridge device not found. Is the driver installed? (scripts/install-driver.sh)\n");
        return 1;
    }
    const std::string sub = a.positional.empty() ? "status" : a.positional[0];
    if (sub == "status")
        return PrintStatus(dev);
    if (sub == "watch")
    {
        while (!gStop.load())
        {
            PrintStatus(dev);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 0;
    }
    if (sub == "load")
    {
        if (a.positional.size() < 2)
        {
            fprintf(stderr, "usage: nam-bridge driver load FILE.nam\n");
            return 1;
        }
        const std::string json = ReadFile(a.positional[1]);
        if (json.empty())
        {
            fprintf(stderr, "Cannot read %s\n", a.positional[1].c_str());
            return 1;
        }
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetString(d, "json", json);
        cf::SetString(d, "fileName", std::filesystem::path(a.positional[1]).stem().string());
        const bool ok = SetDict(dev, protocol::kPropModel, d);
        CFRelease(d);
        if (!ok)
            return 1;
        return PrintStatus(dev);
    }
    if (sub == "unload")
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetBool(d, "unload", true);
        const bool ok = SetDict(dev, protocol::kPropModel, d);
        CFRelease(d);
        return ok ? PrintStatus(dev) : 1;
    }
    if (sub == "set")
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        if (a.Has("in-gain"))
            cf::SetNumber(d, "inputGainDb", a.Num("in-gain", 0));
        if (a.Has("out-gain"))
            cf::SetNumber(d, "outputGainDb", a.Num("out-gain", 0));
        if (a.Has("input"))
        {
            const std::string s = a.Get("input");
            cf::SetNumber(d, "inputSelect", s == "R" || s == "r" || s == "right" ? 1 : (s == "mix" ? 2 : 0));
        }
        const bool ok = SetDict(dev, protocol::kPropConfig, d);
        CFRelease(d);
        return ok ? PrintStatus(dev) : 1;
    }
    fprintf(stderr, "unknown driver command: %s\n", sub.c_str());
    return 1;
}

// ---------------------------------------------------------------------------------------
// loopcheck: act like ToneX. Play a test signal on one output channel of a device, record
// an input channel, align, and compare against an offline render of the same model.

std::vector<float> TestSignal(double rate, double seconds)
{
    std::vector<float> x(static_cast<size_t>(rate * seconds), 0.0f);
    // 0.25 s silence, then decaying plucks at varying levels, then a log sweep.
    const size_t lead = static_cast<size_t>(rate * 0.25);
    const size_t pluckEnd = lead + static_cast<size_t>(rate * (seconds - 0.25) * 0.6);
    for (size_t i = lead; i < pluckEnd; i++)
    {
        const double t = static_cast<double>((i - lead) % static_cast<size_t>(rate / 3)) / rate;
        const size_t k = (i - lead) / static_cast<size_t>(rate / 3);
        const double amp = 0.05 + 0.1 * static_cast<double>(k % 4);
        const double f = 82.4 * std::pow(2.0, static_cast<double>(k % 7) / 4.0);
        x[i] = static_cast<float>(amp * std::exp(-5 * t) *
                                  (std::sin(2 * M_PI * f * t) + 0.4 * std::sin(2 * M_PI * 2 * f * t)));
    }
    const double sweepLen = static_cast<double>(x.size() - pluckEnd) / rate;
    for (size_t i = pluckEnd; i < x.size(); i++)
    {
        const double t = static_cast<double>(i - pluckEnd) / rate;
        const double f0 = 40, f1 = 12000, k = std::log(f1 / f0) / sweepLen;
        x[i] = static_cast<float>(0.2 * std::sin(2 * M_PI * f0 * (std::exp(k * t) - 1) / k));
    }
    return x;
}

struct LoopCtx
{
    const std::vector<float>* play = nullptr;
    std::vector<float> rec;
    int playCh = 0, recCh = 0;
    std::atomic<size_t> playPos{0}, recPos{0};
};

OSStatus LoopProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                  AudioBufferList* out, const AudioTimeStamp*, void* user)
{
    auto* c = static_cast<LoopCtx*>(user);
    UInt32 b, o, s;
    if (in && LocateChannel(in, c->recCh, b, o, s) && in->mBuffers[b].mData)
    {
        const UInt32 frames = in->mBuffers[b].mDataByteSize / (sizeof(float) * s);
        const auto* d = static_cast<const float*>(in->mBuffers[b].mData);
        size_t pos = c->recPos.load();
        for (UInt32 i = 0; i < frames && pos < c->rec.size(); i++)
            c->rec[pos++] = d[i * s + o];
        c->recPos.store(pos);
    }
    if (out)
    {
        for (UInt32 k = 0; k < out->mNumberBuffers; k++)
            if (out->mBuffers[k].mData)
                memset(out->mBuffers[k].mData, 0, out->mBuffers[k].mDataByteSize);
        if (LocateChannel(out, c->playCh, b, o, s) && out->mBuffers[b].mData)
        {
            const UInt32 frames = out->mBuffers[b].mDataByteSize / (sizeof(float) * s);
            auto* d = static_cast<float*>(out->mBuffers[b].mData);
            size_t pos = c->playPos.load();
            for (UInt32 i = 0; i < frames && pos < c->play->size(); i++)
                d[i * s + o] = (*c->play)[pos++];
            c->playPos.store(pos);
        }
    }
    return noErr;
}

int CmdLoopcheck(const Args& a)
{
    if (!a.Has("model"))
    {
        fprintf(stderr, "loopcheck needs --model F.nam (the model the device/bridge is running)\n");
        return 1;
    }
    const AudioObjectID dev = FindDevice(a.Get("device", "BlackHole 16ch"));
    if (dev == kAudioObjectUnknown)
    {
        fprintf(stderr, "Device not found.\n");
        return 1;
    }
    if (a.Has("rate") && !SetNominalRate(dev, a.Num("rate", 48000)))
        fprintf(stderr, "Warning: could not set device rate to %s\n", a.Get("rate").c_str());
    const double rate = NominalRate(dev);
    const auto x = TestSignal(rate, a.Num("seconds", 4));

    // Offline reference through the same engine at the device rate.
    NamEngine ref;
    ref.SetHostRate(rate);
    ref.SetInputGainDb(static_cast<float>(a.Num("in-gain", 0)));
    ref.SetOutputGainDb(static_cast<float>(a.Num("out-gain", 0)));
    std::string err;
    if (!ref.LoadFromFile(a.Get("model"), err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); i += 256)
        ref.Process(x.data() + i, y.data() + i, static_cast<int>(std::min<size_t>(256, x.size() - i)));

    LoopCtx ctx;
    ctx.play = &x;
    ctx.rec.assign(x.size() + static_cast<size_t>(rate), 0.0f);
    ctx.playCh = static_cast<int>(a.Num("play", 1)) - 1;
    ctx.recCh = static_cast<int>(a.Num("rec", 3)) - 1;
    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(dev, LoopProc, &ctx, &procId) != noErr || AudioDeviceStart(dev, procId) != noErr)
    {
        fprintf(stderr, "Could not start the device.\n");
        return 1;
    }
    printf("Playing %.1f s test signal on %s ch%d @ %.0f Hz, recording ch%d...\n", x.size() / rate,
           StringProp(dev, kAudioObjectPropertyName).c_str(), ctx.playCh + 1, rate, ctx.recCh + 1);
    while (ctx.recPos.load() < ctx.rec.size() && !gStop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    AudioDeviceStop(dev, procId);
    AudioDeviceDestroyIOProcID(dev, procId);

    const auto& r = ctx.rec;
    float recPeak = 0;
    for (float v : r)
        recPeak = std::max(recPeak, std::abs(v));
    if (recPeak < 1e-6f)
    {
        printf("Recorded silence. Either nothing is processing the loop, or this terminal lacks Microphone\n"
               "permission (System Settings > Privacy & Security > Microphone).\n");
        return 1;
    }

    // Find the round-trip delay by cross-correlation over the first 1.5 s of signal.
    const size_t maxLag = std::min<size_t>(static_cast<size_t>(rate * 0.5), r.size() - y.size());
    const size_t win = std::min<size_t>(static_cast<size_t>(rate * 1.5), y.size());
    size_t bestLag = 0;
    double best = -1e300;
    for (size_t lag = 0; lag <= maxLag; lag++)
    {
        double acc = 0;
        for (size_t i = 0; i < win; i++)
            acc += static_cast<double>(y[i]) * r[i + lag];
        if (acc > best)
        {
            best = acc;
            bestLag = lag;
        }
    }
    double eErr = 0, eRef = 0;
    for (size_t i = 0; i < y.size(); i++)
    {
        const double d = static_cast<double>(r[i + bestLag]) - y[i];
        eErr += d * d;
        eRef += static_cast<double>(y[i]) * y[i];
    }
    const double errDb = eErr > 0 ? 10 * std::log10(eErr / eRef) : -300;
    printf("Round-trip delay: %zu frames (%.2f ms)\n", bestLag, 1000.0 * bestLag / rate);
    const bool resampled = ref.Info().resampling;
    printf("Recorded peak %.1f dBFS. Error vs offline NAM render: %.1f dB %s\n", 20 * std::log10(recPeak), errDb,
           errDb < -60 ? "(transparent)" : (errDb < -30 ? "(close)" : "(MISMATCH)"));
    if (resampled)
        printf("Device runs at %.0f Hz, model at %.0f Hz: both sides resample, and the resampler's sub-sample\n"
               "phase depends on when audio started, so expect a smooth error that rises with frequency\n"
               "(about -40 dB overall). Run the device at the model's rate for a bit-exact path.\n",
               rate, ref.Info().modelRate);
    if (a.Has("save"))
    {
        WriteWavFloat(a.Get("save") + "-sent.wav", x, rate);
        WriteWavFloat(a.Get("save") + "-recorded.wav", r, rate);
        WriteWavFloat(a.Get("save") + "-expected.wav", y, rate);
    }
    return errDb < (resampled ? -30 : -60) ? 0 : 1;
}

#endif // NB_COREAUDIO

// ---------------------------------------------------------------------------------------
// compare: are two renders the same? (used to check a new platform against test vectors)

int CmdCompare(const Args& a)
{
    if (a.positional.size() < 2)
    {
        fprintf(stderr, "usage: nam-bridge compare A.wav B.wav [--max-error-db -80]\n");
        return 1;
    }
    std::vector<float> x, y;
    double rx = 0, ry = 0;
    if (dsp::wav::Load(a.positional[0].c_str(), x, rx) != dsp::wav::LoadReturnCode::SUCCESS ||
        dsp::wav::Load(a.positional[1].c_str(), y, ry) != dsp::wav::LoadReturnCode::SUCCESS)
    {
        fprintf(stderr, "Cannot read one of the files.\n");
        return 1;
    }
    if (x.size() != y.size() || std::abs(rx - ry) > 0.5)
    {
        fprintf(stderr, "Different length or rate: %zu @ %.0f vs %zu @ %.0f\n", x.size(), rx, y.size(), ry);
        return 1;
    }
    double maxDiff = 0, eErr = 0, eRef = 0;
    for (size_t i = 0; i < x.size(); i++)
    {
        const double d = static_cast<double>(x[i]) - y[i];
        maxDiff = std::max(maxDiff, std::abs(d));
        eErr += d * d;
        eRef += static_cast<double>(y[i]) * y[i];
    }
    const double errDb = eErr > 0 ? 10 * std::log10(eErr / eRef) : -300;
    const double limit = a.Num("max-error-db", -80);
    printf("%zu samples, max diff %.3e, error %.1f dB -> %s\n", x.size(), maxDiff, errDb,
           eErr == 0 ? "bit-identical" : (errDb <= limit ? "PASS" : "FAIL"));
    return errDb <= limit ? 0 : 1;
}

// ---------------------------------------------------------------------------------------
// selftest

int Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    return ok ? 0 : 1;
}

int CmdSelftest(const Args& a)
{
    const std::string model = a.positional.empty() ? "" : a.positional[0];
    if (model.empty())
    {
        fprintf(stderr, "usage: nam-bridge selftest MODEL.nam\n");
        return 1;
    }
    int fails = 0;
    printf("Engine / loopback self-test with %s\n", model.c_str());

    // A deterministic guitar-ish test signal: decaying plucks.
    const double rate = 48000;
    std::vector<float> x(static_cast<size_t>(rate * 2));
    for (size_t i = 0; i < x.size(); i++)
    {
        const double t = static_cast<double>(i % 24000) / rate;
        x[i] = static_cast<float>(0.3 * std::exp(-4 * t) * std::sin(2 * M_PI * 110 * t) +
                                  0.1 * std::exp(-6 * t) * std::sin(2 * M_PI * 330 * t));
    }

    // 1. No model -> silence.
    {
        NamEngine e;
        std::vector<float> y(512, 1.0f);
        e.Process(x.data(), y.data(), 512);
        fails += Check(std::all_of(y.begin(), y.end(), [](float v) { return v == 0.0f; }), "no model -> silence");
    }

    // 2. Loopback returns exactly what the engine produces, at the same sample time.
    NamEngine ref, eng;
    std::string err;
    ref.SetHostRate(rate);
    eng.SetHostRate(rate);
    fails += Check(ref.LoadFromFile(model, err) && eng.LoadFromFile(model, err), "model loads");
    std::vector<float> expect(x.size());
    for (size_t i = 0; i < x.size(); i += 512)
        ref.Process(x.data() + i, expect.data() + i, static_cast<int>(std::min<size_t>(512, x.size() - i)));

    Loopback lb(eng);
    std::vector<float> stereo(x.size() * 2);
    for (size_t i = 0; i < x.size(); i++)
    {
        stereo[2 * i] = x[i];
        stereo[2 * i + 1] = 0.0f; // ToneX-style: signal on the left only
    }
    const double t0 = 123456.0; // arbitrary start time, crossing ring wraps
    std::vector<float> readBack(x.size() * 2);
    double maxDiff = 0;
    for (size_t i = 0; i < x.size(); i += 512)
    {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(512, x.size() - i));
        lb.Write(t0 + static_cast<double>(i), stereo.data() + 2 * i, n, 2);
        lb.Read(t0 + static_cast<double>(i), readBack.data() + 2 * i, n, 2);
    }
    float energy = 0;
    for (size_t i = 0; i < x.size(); i++)
    {
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(readBack[2 * i] - expect[i])));
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(readBack[2 * i + 1] - expect[i])));
        energy += expect[i] * expect[i];
    }
    fails += Check(energy > 1e-3f, "model produces signal");
    fails += Check(maxDiff < 1e-6, "loopback == direct engine output (both channels)");

    // 3. Frames never written read as silence; old frames expire.
    std::vector<float> later(1024, 1.0f);
    lb.Read(t0 + static_cast<double>(x.size()) + 10000, later.data(), 512, 2);
    fails += Check(std::all_of(later.begin(), later.end(), [](float v) { return v == 0.0f; }),
                   "unwritten time -> silence");

    // 4. Input select: Right channel carries nothing here -> silence-in.
    {
        NamEngine e2;
        e2.SetHostRate(rate);
        e2.LoadFromFile(model, err);
        Loopback lb2(e2);
        lb2.SetInputSelect(InputSelect::Right);
        lb2.Write(0, stereo.data(), 512, 2);
        float ip, op;
        e2.TakePeaks(ip, op);
        fails += Check(ip == 0.0f, "input select = right ignores left-channel signal");
    }

    // 5. Resampling path (44.1 kHz host) runs and is level-consistent with 48 kHz.
    {
        NamEngine e3;
        e3.SetHostRate(44100);
        fails += Check(e3.LoadFromFile(model, err) && e3.Info().resampling, "44.1 kHz host -> resampling active");
        std::vector<float> x44(static_cast<size_t>(44100 * 2)), y44(x44.size());
        for (size_t i = 0; i < x44.size(); i++)
        {
            const double t = static_cast<double>(i % 22050) / 44100;
            x44[i] = static_cast<float>(0.3 * std::exp(-4 * t) * std::sin(2 * M_PI * 110 * t) +
                                        0.1 * std::exp(-6 * t) * std::sin(2 * M_PI * 330 * t));
        }
        for (size_t i = 0; i < x44.size(); i += 333) // odd block size on purpose
            e3.Process(x44.data() + i, y44.data() + i, static_cast<int>(std::min<size_t>(333, x44.size() - i)));
        double e44 = 0, e48 = 0;
        for (float v : y44)
            e44 += v * v;
        for (float v : expect)
            e48 += v * v;
        const double rms44 = std::sqrt(e44 / y44.size()), rms48 = std::sqrt(e48 / expect.size());
        const double diffDb = 20 * std::log10(rms44 / rms48);
        printf("         RMS 48k %.4f vs 44.1k %.4f (%.2f dB)\n", rms48, rms44, diffDb);
        fails += Check(std::abs(diffDb) < 0.5, "resampled output level within 0.5 dB");
    }

    // 6. Hot-swapping a model while "audio" runs on another thread.
    {
        NamEngine e4;
        e4.SetHostRate(rate);
        e4.LoadFromFile(model, err);
        std::atomic<bool> run{true};
        std::thread rt([&] {
            std::vector<float> y(256);
            size_t i = 0;
            while (run.load())
            {
                e4.Process(x.data() + (i % (x.size() - 256)), y.data(), 256);
                i += 256;
            }
        });
        bool ok = true;
        for (int k = 0; k < 10; k++)
            ok = ok && e4.LoadFromFile(model, err);
        e4.Unload();
        e4.LoadFromFile(model, err);
        run.store(false);
        rt.join();
        fails += Check(ok, "10 hot swaps + unload while processing");
    }

    printf(fails ? "%d check(s) failed\n" : "All checks passed\n", fails);
    return fails ? 1 : 0;
}

void Usage()
{
    puts("nam-bridge: run NAM captures as a virtual amp for ToneX capture\n"
         "\n"
         "  nam-bridge devices\n"
         "  nam-bridge probe  [--device NAME] [--seconds N]\n"
         "  nam-bridge bridge --model F.nam [--device \"BlackHole 16ch\"] [--in 1] [--out 3]\n"
         "                    [--in-gain dB] [--out-gain dB] [--rate 48000] [--seconds N]\n"
         "  nam-bridge render F.nam IN.wav OUT.wav [--in-gain dB] [--out-gain dB]\n"
         "  nam-bridge driver status|watch|load F.nam|unload\n"
         "  nam-bridge driver set [--in-gain dB] [--out-gain dB] [--input left|right|mix]\n"
         "  nam-bridge selftest F.nam\n"
         "  nam-bridge compare A.wav B.wav [--max-error-db -80]\n"
         "  nam-bridge loopcheck --model F.nam [--device NAME] [--play 1] [--rec 3] [--rate R] [--save PREFIX]");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        Usage();
        return 1;
    }
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    const std::string cmd = argv[1];
    const Args a = Parse(argc, argv, 2);
    try
    {
#if NB_COREAUDIO
        if (cmd == "devices")
            return CmdDevices();
        if (cmd == "probe")
            return CmdProbe(a);
        if (cmd == "bridge")
            return CmdBridge(a);
        if (cmd == "driver")
            return CmdDriver(a);
        if (cmd == "loopcheck")
            return CmdLoopcheck(a);
#endif
        if (cmd == "render")
            return CmdRender(a);
        if (cmd == "selftest")
            return CmdSelftest(a);
        if (cmd == "compare")
            return CmdCompare(a);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    Usage();
    return 1;
}
