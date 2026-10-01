/* GUI snapshot producer: no drawing, IPC waits or GUI locks on the audio path. */
#include "../common/frontproto.h"
static LONG front_buttons, front_heartbeat;
static bool front_enabled;
class FrontBridge : public LCD_Backend
{
    HANDLE map = nullptr;
    VSC_FRONT_STATE *view = nullptr;
    DWORD last = 0;
public:
    ~FrontBridge() override
    {
        if (view)
            UnmapViewOfFile(view);

        if (map)
            CloseHandle(map);
    }
    bool Start(const lcd_t &) override
    {
        map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(VSC_FRONT_STATE), VSC_FRONT_MAP);

        if (!map)
            return false;

        view = (VSC_FRONT_STATE *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VSC_FRONT_STATE));

        if (!view)
            return false;

        memset(view, 0, sizeof(*view));
        view->pid = (LONG)GetCurrentProcessId();
        InterlockedExchange(&view->magic, VSC_FRONT_MAGIC);
        return true;
    }
    void Stop() override {}
    void Render() override {} // LCD rasterization is done in the GUI process.
    void Update(Emulator &emu)
    {
        DWORD now = GetTickCount();
        bool observing = (DWORD)(now - (DWORD)InterlockedCompareExchange(&front_heartbeat, 0, 0)) < 1500;
        LONG buttons = observing ? InterlockedCompareExchange(&front_buttons, 0, 0) : 0;
        emu.GetMCU().button_pressed.store((unsigned)buttons);

        if (!view || !observing || (DWORD)(now - last) < 50)
            return;

        last = now;
        const auto &lcd = emu.GetLCD(); // same thread as Step/LCD_Write; no race.
        InterlockedIncrement(&view->sequence);
        view->enabled = lcd.enable.load();
        view->cursor = lcd.LCD_C;
        view->address = lcd.LCD_DD_RAM;
        memcpy(view->data, lcd.LCD_Data, sizeof(view->data));
        memcpy(view->cg, lcd.LCD_CG, sizeof(view->cg));
        view->buttons = buttons;
        ++view->frames;
        unsigned port = emu.GetMCU().is_mk1 ? emu.GetMCU().io_sd : emu.GetMCU().p0_data;
        view->leds = ((port & 0x40) ? 0 : 1) | ((port & 0x20) ? 0 : 2) | ((port & 0x10) ? 0 : 4);
        InterlockedIncrement(&view->sequence);
    }
};
