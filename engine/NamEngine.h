// NamEngine: a mono NAM processor that is safe to drive from a realtime audio thread.
//
// - Models are loaded and prepared off the audio thread, then swapped in atomically.
// - The host may run at any sample rate; if it differs from the model's rate, audio is
//   resampled around the model (Lanczos, from AudioDSPTools).
// - Host blocks of any size are accepted; they are split into fixed internal chunks.
//
// Threading contract: exactly one realtime thread calls Process(). Every other method is
// for non-realtime threads and may block briefly.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace nambridge {

struct ModelInfo
{
    bool loaded = false;
    std::string name;         // metadata.name, else the file stem / "Unnamed model"
    std::string architecture; // WaveNet, LSTM, ...
    double modelRate = 0.0;   // rate the model was trained at
    double hostRate = 0.0;    // rate we are being driven at
    bool resampling = false;
    int resamplerLatency = 0; // frames at host rate (0 when not resampling)
    bool hasLoudness = false;
    double loudnessDb = 0.0;
    bool hasInputLevel = false;
    double inputLevelDbu = 0.0;
};

class NamEngine
{
public:
    NamEngine();
    ~NamEngine();

    NamEngine(const NamEngine&) = delete;
    NamEngine& operator=(const NamEngine&) = delete;

    // ---- non-realtime ----

    // Parse and prepare a model from the text of a .nam file. On success the model becomes
    // active immediately. On failure the previous model stays active and `error` is set.
    bool LoadFromJson(const std::string& json, const std::string& fallbackName, std::string& error);
    bool LoadFromFile(const std::string& path, std::string& error);
    void Unload();

    // Set the host sample rate. Rebuilds the active model if the rate changed.
    void SetHostRate(double hostRate);

    ModelInfo Info() const;
    std::string ModelJson() const; // empty if none loaded

    // ---- any thread (atomics) ----
    void SetInputGainDb(float db);
    void SetOutputGainDb(float db);
    float InputGainDb() const { return inGainDb_.load(); }
    float OutputGainDb() const { return outGainDb_.load(); }

    // Peak levels (linear) seen since the last call; resets them.
    void TakePeaks(float& inPeak, float& outPeak);

    // ---- realtime ----

    // Process `n` mono frames. `in` and `out` may alias. With no model, writes silence.
    void Process(const float* in, float* out, int n);

    // Clear the model's internal state (call when audio restarts).
    void ResetState();

private:
    struct Slot;

    std::unique_ptr<Slot> BuildSlot(const std::string& json, const std::string& fallbackName,
                                    double hostRate, std::string& error) const;
    void Install(std::unique_ptr<Slot> slot); // swap in, then retire the old slot safely

    std::atomic<Slot*> active_{nullptr};
    std::atomic<bool> rtBusy_{false};
    std::atomic<uint64_t> rtExits_{0}; // completed Process() calls (lets Install() wait without starving)
    std::atomic<bool> resetRequested_{false};

    std::atomic<float> inGainDb_{0.0f};
    std::atomic<float> outGainDb_{0.0f};
    std::atomic<float> inPeak_{0.0f};
    std::atomic<float> outPeak_{0.0f};

    mutable std::mutex mutex_; // guards the fields below and serializes loads
    double hostRate_ = 48000.0;
    std::string json_;
    std::string fallbackName_;
};

} // namespace nambridge
