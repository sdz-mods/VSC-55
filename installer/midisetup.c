/* Win98 MIDI registration and guarded batch-install support. SPDX-License-Identifier: MIT */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/common/control.h"
#include "../src/common/midiproto.h"
#define RUNKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
static int stop_all(void)
{
    DWORD begin = GetTickCount();
    HANDLE processes[2] = {NULL, NULL};
    HWND sent[2] = {NULL, NULL};
    int i, result = 1;
    puts("Stopping the VSC-55 panel and audio host...");

    do
    {
        HWND windows[2];
        HANDLE mutex, event;
        int busy = 0;
        windows[0] = FindWindowA(VSC_PANEL_CLASS, NULL);
        windows[1] = FindWindowA(VSC_CLASS, NULL);

        for (i = 0; i < 2; i++)
        {
            if (windows[i])
            {
                DWORD pid = 0;
                busy = 1;

                if (!processes[i])
                {
                    GetWindowThreadProcessId(windows[i], &pid);
                    processes[i] = OpenProcess(SYNCHRONIZE, FALSE, pid);
                }

                if (sent[i] != windows[i])
                {
                    /* Panel WM_CLOSE only hides it; 1015 is its tray Exit command. */
                    PostMessageA(windows[i], i ? WM_CLOSE : WM_COMMAND, i ? 0 : 1015, 0);
                    sent[i] = windows[i];
                }
            }

            if (processes[i] && WaitForSingleObject(processes[i], 0) != WAIT_OBJECT_0)
                busy = 1;
        }

        /* Also stop an audio host still booting before its window exists. */
        event = OpenEventA(EVENT_MODIFY_STATE, FALSE, VSC_STOP_EVENT);

        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
        }

        mutex = OpenMutexA(SYNCHRONIZE, FALSE, VSC_HOST_MUTEX);

        if (mutex)
        {
            busy = 1;
            CloseHandle(mutex);
        }

        mutex = OpenMutexA(SYNCHRONIZE, FALSE, "VSC55_PANEL_V1");

        if (mutex)
        {
            busy = 1;
            CloseHandle(mutex);
        }

        if (!busy)
        {
            result = 0;
            break;
        }

        Sleep(50);
    }
    while ((DWORD)(GetTickCount() - begin) < 30000);

    for (i = 0; i < 2; i++)
        if (processes[i])
            CloseHandle(processes[i]);

    puts(result ? "Stop timed out. No files were replaced. Close games and VSC-55, then retry." : "VSC-55 has stopped.");
    return result;
}
static int same_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    unsigned char bx[4096], by[4096];
    size_t nx, ny;
    int same = 0;

    if (x && y)
    {
        do
        {
            nx = fread(bx, 1, sizeof(bx), x);
            ny = fread(by, 1, sizeof(by), y);

            if (nx != ny || memcmp(bx, by, nx))
                break;

            if (!nx)
            {
                same = !ferror(x) && !ferror(y);
                break;
            }
        }
        while (1);
    }

    if (x)
        fclose(x);

    if (y)
        fclose(y);

    return same;
}
static int stopped(void)
{
    HANDLE host = OpenMutexA(SYNCHRONIZE, FALSE, VSC_HOST_MUTEX);
    HANDLE panel = OpenMutexA(SYNCHRONIZE, FALSE, "VSC55_PANEL_V1");
    int busy = host || panel || FindWindowA(VSC_CLASS, NULL);

    if (host)
        CloseHandle(host);

    if (panel)
        CloseHandle(panel);

    if (busy)
        puts("Exit the VSC-55 tray panel and stop any old host/receiver, then retry.");

    return !busy;
}
static int startup(int enable)
{
    HKEY key;
    LONG r;
    const char *value = "\"C:\\VSC-55\\VSCCFG.EXE\" /tray";
    r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, RUNKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);

    if (r != ERROR_SUCCESS)
        return 1;

    r = enable ? RegSetValueExA(key, "VSC55Control", 0, REG_SZ, (const BYTE *)value,
                                (DWORD)strlen(value) + 1) : RegDeleteValueA(key, "VSC55Control");
    RegCloseKey(key);

    if (r != ERROR_SUCCESS && !(r == ERROR_FILE_NOT_FOUND && !enable))
        return 1;

    puts(enable ? "Stopped tray startup enabled." : "VSC-55 tray startup removed.");
    return 0;
}
int main(int argc, char **argv)
{
    OSVERSIONINFOA os;
    char windows[MAX_PATH], system[MAX_PATH], ini[MAX_PATH], driver[MAX_PATH], source[MAX_PATH], backup[MAX_PATH];
    char key[16], value[256], chosen[16] = "";
    unsigned i;
    int remove, found = 0;

    if (argc != 2 || (strcmp(argv[1], "install") && strcmp(argv[1], "remove") && strcmp(argv[1], "check") &&
                      strcmp(argv[1], "stop") && strcmp(argv[1], "startup-on") && strcmp(argv[1], "startup-off")))
    {
        puts("midireg install | remove | check | stop | startup-on | startup-off (Windows 98 only)");
        return 2;
    }

    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);

    if (!GetVersionExA(&os) || os.dwPlatformId != VER_PLATFORM_WIN32_WINDOWS || os.dwMajorVersion != 4 ||
            os.dwMinorVersion != 10)
    {
        puts("Refusing registration: this is not Windows 98.");
        return 1;
    }

    if (!strcmp(argv[1], "stop"))
        return stop_all();

    if (!stopped())
        return 1;

    if (!strcmp(argv[1], "startup-on"))
        return startup(1);

    if (!strcmp(argv[1], "startup-off"))
        return startup(0);

    remove = !strcmp(argv[1], "remove");

    if (!GetWindowsDirectoryA(windows, sizeof(windows)) || !GetSystemDirectoryA(system, sizeof(system)))
        return 1;

    if (strlen(windows) > MAX_PATH - 20 || strlen(system) > MAX_PATH - 20)
        return 1;

    sprintf(ini, "%s\\SYSTEM.INI", windows);
    sprintf(driver, "%s\\VSC55.DRV", system);

    if (!GetModuleFileNameA(NULL, source, sizeof(source)) || strlen(source) >= sizeof(source) - 1)
        return 1;

    {
        char *slash = strrchr(source, '\\');

        if (!slash)
            return 1;

        if ((unsigned)(slash - source) > MAX_PATH - 12)
            return 1;

        strcpy(slash + 1, "VSC55.DRV");
    }

    if (!remove && GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES)
    {
        puts("Missing VSC55.DRV beside MIDIREG.EXE.");
        return 1;
    }

    sprintf(backup, "%s\\VSC55.BAK", windows);

    for (i = 0; i < 10; i++)
    {
        if (i)
            sprintf(key, "midi%u", i);
        else
            strcpy(key, "midi");

        GetPrivateProfileStringA("drivers", key, "", value, sizeof(value), ini);

        if (!lstrcmpiA(value, "VSC55.DRV"))
        {
            found = 1;

            if (remove && !WritePrivateProfileStringA("drivers", key, NULL, ini))
                return 1;
        }

        if (!value[0] && !chosen[0])
            strcpy(chosen, key);
    }

    /* Uninstall leaves the system file for rollback. A differing registered
       driver still requires uninstall + reboot; never replace it in place. */
    if (!remove && found && !same_file(source, driver))
    {
        puts("Different VSC55 driver is still registered. Uninstall the old version and REBOOT before installing.");
        return 1;
    }

    if (!strcmp(argv[1], "check"))
    {
        puts("Windows 98, stopped host/panel and driver preflight passed.");
        return 0;
    }

    if (remove)
    {
        WritePrivateProfileStringA(NULL, NULL, NULL, ini);
        puts(found ? "VSC55 MIDI registration removed. Reboot before deleting/replacing the loaded driver." :
             "No VSC55 registration found.");
        puts("Driver file retained for safe rollback. ROMs and all other MIDI entries untouched.");
        return 0;
    }

    if (found)
    {
        puts("Already registered. For updates: remove, reboot, then replace driver and install.");
        return 0;
    }

    if (!chosen[0])
    {
        puts("No free MIDI slots; nothing changed.");
        return 1;
    }

    if (GetFileAttributesA(backup) == INVALID_FILE_ATTRIBUTES && !CopyFileA(ini, backup, TRUE))
    {
        puts("Backup failed; nothing changed.");
        return 1;
    }

    if (!same_file(source, driver) && !CopyFileA(source, driver, FALSE))
    {
        printf("Copy failed (%lu). Driver may still be loaded; reboot after removal.\n", GetLastError());
        return 1;
    }

    if (!WritePrivateProfileStringA("drivers", chosen, "VSC55.DRV", ini))
    {
        puts("Registration failed; driver copied but not registered.");
        return 1;
    }

    WritePrivateProfileStringA(NULL, NULL, NULL, ini);
    printf("Registered [drivers] %s=VSC55.DRV. Backup: %s. Reboot to load the MIDI device.\n", chosen, backup);
    return 0;
}
