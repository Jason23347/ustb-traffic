#pragma once

#include <windows.h>

namespace ustb {

constexpr UINT IDM_USAGE_ABS = 1001;
constexpr UINT IDM_USAGE_OVER = 1002;
constexpr UINT IDM_USAGE_PCT = 1003;
constexpr UINT IDM_OPTIONS = 1004;
constexpr UINT IDM_AUTOSTART = 1005;
constexpr UINT IDM_EXIT = 1006;
constexpr UINT IDM_SIDE_RIGHT = 1007;
constexpr UINT IDM_SIDE_LEFT = 1008;
constexpr UINT IDM_ABOUT = 1009;

void show_context_menu(HWND hwnd, POINT screen);
void show_options_dialog(HWND parent);
void show_about_dialog(HWND parent);

}  // namespace ustb
