/* Win16/Win32 protocol: legacy scalars and v4 driver-owned shared ring. SPDX-License-Identifier: MIT */
#ifndef VSC55_MIDIPROTO_H
#define VSC55_MIDIPROTO_H
/* DriverProc polling ABI: scalar values only across the Win16 thunk. */
#define VSC_ATTACH 0x5000
#define VSC_DETACH 0x5001
#define VSC_HEARTBEAT 0x5002
#define VSC_PEEK 0x5003
#define VSC_LENGTH 0x5004
#define VSC_READ 0x5005
#define VSC_POP 0x5006
#define VSC_STATS 0x5007
#define VSC_DIAG_VERSION 5
enum { DS_VERSION, DS_ACCEPTED, DS_POPPED, DS_FULL, DS_DEAD, DS_BUSY, DS_NOMEM,
       DS_HIGH, DS_PENDING, DS_OLDEST, DS_AGE_MAX, DS_SHORT_REJECT, DS_LONG_REJECT,
       DS_PROGRAM_REJECT, DS_OFF_REJECT, DS_CC_REJECT, DS_LAST_REJECT,
       DS_OPEN_REJECT, DS_CLOSE_REJECT, DS_RESET_REJECT, DS_POST_FAIL, DS_COUNT
     };
enum { PS_POLLS = DS_COUNT, PS_GAP_MAX, PS_BUSY_GAP_MAX, PS_POLL_MAX,
       PS_BACKPRESSURE, PS_FORWARDED, PS_COUNT
     };
#define VSC_CLASS "VSC55_PROTO1_RECEIVER"
#define VSC_MAGIC 0x00535531UL
#define VSC_PING    (WM_USER+0x155)
#define VSC_SESSION (WM_USER+0x156)
#define VSC_OPEN    (WM_USER+0x157)
#define VSC_CLOSE   (WM_USER+0x158)
#define VSC_SHORT   (WM_USER+0x159)
#define VSC_BEGIN   (WM_USER+0x15a)
#define VSC_CHUNK   (WM_USER+0x15b)
#define VSC_COMMIT  (WM_USER+0x15c)
#define VSC_ABORT   (WM_USER+0x15d)
#define VSC_RESET   (WM_USER+0x15e)
#define VSC_STOPPED (WM_USER+0x15f)
#define VSC_DIAG    (WM_USER+0x160)
#define VSC_DIAG_COMMIT 31
#define VSC_BIND (WM_USER+0x161)
#define VSC_WAKE (WM_USER+0x162)
#define VSC_DIRECT_MAGIC 0x56345351UL
#define VSC_DIRECT_CAPACITY 1024
#define VSC_RING_SLOTS (VSC_DIRECT_CAPACITY+1)
/* Fixed Win16/Win32 layout, entirely driver-owned. Indices stay below 65536:
 * Win16's low-word publication is atomic, high words always zero. Producer
 * uses volatile stores in order; x86 preserves store order. Win32 uses
 * interlocked loads/stores for acquire/release. No application pointer crosses.
 * A long buffer is locked until tail has passed it; only the driver frees it. */
#pragma pack(push,4)
typedef struct
{
    DWORD kind, scalar, length, queued, data16;
} VSC_DIRECT_EVENT;
typedef struct
{
    DWORD magic, version, session, enabled, head, tail, wake;
    DWORD stats[DS_COUNT];
    VSC_DIRECT_EVENT events[VSC_RING_SLOTS];
    /* Diagnostic-only breadcrumbs; no callback-side I/O or extra OS calls. */
    DWORD trace_stage, trace_msg, trace_p1, trace_p2;
    DWORD trace_entered, trace_returned, trace_result, trace_busy;
} VSC_DIRECT_QUEUE;
#pragma pack(pop)
#define VSC_OK 1
#define VSC_FULL 2
#define VSC_MAX_SYSEX 16384UL
#define VSC_QUEUE 128
#endif
