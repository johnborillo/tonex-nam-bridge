#include "AsioDriver.h"

#include <avrt.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "Module.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nambridge::asio {

namespace {

constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
constexpr const char* kInputNames[kNumInputs] = {"NAM In 1", "NAM In 2"};
constexpr const char* kOutputNames[kNumOutputs] = {"NAM Out 1", "NAM Out 2"};

void Split64(int64_t v, unsigned long& hi, unsigned long& lo)
{
    const uint64_t u = static_cast<uint64_t>(v);
    hi = static_cast<unsigned long>(u >> 32);
    lo = static_cast<unsigned long>(u & 0xffffffffu);
}

bool IsPowerOfTwo(long v) { return v > 0 && (v & (v - 1)) == 0; }

} // namespace

bool IsSupportedRate(double rate)
{
    return std::any_of(std::begin(kRates), std::end(kRates), [&](double r) { return std::abs(r - rate) < 0.5; });
}

AsioDriver::AsioDriver()
{
    gObjects++;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;
}

AsioDriver::~AsioDriver()
{
    StopClock();
    FreeBuffers();
    if (timer_)
        CloseHandle(timer_);
    if (stopEvent_)
        CloseHandle(stopEvent_);
    gObjects--;
}

// ---- IUnknown ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE AsioDriver::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    // ASIO quirk: hosts pass the driver's CLSID as the IID.
    if (riid == IID_IUnknown || riid == kClsid)
        *ppv = static_cast<IASIO*>(this);
    else if (riid == __uuidof(INamBridgeControl))
        *ppv = static_cast<INamBridgeControl*>(this);
    else
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE AsioDriver::AddRef() { return ++refs_; }

ULONG STDMETHODCALLTYPE AsioDriver::Release()
{
    const ULONG n = --refs_;
    if (n == 0)
        delete this;
    return n;
}

// ---- errors / model ---------------------------------------------------------------------

std::string AsioDriver::LastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void AsioDriver::SetLastError(const std::string& e)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = e;
}

bool AsioDriver::LoadModelFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream f(path, std::ios::binary); // wide path: spaces and non-ASCII are fine
    if (!f)
    {
        error = "Cannot open " + ToUtf8(path.wstring());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!engine_.LoadFromJson(ss.str(), ToUtf8(path.stem().wstring()), error))
        return false;
    SetLastError("");
    return true;
}

void AsioDriver::SetInputSelect(int sel) { inputSelect_.store(std::clamp(sel, 0, 2)); }

// ---- IASIO: identity / setup ------------------------------------------------------------

ASIOBool AsioDriver::init(void* sysHandle)
{
    sysHandle_ = static_cast<HWND>(sysHandle);
    if (initialized_)
        return ASIOTrue;
    // Hosts may create and init the driver just to list it: never fail here.
    try
    {
        settings_.Load();
        engine_.SetInputGainDb(settings_.inputGainDb);
        engine_.SetOutputGainDb(settings_.outputGainDb);
        SetInputSelect(settings_.inputSelect);
        engine_.SetHostRate(rate_.load());
        if (!settings_.modelPath.empty())
        {
            std::string err;
            if (!LoadModelFile(settings_.modelPath, err))
                SetLastError(err);
        }
    }
    catch (const std::exception& e)
    {
        SetLastError(std::string("Settings: ") + e.what());
    }
    initialized_ = true;
    return ASIOTrue;
}

void AsioDriver::getDriverName(char* name)
{
    if (name)
        strcpy_s(name, 32, kDriverName);
}

long AsioDriver::getDriverVersion() { return 1; }

void AsioDriver::getErrorMessage(char* string)
{
    if (!string)
        return;
    const std::string e = LastError();
    const size_t n = std::min<size_t>(e.size(), 123);
    memcpy(string, e.data(), n);
    string[n] = '\0';
}

ASIOError AsioDriver::getChannels(long* numInputChannels, long* numOutputChannels)
{
    if (numInputChannels)
        *numInputChannels = kNumInputs;
    if (numOutputChannels)
        *numOutputChannels = kNumOutputs;
    return ASE_OK;
}

ASIOError AsioDriver::getLatencies(long* inputLatency, long* outputLatency)
{
    // Round trip is exactly one buffer: all of it reported on the output side.
    if (inputLatency)
        *inputLatency = 0;
    if (outputLatency)
        *outputLatency = prepared_ ? bufferSize_ : kPreferredBuffer;
    return ASE_OK;
}

ASIOError AsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity)
{
    if (minSize)
        *minSize = kMinBuffer;
    if (maxSize)
        *maxSize = kMaxBuffer;
    if (preferredSize)
        *preferredSize = kPreferredBuffer;
    if (granularity)
        *granularity = -1; // powers of two
    return ASE_OK;
}

