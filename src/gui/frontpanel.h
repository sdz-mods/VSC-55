#ifndef VSC_FRONTPANEL_H
#define VSC_FRONTPANEL_H
HWND front_create(HINSTANCE instance, HWND controller, HICON icon, HICON small, const char *directory, const char *ini);
void front_show(void);
void front_destroy(void);
void front_update(int busy, int running, int volume, const char *model, const char *message);
#endif
