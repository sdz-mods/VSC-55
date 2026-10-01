/* Registration tests use a private directory and fake Win98 identity.
 * No real Windows SYSTEM.INI or startup registry is accessed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static BOOL WINAPI fixture_version(LPOSVERSIONINFOA os)
{
    os->dwPlatformId = VER_PLATFORM_WIN32_WINDOWS;
    os->dwMajorVersion = 4;
    os->dwMinorVersion = 10;
    return TRUE;
}
static UINT fixture_dir(LPSTR out, UINT size, const char *suffix)
{
    const char *base = getenv("VSC_INSTALL_FIXTURE");

    if (!base || strlen(base) + strlen(suffix) + 1 >= size)
        exit(98);

    sprintf(out, "%s%s", base, suffix);
    return (UINT)strlen(out);
}
static UINT WINAPI fixture_windows(LPSTR p, UINT n)
{
    return fixture_dir(p, n, "\\WINDOWS");
}
static UINT WINAPI fixture_system(LPSTR p, UINT n)
{
    return fixture_dir(p, n, "\\WINDOWS\\SYSTEM");
}
static HWND WINAPI fixture_find(LPCSTR a, LPCSTR b)
{
    return NULL;
}
static HANDLE WINAPI fixture_mutex(DWORD a, BOOL b, LPCSTR c)
{
    return NULL;
}
static LONG WINAPI fixture_registry(HKEY a, LPCSTR b, DWORD c, LPSTR d, DWORD e, REGSAM f, LPSECURITY_ATTRIBUTES g,
                                    PHKEY h, LPDWORD i)
{
    exit(99);
    return ERROR_ACCESS_DENIED;
}
#define GetVersionExA fixture_version
#define GetWindowsDirectoryA fixture_windows
#define GetSystemDirectoryA fixture_system
#define FindWindowA fixture_find
#define OpenMutexA fixture_mutex
#define RegCreateKeyExA fixture_registry
#include "../installer/midisetup.c"
