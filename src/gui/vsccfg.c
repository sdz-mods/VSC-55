/* VSC-55 control panel. GPL-2.0-or-later.
 * X/minimize hide; tray Exit stops and exits.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include "../common/control.h"
#include "../common/version.h"
#include "../common/midiproto.h"
#include "frontpanel.h"
#define WM_TRAY (WM_APP+10)
/* Keep Win98 USER callbacks small: O3/LTO otherwise inlines kilobytes of
 * launch/configuration locals into EVERY recursive window-procedure entry. */
#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif
enum { ID_MODEL = 1001, ID_ROM, ID_BROWSE, ID_DEVICE, ID_BUFFER, ID_VOL, ID_START, ID_STOP, ID_SAVE, ID_PANIC, ID_GS, ID_GM, ID_LOG, ID_OPEN, ID_EXIT, ID_SETTINGS, ID_RATE, ID_AUTOSTART };
static const char *models[] = {"mk1-v1.00", "mk1-v1.10", "mk1-v1.20", "mk1-v1.21", "mk1-v2.00", "mk2-v1.01", "mk2-ctf-sc55-drum-sc55-v1.21"};
static const char *romdirs[] = {"MK1\\1.00", "MK1\\1.10", "MK1\\1.20", "MK1\\1.21", "MK1\\2.00", "MK2\\1.01", "MK2\\CTF1.01"};
static const char *old_romdirs[] = {"MK100", "MK110", "MK120", "MK1", "MK200", "MK2", "MK2CTF"};
static const char *labels[] = {"SC-55 - firmware 1.00", "SC-55 - firmware 1.10", "SC-55 - firmware 1.20", "SC-55 - firmware 1.21", "SC-55 - firmware 2.00", "SC-55mkII - firmware 1.01", "SC-55mkII - CTF patched 1.01"};
static const int rates[] = {11025, 16000, 22050, 32000, 44100, 48000};
static const int buffers[] = {10, 20, 40, 80, 20, 10};
static const int buffer_counts[] = {4, 4, 4, 4, 8, 16};
static HWND wnd, model, rom, device, buffer, rate, autostart, volume, voltext, status, stats, message;
static HFONT font;
static HICON icon, tray_icon;
static int tray, hidden, exiting, stop_pending, last_busy = -1, current = 3, test_mode;
static HANDLE host, map;
static const volatile VSC_STATUS *snapshot;
static char directory[MAX_PATH], ini[MAX_PATH], paths[7][MAX_PATH];
static DWORD child_pid, last_bytes;
static volatile LONG trace_stage;
static int owned, was_running;
static int timer_held;
static const char *panel_class = VSC_PANEL_CLASS;
static NOINLINE void set_text(HWND h, const char *text)
{
    char old[512];
    GetWindowTextA(h, old, sizeof(old));

    if (strcmp(old, text))
        SetWindowTextA(h, text);
}
static void tell(const char *text)
{
    set_text(message, text);
}
static void path(char *dst, const char *name)
{
    sprintf(dst, "%s\\%s", directory, name);
}
static NOINLINE void timer_log(const char *action, MMRESULT result)
{
    char name[MAX_PATH];
    FILE *f;
    path(name, "VSCTIMER.LOG");
    f = fopen(name, "a");

    if (f)
    {
        fprintf(f, "TICK=%lu PID=%lu %s PERIOD_MS=%u RESULT=%u\n", GetTickCount(), GetCurrentProcessId(), action,
                VSC_TIMER_PERIOD_MS, result);
        fclose(f);
    }
}
/* Retain across Stop; release on tray Exit. No request at stopped startup. */
static int hold_timer(void)
{
    MMRESULT r;

    if (timer_held)
        return 1;

    r = timeBeginPeriod(VSC_TIMER_PERIOD_MS);
    timer_log("BEGIN", r);
    timer_held = r == TIMERR_NOERROR;
    return timer_held;
}
static void release_timer(void)
{
    if (timer_held)
    {
        timer_log("END", timeEndPeriod(VSC_TIMER_PERIOD_MS));
        timer_held = 0;
    }
}
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, wnd, (HMENU)(INT_PTR)id,
                           GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}
