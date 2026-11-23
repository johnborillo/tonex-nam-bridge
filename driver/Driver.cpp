// NAM Bridge: a CoreAudio virtual audio device that behaves like a reamp box wired to an amp.
//
// Whatever a client (ToneX Modeler) plays to "NAM Bridge" output is processed by the loaded
// NAM model and comes back on "NAM Bridge" input at the same sample time. Controlled by the
// menu-bar app / CLI through custom properties (see engine/DriverProtocol.h).

#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CFUtil.h"
#include "DriverProtocol.h"
#include "Loopback.h"
#include "NamEngine.h"

#ifndef NAMBRIDGE_VERSION
#define NAMBRIDGE_VERSION "dev"
#endif

using namespace nambridge;

namespace {

constexpr UInt32 kChannels = 2;
constexpr Float64 kDefaultRate = 48000.0;
const std::vector<Float64> kRates = {44100.0, 48000.0, 88200.0, 96000.0};

AudioStreamBasicDescription FloatFormat(Float64 rate)
{
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = kChannels;
    f.mBytesPerFrame = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4 * kChannels;
    return f;
}

std::vector<AudioStreamRangedDescription> AllFormats()
{
    std::vector<AudioStreamRangedDescription> out;
    for (auto r : kRates)
    {
        AudioStreamRangedDescription d = {};
        d.mFormat = FloatFormat(r);
        d.mSampleRateRange.mMinimum = r;
        d.mSampleRateRange.mMaximum = r;
        out.push_back(d);
    }
    return out;
}

class BridgeDevice;

// A stream whose format always follows the device's nominal rate (libASPL streams have a
// single fixed format by default).
class BridgeStream : public aspl::Stream
{
public:
    BridgeStream(std::shared_ptr<const aspl::Context> ctx, std::shared_ptr<aspl::Device> dev,
                 const aspl::StreamParameters& params, BridgeDevice* owner)
    : aspl::Stream(std::move(ctx), std::move(dev), params)
    , owner_(owner)
    {
    }

    AudioStreamBasicDescription GetPhysicalFormat() const override;
    AudioStreamBasicDescription GetVirtualFormat() const override { return GetPhysicalFormat(); }
    std::vector<AudioStreamRangedDescription> GetAvailablePhysicalFormats() const override { return AllFormats(); }
    std::vector<AudioStreamRangedDescription> GetAvailableVirtualFormats() const override { return AllFormats(); }

protected:
    OSStatus SetPhysicalFormatImpl(const AudioStreamBasicDescription& format) override;
    OSStatus SetVirtualFormatImpl(const AudioStreamBasicDescription& format) override
    {
        return SetPhysicalFormatImpl(format);
    }

private:
    BridgeDevice* owner_;
};

class BridgeDevice : public aspl::Device, public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
    BridgeDevice(std::shared_ptr<const aspl::Context> ctx, const aspl::DeviceParameters& params)
    : aspl::Device(std::move(ctx), params)
    , loopback_(engine_)
    {
        engine_.SetHostRate(params.SampleRate);
        RegisterCustomProperty(protocol::kPropModel, *this, &BridgeDevice::GetModelProp, &BridgeDevice::SetModelProp);
        RegisterCustomProperty(protocol::kPropConfig, *this, &BridgeDevice::GetConfigProp,
                               &BridgeDevice::SetConfigProp);
        RegisterCustomProperty(protocol::kPropStatus, *this, &BridgeDevice::GetStatusProp);
    }

    void SetStorage(std::shared_ptr<aspl::Storage> s) { storage_ = std::move(s); }

    // Called from BridgeStream while a configuration change is being performed.
    void ApplyRate(Float64 rate) { SetNominalSampleRateImpl(rate); }

    // ---- persistence ----
    void Restore()
    {
        if (!storage_)
            return;
        auto [gainIn, okIn] = storage_->ReadFloat("inputGainDb");
        if (okIn)
            engine_.SetInputGainDb(static_cast<float>(gainIn));
        auto [gainOut, okOut] = storage_->ReadFloat("outputGainDb");
        if (okOut)
            engine_.SetOutputGainDb(static_cast<float>(gainOut));
        auto [sel, okSel] = storage_->ReadInt("inputSelect");
        if (okSel)
            loopback_.SetInputSelect(static_cast<InputSelect>(std::clamp<SInt64>(sel, 0, 2)));
        auto [json, okJson] = storage_->ReadString("modelJson");
        auto [name, okName] = storage_->ReadString("fileName");
        if (okJson && !json.empty())
        {
            std::string err;
            if (engine_.LoadFromJson(json, okName ? name : "", err))
                SetFileName(okName ? name : "");
            else
                SetError("Restoring last model failed: " + err);
        }
    }

