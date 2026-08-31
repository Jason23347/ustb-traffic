#pragma once

#include <windows.h>

namespace ustb {

bool text_render_init();
void text_render_shutdown();
void text_render_set_dpi(int dpi);
int text_render_width(const wchar_t* text, bool tabular = false);
int text_render_line_height();
bool text_render_begin(HDC hdc, const RECT& bounds);
void text_render_draw(const wchar_t* text, const RECT& rc, COLORREF fg, bool center,
                      bool tabular = false);
bool text_render_end();

}  // namespace ustb
