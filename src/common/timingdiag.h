// Per-thread, bounded diagnostic storage; dump only after both threads stop.
#pragma once
#include <windows.h>
#include <cstdio>
struct TimingDiag
{
    static constexpr unsigned capacity = 4096, stages = 16;
    struct Row
    {
        DWORD start, ms;
        unsigned stage, q0, q1;
        int rt;
        DWORD detail;
    };
    struct Stat
    {
        unsigned calls = 0, slow = 0;
        DWORD maximum = 0;
        unsigned long long total = 0;
    };
    Row rows[capacity] {};
    Stat stats[stages] {};
    unsigned count = 0;
    int realtime = -1;
    volatile LONG *queue;
    explicit TimingDiag(volatile LONG *q): queue(q) {}
    unsigned queued()
    {
        return (unsigned)InterlockedCompareExchange(queue, 0, 0);
    }
    void add(unsigned stage, DWORD start, DWORD ms, unsigned q0, DWORD detail = 0, bool force = false,
             DWORD threshold = 20)
    {
        auto &s = stats[stage];
        ++s.calls;
        s.total += ms;

        if (ms > s.maximum)
            s.maximum = ms;

        if (ms >= threshold || force)
        {
            ++s.slow;
            rows[count++ % capacity] = {start, ms, stage, q0, queued(), realtime, detail};
        }
    }
    void dump(FILE *f, const char *thread, const char *const *names)
    {
        fprintf(f, "TIMING_RING THREAD=%s TOTAL=%u RETAINED=%u OVERWRITTEN=%u THRESHOLD_MS=20\n", thread, count,
                count < capacity ? count : capacity, count > capacity ? count - capacity : 0);

        for (unsigned i = 0; i < stages; i++)
            if (stats[i].calls)
            {
                const auto &s = stats[i];
                fprintf(f, "TIMING_STAT THREAD=%s STAGE=%s CALLS=%u RECORDED=%u MAX_MS=%lu TOTAL_MS=%llu\n", thread, names[i], s.calls,
                        s.slow, (unsigned long)s.maximum, s.total);
            }

        for (unsigned i = count > capacity ? count - capacity : 0; i < count; i++)
        {
            const auto &r = rows[i % capacity];
            fprintf(f, "TIMING THREAD=%s STAGE=%s START=%lu MS=%lu Q0=%u Q1=%u RT=%d DETAIL=%lu\n", thread, names[r.stage],
                    (unsigned long)r.start, (unsigned long)r.ms, r.q0, r.q1, r.rt, (unsigned long)r.detail);
        }
    }
};
struct TimingSpan
{
    TimingDiag &diag;
    unsigned stage, q0;
    DWORD start, detail = 0, threshold;
    bool force = false;
    TimingSpan(TimingDiag &d, unsigned s, DWORD t = 20): diag(d), stage(s), q0(d.queued()), start(GetTickCount()),
        threshold(t) {}
    ~TimingSpan()
    {
        diag.add(stage, start, GetTickCount() - start, q0, detail, force, threshold);
    }
};