ASIOError AsioDriver::canSampleRate(ASIOSampleRate sampleRate)
{
    return IsSupportedRate(sampleRate) ? ASE_OK : ASE_NoClock;
}

ASIOError AsioDriver::getSampleRate(ASIOSampleRate* sampleRate)
{
    if (!sampleRate)
        return ASE_InvalidParameter;
    *sampleRate = rate_.load();
    return ASE_OK;
}

ASIOError AsioDriver::setSampleRate(ASIOSampleRate sampleRate)
{
    if (!IsSupportedRate(sampleRate))
        return ASE_NoClock;
    if (std::abs(sampleRate - rate_.load()) < 0.5)
        return ASE_OK;
    rate_.store(sampleRate);
    if (running_.load())
    {
        // Never change the clock under a running host: ask it to reset. start() applies the
        // new rate to the engine and the clock.
        if (callbacks_ && callbacks_->asioMessage)
            callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }
    else
        engine_.SetHostRate(sampleRate);
    return ASE_OK;
}

ASIOError AsioDriver::getClockSources(ASIOClockSource* clocks, long* numSources)
{
    if (!clocks || !numSources)
        return ASE_InvalidParameter;
    if (*numSources >= 1)
    {
        clocks[0] = {};
        clocks[0].index = 0;
        clocks[0].associatedChannel = -1;
        clocks[0].associatedGroup = -1;
        clocks[0].isCurrentSource = ASIOTrue;
        strcpy_s(clocks[0].name, sizeof clocks[0].name, "Internal");
    }
    *numSources = 1;
    return ASE_OK;
}

ASIOError AsioDriver::setClockSource(long reference) { return reference == 0 ? ASE_OK : ASE_InvalidParameter; }

ASIOError AsioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp)
{
    if (!sPos || !tStamp)
        return ASE_InvalidParameter;
    if (!running_.load())
        return ASE_SPNotAdvancing;
    int64_t samples = 0, ns = 0;
    for (;;)
    {
        const uint32_t s1 = posSeq_.load();
        if (s1 & 1u)
            continue;
        samples = posSamples_.load();
        ns = posTimeNs_.load();
        if (posSeq_.load() == s1)
            break;
    }
    Split64(samples, sPos->hi, sPos->lo);
    Split64(ns, tStamp->hi, tStamp->lo);
    return ASE_OK;
}

ASIOError AsioDriver::getChannelInfo(ASIOChannelInfo* info)
{
    if (!info)
        return ASE_InvalidParameter;
    const long c = info->channel;
    const bool input = info->isInput != ASIOFalse;
    if (c < 0 || c >= (input ? kNumInputs : kNumOutputs))
        return ASE_InvalidParameter;
    info->isActive = (input ? inBuf_[c] : outBuf_[c]) ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB;
    strcpy_s(info->name, sizeof info->name, input ? kInputNames[c] : kOutputNames[c]);
    return ASE_OK;
}

// ---- IASIO: buffers ---------------------------------------------------------------------

void AsioDriver::FreeBuffers()
{
    storage_.clear();
    std::fill(std::begin(inBuf_), std::end(inBuf_), nullptr);
    std::fill(std::begin(outBuf_), std::end(outBuf_), nullptr);
    monoIn_.clear();
    monoOut_.clear();
    callbacks_ = nullptr;
    useTimeInfo_ = false;
    bufferSize_ = 0;
    prepared_ = false;
}

ASIOError AsioDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks)
{
    if (running_.load())
        return ASE_InvalidMode;
    if (!bufferInfos || numChannels <= 0 || !callbacks || !callbacks->bufferSwitch)
        return ASE_InvalidParameter;
    if (bufferSize < kMinBuffer || bufferSize > kMaxBuffer || !IsPowerOfTwo(bufferSize))
        return ASE_InvalidMode;
    bool seenIn[kNumInputs] = {}, seenOut[kNumOutputs] = {};
    for (long n = 0; n < numChannels; n++)
    {
        const bool input = bufferInfos[n].isInput != ASIOFalse;
        const long c = bufferInfos[n].channelNum;
        if (c < 0 || c >= (input ? kNumInputs : kNumOutputs))
            return ASE_InvalidParameter;
        bool& seen = input ? seenIn[c] : seenOut[c];
        if (seen)
            return ASE_InvalidParameter;
        seen = true;
    }

    if (prepared_)
        FreeBuffers();
    try
    {
        storage_.reserve(static_cast<size_t>(numChannels));
        for (long n = 0; n < numChannels; n++)
        {
            storage_.emplace_back(static_cast<size_t>(bufferSize) * 2, 0.0f);
            float* base = storage_.back().data();
            bufferInfos[n].buffers[0] = base;
            bufferInfos[n].buffers[1] = base + bufferSize;
            (bufferInfos[n].isInput != ASIOFalse ? inBuf_ : outBuf_)[bufferInfos[n].channelNum] = base;
        }
        monoIn_.assign(static_cast<size_t>(bufferSize), 0.0f);
        monoOut_.assign(static_cast<size_t>(bufferSize), 0.0f);
    }
    catch (const std::bad_alloc&)
    {
        FreeBuffers();
        return ASE_NoMemory;
    }

    bufferSize_ = bufferSize;
    callbacks_ = callbacks;
    useTimeInfo_ = callbacks->asioMessage && callbacks->bufferSwitchTimeInfo &&
                   callbacks->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                   callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    engine_.ResetState();
    prepared_ = true;
    return ASE_OK;
}

