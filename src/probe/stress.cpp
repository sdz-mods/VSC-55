// Offline deterministic stress benchmark. GPL-2.0-or-later.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "emu.h"
#include "../host/romload.h"
#include "../common/samplefifo.h"
static void check(bool ok, const char *s)
{
    if (!ok)
        throw std::runtime_error(s);
}
struct Render
{
    Emulator &emu;
    AudioFrame<int16_t> a{}, b{};
    SampleFIFO samples;
    unsigned rate, phase = 0, uart_high = 0;
    DWORD watchdog = GetTickCount();
    static void sample(void *p, const AudioFrame<int32_t> &raw)
    {
        static_cast<Render *>(p)->samples.push(raw);
    }
    AudioFrame<int16_t> next()
    {
        unsigned steps = 0;

        while (!samples.count)
        {
            emu.Step();

            if ((++steps & 65535) == 0)
                check(DWORD(GetTickCount() - watchdog) < 600000, "Render watchdog");
        }

        return samples.pop();
    }
    unsigned pending()
    {
        auto &m = emu.GetMCU();
        return (m.uart_write_ptr + uart_buffer_size - m.uart_read_ptr) % uart_buffer_size;
    }
    void midi(std::initializer_list<uint8_t> bytes)
    {
        unsigned n = pending();
        check(n + bytes.size() < uart_buffer_size, "UART capacity exceeded");
        emu.PostMIDI(std::span<const uint8_t>(bytes.begin(), bytes.size()));
        uart_high = std::max(uart_high, n + (unsigned)bytes.size());
    }
    void block(short *out)
    {
        for (unsigned i = 0; i < 960; i++)
        {
            out[2 * i] = (short)(a.left + (int(b.left) - a.left) * (double(phase) / 48000));
            out[2 * i + 1] = (short)(a.right + (int(b.right) - a.right) * (double(phase) / 48000));
            phase += rate;

            while (phase >= 48000)
            {
                phase -= 48000;
                a = b;
                b = next();
            }
        }
    }
};
static const unsigned programs[] = {48, 50, 89, 91, 30, 81, 88, 61, 52, 0, 19, 5, 80, 95, 24, 48};
static void notes(Render &r, unsigned count, unsigned rotation, bool on)
{
    for (unsigned ch = 0; ch < 16; ch++)
        if (ch != 9)
            for (unsigned n = 0; n < count; n++)
                r.midi({uint8_t((on ? 0x90 : 0x80) | ch), uint8_t(42 + n * 3 + rotation), uint8_t(on ? 100 : 0)});
}
int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
    {
        puts("STRESS <romset-name> <ROM-directory> [heavy-PCM-prefix]");
        return 2;
    }

    try
    {
        check(!FindWindowA("VSC55_PROTO1_RECEIVER", nullptr), "Stop the live host before benchmarking");
        printf("BUILD_PROFILE=%s\n", VSC55_BUILD_PROFILE);

        if (argc == 4)
            puts("DIAGNOSTIC_HEAVY_DUMP=1 CASES=2 (writes after timed rendering)");

        LARGE_INTEGER freq;
        check(QueryPerformanceFrequency(&freq) != 0 && freq.QuadPart > 0, "No high resolution timer");
        const RomsetDefinition *def = nullptr;

        for (const auto &d : GetStandardRomsetDefinitions())
            if (!strcmp(argv[1], d.name))
                def = &d;

        check(def != nullptr, "Unknown ROM set");
        RomsetInfo info;
        check(load_directory(argv[2], *def, info), "ROM verification failed");
        printf("VSC55 STRESS 2 (sample FIFO) MODEL=%s SECONDS_PER_CASE=30 BLOCK_MS=20 QPC_HZ=%lld\n", def->name,
               (long long)freq.QuadPart);
        puts("Offline normal-priority benchmark; no MIDI driver or sound output. Disk writes and hashes excluded from timing.");
        fflush(stdout);
        const char *names[] = {"silence", "three_note", "held_120_notes", "voice_churn", "churn_controllers_drums"};

        for (unsigned test = 0; test < 5; test++)
        {
            if (argc == 4 && test < 3)
                continue;

            Emulator emu;
            check(emu.Init({}) && emu.LoadRoms(def->romset, info), "Core init failed");
            emu.Reset();
            Render r{emu};
            r.rate = PCM_GetOutputFrequency(emu.GetPCM());
            emu.SetSampleCallback(Render::sample, &r);

            for (unsigned i = 0; i < r.rate * 3; i++)
                r.next();

            r.midi({0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7});

            for (unsigned i = 0; i < r.rate / 2; i++)
                r.next();

            for (unsigned ch = 0; ch < 16; ch++)
            {
                r.midi({uint8_t(0xc0 | ch), uint8_t(test < 2 ? 0 : programs[ch])});
                r.midi({uint8_t(0xb0 | ch), 7, 80});
                r.midi({uint8_t(0xb0 | ch), 91, 80});
                r.midi({uint8_t(0xb0 | ch), 93, 64});
            }

            for (unsigned i = 0; i < r.rate / 2; i++)
                r.next();

            if (test == 1)
            {
                r.midi({0x90, 60, 100});
                r.midi({0x90, 64, 100});
                r.midi({0x90, 67, 100});
                r.midi({0xb0, 64, 127});
            }

            if (test == 2)
                notes(r, 8, 0, true);

            for (unsigned i = 0; i < r.rate; i++)
                r.next();

            r.a = r.next();
            r.b = r.next();
            r.uart_high = r.pending();
            std::vector<short> audio(1500 * 960 * 2);
            std::vector<double> times;
            times.reserve(1500);
            unsigned rotation = 0, bursts = 0, over = 0;
            double sum = 0, worst_second = 0, second = 0;
            LARGE_INTEGER total_start, total_end;
            QueryPerformanceCounter(&total_start);

            for (unsigned block = 0; block < 1500; block++)
            {
                LARGE_INTEGER begin, end;
                QueryPerformanceCounter(&begin);

                if (test == 1 && block % 50 == 0)
                {
                    for (unsigned n :
                            {
                                60u, 64u, 67u
                            })
                    {
                        r.midi({0x80, uint8_t(n), 0});
                        r.midi({0x90, uint8_t(n), 100});
                    }
                }

                // 240 ms cadence, 60 notes per chord, overlapping instrument releases.
                if (test >= 3 && block % 12 == 0)
                {
                    if (bursts)
                        notes(r, 4, rotation, false);

                    rotation = (bursts % 4) * 2;
                    notes(r, 4, rotation, true);
                    ++bursts;
                }

                if (test == 4 && block % 5 == 0)
                {
                    for (unsigned ch = 0; ch < 16; ch++)
                        if (ch != 9)
                        {
                            r.midi({uint8_t(0xb0 | ch), 1, uint8_t((block / 5) % 128)});
                            r.midi({uint8_t(0xe0 | ch), 0, uint8_t(60 + (block / 5) % 9)});
                        }

                    for (unsigned n :
                            {
                                36u, 38u, 42u, 46u
                            })
                    {
                        if (block)
                            r.midi({0x89, uint8_t(n), 0});
                        r.midi({0x99, uint8_t(n), 110});
                    }
                }

                r.block(audio.data() + block * 1920);
                QueryPerformanceCounter(&end);
                double ms = 1000.0 * double(end.QuadPart - begin.QuadPart) / double(freq.QuadPart);
                check(ms >= 0, "Timer moved backwards");
                times.push_back(ms);
                sum += ms;
                second += ms;

                if (ms > 20)
                    ++over;

                if (block % 50 == 49)
                {
                    worst_second = std::max(worst_second, second);
                    second = 0;
                }

                check(DWORD(GetTickCount() - r.watchdog) < 600000, "Case exceeded ten minutes");
            }

            QueryPerformanceCounter(&total_end);
            double wall = 1000.0 * double(total_end.QuadPart - total_start.QuadPart) / double(freq.QuadPart);
            check(wall > 0, "Invalid timer duration");
            std::sort(times.begin(), times.end());
            SHA256_Digest hash;
            check(SHA256_HashBytes(std::span<uint8_t>((uint8_t *)audio.data(), audio.size()*sizeof(short)), hash), "Hash failed");
            int lo = 32767, hi = -32768;

            for (unsigned i = 0; i < audio.size(); i += 2)
            {
                lo = std::min(lo, int(audio[i]));
                hi = std::max(hi, int(audio[i]));
            }

            if (test >= 2)
                check(hi - lo > 8, "Heavy workload produced no varying audio");

            printf("CASE=%s EMULATED_MS=30000 WALL_MS=%.3f REALTIME_FACTOR=%.3f MEAN_BLOCK_MS=%.3f P95_MS=%.3f P99_MS=%.3f MAX_MS=%.3f OVER_20MS=%u WORST_1SEC_FACTOR=%.3f UART_HIGH=%u UART_PENDING=%u PCM_RANGE=%d\n",
                   names[test], wall, 30000.0 / wall, sum / 1500, times[1424], times[1484], times.back(), over, 1000.0 / worst_second,
                   r.uart_high, r.pending(), hi - lo);
            printf("PCM_SHA256=%s ", names[test]);

            for (auto b : hash)
                printf("%02x", b);

            puts("");
            fflush(stdout);

            if (argc == 4)
            {
                char path[MAX_PATH];
                check(strlen(argv[3]) + strlen(names[test]) + 6 < sizeof(path), "PCM dump path too long");
                snprintf(path, sizeof(path), "%s-%s.pcm", argv[3], names[test]);
                FILE *f = fopen(path, "wb");
                check(f != nullptr, "PCM dump open failed");
                bool ok = fwrite(audio.data(), sizeof(short), audio.size(), f) == audio.size();
                int closed = fclose(f);
                check(ok && !closed, "PCM dump write failed");
            }
        }

        puts("RESULT=PASS (workloads completed; speed below 1x is reported, not a test error)");
        return 0;
    }
    catch (const std::exception &e)
    {
        printf("RESULT=FAIL ERROR=%s\n", e.what());
        return 1;
    }
}
