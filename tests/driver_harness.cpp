// Loads NAMBridge.driver in-process the way coreaudiod does (CFPlugIn entry point + a fake
// AudioServerPlugInHost), then exercises properties, sample-rate changes and the IO path.
// Lets us catch crashes and logic errors before installing into the real audio server.
//
//   driver_harness path/to/NAMBridge.driver model.nam

#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "CFUtil.h"
#include "DriverProtocol.h"
#include "NamEngine.h"

using namespace nambridge;

namespace {

AudioServerPlugInDriverRef gDriver = nullptr;
CFMutableDictionaryRef gStorage = nullptr;
int gFails = 0;

void Check(bool ok, const std::string& what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok)
        gFails++;
}

// ---- fake host ----
OSStatus HostPropertiesChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress*)
{
    return noErr;
}
OSStatus HostCopyFromStorage(AudioServerPlugInHostRef, CFStringRef key, CFPropertyListRef* out)
{
    CFPropertyListRef v = CFDictionaryGetValue(gStorage, key);
    *out = v ? CFRetain(v) : nullptr;
    return v ? noErr : kAudioHardwareUnspecifiedError;
}
OSStatus HostWriteToStorage(AudioServerPlugInHostRef, CFStringRef key, CFPropertyListRef data)
{
    CFDictionarySetValue(gStorage, key, data);
    return noErr;
}
OSStatus HostDeleteFromStorage(AudioServerPlugInHostRef, CFStringRef key)
{
    CFDictionaryRemoveValue(gStorage, key);
    return noErr;
}
OSStatus HostRequestConfigChange(AudioServerPlugInHostRef, AudioObjectID device, UInt64 action, void* info)
{
    // The real HAL stops IO, then calls back; we call back immediately.
    return (*gDriver)->PerformDeviceConfigurationChange(gDriver, device, action, info);
}

AudioServerPlugInHostInterface gHost = {HostPropertiesChanged, HostCopyFromStorage, HostWriteToStorage,
                                        HostDeleteFromStorage, HostRequestConfigChange};

// ---- property helpers ----
AudioObjectPropertyAddress Addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal)
{
    return {s, sc, kAudioObjectPropertyElementMain};
}

template <typename T>
std::vector<T> GetArray(AudioObjectID obj, AudioObjectPropertyAddress a)
{
    UInt32 size = 0;
    if ((*gDriver)->GetPropertyDataSize(gDriver, obj, 0, &a, 0, nullptr, &size) != noErr)
        return {};
    std::vector<T> v(size / sizeof(T));
    (*gDriver)->GetPropertyData(gDriver, obj, 0, &a, 0, nullptr, size, &size, v.data());
    v.resize(size / sizeof(T));
    return v;
}

template <typename T>
T Get(AudioObjectID obj, AudioObjectPropertyAddress a)
{
    T v{};
    UInt32 size = sizeof v;
    (*gDriver)->GetPropertyData(gDriver, obj, 0, &a, 0, nullptr, size, &size, &v);
    return v;
}

template <typename T>
OSStatus Set(AudioObjectID obj, AudioObjectPropertyAddress a, T v)
{
    return (*gDriver)->SetPropertyData(gDriver, obj, 0, &a, 0, nullptr, sizeof v, &v);
}

CFDictionaryRef GetDict(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    return static_cast<CFDictionaryRef>(Get<CFPropertyListRef>(dev, Addr(sel)));
}