ASIOError AsioDriver::disposeBuffers()
{
    if (!prepared_)
        return ASE_InvalidMode;
    StopClock();
    FreeBuffers();
    return ASE_OK;
}

// ---- IASIO: transport -------------------------------------------------------------------

ASIOError AsioDriver::start()
{
    if (!prepared_)
        return ASE_InvalidMode;
    if (running_.load())
        return ASE_OK;
    StopClock(); // joins a clock that was stopped from inside its own callback

    for (auto& s : storage_)
        std::fill(s.begin(), s.end(), 0.0f);
    const double rate = rate_.load();
    engine_.SetHostRate(rate); // no-op unless setSampleRate ran while we were running
    engine_.ResetState();

    if (!timer_)
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) // older Windows: a plain timer, the spin covers the rest
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!timer_)
        {
            SetLastError("Cannot create the clock timer");
            return ASE_HWMalfunction;
        }
    }

    ticks_.store(0);
    processQpc_.store(0);
    posSeq_.store(0);
    posSamples_.store(0);
    posTimeNs_.store(0);
    clockRate_ = rate;
    stopRequested_.store(false);
    ResetEvent(stopEvent_);
    timeBeginPeriod(1);
    running_.store(true);
    try
    {
        clock_ = std::thread(&AsioDriver::ClockLoop, this, rate, bufferSize_);
    }
    catch (const std::exception&)
    {
        running_.store(false);
        timeEndPeriod(1);
        SetLastError("Cannot start the clock thread");
        return ASE_HWMalfunction;
    }
    return ASE_OK;
}

ASIOError AsioDriver::stop()
{
    StopClock();
    return ASE_OK;
}

void AsioDriver::StopClock()
{
    if (!clock_.joinable())
        return;
    stopRequested_.store(true);
    SetEvent(stopEvent_);
    if (std::this_thread::get_id() == clock_.get_id())
        return; // called from a host callback: the loop exits after it returns; joined later
    clock_.join();
    running_.store(false);
    timeEndPeriod(1);
}

double AsioDriver::DspLoad() const
{
    const uint64_t ticks = ticks_.load();
    if (ticks == 0 || bufferSize_ <= 0)
        return 0.0;
    const double audio = static_cast<double>(ticks) * static_cast<double>(bufferSize_) / clockRate_;
    return static_cast<double>(processQpc_.load()) / static_cast<double>(qpcFreq_) / audio;
}

int64_t AsioDriver::QpcToNs(int64_t qpc) const
{
    return (qpc / qpcFreq_) * 1000000000LL + (qpc % qpcFreq_) * 1000000000LL / qpcFreq_;
}

// ---- the clock thread -------------------------------------------------------------------
// No allocation, locks, logging, file or registry I/O below this line.