    // ---- custom properties (non-realtime threads) ----

    CFPropertyListRef GetModelProp() const
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        std::lock_guard<std::mutex> lock(stateMutex_);
        cf::SetString(d, "fileName", fileName_);
        return d;
    }

    void SetModelProp(CFPropertyListRef value)
    {
        auto d = static_cast<CFDictionaryRef>(value);
        if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID())
        {
            SetError("Model property must be a dictionary");
            return;
        }
        double unload = 0;
        if (cf::GetNumber(d, "unload", unload) && unload != 0)
        {
            engine_.Unload();
            SetFileName("");
            SetError("");
            if (storage_)
            {
                storage_->Delete("modelJson");
                storage_->Delete("fileName");
            }
            return;
        }
        std::string json, fileName;
        if (!cf::GetString(d, "json", json) || json.empty())
        {
            SetError("Model property is missing 'json'");
            return;
        }
        cf::GetString(d, "fileName", fileName);
        std::string err;
        if (!engine_.LoadFromJson(json, fileName, err))
        {
            SetError(err);
            return;
        }
        SetFileName(fileName);
        SetError("");
        if (storage_)
        {
            storage_->WriteString("modelJson", json);
            storage_->WriteString("fileName", fileName);
        }
    }

    CFPropertyListRef GetConfigProp() const
    {
        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetNumber(d, "inputGainDb", engine_.InputGainDb());
        cf::SetNumber(d, "outputGainDb", engine_.OutputGainDb());
        cf::SetNumber(d, "inputSelect", static_cast<int>(loopback_.GetInputSelect()));
        return d;
    }

    void SetConfigProp(CFPropertyListRef value)
    {
        auto d = static_cast<CFDictionaryRef>(value);
        if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID())
            return;
        double v = 0;
        if (cf::GetNumber(d, "inputGainDb", v))
        {
            engine_.SetInputGainDb(static_cast<float>(v));
            if (storage_)
                storage_->WriteFloat("inputGainDb", engine_.InputGainDb());
        }
        if (cf::GetNumber(d, "outputGainDb", v))
        {
            engine_.SetOutputGainDb(static_cast<float>(v));
            if (storage_)
                storage_->WriteFloat("outputGainDb", engine_.OutputGainDb());
        }
        if (cf::GetNumber(d, "inputSelect", v))
        {
            const int s = std::clamp(static_cast<int>(v), 0, 2);
            loopback_.SetInputSelect(static_cast<InputSelect>(s));
            if (storage_)
                storage_->WriteInt("inputSelect", s);
        }
    }

    CFPropertyListRef GetStatusProp() const
    {
        const ModelInfo info = engine_.Info();
        float inPk = 0, outPk = 0;
        const_cast<NamEngine&>(engine_).TakePeaks(inPk, outPk);
        auto toDb = [](float x) { return x > 1e-10f ? 20.0 * std::log10(x) : -200.0; };

        CFMutableDictionaryRef d = cf::MakeDict();
        cf::SetBool(d, "loaded", info.loaded);
        cf::SetString(d, "modelName", info.name);
        cf::SetString(d, "architecture", info.architecture);
        cf::SetNumber(d, "modelRate", info.modelRate);
        cf::SetNumber(d, "hostRate", GetNominalSampleRate());
        cf::SetBool(d, "resampling", info.resampling);
        cf::SetBool(d, "hasLoudness", info.hasLoudness);
        cf::SetNumber(d, "loudnessDb", info.loudnessDb);
        cf::SetNumber(d, "inputGainDb", engine_.InputGainDb());
        cf::SetNumber(d, "outputGainDb", engine_.OutputGainDb());
        cf::SetNumber(d, "inputSelect", static_cast<int>(loopback_.GetInputSelect()));
        cf::SetNumber(d, "inPeakDb", toDb(inPk));
        cf::SetNumber(d, "outPeakDb", toDb(outPk));
        cf::SetBool(d, "running", GetIsRunning());
        cf::SetString(d, "version", NAMBRIDGE_VERSION);
        std::lock_guard<std::mutex> lock(stateMutex_);
        cf::SetString(d, "lastError", lastError_);
        cf::SetString(d, "fileName", fileName_);
        return d;
    }

    // ---- control requests (non-realtime) ----

    OSStatus OnStartIO() override
    {
        engine_.SetHostRate(GetNominalSampleRate());
        loopback_.Clear();
        return kAudioHardwareNoError;
    }

    // ---- IO (realtime) ----

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream, Float64 zeroTimestamp, Float64 timestamp,
                            const void* buff, UInt32 buffBytesSize) override
    {
        const UInt32 frames = buffBytesSize / (sizeof(Float32) * kChannels);
        loopback_.Write(timestamp, static_cast<const Float32*>(buff), frames, kChannels);
    }

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client, const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp, Float64 timestamp, void* bytes, UInt32 bytesCount) override
    {
        const UInt32 frames = bytesCount / (sizeof(Float32) * kChannels);
        loopback_.Read(timestamp, static_cast<Float32*>(bytes), frames, kChannels);
    }

