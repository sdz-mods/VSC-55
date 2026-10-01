/* Read-only LCD snapshots. All emulator access stays on its audio thread. */
#ifndef VSC_FRONTPROTO_H
#define VSC_FRONTPROTO_H
#define VSC_FRONT_MAP "VSC55_FRONT_V2"
#define VSC_FRONT_CLASS "VSC55_FRONT_PANEL_V1"
#define VSC_FRONT_MAGIC 0x56534631
#define VSC_CTL_BUTTONS 5
#define VSC_CTL_DISPLAY 6
#define VSC_FRONT_BUTTON_MASK 0x007f7f78UL
typedef struct
{
    LONG magic, pid, sequence, enabled, cursor, address;
    unsigned char data[80], cg[64];
    LONG buttons, frames, leds;
} VSC_FRONT_STATE;
#endif
