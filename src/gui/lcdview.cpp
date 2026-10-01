/* Reuse the pinned GPL core's LCD rasterizer in the GUI, not the audio thread. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "emu.h"
#include "../common/frontproto.h"
#include <cstring>
#include <vector>
#include <algorithm>
namespace
{
struct NullDisplay: LCD_Backend
{
    bool Start(const lcd_t &)override
    {
        return true;
    } void Stop()override {} void Render()override {}
};
NullDisplay display;
lcd_t lcd;
mcu_t mcu;
unsigned pixels[741 * 268];
std::vector<unsigned> scaled;
int scaled_w, scaled_h;
bool initialized;
// GUI-Float's SC-55 contrast curve (GPL-2.0-or-later), display only.
unsigned mix(unsigned c, unsigned factor)
{
    return ((((c >> 16) & 255) * factor >> 8) << 16) | ((((c >> 8) & 255) * factor >> 8) << 8) | ((c & 255) * factor >> 8);
}
}
extern "C" const unsigned *vsc_lcd_render(const VSC_FRONT_STATE *frame, int contrast)
{
    if (!initialized)
    {
        lcd.mcu = &mcu;
        lcd.backend = &display;
        LCD_Start(lcd);
        lcd.color1 = 0x000000;
        lcd.color2 = 0x0050c8;
        initialized = true;
    }

    lcd.enable = frame->enabled != 0;
    lcd.LCD_C = frame->cursor;
    lcd.LCD_DD_RAM = frame->address;
    memcpy(lcd.LCD_Data, frame->data, 80);
    memcpy(lcd.LCD_CG, frame->cg, 64);
    contrast = std::max(1, std::min(16, contrast));
    unsigned con = 17 * (contrast - 1);
    con = (con * con) >> 8;
    lcd.color2 = mix(0x0f6fff, 255 - (con / 4 + 4));
    lcd.color1 = mix(lcd.color2, 17 * (16 - (((contrast + 1) >> 1) + 4)));
    LCD_Render(lcd);

    for (unsigned y = 0; y < 268; y++)
        for (unsigned x = 0; x < 741; x++)
        {
            unsigned c = lcd.buffer[y][x];
            pixels[y * 741 + x] = ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
        }

    scaled_w = scaled_h = 0;
    return pixels;
}
// Win9x GDI HALFTONE is not a reliable downsampling filter. Average coverage
// explicitly, then blit 1:1. Keep the full 741-pixel source stride; the last
// background column is cropped to the faceplate's 740:268 display aperture.
extern "C" const unsigned *vsc_lcd_scaled(int w, int h)
{
    if (w <= 0 || h <= 0)
        return nullptr;

    if (w == scaled_w && h == scaled_h)
        return scaled.data();

    scaled.resize(size_t(w)*h);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int left = x * 740, right = (x + 1) * 740, top = y * 268, bottom = (y + 1) * 268;
            unsigned r = 0, g = 0, b = 0;

            for (int sy = top / h; sy <= (bottom - 1) / h; sy++)
            {
                unsigned wy = std::min(bottom, (sy + 1) * h) - std::max(top, sy * h);

                for (int sx = left / w; sx <= (right - 1) / w; sx++)
                {
                    unsigned weight = wy * (std::min(right, (sx + 1) * w) - std::max(left, sx * w));
                    unsigned c = pixels[sy * 741 + sx];
                    r += ((c >> 16) & 255) * weight;
                    g += ((c >> 8) & 255) * weight;
                    b += (c & 255) * weight;
                }
            }

            const unsigned area = 740 * 268;
            scaled[y * w + x] = (((r + area / 2) / area) << 16) | (((g + area / 2) / area) << 8) | ((b + area / 2) / area);
        }

    scaled_w = w;
    scaled_h = h;
    return scaled.data();
}
