// Core integration regression: preserve callback samples and order across Step().
#include "../src/common/samplefifo.h"
#include "../src/host/romload.h"
#include <cstdio>
struct Capture
{
    SampleFIFO fifo;
    std::vector<AudioFrame<int16_t>> reference;
    unsigned callbacks = 0, multi = 0;
    static void sample(void *p, const AudioFrame<int32_t> &raw)
    {
        auto &c = *static_cast<Capture *>(p);
        AudioFrame<int16_t> frame;
        Normalize(raw, frame);
        c.reference.push_back(frame);
        c.fifo.push(raw);
        ++c.callbacks;
    }
};
int main(int argc, char **argv)
{
    try
    {
        if (argc != 3)
            return 2;

        const RomsetDefinition *def = nullptr;

        for (const auto &d : GetStandardRomsetDefinitions())
            if (!strcmp(d.name, argv[1]))
                def = &d;

        RomsetInfo info;

        if (!def || !load_directory(argv[2], *def, info))
            return 2;

        Emulator emu;

        if (!emu.Init({}) || !emu.LoadRoms(def->romset, info))
            return 2;
        emu.Reset();
        Capture c;
        emu.SetSampleCallback(Capture::sample, &c);
        unsigned rate = PCM_GetOutputFrequency(emu.GetPCM()), consumed = 0;

        while (consumed < rate * 4)
        {
            if (consumed == rate * 3)
            {
                const uint8_t notes[] = {0xc0, 0, 0x90, 60, 100};
                emu.PostMIDI(notes);
            }

            while (!c.fifo.count)
            {
                c.callbacks = 0;
                emu.Step();

                if (c.callbacks > 1)
                    ++c.multi;
            }

            auto frame = c.fifo.pop();
            const auto &ref = c.reference[consumed++];

            if (frame.left != ref.left || frame.right != ref.right)
                throw std::runtime_error("Sample mismatch");

            if (consumed + c.fifo.count != c.reference.size())
                throw std::runtime_error("Sample loss");
        }

        if (!c.multi)
            throw std::runtime_error("Did not exercise multiple callbacks per step");

        printf("PASS MODEL=%s CONSUMED=%u CALLBACKS=%u MULTI_CALLBACK_STEPS=%u PENDING=%u\n", argv[1], consumed,
               (unsigned)c.reference.size(), c.multi, c.fifo.count);
    }
    catch (const std::exception &e)
    {
        puts(e.what());
        return 1;
    }
}
