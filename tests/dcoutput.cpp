#include "../src/common/samplefifo.h"
#include <cstdio>
#include <limits>
int main()
{
    SampleFIFO fifo;

    // Exhaust all ordinary PCM16 levels, also exercising both polarities.
    for (int bias :
            {
                0, 512
            })
    {
        fifo.dc_offset = bias;

        for (int value = -32768; value <= 32767; value++)
        {
            int32_t raw = value * 32768;
            fifo.push({raw, raw});
            auto f = fifo.pop();
            int expected = value - bias;

            if (expected < -32768)
                expected = -32768;

            if (f.left != expected || f.right != expected)
                return 1;
        }
    }
    // Above PCM16 full scale: removing bias before clipping preserves headroom.
    fifo.dc_offset = 512;
    fifo.push({33000 * 32768, 512 * 32768});
    auto a = fifo.pop();

    if (a.left != 32488 || a.right != 0)
        return 2;

    fifo.push({std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()});
    auto b = fifo.pop();

    if (b.left != -32768 || b.right != 32767)
        return 3;

    puts("PASS: both channels, zero and MK1 bias, all PCM16 levels, pre-clipping headroom and raw extremes.");
}
