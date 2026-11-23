// AsioDriver: the NAM Bridge virtual ASIO driver (see docs/WINDOWS_ASIO.md).
//
// No hardware: the driver is its own clock. On each tick k it takes what the host wrote to
// output half (k-1)&1 on the previous tick, runs it through the NAM model and writes the
// result into input half k&1, then calls the host's buffer switch. Round trip: one buffer.
#pragma once

#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NamBridgeControl.h"
#include "NamEngine.h"
#include "Settings.h"

namespace nambridge::asio {

// {0FF6E346-784E-45F1-B522-7B6A486425EA}
inline constexpr CLSID kClsid = {0x0ff6e346, 0x784e, 0x45f1, {0xb5, 0x22, 0x7b, 0x6a, 0x48, 0x64, 0x25, 0xea}};
inline constexpr char kDriverName[] = "NAM Bridge";

inline constexpr int kNumInputs = 2;
inline constexpr int kNumOutputs = 2;
inline constexpr long kMinBuffer = 64;
inline constexpr long kMaxBuffer = 2048;
inline constexpr long kPreferredBuffer = 256;
inline constexpr double kDefaultRate = 48000.0;

bool IsSupportedRate(double rate);

class AsioDriver final : public IASIO, public INamBridgeControl
{
public:
    AsioDriver();
    ~AsioDriver();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IASIO
    ASIOBool init(void* sysHandle) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    void getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                            ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

    // INamBridgeControl (test hook; no persistence)
    HRESULT STDMETHODCALLTYPE LoadModel(const wchar_t* path) override;
    HRESULT STDMETHODCALLTYPE UnloadModel() override;
    HRESULT STDMETHODCALLTYPE GetStatus(NamBridgeStatus* status) override;

    // ---- for the control panel (host UI thread) ----
    NamEngine& Engine() { return engine_; }
    Settings& Config() { return settings_; }
    // Reads the file and loads it. On failure the previous model stays and `error` is set.
    bool LoadModelFile(const std::filesystem::path& path, std::string& error);
    void SetInputSelect(int sel);
    double SampleRate() const { return rate_.load(); }
    long BufferSize() const { return bufferSize_; }
    bool Running() const { return running_.load(); }
    uint64_t Overruns() const { return overruns_.load(); }
    // Fraction of real time spent in the NAM engine since start() (0..1).
    double DspLoad() const;
    std::string LastError() const;
    void SetLastError(const std::string& e);

private:
    void ClockLoop(double rate, long bufferSize);
    void Tick(int64_t k, int64_t qpcNow);
    void StopClock();
    void FreeBuffers();
    int64_t QpcToNs(int64_t qpc) const;

    std::atomic<ULONG> refs_{1};

    NamEngine engine_;
    Settings settings_;
    HWND sysHandle_ = nullptr;
    bool initialized_ = false;

    mutable std::mutex errorMutex_;
    std::string lastError_;

    std::atomic<double> rate_{kDefaultRate};

    // ---- buffers (allocated in createBuffers, immutable while running) ----
    bool prepared_ = false;
    long bufferSize_ = 0;
    ASIOCallbacks* callbacks_ = nullptr;
    bool useTimeInfo_ = false;
    std::vector<std::vector<float>> storage_; // one per created channel, 2 halves each
    float* inBuf_[kNumInputs] = {};           // half h is inBuf_[c] + h * bufferSize_
    float* outBuf_[kNumOutputs] = {};
    std::vector<float> monoIn_, monoOut_;
    ASIOTime asioTime_ = {};

    // ---- clock thread ----
    std::thread clock_;
    HANDLE stopEvent_ = nullptr;
    HANDLE timer_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> inputSelect_{0};
    std::atomic<uint64_t> ticks_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<int64_t> processQpc_{0}; // QPC ticks spent in engine_.Process since start()
    int64_t qpcFreq_ = 1;
    double clockRate_ = kDefaultRate;

    // Position of the current tick, published by the clock thread (seqlock).
    std::atomic<uint32_t> posSeq_{0};
    std::atomic<int64_t> posSamples_{0};
    std::atomic<int64_t> posTimeNs_{0};
};

} // namespace nambridge::asio
