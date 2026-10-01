/* Native Win98 front panel. GPL-2.0-or-later. Artwork is a separate skin;
 * see assets/frontpanel-float/LICENSE.txt and LICENSES.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../common/control.h"
#include "../common/version.h"
#include "../common/frontproto.h"
#include "../common/midiproto.h"
#include "frontpanel.h"
#define NI __attribute__((noinline))
extern const unsigned *vsc_lcd_render(const VSC_FRONT_STATE *frame, int contrast);
extern const unsigned *vsc_lcd_scaled(int width, int height);
typedef struct
{
    const char *name;
    int bit, key, l, t, r, b;
} Button;
/* Hit regions measured on the supplied faceplate, in the reference GUI's 1120x233 logical pixels. */
static const Button buttons[] =
{
    {"ALL", 6, 'W', 754, 35, 780, 61},
    {"MUTE", 5, 'E', 754, 82, 780, 108},
    {"PART <", 22, VK_LEFT, 849, 37, 871, 59},
    {"PART >", 14, VK_RIGHT, 903, 37, 925, 59},
    {"INSTRUMENT -", 3, 'Y', 968, 38, 1021, 56},
    {"INSTRUMENT +", 4, 'U', 1024, 38, 1077, 56},
    {"LEVEL <", 20, 'P', 831, 85, 884, 103},
    {"LEVEL >", 21, VK_OEM_4, 887, 85, 940, 103},
    {"PAN <", 12, 'D', 968, 85, 1021, 103},
    {"PAN >", 13, 'F', 1024, 85, 1077, 103},
    {"REVERB <", 18, 'G', 831, 132, 884, 150},
    {"REVERB >", 19, 'H', 887, 132, 940, 150},
    {"CHORUS <", 10, 'J', 968, 132, 1021, 150},
    {"CHORUS >", 11, 'K', 1024, 132, 1077, 150},
    {"KEY SHIFT <", 16, 'I', 831, 178, 884, 196},
    {"KEY SHIFT >", 17, 'O', 887, 178, 940, 196},
    {"MIDI CH <", 8, 'A', 968, 178, 1021, 196},
    {"MIDI CH >", 9, 'S', 1024, 178, 1077, 196}
};
static HWND window, controller;
static HMENU contrast_menu;
static HBITMAP artwork, background, surface;
static HDC artdc, backdc, surfacedc;
static HGDIOBJ artold, backold, surfaceold;
static HANDLE map;
static const volatile VSC_FRONT_STATE *shared;
static VSC_FRONT_STATE frame, previous;
static const unsigned *lcd_pixels;
static int busy, running, gain = 100, dragging, mouse_button = -1, hover = -1, have_frame, contrast = 8;
static DWORD mouse_mask, key_mask;
static RECT panel, lcdrect, statusrect;
static char config[MAX_PATH], state_text[600], detail[400], model_name[64];
static BITMAPINFO dib;
static int cw, ch, remembering, ticking;
static DWORD paints, paint_ms, polls, renders, layouts, report_tick;
static char gui_log[MAX_PATH];
/* A stopped/hidden front has no refresh timer at all. */
static void update_timer(void)
{
    int wanted = window && running && IsWindowVisible(window) && !IsIconic(window);

    if (wanted && !ticking)
        ticking = SetTimer(window, 1, 100, NULL) != 0;
    else if (!wanted && ticking)
    {
        KillTimer(window, 1);
        ticking = 0;
    }
}
static RECT region(int l, int t, int r, int b)
{
    RECT a = {panel.left + MulDiv(l, panel.right - panel.left, 1120), panel.top + MulDiv(t, panel.bottom - panel.top, 233),
              panel.left + MulDiv(r, panel.right - panel.left, 1120), panel.top + MulDiv(b, panel.bottom - panel.top, 233)
             };
    return a;
}
static void send(int code, LPARAM value)
{
    HWND h = FindWindowA(VSC_CLASS, NULL);

    if (h)
        PostMessageA(h, VSC_CONTROL, code, value);
}
static void release(void)
{
    mouse_mask = key_mask = 0;
    mouse_button = -1;
    dragging = 0;
    send(VSC_CTL_BUTTONS, 0);

    if (GetCapture() == window)
        ReleaseCapture();
}
static void disconnect(void)
{
    if (shared)
        UnmapViewOfFile((const void *)shared);

    shared = NULL;

    if (map)
        CloseHandle(map);

    map = NULL;
    have_frame = 0;
    lcd_pixels = NULL;
}
static void repaint_controls(void)
{
    if (window && IsWindowVisible(window))
        InvalidateRect(window, NULL, FALSE);
}
static NI void remember(void)
{
    RECT r;
    char s[32];

    if (!remembering || IsIconic(window))
        return;

    GetWindowRect(window, &r);
    sprintf(s, "%ld", r.left);
    WritePrivateProfileStringA("FrontPanel", "X", s, config);
    sprintf(s, "%ld", r.top);
    WritePrivateProfileStringA("FrontPanel", "Y", s, config);
    sprintf(s, "%ld", r.right - r.left);
    WritePrivateProfileStringA("FrontPanel", "Width", s, config);
    sprintf(s, "%ld", r.bottom - r.top);
    WritePrivateProfileStringA("FrontPanel", "Height", s, config);
}
static NI void status_text(void)
{
    char s[600];

    if (hover >= 0 && hover < 18)
        sprintf(s, "%s - hold to repeat; Shift-click holds its paired button too", buttons[hover].name);
    else if (hover == 18)
        sprintf(s, "POWER - %s emulation", busy ? "stop" : "start");
    else if (hover == 19)
        sprintf(s, "Output volume: %d%% - point around the knob or use the wheel; double-click for 100%%", gain);
    else if (!busy && detail[0])
        snprintf(s, sizeof(s), "STOPPED | %.64s | %.440s", model_name, detail);
    else
        sprintf(s, "%s | %s | Volume %d%% | Settings... opens the separate control panel",
                running ? "RUNNING" : busy ? "STARTING / STOPPING" : "STOPPED", model_name, gain);

    if (strcmp(s, state_text))
    {
        strcpy(state_text, s);
        InvalidateRect(window, &statusrect, FALSE);
    }
}
static NI void layout(void)
{
    RECT r;
    HDC dc;
    int pw, ph;
    ++layouts;
    GetClientRect(window, &r);
    cw = r.right;
    ch = r.bottom;

    if (cw < 1 || ch < 25)
        return;

    if (background)
    {
        SelectObject(backdc, backold);
        DeleteObject(background);
        background = NULL;
    }

    if (surface)
    {
        SelectObject(surfacedc, surfaceold);
        DeleteObject(surface);
        surface = NULL;
    }

    dc = GetDC(window);
    background = CreateCompatibleBitmap(dc, cw, ch);
    surface = CreateCompatibleBitmap(dc, cw, ch);
    ReleaseDC(window, dc);

    if (surface)
        surfaceold = SelectObject(surfacedc, surface);

    if (!background)
        return;

    backold = SelectObject(backdc, background);
    FillRect(backdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    pw = cw;
    ph = MulDiv(pw, 233, 1120);

    if (ph > ch - 24)
    {
        ph = ch - 24;
        pw = MulDiv(ph, 1120, 233);
    }

    panel.left = (cw - pw) / 2;
    panel.top = (ch - 24 - ph) / 2;
    panel.right = panel.left + pw;
    panel.bottom = panel.top + ph;

    if (artwork)
    {
        SetStretchBltMode(backdc, HALFTONE);
        SetBrushOrgEx(backdc, 0, 0, NULL);
        StretchBlt(backdc, panel.left, panel.top, pw, ph, artdc, 0, 0, 2240, 466, SRCCOPY);
    }

    lcdrect = region(283, 49, 653, 183);
    statusrect.left = 0;
    statusrect.right = cw;
    statusrect.top = ch - 24;
    statusrect.bottom = ch;
    InvalidateRect(window, NULL, FALSE);
}
static void sprite(HDC dc, int sx, int sy, int sw, int sh, int x, int y, int w, int h)
{
    RECT r = region(x, y, x + w, y + h);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, artdc, sx, sy, sw, sh, SRCCOPY);
}
static unsigned knob_source[118 * 118], knob_pixels[118 * 118];
static int knob_ready, knob_gain = -1;
static NI void draw_knob(HDC dc)
{
    BITMAPINFO info;
    RECT r = region(153, 42, 212, 101);
    int x, y;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = 40;
    info.bmiHeader.biWidth = 118;
    info.bmiHeader.biHeight = -118;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;

    if (!knob_ready && artwork)
    {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP b = CreateCompatibleBitmap(dc, 118, 118);
        HGDIOBJ old = SelectObject(mem, b);
        BitBlt(mem, 0, 0, 118, 118, artdc, 306, 84, SRCCOPY);
        SelectObject(mem, old);
        knob_ready = GetDIBits(mem, b, 0, 118, knob_source, &info, DIB_RGB_COLORS) != 0;
        DeleteObject(b);
        DeleteDC(mem);
    }

    if (!knob_ready)
        return;

    if (knob_gain != gain)
    {
        double angle = (-150.0 + 300.0 * gain / VSC_MAX_VOLUME_PERCENT) * 3.141592653589793 / 180, co = cos(angle),
               si = sin(angle);

        for (y = 0; y < 118; y++)
            for (x = 0; x < 118; x++)
            {
                double dx = x - 58.5, dy = y - 58.5;
                int sx = (int)(co * dx + si * dy + 59), sy = (int)(-si * dx + co * dy + 59);
                knob_pixels[y * 118 + x] = (dx * dx + dy * dy < 56 * 56 && sx >= 0 && sx < 118 && sy >= 0 &&
                                            sy < 118) ? knob_source[sy * 118 + sx] : knob_source[y * 118 + x];
            }

        knob_gain = gain;
    }

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, 118, 118, knob_pixels, &info, DIB_RGB_COLORS,
                  SRCCOPY);
}
static NI void paint(void)
{
    DWORD start = GetTickCount();
    PAINTSTRUCT ps;
    HDC target = BeginPaint(window, &ps), dc = surface ? surfacedc : target;
    RECT r;
    int i, saved = SaveDC(dc);
    HGDIOBJ old;
    IntersectClipRect(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
    ++paints;

    /* Compose the complete image off-screen. The display sees one final blit,
     * never the blank faceplate/LCD followed by separate overlays. */
    if (background)
        BitBlt(dc, 0, 0, cw, ch, backdc, 0, 0, SRCCOPY);

    if (have_frame && running && lcd_pixels)
    {
        int w = lcdrect.right - lcdrect.left, h = lcdrect.bottom - lcdrect.top;
        const unsigned *p = vsc_lcd_scaled(w, h);
        dib.bmiHeader.biWidth = w;
        dib.bmiHeader.biHeight = -h;

        if (p)
            SetDIBitsToDevice(dc, lcdrect.left, lcdrect.top, w, h, 0, 0, 0, h, p, &dib, DIB_RGB_COLORS);
    }
    else
    {
        HBRUSH brush = CreateSolidBrush(RGB(28, 28, 25));
        FillRect(dc, &lcdrect, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(156, 151, 116));
        old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        r = lcdrect;
        DrawTextA(dc, !artwork ? "PANEL.BMP missing - reinstall package" : running ? "Waiting for LCD..." : busy ?
                  "Booting firmware..." : "VSC-55  /  POWER OFF", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
    }

    draw_knob(dc);

    if (artwork && have_frame && running)
    {
        if (frame.leds & 1)
            sprite(dc, 0, 466, 52, 52, 754, 35, 26, 26);

        if (frame.leds & 2)
            sprite(dc, 0, 466, 52, 52, 754, 82, 26, 26);

        if (frame.leds & 4)
            sprite(dc, 0, 518, 20, 20, 118, 42, 10, 10);
    }

    /* Model marks are separate cells in the original artwork atlas. */
    if (artwork)
    {
        int type = strncmp(model_name, "mk1-", 4) ? 3 : !strcmp(model_name, "mk1-v1.00") ? 0 : !strcmp(model_name,
                   "mk1-v1.10") ? 2 : -1;

        if (type >= 0)
            sprite(dc, 804 + 262 * (type & 1), 466 + 50 * (type >> 1), 262, 50, 533, 195, 131, 25);

        if (type == -1 || type == 3)
            sprite(dc, type == -1 ? 1592 : 1392, 466, 200, 104, 696, 174, 100, 52);
    }

    for (i = 0; i < 18; i++)
        if ((mouse_mask | key_mask) & (1UL << buttons[i].bit))
        {
            r = region(buttons[i].l, buttons[i].t, buttons[i].r, buttons[i].b);
            DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT);
        }

    FillRect(dc, &statusrect, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    r = statusrect;
    r.left += 8;
    DrawTextA(dc, state_text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    RestoreDC(dc, saved);

    if (surface)
        BitBlt(target, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

    EndPaint(window, &ps);
    paint_ms += GetTickCount() - start;
}
static NI void poll(void)
{
    LONG a, b;
    int retry;
    ++polls;

    if (!running || !IsWindowVisible(window) || IsIconic(window))
        return;

    send(VSC_CTL_DISPLAY, 1);

    if (!shared)
    {
        map = OpenFileMappingA(FILE_MAP_READ, FALSE, VSC_FRONT_MAP);

        if (map)
            shared = (const volatile VSC_FRONT_STATE *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(frame));

        if (!shared && map)
        {
            CloseHandle(map);
            map = NULL;
        }
    }

    if (!shared || shared->magic != VSC_FRONT_MAGIC)
        return;

    for (retry = 0; retry < 3; retry++)
    {
        a = shared->sequence;

        if (a & 1)
            continue;

        MemoryBarrier();
        memcpy(&frame, (const void *)shared, sizeof(frame));
        MemoryBarrier();
        b = shared->sequence;

        if (a == b && !(b & 1) && b)
        {
            int lcd_changed = !have_frame || frame.enabled != previous.enabled || frame.cursor != previous.cursor ||
                              ((frame.cursor || previous.cursor) && frame.address != previous.address) || memcmp(frame.data, previous.data, 80) ||
                              memcmp(frame.cg, previous.cg, 64);

            if (lcd_changed)
            {
                ++renders;
                lcd_pixels = vsc_lcd_render(&frame, contrast);
                InvalidateRect(window, &lcdrect, FALSE);
            }

            if (!have_frame || frame.leds != previous.leds)
            {
                RECT a = region(754, 35, 780, 108), b = region(118, 42, 128, 52);
                InvalidateRect(window, &a, FALSE);
                InvalidateRect(window, &b, FALSE);
            }

            previous = frame;
            have_frame = 1;
            break;
        }
    }
}
static int hit(int x, int y)
{
    POINT p = {x, y};
    RECT r;
    int i;

    for (i = 0; i < 18; i++)
    {
        r = region(buttons[i].l, buttons[i].t, buttons[i].r, buttons[i].b);

        if (PtInRect(&r, p))
            return i;
    }

    r = region(38, 36, 105, 55);

    if (PtInRect(&r, p))
        return 18;

    r = region(153, 42, 212, 101);
    return PtInRect(&r, p) ? 19 : -1;
}
static void volume_to(int value)
{
    if (value < 0)
        value = 0;

    if (value > VSC_MAX_VOLUME_PERCENT)
        value = VSC_MAX_VOLUME_PERCENT;

    gain = value;
    PostMessageA(controller, WM_APP + 57, value, 0);
    status_text();
    repaint_controls();
}
static void volume_at(int x, int y)
{
    RECT r = region(153, 42, 212, 101);
    double dx = x - (r.left + r.right) / 2.0, dy = y - (r.top + r.bottom) / 2.0;
    double angle;

    if (dx * dx + dy * dy < 9)
        return; /* Angle undefined at the centre. */

    angle = atan2(dx, -dy) * 180 / 3.141592653589793;
    volume_to((int)((angle + 150)*VSC_MAX_VOLUME_PERCENT / 300 + 0.5));
}
static NI void help(void)
{
    MessageBoxA(window,
                "Power starts/stops the host. Settings opens a separate window.\n\nHold a button to let the firmware repeat it. Shift-click presses both buttons of a pair.\nKeyboard: arrows = Part; W/E = All/Mute; Y/U = Instrument; P/[ = Level; D/F = Pan; G/H = Reverb; J/K = Chorus; I/O = Key Shift; A/S = MIDI Channel.\n\nVolume: point around the knob, mouse wheel, or double-click for 100%. The bottom gap clamps to 0/1600%. It controls VSC-55 output gain (0-1600%).\n\nClosing the front panel hides both windows; playback continues. Tray Exit stops everything.",
                "VSC-55 front panel controls", MB_OK);
}
static NI void menu_command(int id)
{
    if (id >= 1201 && id <= 1216)
    {
        char s[16];
        contrast = id - 1200;
        sprintf(s, "%d", contrast);
        WritePrivateProfileStringA("FrontPanel", "Contrast", s, config);
        CheckMenuRadioItem(contrast_menu, 1201, 1216, id, MF_BYCOMMAND);

        if (have_frame)
            lcd_pixels = vsc_lcd_render(&frame, contrast);

        InvalidateRect(window, &lcdrect, FALSE);
        return;
    }

    if (id == 1016)
    {
        PostMessageA(controller, WM_COMMAND, 1016, 0);
        return;
    }

    if (id == 1101)
    {
        help();
        return;
    }

    if (id >= 1102 && id <= 1104)
    {
        RECT area, r;
        int width = id == 1102 ? 760 : id == 1103 ? 1120 : 1400;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &area, 0);

        if (width > area.right - area.left - 16)
            width = area.right - area.left - 16;

        r.left = r.top = 0;
        r.right = width;
        r.bottom = MulDiv(width, 233, 1120) + 24;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
        SetWindowPos(window, NULL, area.left + 8, area.top + 8, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
        remember();
        return;
    }

    PostMessageA(controller, WM_COMMAND, id, 0);
}
static LRESULT CALLBACK front_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        window = w;

    switch (msg)
    {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
            {
                release();
                send(VSC_CTL_DISPLAY, 0);
                ShowWindow(w, SW_HIDE);
                ShowWindow(controller, SW_HIDE);
            }
            else
                layout();

            return 0;

        case WM_SHOWWINDOW:
            update_timer();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            paint();
            return 0;

        case WM_TIMER:
            poll();
            return 0;

        case WM_COMMAND:
            menu_command(LOWORD(wp));
            return 0;

        case WM_EXITSIZEMOVE:
            remember();
            return 0;

        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)lp)->ptMinTrackSize.x = 560;
            ((MINMAXINFO *)lp)->ptMinTrackSize.y = 190;
            return 0;

        case WM_LBUTTONDOWN:
        {
            int i = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetFocus(w);

            if (i == 18)
            {
                PostMessageA(controller, WM_COMMAND, busy ? 1008 : 1007, 0);
                return 0;
            }

            if (i == 19)
            {
                dragging = 1;
                SetCapture(w);
                volume_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }

            if (i >= 0 && i < 18 && running)
            {
                mouse_button = i;
                mouse_mask = 1UL << buttons[i].bit;

                if (wp & MK_SHIFT)
                    mouse_mask |= 1UL << buttons[i ^ 1].bit;

                SetCapture(w);
                send(VSC_CTL_BUTTONS, mouse_mask | key_mask);
                repaint_controls();
            }

            return 0;
        }

        case WM_MOUSEMOVE:
            if (dragging)
                volume_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            else
            {
                int h = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

                if (h != hover)
                {
                    hover = h;
                    status_text();
                }
            }

            return 0;

        case WM_LBUTTONUP:
        {
            int save_gain = dragging;
            dragging = 0;
            mouse_mask = 0;
            mouse_button = -1;
            send(VSC_CTL_BUTTONS, key_mask);

            if (GetCapture() == w)
                ReleaseCapture();

            if (save_gain)
                PostMessageA(controller, WM_COMMAND, 1009, 0);

            repaint_controls();
            return 0;
        }

        case WM_LBUTTONDBLCLK:
            if (hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == 19)
            {
                volume_to(100);
                PostMessageA(controller, WM_COMMAND, 1009, 0);
            }

            return 0;

        case WM_MOUSEWHEEL:
        {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &p);

            if (hit(p.x, p.y) == 19)
            {
                volume_to(gain + GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 5);
                PostMessageA(controller, WM_COMMAND, 1009, 0);
            }

            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        {
            int i;

            if (!running)
                return 0;

            for (i = 0; i < 18; i++)
                if ((int)wp == buttons[i].key)
                {
                    if (msg == WM_KEYDOWN)
                        key_mask |= 1UL << buttons[i].bit;
                    else
                        key_mask &= ~(1UL << buttons[i].bit);

                    send(VSC_CTL_BUTTONS, mouse_mask | key_mask);
                    repaint_controls();
                    return 0;
                }

            break;
        }

        case WM_KILLFOCUS:
            release();
            hover = -1;
            status_text();
            repaint_controls();
            return 0;

        case WM_CAPTURECHANGED:
            if (dragging || mouse_mask)
            {
                dragging = 0;
                mouse_mask = 0;
                send(VSC_CTL_BUTTONS, key_mask);
                repaint_controls();
            }

            return 0;

        case WM_CLOSE:
            remember();
            release();
            send(VSC_CTL_DISPLAY, 0);
            ShowWindow(w, SW_HIDE);
            ShowWindow(controller, SW_HIDE);
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wp)
            {
                remember();
                SendMessageA(controller, WM_ENDSESSION, wp, lp);
            }

            return 0;

        case WM_DESTROY:
            release();
            KillTimer(w, 1);
            disconnect();
            return 0;
    }

    return DefWindowProcA(w, msg, wp, lp);
}
HWND front_create(HINSTANCE instance, HWND owner, HICON icon, HICON small, const char *directory, const char *ini)
{
    WNDCLASSA c;
    RECT area, r;
    char path[MAX_PATH];
    HMENU menu, module, view;
    int width, height, x, y;
    controller = owner;
    lstrcpynA(config, ini, MAX_PATH);
    sprintf(path, "%s\\PANEL.BMP", directory);
    artwork = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    artdc = CreateCompatibleDC(NULL);
    backdc = CreateCompatibleDC(NULL);
    surfacedc = CreateCompatibleDC(NULL);

    if (artwork)
        artold = SelectObject(artdc, artwork);

    /* Remove the wordmark in the private, loaded atlas BEFORE scaling/colour
     * conversion. A solid brush on the destination can dither differently from
     * the bitmap on 16-bit displays, leaving a visible rectangle. Copy the
     * adjacent blank column so the whole faceplate follows one bitmap path.
     * The on-disk artwork and all pixels outside this region stay unchanged. */
    if (artwork && !GetPrivateProfileIntA("FrontPanel", "ShowOriginalLogo", 0, ini))
    {
        SetStretchBltMode(artdc, COLORONCOLOR);
        StretchBlt(artdc, 1078, 20, 244, 62, artdc, 1070, 20, 1, 62, SRCCOPY);
    }

    memset(&dib, 0, sizeof(dib));
    dib.bmiHeader.biSize = 40;
    dib.bmiHeader.biWidth = 741;
    dib.bmiHeader.biHeight = -268;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    memset(&c, 0, sizeof(c));
    c.style = CS_DBLCLKS;
    c.hInstance = instance;
    c.lpfnWndProc = front_proc;
    c.lpszClassName = VSC_FRONT_CLASS;
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    c.hIcon = icon;

    if (!RegisterClassA(&c))
        return NULL;

    menu = CreateMenu();
    module = CreatePopupMenu();
    view = CreatePopupMenu();
    AppendMenuA(module, MF_STRING, 1007, "Start");
    AppendMenuA(module, MF_STRING, 1008, "Stop");
    AppendMenuA(module, MF_STRING, 1010, "Panic");
    AppendMenuA(module, MF_SEPARATOR, 0, NULL);
    AppendMenuA(module, MF_STRING, 1015, "Exit");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)module, "Module");
    AppendMenuA(menu, MF_STRING, 1016, "Settings...");
    AppendMenuA(view, MF_STRING, 1102, "Compact");
    AppendMenuA(view, MF_STRING, 1103, "Normal");
    AppendMenuA(view, MF_STRING, 1104, "Large");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)view, "Size");
    AppendMenuA(menu, MF_STRING, 1101, "Controls...");
    contrast = GetPrivateProfileIntA("FrontPanel", "Contrast", 8, ini);

    if (contrast < 1 || contrast > 16)
        contrast = 8;

    contrast_menu = CreatePopupMenu();

    for (x = 1; x <= 16; x++)
    {
        char label[32];
        sprintf(label, x == 8 ? "%d (default)" : "%d", x);
        AppendMenuA(contrast_menu, MF_STRING, 1200 + x, label);
    }

    AppendMenuA(menu, MF_POPUP, (UINT_PTR)contrast_menu, "LCD contrast");
    CheckMenuRadioItem(contrast_menu, 1201, 1216, 1200 + contrast, MF_BYCOMMAND);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &area, 0);
    width = 1120;

    if (width > area.right - area.left - 20)
        width = area.right - area.left - 20;

    r.left = r.top = 0;
    r.right = width;
    r.bottom = MulDiv(width, 233, 1120) + 24;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    width = GetPrivateProfileIntA("FrontPanel", "Width", r.right - r.left, ini);
    height = GetPrivateProfileIntA("FrontPanel", "Height", r.bottom - r.top, ini);

    if (width < 560)
        width = 560;

    if (width > area.right - area.left)
        width = area.right - area.left;

    if (height < 190)
        height = 190;

    if (height > area.bottom - area.top)
        height = area.bottom - area.top;

    x = GetPrivateProfileIntA("FrontPanel", "X", area.left + (area.right - area.left - width) / 2, ini);
    y = GetPrivateProfileIntA("FrontPanel", "Y", area.top + 40, ini);

    if (x < area.left)
        x = area.left;

    if (y < area.top)
        y = area.top;

    if (x + width > area.right)
        x = area.right - width;

    if (y + height > area.bottom)
        y = area.bottom - height;

    window = CreateWindowA(VSC_FRONT_CLASS, VSC_PRODUCT_TITLE " - Sound Canvas", WS_OVERLAPPEDWINDOW, x, y, width, height,
                           NULL, menu, instance, NULL);

    if (!window)
        return NULL;

    SendMessageA(window, WM_SETICON, ICON_SMALL, (LPARAM)small);
    remembering = 1;
    sprintf(gui_log, "%s\\VSCGUI.LOG", directory);
    {
        FILE *f = fopen(gui_log, "w");

        if (f)
        {
            fputs(VSC_PRODUCT_TITLE " GUI diagnostics; interval counters, milliseconds\n", f);
            fclose(f);
        }
    }
    report_tick = GetTickCount();
    layout();
    status_text();
    update_timer();
    return window;
}
void front_show(void)
{
    if (window)
    {
        ShowWindow(window, SW_RESTORE);
        update_timer();
        SetForegroundWindow(window);
        InvalidateRect(window, NULL, FALSE);
    }
}
void front_destroy(void)
{
    if (window)
    {
        remember();
        DestroyWindow(window);
        window = NULL;
    }

    if (background)
    {
        SelectObject(backdc, backold);
        DeleteObject(background);
        background = NULL;
    }

    if (artwork)
    {
        SelectObject(artdc, artold);
        DeleteObject(artwork);
        artwork = NULL;
    }

    if (surface)
    {
        SelectObject(surfacedc, surfaceold);
        DeleteObject(surface);
        surface = NULL;
    }

    if (surfacedc)
        DeleteDC(surfacedc);

    surfacedc = NULL;

    if (backdc)
        DeleteDC(backdc);

    if (artdc)
        DeleteDC(artdc);

    backdc = artdc = NULL;
}
void front_update(int b, int r, int v, const char *name, const char *message)
{
    int change = busy != b || running != r || gain != v || strcmp(model_name, name);

    if (running && !r)
    {
        release();
        disconnect();
    }

    busy = b;
    running = r;
    gain = v;
    lstrcpynA(model_name, name, sizeof(model_name));
    lstrcpynA(detail, message, sizeof(detail));

    if (window)
    {
        status_text();

        if (change && IsWindowVisible(window))
        {
            EnableMenuItem(GetSubMenu(GetMenu(window), 0), 1007, MF_BYCOMMAND | (busy ? MF_GRAYED : MF_ENABLED));
            EnableMenuItem(GetSubMenu(GetMenu(window), 0), 1008, MF_BYCOMMAND | (busy ? MF_ENABLED : MF_GRAYED));
        }

        if (change)
            repaint_controls();

        update_timer();

        if ((DWORD)(GetTickCount() - report_tick) >= 5000 && gui_log[0])
        {
            FILE *f = fopen(gui_log, "a");
            DWORD now = GetTickCount();

            if (f)
            {
                fprintf(f,
                        "TICK=%lu ELAPSED=%lu BUSY=%d RUNNING=%d VISIBLE=%d TIMER=%d PAINTS=%lu PAINT_MS=%lu POLLS=%lu RENDERS=%lu LAYOUTS=%lu\n",
                        now, now - report_tick, busy, running, IsWindowVisible(window), ticking, paints, paint_ms, polls, renders, layouts);
                fclose(f);
            }

            paints = paint_ms = polls = renders = layouts = 0;
            report_tick = now;
        }
    }
}
