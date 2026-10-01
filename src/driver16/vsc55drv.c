/* Win16 NE MIDI output driver. SPDX-License-Identifier: GPL-2.0-or-later
 * Asynchronous PostMessage wake-ups; Win32 consumes a shared queue. */
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include "../common/midiproto.h"
typedef char check_shared_abi[(sizeof(VSC_DIRECT_QUEUE) == 20644) ? 1 : -1];
#define M_NUM 1
#define M_CAPS 2
#define M_OPEN 3
#define M_CLOSE 4
#define M_PREPARE 5
#define M_UNPREPARE 6
#define M_DATA 7
#define M_LONG 8
#define M_RESET 9
typedef struct
{
    HMIDI hMidi;
    DWORD callback, instance, node, ids;
} OPEN_DESC;
extern BOOL FAR PASCAL DriverCallback(DWORD, UINT, HDRVR, UINT, DWORD, DWORD, DWORD);
static BOOL opened = FALSE, busy = FALSE;
static DWORD callback = 0, instance = 0, flags = 0, cookie = 0;
static HMIDIOUT handle = 0;
static HGLOBAL shared_block, blocks[VSC_RING_SLOTS];
static volatile VSC_DIRECT_QUEUE FAR *shared;
static UINT reap;
static HWND receiver;
static DWORD attached;
static DWORD stats[DS_COUNT];
static UINT trace_depth;
static void trace(UINT stage)
{
    if (shared && trace_depth == 1)
    {
        shared->trace_busy = busy;
        shared->trace_stage = stage;
    }
}
static void notify(UINT msg, DWORD p1)
{
    if (callback || (flags & CALLBACK_TYPEMASK))
        DriverCallback(callback, HIWORD(flags), (HDRVR)handle, msg, instance, p1, 0);
}
static void release_blocks(void)
{
    UINT i;

    for (i = 0; i < VSC_RING_SLOTS; i++)
        if (blocks[i])
        {
            trace(20);
            GlobalUnlock(blocks[i]);
            trace(21);
            GlobalFree(blocks[i]);
            trace(22);
            blocks[i] = 0;
        }
}
static void reap_blocks(void)
{
    if (!shared)
        return;

    while (reap != (UINT)shared->tail)
    {
        if (blocks[reap])
        {
            trace(20);
            GlobalUnlock(blocks[reap]);
            trace(21);
            GlobalFree(blocks[reap]);
            trace(22);
            blocks[reap] = 0;
        }

        reap = (reap + 1) % VSC_RING_SLOTS;
    }
}
static void publish_stats(void)
{
    UINT i;

    if (shared)
        for (i = 0; i < DS_COUNT; i++)
            if (i != DS_POPPED && i != DS_AGE_MAX)
                shared->stats[i] = stats[i];
}
/* MCI can deliver short messages in a context where querying a Win32
 * window from this Win16 driver does not return. Discovery/window validation
 * belongs to connect() at Open. Delivery reads only driver-owned memory.
 * The host clears enabled before normal shutdown; a fresh Open validates
 * the current window/session again before reusing or replacing the queue. */
