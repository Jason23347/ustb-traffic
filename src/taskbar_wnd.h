#pragma once

#include <windows.h>

namespace ustb {

constexpr UINT WM_USTB_RESHOW = WM_APP + 1;

bool create_taskbar_window(HINSTANCE instance);
void destroy_taskbar_window();
void reembed_taskbar_window();
void pause_taskbar_updates(bool pause);
void invalidate_taskbar_layout();
int measure_taskbar_width(HDC hdc, HFONT font);
HWND taskbar_owner_hwnd();

}  // namespace ustb
