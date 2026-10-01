/* Win32 panel/host contract. Driver ABI is unchanged. GPL-2.0-or-later. */
#ifndef VSC55_CONTROL_H
#define VSC55_CONTROL_H
#define VSC_STATUS_NAME "VSC55_STATUS_V1"
#define VSC_STOP_EVENT "VSC55_STOP_V1"
#define VSC_HOST_MUTEX "VSC55_HOST_V1"
#define VSC_TIMER_PERIOD_MS 5
#define VSC_MAX_VOLUME_PERCENT 1600
#define VSC_PANEL_CLASS "VSC55_CONTROL_PANEL_V1"
#define VSC_STATUS_MAGIC 0x56534337
#define VSC_CONTROL (WM_APP+55)
enum { VSC_CTL_VOLUME = 1, VSC_CTL_PANIC, VSC_CTL_GS, VSC_CTL_GM };
enum { VSC_STATE_STARTING = 1, VSC_STATE_RUNNING, VSC_STATE_STOPPING, VSC_STATE_FAILED, VSC_STATE_STOPPED };
typedef struct
{
    LONG magic, pid, state, volume, buffer_ms, device;
    LONG blocks, underruns, midi_bytes, queue, command_error;
    char model[64];
} VSC_STATUS;
#endif
