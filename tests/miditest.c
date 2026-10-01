/* Win16 and Win32 WinMM probe. SPDX-License-Identifier: MIT */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#define CALLBACK_PTR DWORD
#define INSTANCE_PTR DWORD
#define CALLBACK_ENTRY FAR PASCAL _loadds
#else
#define CALLBACK_PTR DWORD_PTR
#define INSTANCE_PTR DWORD_PTR
#define CALLBACK_ENTRY CALLBACK
#endif
static unsigned opens, closes, dones;
static BYTE large_sysex[9000];
static FILE *report;
static unsigned failures;
static void CALLBACK_ENTRY midi_callback(HMIDIOUT out, UINT msg, INSTANCE_PTR instance, INSTANCE_PTR p1,
        INSTANCE_PTR p2)
{
    (void)out;
    (void)instance;
    (void)p1;
    (void)p2;

    if (msg == MOM_OPEN)
        ++opens;

    if (msg == MOM_CLOSE)
        ++closes;

    if (msg == MOM_DONE)
        ++dones;
}
static void check(const char *label, int passed, unsigned result)
{
    fprintf(report, "%s %s result=%u\n", passed ? "PASS" : "FAIL", label, result);
    fflush(report);

    if (!passed)
        ++failures;
}
static int run_test(int expect_absent)
{
    UINT id, count = midiOutGetNumDevs(), selected = (UINT) - 1, r;
    MIDIOUTCAPS caps;
    HMIDIOUT out = 0, duplicate = 0;
    MIDIHDR hdr;
    unsigned i;
    BYTE gs[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7};
    BYTE gm[] = {0xf0, 0x7e, 0x7f, 0x09, 1, 0xf7};
#ifdef _WIN32
    report = fopen("MIDI32.LOG", "w");
#else
    report = fopen("MIDI16.LOG", "w");
#endif

    if (!report)
        return 1;

    fprintf(report, "Devices=%u\n", count);

    for (id = 0; id < count; id++)
    {
        memset(&caps, 0, sizeof(caps));
        r = midiOutGetDevCaps(id, &caps, sizeof(caps));

        if (!r)
        {
            fprintf(report, "%u %s\n", id, caps.szPname);

            if (!strcmp(caps.szPname, "VSC-55"))
                selected = id;
        }
    }

    check("enumeration", selected != (UINT) - 1, selected);

    if (selected == (UINT) - 1)
    {
        fclose(report);
        return 1;
    }

    r = midiOutOpen(&out, selected, (CALLBACK_PTR)midi_callback, 0, CALLBACK_FUNCTION);

    if (expect_absent)
    {
        check("receiver absent gives open failure", r != 0, r);

        if (!r)
            midiOutClose(out);

        fclose(report);
        return failures ? 1 : 0;
    }

    check("open", r == 0, r);

    if (r)
    {
        fclose(report);
        return 1;
    }

    check("MOM_OPEN once", opens == 1, opens);
    r = midiOutOpen(&duplicate, selected, 0, 0, CALLBACK_NULL);
    check("duplicate open rejected", r == MMSYSERR_ALLOCATED, r);

    if (!r)
        midiOutClose(duplicate);

    r = midiOutShortMsg(out, 0x000000c0UL);
    check("program", r == 0, r);
    r = midiOutShortMsg(out, 0x00643c90UL);
    check("note on", r == 0, r);
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData = (LPSTR)gs;
    hdr.dwBufferLength = sizeof(gs);
    r = midiOutLongMsg(out, &hdr, sizeof(hdr));
    check("unprepared rejected", r == MIDIERR_UNPREPARED, r);
    r = midiOutPrepareHeader(out, &hdr, sizeof(hdr));
    check("prepare GS", r == 0, r);
    r = midiOutLongMsg(out, &hdr, sizeof(hdr));
    check("GS sysex", r == 0, r);
    check("GS synchronous done flag", (hdr.dwFlags & MHDR_DONE) != 0, hdr.dwFlags);
    r = midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
    check("immediate GS unprepare", r == 0, r);
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData = (LPSTR)gm;
    hdr.dwBufferLength = sizeof(gm);
    r = midiOutPrepareHeader(out, &hdr, sizeof(hdr));
    check("prepare GM", r == 0, r);
    r = midiOutLongMsg(out, &hdr, sizeof(hdr));
    check("GM sysex", r == 0, r);
    r = midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
    check("unprepare GM", r == 0, r);
    large_sysex[0] = 0xf0;

    for (i = 1; i < sizeof(large_sysex) - 1; i++)
        large_sysex[i] = (BYTE)(i & 0x7f);

    large_sysex[sizeof(large_sysex) - 1] = 0xf7;
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData = (LPSTR)large_sysex;
    hdr.dwBufferLength = sizeof(large_sysex);
    r = midiOutPrepareHeader(out, &hdr, sizeof(hdr));
    check("prepare 9000 bytes", r == 0, r);
    r = midiOutLongMsg(out, &hdr, sizeof(hdr));
    check("9000-byte sysex", r == 0, r);
    r = midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
    check("unprepare 9000 bytes", r == 0, r);
    check("MOM_DONE exactly three", dones == 3, dones);
    r = midiOutShortMsg(out, 0x00003c80UL);
    check("note off", r == 0, r);
    r = midiOutReset(out);
    check("reset", r == 0, r);
    r = midiOutClose(out);
    check("close", r == 0, r);
    check("MOM_CLOSE once", closes == 1, closes);
    r = midiOutOpen(&out, selected, 0, 0, CALLBACK_NULL);
    check("reopen", r == 0, r);

    if (!r)
        midiOutClose(out);

    fprintf(report, "TOTAL_FAILURES=%u\n", failures);
    fclose(report);
    return failures ? 1 : 0;
}
#ifdef _WIN32
int main(int argc, char **argv)
{
    return run_test(argc == 2 && !strcmp(argv[1], "--absent"));
}
#else
int PASCAL WinMain(HINSTANCE i, HINSTANCE previous, LPSTR command, int show)
{
    int result;
    (void)i;
    (void)previous;
    (void)show;
    result = run_test(strstr(command, "--absent") != NULL);

    if (!strstr(command, "--quiet"))
        MessageBox(NULL, result ? "FAIL: see MIDI16.LOG" : "PASS: see MIDI16.LOG", "VSC-55 Win16 test", MB_OK);

    return result;
}
#endif
