#include "NamEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "json.hpp"

// ResamplingContainer expects a couple of definitions that iPlug2 normally provides.
namespace iplug {
constexpr double PI = 3.141592653589793238462643383279502884;
}
#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 512
#endif
#include "ResamplingContainer/ResamplingContainer.h"

namespace nambridge {

namespace {

constexpr int kChunk = 256; // internal processing block (host frames)
using Sample = NAM_SAMPLE;  // double unless NAM_SAMPLE_FLOAT is defined

inline float DbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// Exact tanh: we are feeding a capture, so fidelity beats the few % CPU fast_tanh saves.
struct ExactTanhInit
{
    ExactTanhInit() { nam::activations::Activation::disable_fast_tanh(); }
};

} // namespace

struct NamEngine::Slot
{
    std::unique_ptr<nam::DSP> model;
    std::unique_ptr<dsp::ResamplingContainer<Sample, 1, 12>> resampler;
    std::function<void(Sample**, Sample**, int)> modelFunc; // built once: no RT allocation
    std::vector<Sample> in, out;
    Sample* inPtr[1] = {nullptr};
    Sample* outPtr[1] = {nullptr};
    ModelInfo info;
};

NamEngine::NamEngine()
{
    static ExactTanhInit once;
}

NamEngine::~NamEngine()
{
    delete active_.exchange(nullptr);
}

std::unique_ptr<NamEngine::Slot> NamEngine::BuildSlot(const std::string& json, const std::string& fallbackName,
                                                      double hostRate, std::string& error) const
{
    auto slot = std::make_unique<Slot>();
    try
    {
        const nlohmann::json j = nlohmann::json::parse(json);
        slot->model = nam::get_dsp(j);
        if (!slot->model)
        {
            error = "NAM could not build this model";
            return nullptr;
        }
        if (slot->model->NumInputChannels() != 1 || slot->model->NumOutputChannels() != 1)
        {
            error = "Only mono-in/mono-out models are supported";
            return nullptr;
        }

        auto& info = slot->info;
        info.loaded = true;
        info.architecture = j.value("architecture", std::string("unknown"));
        info.name = fallbackName.empty() ? "Unnamed model" : fallbackName;
        if (j.contains("metadata") && j["metadata"].is_object())
        {
            const auto& md = j["metadata"];
            if (md.contains("name") && md["name"].is_string() && !md["name"].get<std::string>().empty())
                info.name = md["name"].get<std::string>();
        }
        info.hostRate = hostRate;
        info.modelRate = slot->model->GetExpectedSampleRate();
        if (info.modelRate <= 0)
            info.modelRate = 48000.0; // NAM's historical default
        info.hasLoudness = slot->model->HasLoudness();
        if (info.hasLoudness)
            info.loudnessDb = slot->model->GetLoudness();
        info.hasInputLevel = slot->model->HasInputLevel();
        if (info.hasInputLevel)
            info.inputLevelDbu = slot->model->GetInputLevel();

        slot->in.assign(kChunk, 0);
        slot->out.assign(kChunk, 0);
        slot->inPtr[0] = slot->in.data();
        slot->outPtr[0] = slot->out.data();

        info.resampling = std::abs(info.modelRate - hostRate) > 0.5 && !slot->model->SupportsArbitrarySampleRate();
        if (info.resampling)
        {
            // Inner blocks can be up to ceil(kChunk * modelRate / hostRate) frames; leave headroom.
            const int innerMax = static_cast<int>(std::ceil(kChunk * info.modelRate / hostRate)) + 64;
            slot->model->Reset(info.modelRate, innerMax);
            slot->resampler = std::make_unique<dsp::ResamplingContainer<Sample, 1, 12>>(info.modelRate);
            slot->resampler->Reset(hostRate, kChunk);
            info.resamplerLatency = slot->resampler->GetLatency();
            nam::DSP* model = slot->model.get();
            slot->modelFunc = [model](Sample** i, Sample** o, int n) { model->process(i, o, n); };
        }
        else
        {
            slot->model->Reset(slot->model->SupportsArbitrarySampleRate() ? hostRate : info.modelRate, kChunk);
        }
    }
    catch (const std::exception& e)
    {
        error = std::string("Invalid .nam file: ") + e.what();
        return nullptr;
    }
    return slot;
}

void NamEngine::Install(std::unique_ptr<Slot> slot)
{
    Slot* old = active_.exchange(slot.release());
    // Wait until the realtime thread is no longer inside Process() with the old slot.
    // rtBusy_ is set before the RT thread loads active_, so once it reads false after the
    // exchange, no RT call can still hold `old`. Otherwise, a call in flight now may hold
    // `old`, but any call that starts later sees the new slot, so it is enough to see one
    // call complete. (Waiting only for rtBusy_ == false can starve when Process() runs back
    // to back: on Windows sleep_for(200 us) sleeps >= 1 ms and the idle gap is nanoseconds.)
    const uint64_t exits = rtExits_.load();
    while (rtBusy_.load() && rtExits_.load() == exits)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    delete old;
}

bool NamEngine::LoadFromJson(const std::string& json, const std::string& fallbackName, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto slot = BuildSlot(json, fallbackName, hostRate_, error);
    if (!slot)
        return false;
    json_ = json;
    fallbackName_ = fallbackName;
    Install(std::move(slot));
    return true;
}

bool NamEngine::LoadFromFile(const std::string& path, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "Cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return LoadFromJson(ss.str(), std::filesystem::path(path).stem().string(), error);
}

void NamEngine::Unload()
{
    std::lock_guard<std::mutex> lock(mutex_);
    json_.clear();
    Install(nullptr);
}

void NamEngine::SetHostRate(double hostRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(hostRate - hostRate_) < 0.5)
        return;
    hostRate_ = hostRate;
    if (json_.empty())
        return;
    std::string error;
    auto slot = BuildSlot(json_, fallbackName_, hostRate_, error);
    Install(std::move(slot)); // on failure this unloads, which is the safe outcome
}

