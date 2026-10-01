/* Output-only gain; integer saturation prevents signed 16-bit wraparound. */
#pragma once
static inline short vsc_output_gain(short sample, int percent)
{
    const int scaled = int(sample) * percent / 100;
    return (short)(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
}
