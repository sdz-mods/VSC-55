// Exercise firmware front-panel mute with actual audio, without an audio device.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "emu.h"
#include "../src/host/romload.h"
struct Capture
{
    unsigned frames = 0, count = 0;
    double energy = 0;
    int offset = 0;
    static void sample(void *p, const AudioFrame<int32_t> &in)
    {
        auto &c = *static_cast<Capture *>(p);
        AudioFrame<int16_t> a;
        Normalize(in, a);
        double l = int(a.left) - c.offset, r = int(a.right) - c.offset;
        c.energy += l * l + r * r;
        ++c.count;
        ++c.frames;
    }
};
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;

    try
    {
        const RomsetDefinition *def = nullptr;

        for (const auto &d : GetStandardRomsetDefinitions())
            if (!strcmp(d.name, argv[1]))
                def = &d;

        if (!def)
            throw std::runtime_error("model");

        RomsetInfo info;

        if (!load_directory(argv[2], *def, info))
            throw std::runtime_error("ROMs");

        Emulator emu;

        if (!emu.Init({}) || !emu.LoadRoms(def->romset, info))
            throw std::runtime_error("init");
        emu.Reset();
        Capture c;
        c.offset = emu.GetMCU().is_mk1 ? 512 : 0;
        emu.SetSampleCallback(Capture::sample, &c);
        unsigned rate = PCM_GetOutputFrequency(emu.GetPCM());
        auto run = [&](unsigned ms)
        {
            unsigned end = c.frames + rate * ms / 1000;

            while (c.frames < end)
                emu.Step();
        };
        auto press = [&](unsigned bit)
        {
            emu.GetMCU().button_pressed.store(1u << bit);
            run(150);
            emu.GetMCU().button_pressed.store(0);
            run(150);
        };
        auto rms = [&]()
        {
            run(1200);
            c.energy = 0;
            c.count = 0;
            run(500);
            return sqrt(c.energy / (2 * c.count));
        };
        run(3000);
        // Dry sustained organ on channel 1; second channel held in reserve.
        const uint8_t setup[] = {0xB0, 91, 0, 0xB0, 93, 0, 0xC0, 16, 0xB1, 91, 0, 0xB1, 93, 0, 0xC1, 16, 0x90, 60, 100};
        emu.PostMIDI(setup);
        double base = rms();
        unsigned port = emu.GetMCU().is_mk1 ? emu.GetMCU().io_sd : emu.GetMCU().p0_data;

        if (!(port & 0x40))
            press(6); // select individual-part mode

        press(5);
        double partmute = rms();
        const uint8_t note2[] = {0x91, 67, 100};
        emu.PostMIDI(note2);
        double other = rms();
        press(6);
        press(5);
        double allmute = rms();
        press(5);
        const uint8_t again[] = {0x90, 60, 100, 0x91, 67, 100};
        emu.PostMIDI(again);
        double restored = rms();
        printf("MODEL=%s RMS baseline=%.3f part1_muted=%.3f part2_playing=%.3f all_muted=%.3f restored=%.3f\n", argv[1], base,
               partmute, other, allmute, restored);

        if (base < 20 || partmute > base * .02 || other < 20 || allmute > base * .02 || restored < 20)
            throw std::runtime_error("mute audio assertion");

        puts("PASS selected-part mute, other part audible, ALL mute and restore");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
