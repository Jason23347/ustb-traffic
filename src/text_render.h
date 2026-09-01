#pragma once

#include <windows.h>

namespace ustb {

bool text_render_init();
void text_render_shutdown();
void text_render_set_dpi(int dpi);
// Re-read shell UI font (same source as taskbar clock/date) after
// WM_SETTINGCHANGE / theme changes.
void text_render_reload_system_font();
// taskbar_font_dip: 0 = auto (match system tray clock), else fixed DIP size.
void text_render_set_font_dip(unsigned dip);
int text_render_width(const wchar_t* text, bool tabular = false);
int text_render_line_height();
bool text_render_begin(HDC hdc, const RECT& bounds);
void text_render_fill_rounded_rect(const RECT& rc, float radius_px, COLORREF rgb,
                                   float alpha);
void text_render_draw_rounded_rect(const RECT& rc, float radius_px, COLORREF rgb,
                                   float alpha, float stroke_px);
void text_render_draw(const wchar_t* text, const RECT& rc, COLORREF fg, bool center,
                      bool tabular = false);
bool text_render_end();

}  // namespace ustb
