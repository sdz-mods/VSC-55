// VSC-55 live Win98 host. GPL-2.0-or-later; links the unchanged Nuked-SC55 core.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <process.h>
#include <algorithm>
#include <stdexcept>
#include "emu.h"
#include "romload.h"
#include "../common/midiproto.h"
#include "../common/samplefifo.h"
#include "../common/control.h"
#include "frontbridge.h"
static unsigned RATE = 48000;
static unsigned requested_rate = 48000;
static void load_output_rate()
{
    char path[MAX_PATH];

    if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
        return;

    char *slash = strrchr(path, '\\');

    if (!slash || size_t(slash - path) + 11 >= MAX_PATH)
        return;

    strcpy(slash + 1, "VSC55.INI");
    requested_rate = GetPrivateProfileIntA("Audio", "SampleRate", 48000, path);

    for (unsigned rate :
            {
                11025u, 16000u, 22050u, 32000u, 44100u, 48000u
            })

        if (requested_rate == rate)
        {
            RATE = rate;
            return;
        }
}
static unsigned buffer_count = 8;
static unsigned BLOCK = 960, block_ms = 20;
static UINT output_device = WAVE_MAPPER;
#include "../common/output_gain.h"
#include "../common/version.h"
static LONG output_volume = 100;
static VSC_STATUS *panel_status;
struct PanelBridge
{
    HANDLE instance = nullptr, map = nullptr, stop = nullptr;
    ~PanelBridge()
    {
        if (panel_status)
        {
            InterlockedExchange(&panel_status->state, VSC_STATE_STOPPED);
            UnmapViewOfFile(panel_status);
            panel_status = nullptr;
        }

        if (map)
            CloseHandle(map);

        if (stop)
            CloseHandle(stop);

        if (instance)
            CloseHandle(instance);
    }
};
struct Event
{
    unsigned kind, size, offset;
    DWORD queued;
    unsigned char bytes[VSC_MAX_SYSEX];
};
static Event events[VSC_QUEUE];
static unsigned head, tail, highwater, expected, received;
static LONG used;
#ifdef VSC55_TIMING_DIAGNOSTICS
#define VSC_TIMING(...) __VA_ARGS__
#include "../common/timingdiag.h"
enum { TD_LOOP, TD_FRONT, TD_PRIORITY, TD_SCAN, TD_WAIT, TD_RENDER, TD_WRITE, TD_SNAPSHOT, TD_FEED_AGE, TD_RX_DRAIN, TD_RX_WAIT, TD_RX_MESSAGES, TD_UNDERRUN, TD_RESTART };
static const char *const timing_names[16] = {"LOOP", "FRONT", "PRIORITY", "SCAN", "WAIT", "RENDER", "WRITE", "SNAPSHOT", "FEED_AGE", "RX_DRAIN", "RX_WAIT", "RX_MESSAGES", "UNDERRUN", "RESTART", "UNUSED14", "UNUSED15"};
static TimingDiag worker_timing(&used), receiver_timing(&used);
static DWORD last_feed_trace;
#else
#define VSC_TIMING(...)
#endif
static unsigned queue_used()
{
    return (unsigned)InterlockedCompareExchange(&used, 0, 0);
}
static unsigned char staging[VSC_MAX_SYSEX];
static LONG stopping, finished, failed, ready, midi_activity;
static HANDLE ready_event;
static DWORD session;
static bool active;
static unsigned accepted, completed, midi_bytes, uart_high, uart_pending, backpressure, underruns, blocks, nonzero;
static unsigned midi_hash = 2166136261u;
static const RomsetDefinition *definition;
static char rom_path[MAX_PATH], capture_path[MAX_PATH];
static unsigned seconds;
static bool demo, no_midi;
static VSC_DIRECT_QUEUE *direct_queue;
using MapSLFunction = void *(WINAPI *)(DWORD);
static MapSLFunction map_sl;
static DWORD last_direct_report, last_drain;
static unsigned direct_wakes;
static DWORD shared_read(DWORD *value)
{
    return (DWORD)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
}
static void shared_write(DWORD *value, DWORD data)
{
    InterlockedExchange((volatile LONG *)value, (LONG)data);
}
static_assert(sizeof(VSC_DIRECT_EVENT) == 20 && sizeof(VSC_DIRECT_QUEUE) == 20644, "Win16 queue ABI");
static FILE *log_file;
static constexpr unsigned HISTORY = 600;
struct BridgeSample
{
    DWORD tick, final, values[PS_COUNT];
    unsigned host_queue;
    DWORD trace[8];
};
static BridgeSample bridge_history[HISTORY];
static DWORD bridge_values[PS_COUNT];
static unsigned bridge_samples;
struct AudioSample
{
    DWORD tick, elapsed, audio_ms, uart, host_age_ms, render_max_us, over20, gap_max_ms, lag_ms;
    unsigned queue, underruns, buffers;
};
static AudioSample audio_history[HISTORY];
static unsigned audio_samples, host_age_max, render_over20;
static DWORD render_max_us, render_gap_max, previous_render_end, audio_start;
static unsigned long long render_total_us;
static LARGE_INTEGER render_frequency;
static void snapshot_audio(Emulator &emu, unsigned buffers)
{
    if (panel_status)
    {
        InterlockedExchange(&panel_status->blocks, blocks);
        InterlockedExchange(&panel_status->underruns, underruns);
        InterlockedExchange(&panel_status->midi_bytes, midi_bytes);
        InterlockedExchange(&panel_status->queue, queue_used());
    }

    DWORD now = GetTickCount(), elapsed = now - audio_start;
    DWORD audio_ms = (DWORD)((unsigned long long)blocks * BLOCK * 1000 / RATE);
    auto &m = emu.GetMCU();
    uart_pending = (m.uart_write_ptr + uart_buffer_size - m.uart_read_ptr) % uart_buffer_size;
    audio_history[audio_samples++ % HISTORY] = {now, elapsed, audio_ms, uart_pending, host_age_max,
                                                render_max_us, render_over20, render_gap_max, elapsed > audio_ms ? elapsed - audio_ms : 0,
                                                queue_used(), underruns, buffers
                                               };
}
static void write_diagnostics()
{
    VSC_TIMING(worker_timing.dump(log_file, "WORKER", timing_names);)
    VSC_TIMING(receiver_timing.dump(log_file, "RECEIVER", timing_names);)
    const char *fields[] = {"VERSION", "ACCEPTED", "POPPED", "FULL", "DEAD", "BUSY", "NOMEM",
                            "QUEUE_HIGH", "PENDING", "OLDEST_MS", "AGE_MAX_MS", "SHORT_REJECT", "LONG_REJECT",
                            "PROGRAM_REJECT", "OFF_REJECT", "CC_REJECT", "LAST_REJECT", "OPEN_REJECT", "CLOSE_REJECT", "RESET_REJECT", "POST_FAIL",
                            "POLLS", "POLL_GAP_MAX_MS", "BUSY_GAP_MAX_MS", "POLL_MAX_MS", "HOST_FULL", "FORWARDED"
                           };
    fprintf(log_file,
            "DIAG_SUMMARY BRIDGE_SAMPLES=%u AUDIO_SAMPLES=%u RETAINED_LIMIT=%u DRIVER_VERSION=%lu RENDER_MEAN_MS=%.3f RENDER_MAX_MS=%.3f RENDER_OVER20=%u HOST_AGE_MAX_MS=%u\n",
            bridge_samples, audio_samples, HISTORY, (unsigned long)bridge_values[DS_VERSION],
            blocks ? double(render_total_us) / blocks / 1000 : 0,
            double(render_max_us) / 1000, render_over20, host_age_max);

    for (unsigned i = bridge_samples > HISTORY ? bridge_samples - HISTORY : 0; i < bridge_samples; i++)
    {
        const auto &s = bridge_history[i % HISTORY];
        fprintf(log_file,
                "DRIVER_TRACE TICK=%lu STAGE=%lu MSG=%lu P1=%08lx P2=%08lx ENTERED=%lu RETURNED=%lu RESULT=%lu BUSY=%lu\n",
                (unsigned long)s.tick, (unsigned long)s.trace[0], (unsigned long)s.trace[1], (unsigned long)s.trace[2],
                (unsigned long)s.trace[3], (unsigned long)s.trace[4], (unsigned long)s.trace[5], (unsigned long)s.trace[6],
                (unsigned long)s.trace[7]);
        fprintf(log_file, "BRIDGE TICK=%lu FINAL=%lu HOST_QUEUE=%u", (unsigned long)s.tick, (unsigned long)s.final,
                s.host_queue);

        for (unsigned n = 0; n < PS_COUNT; n++)
            fprintf(log_file, " %s=%lu", fields[n], (unsigned long)s.values[n]);

        fputc('\n', log_file);
    }

    for (unsigned i = audio_samples > HISTORY ? audio_samples - HISTORY : 0; i < audio_samples; i++)
    {
        const auto &s = audio_history[i % HISTORY];
        fprintf(log_file,
                "AUDIO TICK=%lu ELAPSED_MS=%lu AUDIO_MS=%lu LAG_MS=%lu UART=%lu HOST_QUEUE=%u HOST_AGE_MAX_MS=%lu RENDER_MAX_US=%lu OVER20=%lu GAP_MAX_MS=%lu UNDERRUNS=%u BUFFERS=%u\n",
                (unsigned long)s.tick, (unsigned long)s.elapsed, (unsigned long)s.audio_ms, (unsigned long)s.lag_ms,
                (unsigned long)s.uart, s.queue, (unsigned long)s.host_age_ms, (unsigned long)s.render_max_us,
                (unsigned long)s.over20, (unsigned long)s.gap_max_ms, s.underruns, s.buffers);
    }
}
// Active/idle scheduling policy: protect
// active audio/MIDI, release priority after two quiet seconds and on exit.
struct RenderPriority
{
    DWORD original_class = GetPriorityClass(GetCurrentProcess());
    int original_thread = GetThreadPriority(GetCurrentThread());
    bool requested = false, elevated = false;
    unsigned entries = 0, failures = 0, mismatches = 0;
    DWORD last_error = 0, observed_class = 0;
    int observed_thread = 0;
    RenderPriority()
    {
        if (!original_class)
            original_class = NORMAL_PRIORITY_CLASS;

        if (original_thread == THREAD_PRIORITY_ERROR_RETURN)
            original_thread = THREAD_PRIORITY_NORMAL;
    }
    void restore()
    {
        if (elevated)
        {
            if (!SetThreadPriority(GetCurrentThread(), original_thread))
            {
                last_error = GetLastError();
                ++failures;
            }

            if (!SetPriorityClass(GetCurrentProcess(), original_class))
            {
                last_error = GetLastError();
                ++failures;
            }

            elevated = false;
        }
    }
    void set(bool active)
    {
        if (active == requested)
            return;

        requested = active;

        if (!active)
        {
            restore();
            return;
        }

        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS))
        {
            last_error = GetLastError();
            ++failures;
            return;
        }

        elevated = true;

        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        {
            last_error = GetLastError();
            ++failures;
            restore();
            return;
        }

        observed_class = GetPriorityClass(GetCurrentProcess());
        observed_thread = GetThreadPriority(GetCurrentThread());
        ++entries;

        if (observed_class != REALTIME_PRIORITY_CLASS || observed_thread != THREAD_PRIORITY_TIME_CRITICAL)
            ++mismatches;
    }
    ~RenderPriority()
    {
        restore();
    }
};
static void require(bool ok, const char *text)
{
    if (!ok)
        throw std::runtime_error(text);
}
static void logline(const char *text)
{
    fprintf(log_file, "%lu %s\n", (unsigned long)GetTickCount(), text);
    fflush(log_file);
}
static void le16(FILE *f, unsigned v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void le32(FILE *f, unsigned v)
{
    le16(f, v);
    le16(f, v >> 16);
}
static void wav_header(FILE *f, unsigned frames)
{
    rewind(f);
    fwrite("RIFF", 1, 4, f);
    le32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16);
    le16(f, 1);
    le16(f, 2);
    le32(f, RATE);
    le32(f, RATE * 4);
    le16(f, 4);
    le16(f, 16);
    fwrite("data", 1, 4, f);
    le32(f, frames * 4);
}
static LRESULT enqueue(unsigned kind, const unsigned char *bytes, unsigned size)
{
    // Single producer (window thread), single consumer (audio worker).
    // Publish/release with interlocked operations; never block realtime audio.
    if (queue_used() == VSC_QUEUE)
    {
        ++backpressure;
        return VSC_FULL;
    }

    Event &e = events[head];
    e.kind = kind;
    e.size = size;
    e.offset = 0;
    e.queued = GetTickCount();

    if (size)
        memcpy(e.bytes, bytes, size);

    head = (head + 1) % VSC_QUEUE;
    unsigned occupancy = (unsigned)InterlockedIncrement(&used);
    ++accepted;
    InterlockedExchange(&midi_activity, (LONG)GetTickCount());
    highwater = std::max(highwater, occupancy);
    return VSC_OK;
}
static LRESULT protocol(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == VSC_PING)
        return VSC_MAGIC;

    if (msg == VSC_SESSION)
        return session;

    if (no_midi || !InterlockedCompareExchange(&ready, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0))
        return 0;

    if (msg == VSC_OPEN)
    {
        if (active)
            return 0;

        LRESULT r = enqueue(msg, nullptr, 0);

        if (r == VSC_OK)
            active = true;

        return r;
    }

    if (!active)
        return 0;

    if (msg == VSC_ABORT)
    {
        expected = received = 0;
        return VSC_OK;
    }

    if (msg == VSC_CLOSE || msg == VSC_RESET)
    {
        unsigned char reset[144];
        unsigned at = 0;

        for (unsigned ch = 0; ch < 16; ch++)
            for (unsigned cc :
                    {
                        64u, 120u, 123u
                    })
            {
                reset[at++] = 0xb0 | ch;
                reset[at++] = cc;
                reset[at++] = 0;
            }
        LRESULT r = enqueue(msg, reset, at);

        if (r == VSC_OK)
        {
            expected = received = 0;

            if (msg == VSC_CLOSE)
                active = false;
        }

        return r;
    }

    if (msg == VSC_BEGIN)
    {
        if (expected || !lp || (DWORD)lp > VSC_MAX_SYSEX)
            return 0;

        bool full = queue_used() == VSC_QUEUE;

        if (full)
        {
            ++backpressure;
            return VSC_FULL;
        }

        expected = (unsigned)lp;
        received = 0;
        return VSC_OK;
    }

    if (msg == VSC_CHUNK)
    {
        if (!expected || wp < 1 || wp > 3 || received + wp > expected)
            return 0;

        for (unsigned i = 0; i < wp; i++)
            staging[received++] = (unsigned char)((DWORD)lp >> (i * 8));

        return VSC_OK;
    }

    if (msg == VSC_COMMIT)
    {
        if (!expected || received != expected)
            return 0;

        LRESULT r = enqueue(msg, staging, received);

        if (r == VSC_OK)
            expected = received = 0;

        return r;
    }

    if (msg == VSC_SHORT)
    {
        unsigned char bytes[3];
        unsigned status = (unsigned)lp & 255;

        if (expected || status < 0x80 || status == 0xf0 || status == 0xf7)
            return 0;

        unsigned size = status < 0xf0 ? (((status & 0xf0) == 0xc0 ||
                                          (status & 0xf0) == 0xd0) ? 2 : 3) : (status == 0xf2 ? 3 : ((status == 0xf1 || status == 0xf3) ? 2 : 1));

        for (unsigned i = 0; i < size; i++)
            bytes[i] = (unsigned char)((DWORD)lp >> (i * 8));

        return enqueue(msg, bytes, size);
    }

    return 0;
}
// One Win32 window thread consumes driver-owned storage. The audio worker
// continues using its existing SPSC queue; no Win16 API calls are needed here.
static void direct_snapshot(bool final)
{
    if (!direct_queue)
        return;

    auto *q = direct_queue;

    for (unsigned i = 0; i < DS_COUNT; i++)
        bridge_values[i] = shared_read(&q->stats[i]);

    unsigned head = shared_read(&q->head), tail = shared_read(&q->tail);
    bridge_values[DS_PENDING] = (head + VSC_RING_SLOTS - tail) % VSC_RING_SLOTS;
    bridge_values[DS_OLDEST] = head != tail ? GetTickCount() - q->events[tail].queued : 0;
    bridge_values[DS_AGE_MAX] = std::max(bridge_values[DS_AGE_MAX], bridge_values[DS_OLDEST]);
    auto &sample = bridge_history[bridge_samples++ % HISTORY];
    sample.tick = GetTickCount();
    sample.final = final;
    sample.host_queue = queue_used();
    DWORD *trace = &q->trace_stage;

    for (unsigned i = 0; i < 8; i++)
        sample.trace[i] = shared_read(trace + i);

    memcpy(sample.values, bridge_values, sizeof(bridge_values));
    last_direct_report = sample.tick;
}
static bool drain_direct()
{
    VSC_TIMING(TimingSpan timing(receiver_timing, TD_RX_DRAIN);)

    if (!direct_queue)
        return true;

    auto *q = direct_queue;
    DWORD start = GetTickCount();
    DWORD gap = last_drain ? start - last_drain : 0;
    last_drain = start;
    bridge_values[PS_GAP_MAX] = std::max(bridge_values[PS_GAP_MAX], gap);
    unsigned head = shared_read(&q->head), tail = shared_read(&q->tail);

    if (head >= VSC_RING_SLOTS || tail >= VSC_RING_SLOTS)
        return false;

    if (head == tail)
        return true;

    ++bridge_values[PS_POLLS];
    bridge_values[PS_BUSY_GAP_MAX] = std::max(bridge_values[PS_BUSY_GAP_MAX], gap);
    unsigned budget = 128;

    while (tail != shared_read(&q->head) && budget--)
    {
        if (queue_used() == VSC_QUEUE)
        {
            ++bridge_values[PS_BACKPRESSURE];
            break;
        }

        VSC_DIRECT_EVENT event = q->events[tail];
        LRESULT result = 0;

        if (event.kind == VSC_COMMIT)
        {
            if (!event.length || event.length > VSC_MAX_SYSEX || !event.data16)
                return false;

            auto *data = (unsigned char *)map_sl(event.data16);

            if (!data || IsBadReadPtr(data, event.length))
                return false;

            if (!active || expected)
                return false;

            result = enqueue(VSC_COMMIT, data, event.length);
        }
        else
        {
            if (event.length || event.data16 || (event.kind != VSC_OPEN && event.kind != VSC_CLOSE && event.kind != VSC_RESET &&
                                                 event.kind != VSC_SHORT))
                return false;

            result = protocol(event.kind, 0, event.scalar);
        }

        if (result == VSC_FULL)
        {
            ++bridge_values[PS_BACKPRESSURE];
            break;
        }

        if (result != VSC_OK)
            return false;

        DWORD age = GetTickCount() - event.queued;
        shared_write(&q->stats[DS_AGE_MAX], std::max(shared_read(&q->stats[DS_AGE_MAX]), age));
        shared_write(&q->stats[DS_POPPED], shared_read(&q->stats[DS_POPPED]) + 1);
        ++bridge_values[PS_FORWARDED];
        tail = (tail + 1) % VSC_RING_SLOTS;
        shared_write(&q->tail, tail); // Release only AFTER all borrowed bytes have been copied.

        if ((DWORD)(GetTickCount() - start) >= 4)
            break; // bound window-thread work, not a fixed events/timer rate
    }

    bridge_values[PS_POLL_MAX] = std::max(bridge_values[PS_POLL_MAX], GetTickCount() - start);
    return true;
}
// Only the audio worker touches emulator state. Never fill the entire UART ring:
// equal read/write pointers mean empty, so one slot must remain unused.
static void feed(Emulator &emu)
{
    auto &mcu = emu.GetMCU();
    unsigned pending = (mcu.uart_write_ptr + uart_buffer_size - mcu.uart_read_ptr) % uart_buffer_size;
    uart_high = std::max(uart_high, pending);
    unsigned capacity = uart_buffer_size - 1 - pending;
#ifdef VSC55_TIMING_DIAGNOSTICS

    if (queue_used())
    {
        DWORD now = GetTickCount(), age = now - events[tail].queued;

        if (age >= 20 && (!last_feed_trace || now - last_feed_trace >= 100))
        {
            worker_timing.add(TD_FEED_AGE, now, age, queue_used(), pending, true);
            last_feed_trace = now;
        }
    }

#endif
    unsigned char bytes[128];
    unsigned count = 0;

    while (queue_used() && count < std::min(capacity, 128u))
    {
        Event &e = events[tail];
        host_age_max = std::max(host_age_max, (unsigned)(GetTickCount() - e.queued));
        unsigned n = std::min(e.size - e.offset, std::min(capacity, 128u) - count);

        if (n)
        {
            memcpy(bytes + count, e.bytes + e.offset, n);
            count += n;
            e.offset += n;
        }

        if (e.offset == e.size)
        {
            tail = (tail + 1) % VSC_QUEUE;
            InterlockedDecrement(&used);
            ++completed;
        }
        else
            break;
    }

    if (count)
    {
        for (unsigned i = 0; i < count; i++)
            midi_hash = (midi_hash ^ bytes[i]) * 16777619u;

        emu.PostMIDI(std::span<const uint8_t>(bytes, count));
        midi_bytes += count;
    }

    uart_high = std::max(uart_high, pending + count);
}
struct Stream
{
    Emulator &emu;
    AudioFrame<int16_t> a{}, b{};
    SampleFIFO samples;
    unsigned native_rate, phase = 0, native_count = 0;
    bool feeding = false;
    static void sample(void *ptr, const AudioFrame<int32_t> &raw)
    {
        static_cast<Stream *>(ptr)->samples.push(raw);
    }
    AudioFrame<int16_t> next()
    {
        unsigned steps = 0;

        while (!samples.count)
        {
            if ((++steps & 65535) == 0 && InterlockedCompareExchange(&stopping, 0, 0))
                throw std::runtime_error("Stopped during render");

            emu.Step();
        }

        if (feeding && (++native_count & 63) == 0)
            feed(emu);

        return samples.pop();
    }
    void start()
    {
        a = next();
        b = next();
        feeding = true;
    }
    void render(short *buffer)
    {
        feed(emu);
        LONG volume = InterlockedCompareExchange(&output_volume, 0, 0);

        for (unsigned i = 0; i < BLOCK; i++)
        {
            buffer[2 * i] = (short)(a.left + (int(b.left) - a.left) * (double(phase) / RATE));
            buffer[2 * i + 1] = (short)(a.right + (int(b.right) - a.right) * (double(phase) / RATE));

            if (volume != 100)
            {
                buffer[2 * i] = vsc_output_gain(buffer[2 * i], volume);
                buffer[2 * i + 1] = vsc_output_gain(buffer[2 * i + 1], volume);
            }

            if (buffer[2 * i] || buffer[2 * i + 1])
                ++nonzero;

            phase += native_rate;

            while (phase >= RATE)
            {
                phase -= RATE;
                a = b;
                b = next();
            }
        }
    }
};
static void measured_render(Stream &stream, short *buffer)
{
    DWORD now = GetTickCount();

    if (previous_render_end)
        render_gap_max = std::max(render_gap_max, now - previous_render_end);

    LARGE_INTEGER begin, end;
    QueryPerformanceCounter(&begin);
    {
        VSC_TIMING(TimingSpan timing(worker_timing, TD_RENDER);) stream.render(buffer);
    }
    QueryPerformanceCounter(&end);
    unsigned long long us = (unsigned long long)((end.QuadPart - begin.QuadPart) * 1000000 / render_frequency.QuadPart);
    render_total_us += us;
    render_max_us = std::max(render_max_us, (DWORD)us);

    if (us > 20000)
        ++render_over20;

    previous_render_end = GetTickCount();
}
static bool audible(const short *samples)
{
    // Detect variation independently per channel, not absolute amplitude.
    // This also avoids treating any residual DC baseline as active audio.
    for (unsigned ch = 0; ch < 2; ch++)
    {
        int lo = samples[ch], hi = lo;

        for (unsigned i = 1; i < BLOCK; i++)
        {
            int value = samples[i * 2 + ch];
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }

        if (hi - lo > 8)
            return true;
    }

    return false;
}
static unsigned __stdcall audio_thread(void *)
{
    logline("WORKER_ENTER");
    RenderPriority priority;
    HWAVEOUT out = nullptr;
    HANDLE event = nullptr;
    FILE *capture = nullptr;
    std::vector<short> capture_audio;
    std::vector<WAVEHDR> headers(buffer_count);
    std::vector<std::vector<short>> buffers(buffer_count, std::vector<short>(BLOCK * 2));
    std::vector<unsigned char> in_flight(buffer_count);
    unsigned frames = 0;
    bool timer_period = false;

    try
    {
        logline("LOAD_ROMS");
        RomsetInfo info;
        require(load_directory(rom_path, *definition, info), "ROM set missing or incorrect");
        FrontBridge front;
        EMU_Options options;

        if (front_enabled)
            options.lcd_backend = &front;

        Emulator emu;
        require(emu.Init(options), "Emulator init failed");
        require(emu.LoadRoms(definition->romset, info), "ROM load failed");
        emu.Reset();

        if (front_enabled)
            require(emu.StartLCD(), "Front panel snapshot initialization failed");

        Stream stream{emu};
        stream.native_rate = PCM_GetOutputFrequency(emu.GetPCM());
        emu.SetSampleCallback(Stream::sample, &stream);
        stream.samples.dc_offset = emu.GetMCU().is_mk1 ? 512 : 0;
        fprintf(log_file, "OUTPUT_DC_SUBTRACT_PCM16=%d\n", stream.samples.dc_offset);
        fprintf(log_file, "AUDIO_SAMPLE_FIFO=1 NATIVE_RATE=%u OUTPUT_RATE=%u\n", stream.native_rate, RATE);
        logline("BOOTING");

        for (unsigned i = 0; i < stream.native_rate * 3; i++)
        {
            if ((i & 4095) == 0 && InterlockedCompareExchange(&stopping, 0, 0))
                throw std::runtime_error("Boot stopped");

            stream.next();
        }

        stream.start();
        require(QueryPerformanceFrequency(&render_frequency) != 0 &&
                render_frequency.QuadPart > 0, "Diagnostic timer unavailable");
        MMRESULT timer_result = timeBeginPeriod(VSC_TIMER_PERIOD_MS);
        timer_period = timer_result == TIMERR_NOERROR;
        fprintf(log_file, "TIMER_PERIOD_MS=%u TIMER_BEGIN_RESULT=%u\n", VSC_TIMER_PERIOD_MS, timer_result);

        if (capture_path[0])
        {
            capture_audio.resize(size_t(seconds)*RATE * 2);
            capture = fopen(capture_path, "wb");
            require(capture != nullptr, "Capture open failed");
            wav_header(capture, 0);
        }
        else
        {
            event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            require(event != nullptr, "Audio event failed");
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = RATE;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = RATE * 4;
            MMRESULT r = waveOutOpen(&out, output_device, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT);
            fprintf(log_file, "WAVE_OPEN=%u\n", r);
            require(r == 0, "waveOutOpen failed");
            require(waveOutPause(out) == 0, "waveOutPause failed");

            for (unsigned i = 0; i < buffer_count; i++)
            {
                headers[i].lpData = (char *)buffers[i].data();
                headers[i].dwBufferLength = BLOCK * 4;
                require(waveOutPrepareHeader(out, &headers[i], sizeof(WAVEHDR)) == 0, "Prepare header failed");
            }
        }

        logline(capture ? "READY capture (no sound device)" : "READY live waveOut stereo");
        fprintf(log_file, "OUTPUT_DEVICE=%u BUFFER_MS=%u BUFFER_COUNT=%u VOLUME=%ld\n", output_device, block_ms, buffer_count,
                output_volume);
        InterlockedExchange(&ready, 1);
        SetEvent(ready_event);
        DWORD start = GetTickCount(), last_completion = start, last_audio = start;
        bool primed = false;
        unsigned slot = 0;
        bool demo_on = false, demo_off = false;
        audio_start = start;
        DWORD last_snapshot = start;
        unsigned diagnostic_buffers = 0;

        while (!InterlockedCompareExchange(&stopping, 0, 0))
        {
            VSC_TIMING(TimingSpan loop_timing(worker_timing, TD_LOOP);)
            {

                VSC_TIMING(TimingSpan timing(worker_timing, TD_FRONT);) if (front_enabled)
                    front.Update(emu);
            }

            if (seconds && frames >= seconds * RATE)
                break;

            DWORD now = GetTickCount();
            DWORD last_midi = (DWORD)InterlockedCompareExchange(&midi_activity, 0, 0);
            {
                VSC_TIMING(TimingSpan timing(worker_timing, TD_PRIORITY);)
                VSC_TIMING(bool previous = priority.elevated;)
                priority.set((DWORD)(now - last_audio) < 2000 || (last_midi && (DWORD)(now - last_midi) < 2000));
                VSC_TIMING(worker_timing.realtime = priority.elevated ? 1 : 0; timing.force = previous != priority.elevated;)
                VSC_TIMING(timing.detail = previous ? 1 : 0;)
            }

            if (demo && !demo_on && frames >= RATE / 2)
            {
                const unsigned char notes[] = {0xc0, 0, 0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100};
                enqueue(VSC_SHORT, notes, sizeof(notes));
                demo_on = true;
            }

            if (demo && !demo_off && frames >= RATE * 3)
            {
                const unsigned char off[] = {0x80, 60, 0, 0x80, 64, 0, 0x80, 67, 0};
                enqueue(VSC_SHORT, off, sizeof(off));
                demo_off = true;
            }

            if (capture)
            {
                DWORD target = (DWORD)((double)frames * 1000 / RATE);

                while ((DWORD)(GetTickCount() - start) < target && !InterlockedCompareExchange(&stopping, 0, 0))
                    Sleep(1);

                measured_render(stream, buffers[0].data());

                if (audible(buffers[0].data()))
                    last_audio = GetTickCount();

                // Buffer diagnostic capture until shutdown. Avoid repeated
                // process-priority transitions and Win16 disk thunks per block.
                unsigned take = std::min(BLOCK, seconds * RATE - frames);
                memcpy(capture_audio.data() + size_t(frames) * 2, buffers[0].data(), take * 4);
                frames += take;
                ++blocks;
            }
            else
            {
                unsigned pending = 0;
                {
                    VSC_TIMING(TimingSpan timing(worker_timing, TD_SCAN);)

                    for (unsigned i = 0; i < buffer_count; i++)
                    {
                        if (in_flight[i] && (headers[i].dwFlags & WHDR_DONE))
                        {
                            in_flight[i] = false;
                            last_completion = GetTickCount();
                        }

                        if (in_flight[i])
                            ++pending;
                    }
                }

                if (primed && !pending)
                {
                    ++underruns;
                    VSC_TIMING(worker_timing.add(TD_UNDERRUN, GetTickCount(), 0, queue_used(), blocks, true);)
                }

                if (in_flight[slot])
                {
                    {
                        VSC_TIMING(TimingSpan timing(worker_timing, TD_WAIT, 30);) DWORD result = WaitForSingleObject(event, 10);
                        VSC_TIMING(timing.detail = result;)
                    }
                    require((DWORD)(GetTickCount() - last_completion) < 5000, "Audio completion timeout");
                    continue;
                }

                measured_render(stream, buffers[slot].data());

                if (audible(buffers[slot].data()))
                    last_audio = GetTickCount();

                {
                    VSC_TIMING(TimingSpan timing(worker_timing, TD_WRITE);)
                    MMRESULT result = waveOutWrite(out, &headers[slot], sizeof(WAVEHDR));
                    VSC_TIMING(timing.detail = result;)
                    require(result == 0, "waveOutWrite failed");
                }
                in_flight[slot] = true;
                slot = (slot + 1) % buffer_count;
                frames += BLOCK;
                ++blocks;
                diagnostic_buffers = pending + 1;

                if (!primed && blocks == buffer_count)
                {
                    VSC_TIMING(TimingSpan timing(worker_timing, TD_RESTART);)VSC_TIMING(timing.force = true;
                                                                                       )require(waveOutRestart(out) == 0, "waveOutRestart failed");
                    primed = true;
                    last_completion = GetTickCount();
                }
            }

            if ((DWORD)(GetTickCount() - last_snapshot) >= 1000)
            {
                VSC_TIMING(TimingSpan timing(worker_timing, TD_SNAPSHOT);)snapshot_audio(emu, diagnostic_buffers);
                last_snapshot = GetTickCount();
            }
        }

        snapshot_audio(emu, diagnostic_buffers);
        auto &mcu = emu.GetMCU();
        uart_pending = (mcu.uart_write_ptr + uart_buffer_size - mcu.uart_read_ptr) % uart_buffer_size;
    }
    catch (const std::exception &e)
    {
        priority.restore();

        if (InterlockedCompareExchange(&stopping, 0, 0))
            fprintf(log_file, "STOPPED %s\n", e.what());
        else
        {
            fprintf(log_file, "ERROR %s\n", e.what());
            InterlockedExchange(&failed, 1);
        }
    }

    priority.restore();
    fprintf(log_file,
            "PRIORITY_POLICY=ACTIVE_REALTIME ENTRIES=%u FAILURES=%u MISMATCHES=%u LAST_ERROR=%lu ACTIVE_CLASS=%lu ACTIVE_THREAD=%d RESTORED_CLASS=%lu RESTORED_THREAD=%d\n",
            priority.entries, priority.failures, priority.mismatches, (unsigned long)priority.last_error,
            (unsigned long)priority.observed_class, priority.observed_thread,
            (unsigned long)GetPriorityClass(GetCurrentProcess()), GetThreadPriority(GetCurrentThread()));

    if (out)
    {
        waveOutReset(out);

        for (auto &header : headers)
            if (header.dwFlags & WHDR_PREPARED)
                if (waveOutUnprepareHeader(out, &header, sizeof(header)))
                    InterlockedExchange(&failed, 1);

        if (waveOutClose(out))
            InterlockedExchange(&failed, 1);
    }

    if (timer_period)
        fprintf(log_file, "TIMER_END_RESULT=%u\n", timeEndPeriod(VSC_TIMER_PERIOD_MS));

    if (event)
        CloseHandle(event);

    if (capture)
    {
        wav_header(capture, frames);

        if (fwrite(capture_audio.data(), 4, frames, capture) != frames || ferror(capture))
            InterlockedExchange(&failed, 1);

        if (fclose(capture))
            InterlockedExchange(&failed, 1);
    }

    InterlockedExchange(&ready, 0);
    InterlockedExchange(&finished, 1);
    SetEvent(ready_event);
    return 0;
}
static DWORD stop_requested;
static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == VSC_CONTROL)
    {
        if (wp == VSC_CTL_DISPLAY)
        {
            InterlockedExchange(&front_heartbeat, lp ? (LONG)GetTickCount() : 0);

            if (!lp)
                InterlockedExchange(&front_buttons, 0);

            return 0;
        }

        if (wp == VSC_CTL_BUTTONS)
        {
            InterlockedExchange(&front_buttons, (LONG)lp & VSC_FRONT_BUTTON_MASK);
            return 0;
        }

        if (wp == VSC_CTL_VOLUME && lp >= 0 && lp <= VSC_MAX_VOLUME_PERCENT)
        {
            InterlockedExchange(&output_volume, (LONG)lp);

            if (panel_status)
                InterlockedExchange(&panel_status->volume, (LONG)lp);

            return 0;
        }

        unsigned char bytes[144];
        unsigned n = 0;

        if (wp == VSC_CTL_PANIC)
        {
            for (unsigned ch = 0; ch < 16; ch++)
                for (unsigned cc :
                        {
                            64u, 120u, 123u
                        })
                {
                    bytes[n++] = 0xb0 | ch;
                    bytes[n++] = cc;
                    bytes[n++] = 0;
                }
        }
        else if (wp == VSC_CTL_GS)
        {
            const unsigned char reset[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7};
            memcpy(bytes, reset, sizeof(reset));
            n = sizeof(reset);
        }
        else if (wp == VSC_CTL_GM)
        {
            const unsigned char reset[] = {0xf0, 0x7e, 0x7f, 9, 1, 0xf7};
            memcpy(bytes, reset, sizeof(reset));
            n = sizeof(reset);
        }

        if (n && panel_status)
            InterlockedExchange(&panel_status->command_error, enqueue(VSC_COMMIT, bytes, n) == VSC_OK ? 0 : 1);

        return 0;
    }

    if (msg == VSC_BIND)
    {
        if (no_midi || wp != VSC_DIAG_VERSION || !map_sl || direct_queue)
            return 0;

        auto *q = (VSC_DIRECT_QUEUE *)map_sl((DWORD)lp);

        if (!q || IsBadReadPtr(q, sizeof(*q)) || q->magic != VSC_DIRECT_MAGIC || q->version != VSC_DIAG_VERSION ||
                q->session != session || !q->enabled)
        {
            InterlockedExchange(&failed, 1);
            PostMessageA(window, WM_CLOSE, 0, 0);
            return 0;
        }

        direct_queue = q;
        direct_snapshot(false);
        return VSC_OK;
    }

    if (msg == VSC_WAKE)
    {
        if ((DWORD)lp == session)
        {
            ++direct_wakes;

            if (direct_queue)
                shared_write(&direct_queue->wake, 0);
        }

        return 0;
    }

    if (msg >= VSC_PING && msg <= VSC_RESET)
        return protocol(msg, wp, lp);

    if (msg == WM_QUERYENDSESSION)
        return TRUE;

    if (msg == WM_ENDSESSION)
    {
        if (wp)
            SendMessageA(window, WM_CLOSE, 0, 0);

        return 0;
    }

    if (msg == WM_CLOSE)
    {
        if (panel_status)
            InterlockedExchange(&panel_status->state, VSC_STATE_STOPPING);

        if (!stop_requested)
        {
            stop_requested = GetTickCount();

            if (!stop_requested)
                stop_requested = 1;
        }

        SetWindowLongA(window, 4, 0); // prevent a new client rebinding during shutdown

        if (direct_queue)
        {
            shared_write(&direct_queue->enabled, 0);
            direct_snapshot(true);
            direct_queue = nullptr;
        }

        DestroyWindow(window);
        return 0;
    }

    if (msg == WM_TIMER)
    {
        if (direct_queue && (DWORD)(GetTickCount() - last_direct_report) >= 1000)
            direct_snapshot(false);

        if (stop_requested && (DWORD)(GetTickCount() - stop_requested) > 5000)
        {
            InterlockedExchange(&failed, 1);
            DestroyWindow(window);
        }
        else if (InterlockedCompareExchange(&finished, 0, 0))
            PostMessageA(window, WM_CLOSE, 0, 0);

        return 0;
    }

    if (msg == WM_DESTROY)
    {
        InterlockedExchange(&stopping, 1);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(window, msg, wp, lp);
}
static LONG WINAPI crash_report(EXCEPTION_POINTERS *fault)
{
    DWORD info[] = {fault->ExceptionRecord->ExceptionCode, (DWORD)fault->ExceptionRecord->ExceptionAddress, fault->ContextRecord->Eip, fault->ContextRecord->Esp, fault->ContextRecord->SegSs, fault->ContextRecord->SegDs, fault->ContextRecord->Ebp, fault->ContextRecord->Eax, fault->ContextRecord->Ebx, fault->ContextRecord->Ecx, fault->ContextRecord->Edx, fault->ExceptionRecord->NumberParameters, (DWORD)fault->ExceptionRecord->ExceptionInformation[0], (DWORD)fault->ExceptionRecord->ExceptionInformation[1]};
    HANDLE file = CreateFileA("VSCERR.BIN", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, info, sizeof(info), &written, nullptr);
        void *stack = (void *)fault->ContextRecord->Esp;

        if (!IsBadReadPtr(stack, 256))
            WriteFile(file, stack, 256, &written, nullptr);

        CloseHandle(file);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--stop"))
    {
        HWND w = FindWindowA(VSC_CLASS, nullptr);

        if (!w)
        {
            puts("No host running");
            return 1;
        }

        DWORD pid = 0, code = 1;
        GetWindowThreadProcessId(w, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);

        if (!process)
        {
            puts("Cannot monitor host shutdown");
            return 1;
        }

        if (!PostMessageA(w, WM_CLOSE, 0, 0) || WaitForSingleObject(process, 15000) != WAIT_OBJECT_0)
        {
            CloseHandle(process);
            puts("Host did not stop");
            return 1;
        }

        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        printf("Host stopped, exit=%lu. VSC55.LOG is ready to copy.\n", (unsigned long)code);
        return code ? 1 : 0;
    }

    if (argc == 2 && !strcmp(argv[1], "--list"))
    {
        for (const auto &def : GetStandardRomsetDefinitions())
            puts(def.name);

        return 0;
    }

    if (argc < 3)
    {
        puts("VSC55 <romset> <ROM-directory> [--seconds N] [--demo] [--capture WAV]\n  [--device N|-1] [--buffer-ms 10|20|40|80] [--buffer-count 4|8|16] [--volume 0..1600] [--front-panel]\nVSC55 --stop | --list");
        return 2;
    }

    for (const auto &def : GetStandardRomsetDefinitions())
        if (!strcmp(argv[1], def.name))
            definition = &def;

    if (!definition || !GetFullPathNameA(argv[2], MAX_PATH, rom_path, nullptr))
    {
        puts("Invalid ROM set/path");
        return 2;
    }

    for (int i = 3; i < argc; i++)
    {
        if (!strcmp(argv[i], "--front-panel"))
        {
            front_enabled = true;
            continue;
        }

        if ((!strcmp(argv[i], "--device") || !strcmp(argv[i], "--buffer-ms") || !strcmp(argv[i], "--buffer-count") ||
                !strcmp(argv[i], "--volume")) && i + 1 < argc)
        {
            const char *option = argv[i];
            char *end = nullptr;
            long value = strtol(argv[++i], &end, 10);

            if (!*argv[i] || *end)
            {
                puts("Invalid numeric option");
                return 2;
            }

            if (!strcmp(option, "--device"))
            {
                if (value < -1 || value > 65534)
                    return 2;

                output_device = value == -1 ? WAVE_MAPPER : (UINT)value;
            }
            else if (!strcmp(option, "--buffer-count"))
            {
                if (value != 4 && value != 8 && value != 16)
                    return 2;

                buffer_count = (unsigned)value;
            }
            else if (!strcmp(option, "--volume"))
            {
                if (value < 0 || value > VSC_MAX_VOLUME_PERCENT)
                    return 2;

                output_volume = value;
            }
            else
            {
                if (value != 10 && value != 20 && value != 40 && value != 80)
                    return 2;

                block_ms = (unsigned)value;
            }

            continue;
        }

        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--demo"))
            demo = true;
        else if (!strcmp(argv[i], "--no-midi"))
            no_midi = true;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
            GetFullPathNameA(argv[++i], MAX_PATH, capture_path, nullptr);
        else
        {
            puts("Invalid option");
            return 2;
        }
    }

    load_output_rate();
    BLOCK = (RATE * block_ms + 500) / 1000;

    if (demo && !no_midi)
    {
        puts("--demo requires --no-midi (one queue producer)");
        return 2;
    }

    if (seconds > 3600 || (capture_path[0] && !seconds))
    {
        puts("Capture requires --seconds 1..3600");
        return 2;
    }

    PanelBridge panel;
    panel.instance = CreateMutexA(nullptr, FALSE, VSC_HOST_MUTEX);

    if (!panel.instance || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        puts("Host already running");
        return 1;
    }

    if (FindWindowA(VSC_CLASS, nullptr) || FindWindowA("VSC55_PUMP", nullptr))
    {
        puts("Stop the existing receiver/host first");
        return 1;
    }

    char directory[MAX_PATH];
    GetModuleFileNameA(nullptr, directory, MAX_PATH);
    char *slash = strrchr(directory, '\\');

    if (!slash)
        return 1;

    *slash = 0;

    if (strlen(directory) + 14 >= MAX_PATH)
        return 1;

    SetCurrentDirectoryA(directory);
    SetUnhandledExceptionFilter(crash_report);
    log_file = fopen("VSC55.LOG", "w");

    if (!log_file)
        return 1;

    fprintf(log_file, "VSC55 live build 16 (direct transport v5) MODEL=%s\n", definition->name);
    fflush(log_file);
#ifdef VSC55_TIMING_DIAGNOSTICS
    fputs("TIMING_DIAGNOSTICS=1\n", log_file);
#else
    fputs("TIMING_DIAGNOSTICS=0\n", log_file);
#endif
    fprintf(log_file, "BUILD_PROFILE=%s\n", VSC55_BUILD_PROFILE);
    fprintf(log_file, "SOFTWARE_REVISION=%s\n", VSC_SOFTWARE_REVISION);
    {
        HANDLE console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        fprintf(log_file, "HOST_CONSOLE_ATTACHED=%u\n", console != INVALID_HANDLE_VALUE ? 1u : 0u);

        if (console != INVALID_HANDLE_VALUE)
            CloseHandle(console);
    }
    logline("MAIN_INIT");
    fprintf(log_file, "OUTPUT_RATE_REQUESTED=%u OUTPUT_RATE=%u BUFFER_FRAMES=%u\n", requested_rate, RATE, BLOCK);

    if (requested_rate != RATE)
        logline("Unsupported Audio/SampleRate; using 48000 Hz");

    map_sl = (MapSLFunction)GetProcAddress(GetModuleHandleA("KERNEL32.DLL"), "MapSL");

    if (!no_midi && !map_sl)
    {
        logline("ERROR Win98 shared-pointer mapping unavailable");
        fclose(log_file);
        return 1;
    }

    fprintf(log_file, "TRANSPORT=DIRECT_SHARED_POSTMESSAGE DRIVER_CAPACITY=%u HOST_CAPACITY=%u\n", VSC_DIRECT_CAPACITY,
            VSC_QUEUE);
    panel.map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(VSC_STATUS), VSC_STATUS_NAME);
    panel.stop = CreateEventA(nullptr, TRUE, FALSE, VSC_STOP_EVENT);

    if (panel.map)
        panel_status = (VSC_STATUS *)MapViewOfFile(panel.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VSC_STATUS));

    if (!panel_status || !panel.stop)
    {
        logline("ERROR control panel bridge unavailable");
        fclose(log_file);
        return 1;
    }

    ResetEvent(panel.stop);
    memset(panel_status, 0, sizeof(*panel_status));
    panel_status->pid = GetCurrentProcessId();
    panel_status->state = VSC_STATE_STARTING;
    panel_status->volume = output_volume;
    panel_status->buffer_ms = block_ms;
    panel_status->device = (LONG)output_device;
    lstrcpynA(panel_status->model, definition->name, sizeof(panel_status->model));
    InterlockedExchange(&panel_status->magic, VSC_STATUS_MAGIC);
    ready_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    unsigned worker_id = 0; // Initialize CRT per-thread state; Win98 needs a thread-ID pointer.
    HANDLE worker = ready_event ? (HANDLE)_beginthreadex(nullptr, 0, audio_thread, nullptr, 0, &worker_id) : nullptr;

    if (!worker)
    {
        fprintf(log_file, "WORKER_CREATE_FAILED error=%lu event=%p\n", (unsigned long)GetLastError(), ready_event);
        fclose(log_file);
        return 1;
    }

    logline("WORKER_CREATED");
    HANDLE startup_events[] = {ready_event, panel.stop};

    if (WaitForMultipleObjects(2, startup_events, FALSE, 120000) != WAIT_OBJECT_0 ||
            !InterlockedCompareExchange(&ready, 0, 0))
        InterlockedExchange(&stopping, 1);
    else
    {
        session = GetTickCount()^GetCurrentProcessId();

        if (!session)
            session = 1;

        WNDCLASSA cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = GetModuleHandleA(nullptr);
        cls.lpszClassName = VSC_CLASS;
        cls.cbWndExtra = 8;
        HWND window = nullptr;

        if (RegisterClassA(&cls))
            window = CreateWindowA(VSC_CLASS, "VSC-55 live synthesizer", 0, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, nullptr);

        if (!window || !SetTimer(window, 1, 100, nullptr))
        {
            InterlockedExchange(&failed, 1);
            InterlockedExchange(&stopping, 1);

            if (window)
                DestroyWindow(window);
        }
        else
        {
            InterlockedExchange(&panel_status->state, VSC_STATE_RUNNING);
            SetWindowLongA(window, 0, (LONG)session);

            if (!no_midi)
                SetWindowLongA(window, 4, VSC_DIRECT_MAGIC);

            puts(no_midi ? "Audio test running without MIDI bridge." :
                 "SC-55 booted; direct MIDI transport ready. Stop with VSC55 --stop.");
            fflush(stdout);
            // Wait in Win32 and explicitly drain messages. Avoid
            // blocking GetMessage's Win16 thunk while the process is realtime.
            MSG msg;
            bool quit = false;

            while (!quit)
            {
                if (!drain_direct())
                {
                    InterlockedExchange(&failed, 1);
                    PostMessageA(window, WM_CLOSE, 0, 0);
                }

                DWORD wait;
                {
                    VSC_TIMING(TimingSpan timing(receiver_timing, TD_RX_WAIT, direct_queue ? 30 : 120);
                              )wait = MsgWaitForMultipleObjects(1, &panel.stop, FALSE, direct_queue ? 10 : 100, QS_ALLINPUT);
                    VSC_TIMING(timing.detail = wait;)
                }

                if (wait == WAIT_OBJECT_0)
                {
                    ResetEvent(panel.stop);
                    PostMessageA(window, WM_CLOSE, 0, 0);
                }

                if (wait == WAIT_FAILED)
                {
                    InterlockedExchange(&failed, 1);
                    DestroyWindow(window);
                    break;
                }

                VSC_TIMING(TimingSpan messages_timing(receiver_timing, TD_RX_MESSAGES);)

                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT)
                    {
                        quit = true;
                        break;
                    }

                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
            }
        }
    }

    InterlockedExchange(&stopping, 1);
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    CloseHandle(ready_event);
    write_diagnostics();
    fprintf(log_file,
            "RESULT=%s BLOCKS=%u NONZERO=%u UNDERRUNS=%u ACCEPTED=%u COMPLETED=%u MIDI_BYTES=%u UART_HIGH=%u UART_PENDING=%u HOST_QUEUE_HIGH=%u BACKPRESSURE=%u PENDING=%u\n",
            failed ? "FAIL" : "PASS", blocks, nonzero, underruns, accepted, completed, midi_bytes, uart_high, uart_pending,
            highwater, backpressure, used);
    fprintf(log_file, "DIRECT_WAKES=%u MIDI_FNV1A=%08x\n", direct_wakes, midi_hash);
    fclose(log_file);
    return failed ? 1 : 0;
}