static BOOL alive(void)
{
    BOOL valid;
    trace(10);
    valid = shared && receiver && attached && shared->enabled
            && shared->magic == VSC_DIRECT_MAGIC
            && shared->version == VSC_DIAG_VERSION && shared->session == attached;
    trace(17);
    return valid;
}
static BOOL connect(void)
{
    HWND window = FindWindow(VSC_CLASS, NULL);
    DWORD generation;
    void FAR *memory;

    if (!window || (DWORD)GetWindowLong(window, 4) != VSC_DIRECT_MAGIC)
        return FALSE;

    generation = (DWORD)GetWindowLong(window, 0);

    if (!generation)
        return FALSE;

    if (alive() && attached == generation && receiver == window)
        return TRUE;

    release_blocks();

    if (shared_block)
    {
        GlobalUnlock(shared_block);
        GlobalFree(shared_block);
    }

    shared = NULL;
    shared_block = 0;
    attached = 0;
    receiver = 0;
    opened = FALSE;
    callback = flags = 0;
    shared_block = GlobalAlloc(GMEM_MOVEABLE | GMEM_SHARE | GMEM_ZEROINIT, sizeof(VSC_DIRECT_QUEUE));

    if (!shared_block)
        return FALSE;

    memory = GlobalLock(shared_block);

    if (!memory)
    {
        GlobalFree(shared_block);
        shared_block = 0;
        return FALSE;
    }

    shared = (volatile VSC_DIRECT_QUEUE FAR *)memory;
    memset(stats, 0, sizeof(stats));
    stats[DS_VERSION] = VSC_DIAG_VERSION;
    shared->magic = VSC_DIRECT_MAGIC;
    shared->version = VSC_DIAG_VERSION;
    shared->session = generation;
    shared->enabled = 1;
    reap = 0;
    attached = generation;
    receiver = window;
    publish_stats();

    if (!PostMessage(receiver, VSC_BIND, VSC_DIAG_VERSION, (LPARAM)(DWORD)memory))
    {
        ++stats[DS_POST_FAIL];
        shared->enabled = 0;
        publish_stats();
        return FALSE;
    }

    return TRUE;
}
static DWORD enqueue(UINT kind, DWORD scalar, LPCSTR bytes, UINT length)
{
    UINT head, next, tail, occupancy;
    volatile VSC_DIRECT_EVENT FAR *e;
    void FAR *data = NULL;
    BOOL posted;
    trace(30);

    if (!alive())
    {
        ++stats[DS_DEAD];
        return MMSYSERR_NOTENABLED;
    }

    trace(31);
    reap_blocks();
    trace(32);
    head = (UINT)shared->head;
    tail = (UINT)shared->tail;
    next = (head + 1) % VSC_RING_SLOTS;

    if (next == tail)
    {
        ++stats[DS_FULL];
        return MIDIERR_NOTREADY;
    }

    if (length)
    {
        blocks[head] = GlobalAlloc(GMEM_MOVEABLE | GMEM_SHARE, length);

        if (!blocks[head])
        {
            ++stats[DS_NOMEM];
            return MMSYSERR_NOMEM;
        }

        data = GlobalLock(blocks[head]);

        if (!data)
        {
            GlobalFree(blocks[head]);
            blocks[head] = 0;
            ++stats[DS_NOMEM];
            return MMSYSERR_NOMEM;
        }

        _fmemcpy(data, bytes, length);
    }

    e = &shared->events[head];
    e->kind = kind;
    e->scalar = scalar;
    e->length = length;
    trace(40);
    e->queued = GetTickCount();
    trace(41);
    e->data16 = (DWORD)data;
    ++stats[DS_ACCEPTED];
    occupancy = (next + VSC_RING_SLOTS - tail) % VSC_RING_SLOTS;

    if (occupancy > stats[DS_HIGH])
        stats[DS_HIGH] = occupancy;

    trace(42);
    shared->head = next;
    trace(43);

    /* Coalesce wake-ups: Win98's message queue can fill near 128 posts.
     * Data ownership is the ring, not the wake message. A lost notification
     * is recovered by the host's 10 ms Win32 wait; never retract published data. */
    if (!shared->wake)
    {
        shared->wake = 1;
        trace(50);
        posted = PostMessage(receiver, VSC_WAKE, 0, attached);
        trace(51);

        if (!posted)
        {
            ++stats[DS_POST_FAIL];
            shared->wake = 0;
        }
    }

    return 0;
}
static DWORD mod_message(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    DWORD result;
    LPMIDIHDR hdr;
    DWORD length;
    trace(3);
    reap_blocks();
    trace(4);

    if (msg == M_NUM)
        return 1;

    if (id != 0)
        return MMSYSERR_BADDEVICEID;

    if (msg == M_CAPS)
    {
        MIDIOUTCAPS caps;
        UINT n;

        if (!p1)
            return MMSYSERR_INVALPARAM;

        memset(&caps, 0, sizeof(caps));
        caps.vDriverVersion = 0x0100;
        lstrcpy(caps.szPname, "VSC-55");
        caps.wTechnology = MOD_MIDIPORT;
        caps.wChannelMask = 0xffff;
        n = p2 < sizeof(caps) ? (UINT)p2 : sizeof(caps);

        if (IsBadWritePtr((void FAR *)p1, n))
            return MMSYSERR_INVALPARAM;

        _fmemcpy((void FAR *)p1, &caps, n);
        return 0;
    }

    if (busy)
    {
        ++stats[DS_BUSY];
        return MIDIERR_NOTREADY;
    }

    if (msg == M_OPEN)
    {
        OPEN_DESC FAR *desc = (OPEN_DESC FAR *)p1;

        if (!connect())
        {
            ++stats[DS_DEAD];
            return MMSYSERR_NOTENABLED;
        }

        if (opened)
            return MMSYSERR_ALLOCATED;

        if (!user || !p1 || IsBadWritePtr((void FAR *)user, sizeof(DWORD)) || IsBadReadPtr(desc, sizeof(OPEN_DESC)))
            return MMSYSERR_INVALPARAM;

        if (p2 & ~CALLBACK_TYPEMASK)
            return MMSYSERR_NOTSUPPORTED;

        busy = TRUE;
        result = enqueue(VSC_OPEN, 0, NULL, 0);

        if (result)
        {
            busy = FALSE;
            return result;
        }

        handle = (HMIDIOUT)desc->hMidi;
        callback = desc->callback;
        instance = desc->instance;
        flags = p2;
        ++cookie;

        if (!cookie)
            ++cookie;

        *(DWORD FAR *)user = cookie;
        opened = TRUE;
        busy = FALSE;
        notify(MOM_OPEN, 0);
        return 0;
    }

    if (!opened || user != cookie)
        return MMSYSERR_INVALHANDLE;

    if (msg == M_CLOSE)
    {
        DWORD cb = callback, inst = instance, fl = flags;
        HMIDIOUT h = handle;
        busy = TRUE;
        result = alive() ? enqueue(VSC_CLOSE, 0, NULL, 0) : 0;

        if (result)
        {
            busy = FALSE;
            return result;
        }

        opened = FALSE;
        callback = 0;
        flags = 0;
        busy = FALSE;

        if (cb || (fl & CALLBACK_TYPEMASK))
            DriverCallback(cb, HIWORD(fl), (HDRVR)h, MOM_CLOSE, inst, 0, 0);

        return 0;
    }

    if (msg == M_DATA || msg == M_RESET)
    {
        if (msg == M_DATA && ((p1 & 0xff) < 0x80))
            return MMSYSERR_INVALPARAM;

        busy = TRUE;
        result = enqueue(msg == M_DATA ? VSC_SHORT : VSC_RESET, p1, NULL, 0);
        busy = FALSE;
        return result;
    }

    if (msg != M_PREPARE && msg != M_UNPREPARE && msg != M_LONG)
        return MMSYSERR_NOTSUPPORTED;

    if (!p1 || p2 < sizeof(MIDIHDR) || IsBadWritePtr((void FAR *)p1, sizeof(MIDIHDR)))
        return MMSYSERR_INVALPARAM;

    hdr = (LPMIDIHDR)p1;

    if (hdr->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    if (msg == M_UNPREPARE)
    {
        hdr->dwFlags &= ~MHDR_PREPARED;
        return 0;
    }

    length = hdr->dwBufferLength;

    if (!hdr->lpData || !length || length > VSC_MAX_SYSEX || IsBadReadPtr(hdr->lpData, (UINT)length))
        return MMSYSERR_INVALPARAM;

    if (msg == M_PREPARE)
    {
        hdr->dwFlags |= MHDR_PREPARED;
        return 0;
    }

    if (!(hdr->dwFlags & MHDR_PREPARED))
        return MIDIERR_UNPREPARED;

    busy = TRUE;
    result = enqueue(VSC_COMMIT, 0, hdr->lpData, (UINT)length);

    if (result)
    {
        busy = FALSE;
        return result;
    }

    /* Driver queue owns a complete copy. No asynchronous pointer or header survives. */
    hdr->dwFlags &= ~MHDR_INQUEUE;
    hdr->dwFlags |= MHDR_DONE;
    busy = FALSE;
    notify(MOM_DONE, p1);
    return 0;
}
DWORD FAR PASCAL _loadds MODMESSAGE(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    DWORD result;
    UINT status = (UINT)p1 & 0xf0;
    ++trace_depth;

    if (shared && trace_depth == 1)
    {
        shared->trace_msg = msg;
        shared->trace_p1 = p1;
        shared->trace_p2 = p2;
        ++shared->trace_entered;
        trace(1);
    }

    result = mod_message(id, msg, user, p1, p2);

    if (shared && trace_depth == 1)
    {
        shared->trace_result = result;
        ++shared->trace_returned;
        trace(200);
    }

    --trace_depth;

    if (result && msg == M_DATA)
    {
        ++stats[DS_SHORT_REJECT];
        stats[DS_LAST_REJECT] = p1;

        if (status == 0xc0)
            ++stats[DS_PROGRAM_REJECT];

        if (status == 0x80 || (status == 0x90 && !(p1 & 0x7f0000UL)))
            ++stats[DS_OFF_REJECT];

        if (status == 0xb0)
            ++stats[DS_CC_REJECT];
    }

    if (result && msg == M_LONG)
        ++stats[DS_LONG_REJECT];

    if (result && msg == M_OPEN)
        ++stats[DS_OPEN_REJECT];

    if (result && msg == M_CLOSE)
        ++stats[DS_CLOSE_REJECT];

    if (result && msg == M_RESET)
        ++stats[DS_RESET_REJECT];

    publish_stats();
    return result;
}
DWORD FAR PASCAL _loadds MIDMESSAGE(UINT id, UINT msg, DWORD u, DWORD p1, DWORD p2)
{
    (void)id;
    (void)u;
    (void)p1;
    (void)p2;
    return msg == M_NUM ? 0 : MMSYSERR_NOTSUPPORTED;
}
DWORD FAR PASCAL _loadds __exportedstub(void)
{
    return 0;
}
LRESULT FAR PASCAL _loadds DriverProc(DWORD id, HDRVR driver, UINT msg, LPARAM p1, LPARAM p2)
{
    if (msg == VSC_ATTACH)
        return 0; /* Legacy polling helper is deliberately unsupported. */

    if (msg == VSC_STATS)
    {
        UINT head, tail;
        DWORD age;

        if (!shared || (DWORD)p1 != attached || (DWORD)p2 >= DS_COUNT)
            return 0;

        head = (UINT)shared->head;
        tail = (UINT)shared->tail;

        if (p2 == DS_PENDING)
            return (head + VSC_RING_SLOTS - tail) % VSC_RING_SLOTS;

        age = head != tail ? (DWORD)(GetTickCount() - shared->events[tail].queued) : 0;

        if (p2 == DS_OLDEST)
            return age;

        if (p2 == DS_AGE_MAX)
            return age > shared->stats[DS_AGE_MAX] ? age : shared->stats[DS_AGE_MAX];

        return shared->stats[(UINT)p2];
    }

    switch (msg)
    {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
            return 1;

        case 8:
            return 0;
    }

    return DefDriverProc(id, driver, msg, p1, p2);
}
int FAR PASCAL LibMain(HINSTANCE i, WORD h, WORD c, LPSTR s)
{
    (void)i;
    (void)h;
    (void)c;
    (void)s;
    return 1;
}
int FAR PASCAL WEP(int code)
{
    (void)code;
    return 1;
}