void AsioDriver::ClockLoop(double rate, long bufferSize)
{
    DWORD taskIndex = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (av)
        AvSetMmThreadPriority(av, AVRT_PRIORITY_CRITICAL);

    const double periodTicks = static_cast<double>(bufferSize) * static_cast<double>(qpcFreq_) / rate;
    const int64_t spinTicks = qpcFreq_ / 5000; // last ~200 us are spun, not slept
    HANDLE waits[2] = {stopEvent_, timer_};

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t t0 = now.QuadPart; // absolute schedule: deadline(k) = t0 + (k - kBase) * period
    int64_t kBase = 0;

    for (int64_t k = 0; !stopRequested_.load(std::memory_order_relaxed); k++)
    {
        int64_t deadline = t0 + static_cast<int64_t>(std::llround(static_cast<double>(k - kBase) * periodTicks));
        QueryPerformanceCounter(&now);
        const int64_t remaining = deadline - now.QuadPart;
        if (remaining > spinTicks)
        {
            LARGE_INTEGER due; // negative = relative, in 100 ns units
            due.QuadPart = -static_cast<LONGLONG>(static_cast<double>(remaining - spinTicks) * 1e7 /
                                                  static_cast<double>(qpcFreq_));
            if (due.QuadPart < 0 && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
            {
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
                    break;
            }
        }
        do
        {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < deadline);
        if (stopRequested_.load(std::memory_order_relaxed))
            break;

        if (static_cast<double>(now.QuadPart - deadline) > 2.0 * periodTicks)
        {
            overruns_.fetch_add(1, std::memory_order_relaxed);
            t0 = now.QuadPart; // resynchronize, keep k
            kBase = k;
            deadline = now.QuadPart;
        }
        Tick(k, deadline);
    }

    if (av)
        AvRevertMmThreadCharacteristics(av);
    running_.store(false);
}

void AsioDriver::Tick(int64_t k, int64_t qpcNow)
{
    const long n = bufferSize_;
    const long cur = static_cast<long>(k & 1);
    const long prev = cur ^ 1;
    float* processed = monoOut_.data();

    if (k == 0)
        std::fill(monoOut_.begin(), monoOut_.end(), 0.0f); // nothing has been played yet
    else
    {
        // What the host wrote on the previous tick, reduced to mono.
        const float* l = outBuf_[0] ? outBuf_[0] + prev * n : nullptr;
        const float* r = outBuf_[1] ? outBuf_[1] + prev * n : nullptr;
        float* mono = monoIn_.data();
        switch (inputSelect_.load(std::memory_order_relaxed))
        {
        case 1:
            if (r)
                std::copy(r, r + n, mono);
            else
                std::fill(mono, mono + n, 0.0f);
            break;
        case 2:
            for (long i = 0; i < n; i++)
                mono[i] = 0.5f * ((l ? l[i] : 0.0f) + (r ? r[i] : 0.0f));
            break;
        default:
            if (l)
                std::copy(l, l + n, mono);
            else
                std::fill(mono, mono + n, 0.0f);
            break;
        }
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        engine_.Process(mono, processed, static_cast<int>(n));
        QueryPerformanceCounter(&b);
        processQpc_.fetch_add(b.QuadPart - a.QuadPart, std::memory_order_relaxed);
    }

    for (float* in : inBuf_)
        if (in)
            std::copy(processed, processed + n, in + cur * n);

    const int64_t samples = k * n;
    const int64_t ns = QpcToNs(qpcNow);
    posSeq_.fetch_add(1);
    posSamples_.store(samples);
    posTimeNs_.store(ns);
    posSeq_.fetch_add(1);
    ticks_.fetch_add(1, std::memory_order_relaxed);

    if (useTimeInfo_)
    {
        AsioTimeInfo& ti = asioTime_.timeInfo;
        ti.speed = 1.0;
        Split64(ns, ti.systemTime.hi, ti.systemTime.lo);
        Split64(samples, ti.samplePosition.hi, ti.samplePosition.lo);
        ti.sampleRate = clockRate_;
        ti.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        asioTime_.timeCode.flags = 0;
        callbacks_->bufferSwitchTimeInfo(&asioTime_, cur, ASIOFalse);
    }
    else
        callbacks_->bufferSwitch(cur, ASIOFalse);
}

// ---- IASIO: misc ------------------------------------------------------------------------

ASIOError AsioDriver::controlPanel()
{
    ShowControlPanel(*this, sysHandle_ ? sysHandle_ : GetActiveWindow());
    return ASE_OK;
}

ASIOError AsioDriver::future(long selector, void*)
{
    return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_InvalidParameter;
}

ASIOError AsioDriver::outputReady() { return ASE_NotPresent; }

// ---- INamBridgeControl ------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE AsioDriver::LoadModel(const wchar_t* path)
{
    if (!path)
        return E_POINTER;
    std::string err;
    if (!LoadModelFile(path, err))
    {
        SetLastError(err);
        return E_FAIL;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AsioDriver::UnloadModel()
{
    engine_.Unload();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AsioDriver::GetStatus(NamBridgeStatus* status)
{
    if (!status)
        return E_POINTER;
    const ModelInfo info = engine_.Info();
    status->modelLoaded = info.loaded;
    status->resampling = info.resampling;
    status->running = running_.load();
    status->sampleRate = rate_.load();
    status->modelRate = info.modelRate;
    status->bufferSize = bufferSize_;
    status->ticks = ticks_.load();
    status->overruns = overruns_.load();
    status->processSeconds = static_cast<double>(processQpc_.load()) / static_cast<double>(qpcFreq_);
    return S_OK;
}

} // namespace nambridge::asio
