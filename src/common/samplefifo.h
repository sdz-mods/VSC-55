// Preserve every callback from one emulator step. GPL-2.0-or-later.
#pragma once
#include "emu.h"
#include <stdexcept>
struct SampleFIFO
{
    AudioFrame<int16_t> frames[64] {};
    unsigned head = 0, count = 0;
    int dc_offset = 0; // PCM16 units; configured once by the output host.
    void push(const AudioFrame<int32_t> &raw)
    {
        if (count == 64)
            throw std::runtime_error("Core sample FIFO overflow");

        auto &frame = frames[(head + count) % 64];
        // Widen before subtraction, then clip only after bias removal.
        // 512 PCM16 units = 4096 in the core's PCM accumulator scale.
        frame.left = (int16_t)Clamp<int64_t>((int64_t(raw.left) - int64_t(dc_offset) * 32768) >> 15, INT16_MIN, INT16_MAX);
        frame.right = (int16_t)Clamp<int64_t>((int64_t(raw.right) - int64_t(dc_offset) * 32768) >> 15, INT16_MIN, INT16_MAX);
        ++count;
    }
    AudioFrame<int16_t> pop()
    {
        if (!count)
            throw std::runtime_error("Core sample FIFO empty");

        auto frame = frames[head];
        head = (head + 1) % 64;
        --count;
        return frame;
    }
};
