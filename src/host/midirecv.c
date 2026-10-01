/* Diagnostic receiver: no synthesis. SPDX-License-Identifier: MIT */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "../common/midiproto.h"
typedef struct
{
    unsigned kind, size;
    DWORD sequence;
    BYTE bytes[VSC_MAX_SYSEX];
} EVENT;
static EVENT queue[VSC_QUEUE], staging;
static unsigned head, tail, used, expected, received;
static DWORD session, sequence;
static int active;
static FILE *log_file;
static int log_failed;
static FILE *status_file;
static void status_log(const char *stage, DWORD value)
{
    if (status_file)
    {
        fprintf(status_file, "%lu %s %lu\n", (unsigned long)GetTickCount(), stage, (unsigned long)value);
        fflush(status_file);
    }
}

static LRESULT enqueue_event(unsigned kind, unsigned size)
{
    EVENT *event;

    if (used == VSC_QUEUE)
        return VSC_FULL;

    event = &queue[head];
    event->kind = kind;
    event->size = size;
    event->sequence = ++sequence;

    if (size)
        memcpy(event->bytes, staging.bytes, size);

    head = (head + 1) % VSC_QUEUE;
    ++used;
    return VSC_OK;
}
static void drain(void)
{
    while (used)
    {
        EVENT *event = &queue[tail];
        unsigned i;
        DWORD hash = 2166136261UL;

        for (i = 0; i < event->size; i++)
            hash = (hash ^ event->bytes[i]) * 16777619UL;

        fprintf(log_file, "seq=%lu kind=%u size=%u fnv=%08lx bytes=", (unsigned long)event->sequence, event->kind, event->size,
                (unsigned long)hash);

        for (i = 0; i < event->size; i++)
            fprintf(log_file, "%02x", event->bytes[i]);

        fputc('\n', log_file);
        tail = (tail + 1) % VSC_QUEUE;
        --used;
    }

    if (fflush(log_file) || ferror(log_file))
        log_failed = 1;
}
static LRESULT protocol(UINT message, WPARAM wp, LPARAM lp)
{
    DWORD value = (DWORD)lp;
    unsigned count = (unsigned)wp, i;
    LRESULT r;

    if (message == VSC_PING)
        return VSC_MAGIC;

    if (message == VSC_SESSION)
        return (LRESULT)session;

    if (message == VSC_OPEN)
    {
        if (active)
            return 0;

        r = enqueue_event(VSC_OPEN, 0);

        if (r == VSC_OK)
            active = 1;

        return r;
    }

    if (!active)
        return 0;

    if (message == VSC_CLOSE)
    {
        expected = received = 0;
        active = 0;
        return enqueue_event(VSC_CLOSE, 0);
    }

    if (message == VSC_ABORT)
    {
        expected = received = 0;
        return VSC_OK;
    }

    if (message == VSC_RESET)
    {
        /* Diagnostic log retains earlier events; real synth must apply reset as a barrier. */
        r = enqueue_event(VSC_RESET, 0);

        if (r == VSC_OK)
            expected = received = 0;

        return r;
    }

    if (message == VSC_BEGIN)
    {
        if (expected || !value || value > VSC_MAX_SYSEX)
            return 0;

        if (used == VSC_QUEUE)
            return VSC_FULL;

        expected = value;
        received = 0;
        return VSC_OK;
    }

    if (message == VSC_CHUNK)
    {
        if (!expected || !count || count > 3 || received + count > expected)
            return 0;

        for (i = 0; i < count; i++)
            staging.bytes[received++] = (BYTE)(value >> (8 * i));

        return VSC_OK;
    }

    if (message == VSC_COMMIT)
    {
        if (!expected || received != expected)
            return 0;

        r = enqueue_event(VSC_COMMIT, received);

        if (r == VSC_OK)
            expected = received = 0;

        return r;
    }

    if (message == VSC_SHORT)
    {
        BYTE status = (BYTE)value;
        unsigned size;

        if (expected || status < 0x80 || status == 0xf0 || status == 0xf7)
            return 0;

        if (status < 0xf0)
            size = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 2 : 3;
        else
            size = status == 0xf2 ? 3 : ((status == 0xf1 || status == 0xf3) ? 2 : 1);

        for (i = 0; i < size; i++)
            staging.bytes[i] = (BYTE)(value >> (i * 8));

        return enqueue_event(VSC_SHORT, size);
    }

    return 0;
}
static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= VSC_PING && msg <= VSC_RESET)
        return protocol(msg, wp, lp);

    if (msg == WM_TIMER)
    {
        drain();
        return 0;
    }

    if (msg == WM_CLOSE)
    {
        HWND pump = FindWindowA("VSC55_PUMP", NULL);
        status_log("STOP_REQUEST", sequence);

        if (pump)
            PostMessageA(pump, WM_CLOSE, 0, 0);
        else
        {
            status_log("PUMP_MISSING", 0);
            log_failed = 1;
            DestroyWindow(window);
        }

        return 0;
    }

    if (msg == VSC_STOPPED)
    {
        status_log("PUMP_FORWARDED", (DWORD)lp);
        status_log("PUMP_FAILED", (DWORD)wp);

        if (wp)
            log_failed = 1;

        DestroyWindow(window);
        return 0;
    }

    if (msg == WM_DESTROY)
    {
        drain();
        status_log("RECEIVED_EVENTS", sequence);
        status_log("LOG_FAILED", log_failed);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(window, msg, wp, lp);
}
static int selftest(void)
{
    unsigned i;
    int failures = 0;
#define CHECK(x) do { if(!(x)) { printf("FAIL line %d\n",__LINE__); ++failures; } } while(0)
    CHECK(protocol(VSC_SHORT, 0, 0x643c90) == 0);
    CHECK(protocol(VSC_OPEN, 0, 0) == VSC_OK);
    CHECK(protocol(VSC_OPEN, 0, 0) == 0);
    CHECK(protocol(VSC_BEGIN, 0, 6) == VSC_OK);
    CHECK(protocol(VSC_SHORT, 0, 0x643c90) == 0);
    CHECK(protocol(VSC_CHUNK, 3, 0x7f7ef0) == VSC_OK);
    CHECK(protocol(VSC_COMMIT, 0, 0) == 0);
    CHECK(protocol(VSC_CHUNK, 3, 0xf70109) == VSC_OK);
    CHECK(protocol(VSC_COMMIT, 0, 0) == VSC_OK);
    CHECK(used == 2 && queue[1].size == 6 && queue[1].bytes[5] == 0xf7);
    CHECK(protocol(VSC_RESET, 0, 0) == VSC_OK);
    drain();

    for (i = 0; i < VSC_QUEUE; i++)
        CHECK(protocol(VSC_SHORT, 0, 0x643c90) == VSC_OK);

    CHECK(protocol(VSC_SHORT, 0, 0x643c90) == VSC_FULL);
    CHECK(protocol(VSC_BEGIN, 0, 12) == VSC_FULL);
    drain();
    CHECK(protocol(VSC_BEGIN, 0, VSC_MAX_SYSEX + 1) == 0);
    CHECK(protocol(VSC_BEGIN, 0, 2) == VSC_OK);
    CHECK(protocol(VSC_CHUNK, 3, 0) == 0);
    CHECK(protocol(VSC_ABORT, 0, 0) == VSC_OK);
    CHECK(protocol(VSC_CLOSE, 0, 0) == VSC_OK);
    CHECK(protocol(VSC_SHORT, 0, 0x643c90) == 0);
    drain();
    printf("Receiver selftest: %d failures\n", failures);
    return failures ? 1 : 0;
}
int main(int argc, char **argv)
{
    WNDCLASSA cls;
    HWND window;
    MSG msg;
    int status = 0;
    STARTUPINFOA startup;
    PROCESS_INFORMATION child;
    char pump_path[MAX_PATH], *slash;

    if (argc == 2 && !strcmp(argv[1], "--stop"))
    {
        unsigned wait;
        window = FindWindowA(VSC_CLASS, NULL);

        if (!window || !PostMessageA(window, WM_CLOSE, 0, 0))
            return 1;

        for (wait = 0; wait < 200; wait++)
        {
            if (!IsWindow(window))
                return 0;

            Sleep(50);
        }

        return 1;
    }

    if (FindWindowA(VSC_CLASS, NULL))
    {
        puts("Receiver already running");
        return 1;
    }

    GetModuleFileNameA(NULL, pump_path, sizeof(pump_path));
    slash = strrchr(pump_path, '\\');

    if (!slash)
        return 1;

    *slash = 0;

    if (!SetCurrentDirectoryA(pump_path))
        return 1;

    *slash = '\\';
    status_file = fopen("RECVSTAT.LOG", "a");

    if (!status_file)
        return 1;

    status_log("START_DIAGNOSTIC_2", GetCurrentProcessId());
    fprintf(status_file, "DIRECTORY %.*s\n", (int)(slash - pump_path), pump_path);
    fflush(status_file);
    log_file = fopen("MIDIRECV.LOG", "a");

    if (!log_file)
    {
        status_log("OPEN_LOG_FAILED", 0);
        fclose(status_file);
        return 1;
    }

    fprintf(log_file, "# SESSION_START diagnostic=2 tick=%lu\n", (unsigned long)GetTickCount());
    fflush(log_file);
    session = GetTickCount() ^ GetCurrentProcessId();

    if (!session)
        session = 1;

    status_log("SESSION", session);

    if (argc == 2 && !strcmp(argv[1], "--selftest"))
    {
        status = selftest();
        fclose(log_file);
        return status || log_failed;
    }

    memset(&cls, 0, sizeof(cls));
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = VSC_CLASS;

    if (!RegisterClassA(&cls))
    {
        fclose(log_file);
        return 1;
    }

    window = CreateWindowA(VSC_CLASS, "VSC-55 diagnostic MIDI receiver", 0, 0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL);

    if (!window || !SetTimer(window, 1, 10, NULL))
    {
        if (window)
            DestroyWindow(window);

        fclose(log_file);
        return 1;
    }

    GetModuleFileNameA(NULL, pump_path, sizeof(pump_path));
    slash = strrchr(pump_path, '\\');

    if (!slash)
    {
        DestroyWindow(window);
        fclose(log_file);
        return 1;
    }

    strcpy(slash + 1, "MIDIPUMP.EXE");
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    if (!CreateProcessA(pump_path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
    {
        DestroyWindow(window);
        fclose(log_file);
        return 1;
    }

    status_log("PUMP_PROCESS", child.dwProcessId);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    puts("VSC-55 receiver ready. Logs MIDI only; no sound. Stop with midirecv --stop.");

    while ((status = GetMessageA(&msg, NULL, 0, 0)) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    drain();
    fprintf(log_file, "# SESSION_END events=%lu failed=%d\n", (unsigned long)sequence, log_failed);
    fclose(log_file);
    fclose(status_file);
    return status < 0 || log_failed;
}
