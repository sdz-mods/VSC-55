/* Plays a chord through the installed endpoint. SPDX-License-Identifier: MIT */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    UINT id, selected = (UINT) - 1, r;
    MIDIOUTCAPS caps;
    HMIDIOUT out;

    for (id = 0; id < midiOutGetNumDevs(); id++)
        if (!midiOutGetDevCaps(id, &caps, sizeof(caps)) && !strcmp(caps.szPname, "VSC-55"))
            selected = id;

    if (selected == (UINT) - 1)
    {
        puts("Endpoint missing");
        return 1;
    }

    r = midiOutOpen(&out, selected, 0, 0, CALLBACK_NULL);

    if (r)
    {
        printf("Open failed %u\n", r);
        return 1;
    }

    r = midiOutShortMsg(out, 0xc0);
    r |= midiOutShortMsg(out, 0x643c90);
    r |= midiOutShortMsg(out, 0x644090);
    r |= midiOutShortMsg(out, 0x644390);
    Sleep(2000);
    r |= midiOutShortMsg(out, 0x3c80);
    r |= midiOutShortMsg(out, 0x4080);
    r |= midiOutShortMsg(out, 0x4380);
    Sleep(500);
    r |= midiOutClose(out);
    printf("MIDITONE %s\n", r ? "FAIL" : "PASS");
    return r ? 1 : 0;
}