std::string ReadFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: driver_harness NAMBridge.driver model.nam\n");
        return 2;
    }
    const std::string bundle = argv[1], modelPath = argv[2];
    gStorage = cf::MakeDict();

    const std::string bin = bundle + "/Contents/MacOS/NAMBridge";
    void* h = dlopen(bin.c_str(), RTLD_NOW | RTLD_LOCAL);
    Check(h != nullptr, "dlopen driver binary");
    if (!h)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    using Entry = void* (*)(CFAllocatorRef, CFUUIDRef);
    auto entry = reinterpret_cast<Entry>(dlsym(h, "NAMBridgeEntryPoint"));
    Check(entry != nullptr, "entry point exported");
    gDriver = static_cast<AudioServerPlugInDriverRef>(entry(kCFAllocatorDefault, kAudioServerPlugInTypeUUID));
    Check(gDriver != nullptr, "factory returns a driver");
    if (!gDriver)
        return 1;

    AudioServerPlugInHostRef host = &gHost;
    Check((*gDriver)->Initialize(gDriver, host) == noErr, "Initialize");

    auto devices = GetArray<AudioObjectID>(kAudioObjectPlugInObject, Addr(kAudioPlugInPropertyDeviceList));
    Check(devices.size() == 1, "plug-in publishes one device");
    if (devices.empty())
        return 1;
    const AudioObjectID dev = devices[0];

    CFStringRef uid = Get<CFStringRef>(dev, Addr(kAudioDevicePropertyDeviceUID));
    Check(cf::ToString(uid) == protocol::kDeviceUID, "device UID = " + cf::ToString(uid));
    CFRelease(uid);
    CFStringRef name = Get<CFStringRef>(dev, Addr(kAudioObjectPropertyName));
    Check(cf::ToString(name) == "NAM Bridge", "device name = " + cf::ToString(name));
    CFRelease(name);

    auto inStreams = GetArray<AudioObjectID>(dev, Addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput));
    auto outStreams = GetArray<AudioObjectID>(dev, Addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput));
    Check(inStreams.size() == 1 && outStreams.size() == 1, "one input + one output stream");
    if (inStreams.empty() || outStreams.empty())
        return 1;
    const AudioObjectID inS = inStreams[0], outS = outStreams[0];

    auto rates = GetArray<AudioValueRange>(dev, Addr(kAudioDevicePropertyAvailableNominalSampleRates));
    Check(rates.size() == 4, "4 sample rates offered (" + std::to_string(rates.size()) + ")");
    Check(Get<Float64>(dev, Addr(kAudioDevicePropertyNominalSampleRate)) == 48000.0, "default rate 48 kHz");
    auto fmt = Get<AudioStreamBasicDescription>(outS, Addr(kAudioStreamPropertyPhysicalFormat));
    Check(fmt.mFormatFlags & kAudioFormatFlagIsFloat && fmt.mBitsPerChannel == 32 && fmt.mChannelsPerFrame == 2,
          "stream format float32 stereo");
    auto fmts = GetArray<AudioStreamRangedDescription>(outS, Addr(kAudioStreamPropertyAvailablePhysicalFormats));
    Check(fmts.size() == 4, "stream lists 4 formats");

    // Status before any model.
    CFDictionaryRef st = GetDict(dev, protocol::kPropStatus);
    double loaded = 1;
    cf::GetNumber(st, "loaded", loaded);
    Check(st && loaded == 0, "status: no model at start");
    if (st)
        CFRelease(st);

    // Load model through the custom property.
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetString(d, "json", ReadFile(modelPath));
        cf::SetString(d, "fileName", std::filesystem::path(modelPath).stem().string());
        CFPropertyListRef v = d;
        Check(Set(dev, Addr(protocol::kPropModel), v) == noErr, "set model property");
        CFRelease(d);
    }
    st = GetDict(dev, protocol::kPropStatus);
    std::string modelName, lastError;
    cf::GetNumber(st, "loaded", loaded);
    cf::GetString(st, "modelName", modelName);
    cf::GetString(st, "lastError", lastError);
    Check(loaded == 1, "status: model loaded -> " + modelName + (lastError.empty() ? "" : " err=" + lastError));
    CFRelease(st);
    Check(CFDictionaryContainsKey(gStorage, CFSTR("modelJson")), "model persisted to storage");

    // Bad model is rejected without unloading the good one.
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetString(d, "json", "{not json");
        CFPropertyListRef v = d;
        Set(dev, Addr(protocol::kPropModel), v);
        CFRelease(d);
        st = GetDict(dev, protocol::kPropStatus);
        cf::GetNumber(st, "loaded", loaded);
        cf::GetString(st, "lastError", lastError);
        Check(loaded == 1 && !lastError.empty(), "invalid model rejected, previous kept (" + lastError.substr(0, 40) + ")");
        CFRelease(st);
    }

    // Config.
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetNumber(d, "inputGainDb", -3.0);
        cf::SetNumber(d, "outputGainDb", -6.0);
        CFPropertyListRef v = d;
        Check(Set(dev, Addr(protocol::kPropConfig), v) == noErr, "set config property");
        CFRelease(d);
        CFDictionaryRef c = GetDict(dev, protocol::kPropConfig);
        double ig = 0, og = 0;
        cf::GetNumber(c, "inputGainDb", ig);
        cf::GetNumber(c, "outputGainDb", og);
        Check(ig == -3.0 && og == -6.0, "config round-trips");
        CFRelease(c);
    }

    // Sample-rate change, both via the device and via a stream format.
    Check(Set(dev, Addr(kAudioDevicePropertyNominalSampleRate), Float64(44100.0)) == noErr, "set device rate 44.1k");
    Check(Get<Float64>(dev, Addr(kAudioDevicePropertyNominalSampleRate)) == 44100.0, "device now 44.1k");
    fmt = Get<AudioStreamBasicDescription>(inS, Addr(kAudioStreamPropertyPhysicalFormat));
    Check(fmt.mSampleRate == 44100.0, "stream format follows (44.1k)");
    st = GetDict(dev, protocol::kPropStatus);
    double resampling = 0;
    cf::GetNumber(st, "resampling", resampling);
    Check(resampling == 1, "engine resampling at 44.1k");
    CFRelease(st);
    AudioStreamBasicDescription f48 = fmt;
    f48.mSampleRate = 48000.0;
    Check(Set(outS, Addr(kAudioStreamPropertyPhysicalFormat), f48) == noErr, "set stream format 48k");
    Check(Get<Float64>(dev, Addr(kAudioDevicePropertyNominalSampleRate)) == 48000.0, "device follows stream (48k)");

    // ---- IO: emulate the HAL's cycle (ReadInput, then WriteMix) for one client ----
    {
        // Reset gains so we can compare against a plain engine.
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetNumber(d, "inputGainDb", 0.0);
        cf::SetNumber(d, "outputGainDb", 0.0);
        CFPropertyListRef v = d;
        Set(dev, Addr(protocol::kPropConfig), v);
        CFRelease(d);
    }
    AudioServerPlugInClientInfo client = {1, getpid(), false, CFSTR("harness")};
    Check((*gDriver)->AddDeviceClient(gDriver, dev, &client) == noErr, "AddDeviceClient");
    Check((*gDriver)->StartIO(gDriver, dev, 1) == noErr, "StartIO");
    Float64 sampleTime = 0;
    UInt64 hostTime = 0, seed = 0;
    Check((*gDriver)->GetZeroTimeStamp(gDriver, dev, 1, &sampleTime, &hostTime, &seed) == noErr, "GetZeroTimeStamp");

    const UInt32 N = 512;
    const int cycles = 200;
    const double rate = 48000;
    std::vector<float> sent, received;
    std::vector<float> outBuf(N * 2), inBuf(N * 2);
    const Float64 base = 1000000.0;
    for (int k = 0; k < cycles; k++)
    {
        AudioServerPlugInIOCycleInfo cyc = {};
        cyc.mIOCycleCounter = static_cast<UInt64>(k);
        cyc.mNominalIOBufferFrameSize = N;
        cyc.mInputTime.mSampleTime = base + k * N - N;  // input lags "now"
        cyc.mOutputTime.mSampleTime = base + k * N + N; // output leads "now"
        cyc.mInputTime.mFlags = cyc.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;

        Boolean will = false, inPlace = false;
        (*gDriver)->WillDoIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationReadInput, &will, &inPlace);
        if (will)
        {
            (*gDriver)->BeginIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc);
            (*gDriver)->DoIOOperation(gDriver, dev, inS, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc,
                                      inBuf.data(), nullptr);
            (*gDriver)->EndIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationReadInput, N, &cyc);
        }
        for (UInt32 i = 0; i < N; i++)
            received.push_back(inBuf[2 * i]);

        // ToneX-like output: signal on the left channel only.
        for (UInt32 i = 0; i < N; i++)
        {
            const double t = (static_cast<double>(k) * N + i) / rate;
            const float x = static_cast<float>(0.3 * std::sin(2 * M_PI * 110 * t) * std::exp(-2 * std::fmod(t, 0.5)));
            outBuf[2 * i] = x;
            outBuf[2 * i + 1] = 0.0f;
            sent.push_back(x);
        }
        (*gDriver)->WillDoIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace);
        if (will)
        {
            (*gDriver)->BeginIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc);
            (*gDriver)->DoIOOperation(gDriver, dev, outS, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc,
                                      outBuf.data(), nullptr);
            (*gDriver)->EndIOOperation(gDriver, dev, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc);
        }
    }
    Check((*gDriver)->StopIO(gDriver, dev, 1) == noErr, "StopIO");
    (*gDriver)->RemoveDeviceClient(gDriver, dev, &client);

    // Output written at sample time T is readable at input time T. Output time of cycle k is
    // input time of cycle k+2, so received[i + 2N] corresponds to sent[i].
    NamEngine ref;
    ref.SetHostRate(rate);
    std::string err;
    ref.LoadFromFile(modelPath, err);
    std::vector<float> expect(sent.size());
    for (size_t i = 0; i < sent.size(); i += N)
        ref.Process(sent.data() + i, expect.data() + i, N);
    double maxDiff = 0, energy = 0;
    for (size_t i = 0; i + 2 * N < received.size(); i++)
    {
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(received[i + 2 * N] - expect[i])));
        energy += expect[i] * expect[i];
    }
    bool leadSilent = true;
    for (size_t i = 0; i < 2 * N; i++)
        leadSilent = leadSilent && received[i] == 0.0f;
    Check(energy > 1.0, "device returns processed signal");
    Check(leadSilent, "no stale audio before the first write");
    char buf[96];
    snprintf(buf, sizeof buf, "input == NAM(output) sample-exact (max diff %.2e)", maxDiff);
    Check(maxDiff < 1e-6, buf);

    st = GetDict(dev, protocol::kPropStatus);
    double inPk = -200, outPk = -200;
    cf::GetNumber(st, "inPeakDb", inPk);
    cf::GetNumber(st, "outPeakDb", outPk);
    snprintf(buf, sizeof buf, "status meters moved (in %.1f dB, out %.1f dB)", inPk, outPk);
    Check(inPk > -20 && outPk > -40, buf);
    CFRelease(st);

    // Unload.
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetBool(d, "unload", true);
        CFPropertyListRef v = d;
        Set(dev, Addr(protocol::kPropModel), v);
        CFRelease(d);
        st = GetDict(dev, protocol::kPropStatus);
        cf::GetNumber(st, "loaded", loaded);
        Check(loaded == 0 && !CFDictionaryContainsKey(gStorage, CFSTR("modelJson")), "unload clears model + storage");
        CFRelease(st);
    }

    printf(gFails ? "%d check(s) failed\n" : "All driver checks passed\n", gFails);
    return gFails ? 1 : 0;
}
