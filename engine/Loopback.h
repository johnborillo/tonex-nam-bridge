// Loopback: the heart of the virtual "reamp box".
//
// Whatever a client plays to the device's output at sample time T is run through the NAM
// engine and becomes readable on the device's input at the same sample time T. Each ring
// slot remembers the sample time it holds, so stale or never-written frames read as
// silence instead of looping old audio.
//
// One realtime thread writes and reads (the device's IO thread). Header-only so it can be
// shared by the driver and the test harness.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "NamEngine.h"

namespace nambridge {

enum class InputSelect : int
{
    Left = 0,
    Right = 1,
    Mix = 2, // (L + R) / 2
};

class Loopback
{
public:
    static constexpr int64_t kRingFrames = 1 << 16; // ~1.4 s at 48 kHz
    static constexpr int kScratch = 1024;

    explicit Loopback(NamEngine& engine)
    : engine_(engine)
    , ring_(kRingFrames, 0.0f)
    , stamp_(kRingFrames, -1)
    , monoIn_(kScratch)
    , monoOut_(kScratch)
    {
    }

    void SetInputSelect(InputSelect s) { select_.store(static_cast<int>(s)); }
    InputSelect GetInputSelect() const { return static_cast<InputSelect>(select_.load()); }

    // Non-RT: forget everything (call before IO starts).
    void Clear()
    {
        std::fill(stamp_.begin(), stamp_.end(), int64_t(-1));
        engine_.ResetState();
    }

    // RT: device output (interleaved float, `channels` wide) at sample time `time`.
    void Write(double time, const float* frames, uint32_t frameCount, uint32_t channels)
    {
        int64_t t = static_cast<int64_t>(std::llround(time));
        const int sel = select_.load();
        for (uint32_t done = 0; done < frameCount;)
        {
            const int m = static_cast<int>(std::min<uint32_t>(kScratch, frameCount - done));
            const float* f = frames + static_cast<size_t>(done) * channels;
            for (int i = 0; i < m; i++)
            {
                const float l = f[i * channels];
                const float r = channels > 1 ? f[i * channels + 1] : l;
                monoIn_[i] = sel == 0 ? l : (sel == 1 ? r : 0.5f * (l + r));
            }
            engine_.Process(monoIn_.data(), monoOut_.data(), m);
            for (int i = 0; i < m; i++)
            {
                const int64_t idx = (t + i) & (kRingFrames - 1);
                ring_[idx] = monoOut_[i];
                stamp_[idx] = t + i;
            }
            t += m;
            done += m;
        }
    }

    // RT: device input at sample time `time`; the processed mono signal goes to every channel.
    void Read(double time, float* frames, uint32_t frameCount, uint32_t channels) const
    {
        const int64_t t = static_cast<int64_t>(std::llround(time));
        for (uint32_t i = 0; i < frameCount; i++)
        {
            const int64_t idx = (t + i) & (kRingFrames - 1);
            const float v = stamp_[idx] == t + static_cast<int64_t>(i) ? ring_[idx] : 0.0f;
            for (uint32_t c = 0; c < channels; c++)
                frames[i * channels + c] = v;
        }
    }

private:
    NamEngine& engine_;
    std::atomic<int> select_{static_cast<int>(InputSelect::Left)};
    std::vector<float> ring_;
    std::vector<int64_t> stamp_;
    std::vector<float> monoIn_, monoOut_;
};

} // namespace nambridge