static NOINLINE void tray_notify(DWORD action)
{
    NOTIFYICONDATAA n;
    memset(&n, 0, sizeof(n));
    n.cbSize = NOTIFYICONDATAA_V1_SIZE;
    n.hWnd = wnd;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = tray_icon;

    /* Deletion identifies the entry only; do not pass a potentially retiring icon. */
    if (action == NIM_DELETE)
    {
        n.uFlags = 0;
        n.hIcon = NULL;
    }

    lstrcpynA(n.szTip, VSC_PRODUCT_TITLE " Control Panel", 64);

    if (Shell_NotifyIconA(action, &n))
        tray = action != NIM_DELETE;
}
static void hide_panel(void)
{
    if (!tray)
        tray_notify(NIM_ADD);

    if (tray)
    {
        hidden = 1;
        ShowWindow(wnd, SW_HIDE);
    }
}
static void show_panel(void)
{
    hidden = 0;
    ShowWindow(wnd, SW_RESTORE);
    SetForegroundWindow(wnd);
}
static void close_mapping(void)
{
    if (snapshot)
        UnmapViewOfFile((const void *)snapshot);

    snapshot = NULL;

    if (map)
        CloseHandle(map);

    map = NULL;
}
static void attach(void)
{
    if (!snapshot)
    {
        map = OpenFileMappingA(FILE_MAP_READ, FALSE, VSC_STATUS_NAME);

        if (map)
            snapshot = (const volatile VSC_STATUS *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(VSC_STATUS));

        if (!snapshot && map)
        {
            CloseHandle(map);
            map = NULL;
        }
    }

    if (!host && snapshot && snapshot->magic == VSC_STATUS_MAGIC && snapshot->pid != 0)
    {
        host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)snapshot->pid);

        if (host)
            child_pid = (DWORD)snapshot->pid;
        else
            close_mapping();
    }

    if (!host)
    {
        HWND h = FindWindowA(VSC_CLASS, NULL);

        if (h)
        {
            GetWindowThreadProcessId(h, &child_pid);
            host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, child_pid);
        }
    }
}
static void stop_host(void)
{
    HANDLE e;
    stop_pending = 1;
    e = OpenEventA(EVENT_MODIFY_STATE, FALSE, VSC_STOP_EVENT);

    if (e)
    {
        SetEvent(e);
        CloseHandle(e);
    }
    else
    {
        HWND h = FindWindowA(VSC_CLASS, NULL);

        if (h)
            PostMessageA(h, WM_CLOSE, 0, 0);
    }
}
static void command(int cmd, LPARAM value)
{
    HWND h = FindWindowA(VSC_CLASS, NULL);

    if (!h || !PostMessageA(h, VSC_CONTROL, cmd, value))
        tell("The synthesizer is not ready for this command.");
}
static void devices(void)
{
    char wanted[MAXPNAMELEN + 32];
    UINT i;
    int selected = 0, found = 0, index;
    GetPrivateProfileStringA("Audio", "Device", "Default sound device", wanted, sizeof(wanted), ini);
    SendMessageA(device, CB_ADDSTRING, 0, (LPARAM)"Default sound device");
    SendMessageA(device, CB_SETITEMDATA, 0, (LPARAM) - 1);
    found = !strcmp(wanted, "Default sound device");

    for (i = 0; i < waveOutGetNumDevs(); i++)
    {
        WAVEOUTCAPSA c;

        if (waveOutGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR)
        {
            index = (int)SendMessageA(device, CB_ADDSTRING, 0, (LPARAM)c.szPname);
            SendMessageA(device, CB_SETITEMDATA, index, i);

            if (!found && !strcmp(wanted, c.szPname))
            {
                selected = index;
                found = 1;
            }
        }
    }

    if (!found)
    {
        selected = (int)SendMessageA(device, CB_ADDSTRING, 0, (LPARAM)wanted);
        SendMessageA(device, CB_SETITEMDATA, selected, (LPARAM) - 2);
    }

    SendMessageA(device, CB_SETCURSEL, selected, 0);
}
static NOINLINE int save(void)
{
    char num[32], name[128];
    int i, ok = 1, sel;
    GetWindowTextA(rom, paths[current], MAX_PATH);
    ok &= WritePrivateProfileStringA("Synth", "Model", models[current], ini);
    ok &= WritePrivateProfileStringA("Synth", "AutoStart", SendMessageA(autostart, BM_GETCHECK, 0,
                                     0) == BST_CHECKED ? "1" : "0", ini);
    sel = (int)SendMessageA(rate, CB_GETCURSEL, 0, 0);
    sprintf(num, "%d", rates[sel >= 0 ? sel : 5]);
    ok &= WritePrivateProfileStringA("Audio", "SampleRate", num, ini);

    for (i = 0; i < 7; i++)
        ok &= WritePrivateProfileStringA("ROMs", models[i], paths[i], ini);

    sel = (int)SendMessageA(device, CB_GETCURSEL, 0, 0);
    SendMessageA(device, CB_GETLBTEXT, sel, (LPARAM)name);
    ok &= WritePrivateProfileStringA("Audio", "Device", name, ini);
    sel = (int)SendMessageA(buffer, CB_GETCURSEL, 0, 0);
    sprintf(num, "%d", buffers[sel >= 0 ? sel : 4]);
    ok &= WritePrivateProfileStringA("Audio", "BufferMs", num, ini);
    sprintf(num, "%d", buffer_counts[sel >= 0 ? sel : 4]);
    ok &= WritePrivateProfileStringA("Audio", "BufferCount", num, ini);
    sprintf(num, "%d", (int)SendMessageA(volume, TBM_GETPOS, 0, 0));
    ok &= WritePrivateProfileStringA("Audio", "Volume", num, ini);
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);

    if (!ok)
        tell("Could not save VSC55.INI. Check folder permissions or free space.");

    return ok;
}
static NOINLINE void start_host(void)
{
    char exe[MAX_PATH], cmd[1024], escaped[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int dev, bs, bc, vol, sel;
    DWORD attr;
    attach();

    if (host)
        return;

    GetWindowTextA(rom, paths[current], MAX_PATH);
    attr = GetFileAttributesA(paths[current]);

    if (strchr(paths[current], '"') || attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        tell("Select an existing ROM folder before starting.");
        return;
    }

    sel = (int)SendMessageA(device, CB_GETCURSEL, 0, 0);
    dev = (int)SendMessageA(device, CB_GETITEMDATA, sel, 0);

    if (dev == -2)
    {
        tell("The saved sound device is unavailable. Select another device.");
        return;
    }

    if (!save())
        return;

    if (!hold_timer())
    {
        tell("Could not request the 5 ms timer period. See VSCTIMER.LOG.");
        return;
    }

    sel = (int)SendMessageA(buffer, CB_GETCURSEL, 0, 0);
    bs = buffers[sel >= 0 ? sel : 4];
    bc = buffer_counts[sel >= 0 ? sel : 4];
    vol = (int)SendMessageA(volume, TBM_GETPOS, 0, 0);
    /* Windows CRT quoting: double trailing backslashes before the closing quote. */
    {
        size_t len = strlen(paths[current]), n = len;
        strcpy(escaped, paths[current]);

        while (n && paths[current][n - 1] == '\\')
        {
            escaped[len++] = '\\';
            --n;
        }

        escaped[len] = 0;
    }
    path(exe, "VSC55.EXE");
    sprintf(cmd, "\"%s\" %s \"%s\" --front-panel --device %d --buffer-ms %d --buffer-count %d --volume %d%s", exe,
            models[current], escaped, dev, bs, bc, vol, test_mode ? " --no-midi" : "");
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    /* Win98 ignores CREATE_NO_WINDOW. A detached host has no console to
       trigger the system's console-program shutdown confirmation. */
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, NORMAL_PRIORITY_CLASS | DETACHED_PROCESS, NULL, directory, &si, &pi))
    {
        tell("Could not launch VSC55.EXE. Check the installation files.");
        return;
    }

    host = pi.hProcess;
    child_pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    owned = 1;
    stop_pending = 0;
    last_bytes = 0;
    tell("Starting: validating ROMs and booting firmware...");
}
static void failure_text(void)
{
    char filename[MAX_PATH], line[512], last[512] = "Host stopped with an error. Open VSC55.LOG for details.";
    FILE *f;
    path(filename, "VSC55.LOG");
    f = fopen(filename, "r");

    if (f)
    {
        while (fgets(line, sizeof(line), f))
            if (strstr(line, "ERROR "))
            {
                lstrcpynA(last, strstr(line, "ERROR ") + 6, sizeof(last));
                last[strcspn(last, "\r\n")] = 0;
            }

        fclose(f);
    }

    tell(last);
}
static NOINLINE void refresh(void)
{
    trace_stage = 10;

    if (exiting == 2)
        return;

    int busy, running = 0, i;
    char text[256];
    DWORD code;
    attach();

    if (host && WaitForSingleObject(host, 0) == WAIT_OBJECT_0)
    {
        code = 0;
        GetExitCodeProcess(host, &code);
        CloseHandle(host);
        host = NULL;
        close_mapping();

        if (code && owned && !stop_pending)
            failure_text();
        else if (owned)
            tell("Stopped. The emulation host has exited.");

        owned = 0;
        stop_pending = 0;
        was_running = 0;
        last_bytes = 0;
    }

    busy = host != NULL;

    if (busy && !hold_timer())
        tell("Could not retain the 5 ms timer request. See VSCTIMER.LOG.");

    if (exiting && !busy)
    {
        trace_stage = 11;
        exiting = 2;
        KillTimer(wnd, 1);

        if (tray)
            tray_notify(NIM_DELETE);

        front_destroy();
        DestroyWindow(wnd);
        return;
    }

    if (stop_pending && busy)
        stop_host();

    if (snapshot && snapshot->magic == VSC_STATUS_MAGIC && (DWORD)snapshot->pid == child_pid)
        running = busy && snapshot->state == VSC_STATE_RUNNING && !stop_pending;

    if (running && !was_running)
        tell(test_mode ? "TEST MODE: MIDI bridge disabled." :
             "Ready. Select VSC-55 as the MIDI output in VOPL3 or your player.");

    was_running = running;
    GetWindowTextA(message, text, sizeof(text));
    front_update(busy, running, (int)SendMessageA(volume, TBM_GETPOS, 0, 0), models[current], text);

    if (!IsWindowVisible(wnd))
    {
        last_busy = -1;
        return;
    }

    if (last_busy != busy)
    {
        int ids[] = {ID_MODEL, ID_ROM, ID_BROWSE, ID_DEVICE, ID_BUFFER, ID_RATE};

        for (i = 0; i < 6; i++)
            EnableWindow(GetDlgItem(wnd, ids[i]), !busy);

        EnableWindow(GetDlgItem(wnd, ID_START), !busy);
        EnableWindow(GetDlgItem(wnd, ID_STOP), busy);
        last_busy = busy;
    }

    for (i = ID_PANIC; i <= ID_GM; i++)
    {
        HWND h = GetDlgItem(wnd, i);

        if (IsWindowEnabled(h) != running)
            EnableWindow(h, running);
    }

    set_text(status, !busy ? "STOPPED - emulation is not running" : stop_pending ? "STOPPING - waiting for the audio host" :
             running ? "RUNNING - MIDI receiver ready" : "STARTING - validating ROMs / booting firmware");

    if (running && snapshot)
    {
        LONG bytes = snapshot->midi_bytes;
        sprintf(text, "MIDI: %s   Bytes: %lu   Underruns: %lu", bytes != (LONG)last_bytes ? "active" : "quiet",
                (unsigned long)bytes, (unsigned long)snapshot->underruns);
        last_bytes = bytes;
        set_text(stats, text);

        if (snapshot->command_error)
            tell("Command queue was full. Retry the last panic/reset command.");
    }
    else
        set_text(stats, "MIDI and sound output are inactive until the host is ready.");
}
static NOINLINE void browse(void)
{
    BROWSEINFOA b;
    LPITEMIDLIST id;
    char selected[MAX_PATH];
    memset(&b, 0, sizeof(b));
    b.hwndOwner = wnd;
    b.lpszTitle = "Select the folder containing this firmware's ROM files";
    b.ulFlags = BIF_RETURNONLYFSDIRS;
    id = SHBrowseForFolderA(&b);

    if (id)
    {
        if (SHGetPathFromIDListA(id, selected))
            SetWindowTextA(rom, selected);

        {
            LPMALLOC allocator;

            if (SHGetMalloc(&allocator) == NOERROR)
            {
                allocator->lpVtbl->Free(allocator, id);
                allocator->lpVtbl->Release(allocator);
            }
        }
    }
}
static NOINLINE void ui(void)
{
    char value[128], default_path[MAX_PATH];
    int i, n, v, count;
    mk("BUTTON", "SC-55 synthesizer", BS_GROUPBOX, 8, 6, 444, 126, 0);
    mk("STATIC", "Model / firmware:", 0, 20, 28, 112, 18, 0);
    model = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 134, 24, 304, 180, ID_MODEL);
    GetPrivateProfileStringA("Synth", "Model", "mk1-v1.21", value, sizeof(value), ini);

    for (i = 0; i < 7; i++)
    {
        SendMessageA(model, CB_ADDSTRING, 0, (LPARAM)labels[i]);

        if (!strcmp(value, models[i]))
            current = i;

        char legacy[MAX_PATH];
        sprintf(default_path, "%s\\ROMS\\%s", directory, romdirs[i]);
        GetPrivateProfileStringA("ROMs", models[i], default_path, paths[i], MAX_PATH, ini);
        /* Older panels saved every revision against the two baseline sets.
         * Migrate only that exact default; preserve user-selected folders. */
        sprintf(legacy, "%s\\ROMS\\%s", directory, i < 5 ? "MK1" : "MK2");

        if (!lstrcmpiA(paths[i], legacy))
            lstrcpynA(paths[i], default_path, MAX_PATH);

        sprintf(legacy, "%s\\ROMS\\%s", directory, old_romdirs[i]);

        if (!lstrcmpiA(paths[i], legacy))
            lstrcpynA(paths[i], default_path, MAX_PATH);
    }

    SendMessageA(model, CB_SETCURSEL, current, 0);
    mk("STATIC", "ROM folder:", 0, 20, 50, 390, 18, 0);
    rom = mk("EDIT", paths[current], WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 20, 70, 330, 23, ID_ROM);
    SendMessageA(rom, EM_LIMITTEXT, MAX_PATH - 1, 0);
    mk("BUTTON", "Browse...", WS_TABSTOP, 358, 69, 80, 25, ID_BROWSE);
    mk("BUTTON", "Start", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 100, 92, 26, ID_START);
    mk("BUTTON", "Stop", WS_TABSTOP, 120, 100, 92, 26, ID_STOP);
    autostart = mk("BUTTON", "Start emulation when VSC-55 opens", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 232, 96, 206,
                   32, ID_AUTOSTART);
    SendMessageA(autostart, BM_SETCHECK, GetPrivateProfileIntA("Synth", "AutoStart", 0,
                 ini) == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    mk("BUTTON", "Audio output", BS_GROUPBOX, 8, 140, 444, 138, 0);
    mk("STATIC", "Sound device:", 0, 20, 160, 100, 18, 0);
    device = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 124, 156, 314, 180, ID_DEVICE);
    devices();
    mk("STATIC", "Sample rate:", 0, 20, 188, 100, 18, 0);
    rate = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 124, 184, 314, 160, ID_RATE);
    n = GetPrivateProfileIntA("Audio", "SampleRate", 48000, ini);
    v = 5;

    for (i = 0; i < 6; i++)
    {
        sprintf(value, "%d Hz%s", rates[i], rates[i] == 48000 ? " (default)" : "");
        SendMessageA(rate, CB_ADDSTRING, 0, (LPARAM)value);

        if (rates[i] == n)
            v = i;
    }

    SendMessageA(rate, CB_SETCURSEL, v, 0);
    mk("STATIC", "Buffer size:", 0, 20, 216, 100, 18, 0);
    buffer = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 124, 212, 314, 140, ID_BUFFER);
    n = GetPrivateProfileIntA("Audio", "BufferMs", 20, ini);
    count = GetPrivateProfileIntA("Audio", "BufferCount", n == 20 ? 8 : 4, ini);
    v = 4;

    for (i = 0; i < 6; i++)
    {
        sprintf(value, "%d ms x %d buffers (%d ms queued)", buffers[i], buffer_counts[i], buffers[i]*buffer_counts[i]);
        SendMessageA(buffer, CB_ADDSTRING, 0, (LPARAM)value);

        if (buffers[i] == n && buffer_counts[i] == count)
            v = i;
    }

    SendMessageA(buffer, CB_SETCURSEL, v, 0);
    mk("STATIC", "Volume:", 0, 20, 252, 90, 18, 0);
    volume = mk(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 98, 244, 280, 28, ID_VOL);
    SendMessageA(volume, TBM_SETRANGE, TRUE, MAKELONG(0, VSC_MAX_VOLUME_PERCENT));
    SendMessageA(volume, TBM_SETTICFREQ, VSC_MAX_VOLUME_PERCENT / 4, 0);
    n = GetPrivateProfileIntA("Audio", "Volume", 100, ini);

    if (n < 0 || n > VSC_MAX_VOLUME_PERCENT)
        n = 100;

    SendMessageA(volume, TBM_SETPOS, TRUE, n);
    sprintf(value, "%d%%", n);
    voltext = mk("STATIC", value, 0, 386, 252, 52, 18, 0);
    mk("BUTTON", "Status and MIDI controls", BS_GROUPBOX, 8, 286, 444, 88, 0);
    status = mk("STATIC", "", 0, 20, 306, 418, 18, 0);
    stats = mk("STATIC", "", 0, 20, 324, 418, 18, 0);
    mk("BUTTON", "Panic", WS_TABSTOP, 20, 346, 108, 24, ID_PANIC);
    mk("BUTTON", "GS reset", WS_TABSTOP, 138, 346, 108, 24, ID_GS);
    mk("BUTTON", "GM reset", WS_TABSTOP, 256, 346, 108, 24, ID_GM);
    mk("BUTTON", "Save settings", WS_TABSTOP, 116, 382, 112, 26, ID_SAVE);
    mk("BUTTON", "Open log", WS_TABSTOP, 236, 382, 108, 26, ID_LOG);
    message = mk("STATIC", "Settings can be closed during playback. Tray Exit stops VSC-55.", 0, 20, 416, 418, 28, 0);
}
static NOINLINE void open_log(void)
{
    char log[MAX_PATH];
    path(log, "VSC55.LOG");
    ShellExecuteA(wnd, "open", "notepad.exe", log, directory, SW_SHOWNORMAL);
}
static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    static UINT taskbar;

    if (taskbar && msg == taskbar)
    {
        tray = 0;
        tray_notify(NIM_ADD);
        return 0;
    }

    switch (msg)
    {
        case WM_APP+57:
        {
            char t[16];
            int v = (int)wp;

            if (v < 0)
                v = 0;

            if (v > VSC_MAX_VOLUME_PERCENT)
                v = VSC_MAX_VOLUME_PERCENT;

            SendMessageA(volume, TBM_SETPOS, TRUE, v);
            sprintf(t, "%d%%", v);
            set_text(voltext, t);

            if (host)
                command(VSC_CTL_VOLUME, v);

            refresh();
            return 0;
        }

        case WM_CREATE:
            wnd = w;
            ui();
            taskbar = RegisterWindowMessageA("TaskbarCreated");
            tray_notify(NIM_ADD);
            SetTimer(w, 1, 1000, NULL);
            refresh();
            return 0;

        case WM_TIMER:
            refresh();
            return 0;

        case WM_HSCROLL:
            if ((HWND)lp == volume)
            {
                char t[16];
                int v = (int)SendMessageA(volume, TBM_GETPOS, 0, 0);
                sprintf(t, "%d%%", v);
                set_text(voltext, t);

                if (host)
                    command(VSC_CTL_VOLUME, v);

                refresh();
            }

            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
                case ID_MODEL:
                    if (HIWORD(wp) == CBN_SELCHANGE)
                    {
                        GetWindowTextA(rom, paths[current], MAX_PATH);
                        current = (int)SendMessageA(model, CB_GETCURSEL, 0, 0);
                        SetWindowTextA(rom, paths[current]);
                    }

                    break;

                case ID_BROWSE:
                    browse();
                    break;

                case ID_START:
                    start_host();
                    refresh();
                    break;

                case ID_STOP:
                    stop_host();
                    refresh();
                    break;

                case ID_SAVE:
                    if (save())
                        tell("Settings saved. Autostart applies on the next VSC-55 launch.");

                    break;

                case ID_PANIC:
                    command(VSC_CTL_PANIC, 0);
                    break;

                case ID_GS:
                case ID_GM:
                    if (MessageBoxA(w, "Reset the emulated module? This changes its MIDI settings and can interrupt playback.",
                                    "VSC-55 reset", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        command(LOWORD(wp) == ID_GS ? VSC_CTL_GS : VSC_CTL_GM, 0);

                    break;

                case ID_LOG:
                    open_log();
                    break;

                case ID_OPEN:
                    front_show();
                    refresh();
                    break;

                case ID_SETTINGS:
                    show_panel();
                    last_busy = -1;
                    refresh();
                    break;

                case ID_EXIT:
                    exiting = 1;
                    attach();

                    if (host)
                        stop_host();

                    refresh();
                    break;
            }

            return 0;

        case WM_TRAY:
            if (lp == WM_LBUTTONDBLCLK)
            {
                front_show();
                refresh();
            }

            if (lp == WM_RBUTTONUP)
            {
                HMENU m = CreatePopupMenu();
                POINT p;
                AppendMenuA(m, MF_STRING, ID_OPEN, "Open front panel");
                AppendMenuA(m, MF_STRING, ID_SETTINGS, "Settings...");
                AppendMenuA(m, MF_STRING | (host ? MF_GRAYED : 0), ID_START, "Start");
                AppendMenuA(m, MF_STRING | (!host ? MF_GRAYED : 0), ID_STOP, "Stop");
                AppendMenuA(m, MF_SEPARATOR, 0, NULL);
                AppendMenuA(m, MF_STRING, ID_EXIT, "Exit (stop synthesizer)");
                GetCursorPos(&p);
                SetForegroundWindow(w);
                TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, w, NULL);
                DestroyMenu(m);
                PostMessageA(w, WM_NULL, 0, 0);
            }

            return 0;

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED && exiting != 2)
                hide_panel();

            return 0;

        case WM_CLOSE:
            if (exiting != 2)
                hide_panel();

            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wp && exiting != 2)
            {
                stop_host();
                exiting = 2;

                if (tray)
                    tray_notify(NIM_DELETE);

                front_destroy();
                DestroyWindow(w);
            }

            return 0;

        /* Win98 shell deletion must precede USER's window/icon destruction. */
        case WM_DESTROY:
            trace_stage = 12;
            exiting = 2;
            KillTimer(w, 1);
            release_timer();
            tray = 0;
            trace_stage = 13;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(w, msg, wp, lp);
}
static LONG WINAPI crash_report(EXCEPTION_POINTERS *fault)
{
    char name[MAX_PATH];
    HANDLE f;
    DWORD done;
    DWORD data[8];
    data[0] = fault->ExceptionRecord->ExceptionCode;
    data[1] = fault->ContextRecord->Eip;
    data[2] = fault->ContextRecord->Esp;
    data[3] = fault->ContextRecord->Ebp;
    data[4] = trace_stage;
    data[5] = fault->ContextRecord->SegSs;
    data[6] = fault->ExceptionRecord->ExceptionInformation[0];
    data[7] = fault->ExceptionRecord->ExceptionInformation[1];
    path(name, "VSCGUI.ERR");
    f = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f != INVALID_HANDLE_VALUE)
    {
        WriteFile(f, data, sizeof(data), &done, NULL);

        if (!IsBadReadPtr((void *)fault->ContextRecord->Esp, 1024))
            WriteFile(f, (void *)fault->ContextRecord->Esp, 1024, &done, NULL);

        CloseHandle(f);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA c;
    RECT r = {0, 0, 460, 450}, area;
    int x, y;
    MSG msg;
    INITCOMMONCONTROLSEX common;
    HANDLE instance;
    char *slash;
    (void)prev;
    (void)show;
    instance = CreateMutexA(NULL, FALSE, "VSC55_PANEL_V1");

    if (!instance)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND old = FindWindowA(panel_class, NULL);

        if (old)
            PostMessageA(old, WM_COMMAND, ID_OPEN, 0);

        CloseHandle(instance);
        return 0;
    }

    if (!GetModuleFileNameA(NULL, directory, MAX_PATH) || strlen(directory) > MAX_PATH - 32)
        return 1;

    slash = strrchr(directory, '\\');

    if (!slash)
        return 1;

    *slash = 0;
    SetCurrentDirectoryA(directory);
    path(ini, "VSC55.INI");
    SetUnhandledExceptionFilter(crash_report);
    test_mode = cmd && strstr(cmd, "/test-no-midi") != NULL;
    /* The priority ceiling prevents a DOS VM starving this GUI while it owns Win16Mutex. */
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    common.dwSize = sizeof(common);
    common.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&common);
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    icon = LoadIconA(inst, MAKEINTRESOURCEA(1));

    if (!icon)
        icon = LoadIconA(NULL, IDI_APPLICATION);

    /* Bypass the shared cache: Win98 can reuse the large icon by resource ID,
       ignoring a later request for 16px. This handle is owned by the panel. */
    tray_icon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, 0);

    if (!tray_icon)
        tray_icon = icon;

    memset(&c, 0, sizeof(c));
    c.hInstance = inst;
    c.lpfnWndProc = proc;
    c.lpszClassName = panel_class;
    c.hIcon = icon;
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    if (!RegisterClassA(&c))
        return 1;

    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    area.left = area.top = 0;
    area.right = GetSystemMetrics(SM_CXSCREEN);
    area.bottom = GetSystemMetrics(SM_CYSCREEN);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &area, 0);
    x = area.left + (area.right - area.left - (r.right - r.left)) / 2;
    y = area.top + (area.bottom - area.top - (r.bottom - r.top)) / 2;

    if (x < area.left)
        x = area.left;

    if (y < area.top)
        y = area.top;

    wnd = CreateWindowA(panel_class, VSC_PRODUCT_TITLE " Settings",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, inst,
                        NULL);

    if (!wnd)
        return 1;

    SendMessageA(wnd, WM_SETICON, ICON_SMALL, (LPARAM)tray_icon);

    if (!front_create(inst, wnd, icon, tray_icon, directory, ini))
    {
        if (tray)
            tray_notify(NIM_DELETE);

        DestroyWindow(wnd);
        return 1;
    }

    hide_panel();
    refresh();

    if (!(cmd && strstr(cmd, "/tray")))
        front_show();

    /* One startup request, after both windows/settings are initialized.
     * Reopening a window or pressing Stop must not trigger another start. */
    if (GetPrivateProfileIntA("Synth", "AutoStart", 0, ini) == 1)
        PostMessageA(wnd, WM_COMMAND, ID_START, 0);

    trace_stage = 20;

    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        trace_stage = 21;

        if (!((msg.hwnd == wnd || IsChild(wnd, msg.hwnd)) && IsDialogMessageA(wnd, &msg)))
        {
            trace_stage = 22;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        trace_stage = 20;
    }

    trace_stage = 30;
    close_mapping();

    if (tray_icon && tray_icon != icon)
        DestroyIcon(tray_icon);

    if (host)
        CloseHandle(host);

    CloseHandle(instance);
    return 0;
}