protected:
    OSStatus SetNominalSampleRateImpl(Float64 rate) override
    {
        const OSStatus st = aspl::Device::SetNominalSampleRateImpl(rate);
        engine_.SetHostRate(rate);
        if (storage_)
            storage_->WriteFloat("sampleRate", rate);
        return st;
    }

private:
    void SetError(const std::string& e)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastError_ = e;
    }
    void SetFileName(const std::string& n)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        fileName_ = n;
    }

    NamEngine engine_;
    Loopback loopback_;
    std::shared_ptr<aspl::Storage> storage_;

    mutable std::mutex stateMutex_;
    std::string lastError_;
    std::string fileName_;
};

AudioStreamBasicDescription BridgeStream::GetPhysicalFormat() const
{
    return FloatFormat(owner_->GetNominalSampleRate());
}

OSStatus BridgeStream::SetPhysicalFormatImpl(const AudioStreamBasicDescription& format)
{
    if (std::abs(format.mSampleRate - owner_->GetNominalSampleRate()) > 0.5)
        owner_->ApplyRate(format.mSampleRate);
    return kAudioHardwareNoError;
}

class InitHandler : public aspl::DriverRequestHandler
{
public:
    InitHandler(std::shared_ptr<aspl::Driver>* driver, std::shared_ptr<BridgeDevice> device)
    : driver_(driver)
    , device_(std::move(device))
    {
    }

    OSStatus OnInitialize() override
    {
        // Storage is only usable once the host has initialized the plug-in.
        device_->SetStorage((*driver_)->GetStorage());
        auto [rate, ok] = (*driver_)->GetStorage()->ReadFloat("sampleRate");
        if (ok && std::find(kRates.begin(), kRates.end(), rate) != kRates.end())
            device_->SetNominalSampleRateAsync(rate);
        device_->Restore();
        return kAudioHardwareNoError;
    }

private:
    std::shared_ptr<aspl::Driver>* driver_;
    std::shared_ptr<BridgeDevice> device_;
};

std::shared_ptr<aspl::Driver> CreateDriver()
{
    static std::shared_ptr<aspl::Driver> driver;

    // Quiet by default; build with -DNAMBRIDGE_TRACE to log every HAL call to syslog.
#ifdef NAMBRIDGE_TRACE
    auto context = std::make_shared<aspl::Context>(std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Syslog));
#else
    auto context = std::make_shared<aspl::Context>(std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Noop));
#endif

    aspl::DeviceParameters params;
    params.Name = protocol::kDeviceName;
    params.Manufacturer = "tonex-nam-bridge";
    params.DeviceUID = protocol::kDeviceUID;
    params.ModelUID = "NAMBridge_Model_UID";
    params.SampleRate = static_cast<UInt32>(kDefaultRate);
    params.ChannelCount = kChannels;
    params.EnableMixing = true;
    params.CanBeDefault = false; // keep system audio away from the amp
    params.CanBeDefaultForSystemSounds = false;

    auto device = std::make_shared<BridgeDevice>(context, params);

    std::vector<AudioValueRange> rates;
    for (auto r : kRates)
        rates.push_back({r, r});
    device->SetAvailableSampleRatesAsync(rates);

    for (auto dir : {aspl::Direction::Output, aspl::Direction::Input})
    {
        aspl::StreamParameters sp;
        sp.Direction = dir;
        sp.StartingChannel = 1;
        sp.Format = FloatFormat(kDefaultRate);
        device->AddStreamAsync(std::make_shared<BridgeStream>(context, device, sp, device.get()));
    }

    device->SetControlHandler(device);
    device->SetIOHandler(device);

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);

    driver = std::make_shared<aspl::Driver>(context, plugin);
    driver->SetDriverHandler(std::make_shared<InitHandler>(&driver, device));
    return driver;
}

} // namespace

extern "C" void* NAMBridgeEntryPoint(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID))
        return nullptr;
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