ModelInfo NamEngine::Info() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* s = active_.load();
    ModelInfo info = s ? s->info : ModelInfo{};
    info.hostRate = hostRate_;
    return info;
}

std::string NamEngine::ModelJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return json_;
}

void NamEngine::SetInputGainDb(float db) { inGainDb_.store(std::clamp(db, -60.0f, 40.0f)); }
void NamEngine::SetOutputGainDb(float db) { outGainDb_.store(std::clamp(db, -60.0f, 40.0f)); }

void NamEngine::TakePeaks(float& inPeak, float& outPeak)
{
    inPeak = inPeak_.exchange(0.0f);
    outPeak = outPeak_.exchange(0.0f);
}

void NamEngine::ResetState() { resetRequested_.store(true); }

void NamEngine::Process(const float* in, float* out, int n)
{
    rtBusy_.store(true);
    Slot* s = active_.load();

    const float inGain = DbToGain(inGainDb_.load());
    const float outGain = DbToGain(outGainDb_.load());
    float inPeak = 0.0f, outPeak = 0.0f;

    if (!s)
    {
        for (int i = 0; i < n; i++)
            inPeak = std::max(inPeak, std::abs(in[i] * inGain));
        std::fill(out, out + n, 0.0f);
    }
    else
    {
        if (resetRequested_.exchange(false))
        {
            // Re-prime state without allocating: run a short block of silence through.
            std::fill(s->in.begin(), s->in.end(), 0);
            for (int k = 0; k < 8; k++)
            {
                if (s->resampler)
                    s->resampler->ProcessBlock(s->inPtr, s->outPtr, kChunk, s->modelFunc);
                else
                    s->model->process(s->inPtr, s->outPtr, kChunk);
            }
        }

        for (int done = 0; done < n;)
        {
            const int m = std::min(kChunk, n - done);
            for (int i = 0; i < m; i++)
            {
                const float x = in[done + i] * inGain;
                inPeak = std::max(inPeak, std::abs(x));
                s->in[i] = static_cast<Sample>(x);
            }
            if (s->resampler)
                s->resampler->ProcessBlock(s->inPtr, s->outPtr, m, s->modelFunc);
            else
                s->model->process(s->inPtr, s->outPtr, m);
            for (int i = 0; i < m; i++)
            {
                float y = static_cast<float>(s->out[i]) * outGain;
                if (!std::isfinite(y))
                    y = 0.0f;
                outPeak = std::max(outPeak, std::abs(y));
                out[done + i] = y;
            }
            done += m;
        }
    }

    // Lock-free running max for meters.
    float cur = inPeak_.load();
    while (inPeak > cur && !inPeak_.compare_exchange_weak(cur, inPeak)) {}
    cur = outPeak_.load();
    while (outPeak > cur && !outPeak_.compare_exchange_weak(cur, outPeak)) {}

    rtExits_.fetch_add(1);
    rtBusy_.store(false);
}

} // namespace nambridge
