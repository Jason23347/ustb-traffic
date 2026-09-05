#include "taskbar_wnd.h"

#include "app.h"
#include "autostart.h"
#include "format.h"
#include "menu.h"
#include "poller.h"
#include "text_render.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ustb {
namespace {

constexpr wchar_t kHostClass[] = L"UstbTrafficHost";
constexpr wchar_t kWndClass[] = L"UstbTrafficTaskbar";
constexpr wchar_t kFlyoutClass[] = L"UstbTrafficFlyout";
constexpr UINT kPresentTimer = 1;
constexpr UINT kEmbedTimer = 2;
constexpr UINT kFlyoutTimer = 3;
constexpr UINT kFlyoutAnimTimer = 4;
constexpr UINT kHoverBgAnimTimer = 5;
// PopInThemeAnimation uses ControlFastAnimationDuration (167ms): opacity +
// vertical translation (Fluent timing-and-easing).
constexpr UINT kFlyoutFadeMs = 167;
constexpr UINT kFlyoutAnimIntervalMs = 16;
// Typical tooltip PopIn FromVerticalOffset (~40–50 DIP guidance).
constexpr int kFlyoutPopOffsetDip = 40;
// Win11 taskbar OmniButton uses Border.BackgroundTransition / BrushTransition
// at ControlFasterAnimationDuration (83ms) for PointerOver fill crossfade.
constexpr UINT kHoverBgFadeMs = 83;
// Win11 ThemeShadow tooltip recipe (Z=16 → Elevation=8):
// directional only (ambient=0 at low elevation), blur=Elevation, Y=Elevation*0.5.
// Windowed tooltip insets L,T,R,B = 4,1,4,8. CornerRadius=4.
constexpr int kFlyoutShadowElevDip = 8;
constexpr int kFlyoutShadowInsetL = 4;
constexpr int kFlyoutShadowInsetT = 1;
constexpr int kFlyoutShadowInsetR = 4;
constexpr int kFlyoutShadowInsetB = 8;
constexpr int kFlyoutCornerRadiusDip = 4;

enum class FlyoutVis : BYTE { Hidden, FadingIn, Shown, FadingOut };

struct FlyoutSurface {
  HDC mem = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ old_bmp = nullptr;
  int width = 0;
  int height = 0;
  POINT pos{};
};

HWND g_host = nullptr;
HWND g_hwnd = nullptr;
HWND g_flyout = nullptr;
HWND g_tray_owner = nullptr;
int g_dpi = 96;
COLORREF g_fg = RGB(255, 255, 255);
bool g_light_theme = false;
bool g_pause_updates = false;
bool g_user_exit = false;
bool g_hovered = false;
bool g_tracking_mouse = false;
bool g_flyout_pending = false;
FlyoutVis g_flyout_vis = FlyoutVis::Hidden;
float g_flyout_opacity = 0.0f;
float g_flyout_anim_from = 0.0f;
float g_flyout_anim_to = 0.0f;
ULONGLONG g_flyout_anim_start = 0;
FlyoutSurface g_flyout_surf{};
bool g_flyout_above = true;
// 0..1 multiplier on OmniButton PointerOver fill (BrushTransition progress).
float g_hover_bg = 0.0f;
float g_hover_bg_from = 0.0f;
float g_hover_bg_to = 0.0f;
ULONGLONG g_hover_bg_start = 0;
bool g_hover_bg_animating = false;
int g_last_x = INT_MIN;
int g_last_y = INT_MIN;
int g_last_w = 0;
int g_last_h = 0;

bool create_display_window(HINSTANCE instance, bool wait_for_tray);
void attach_tray_owner();
bool present();
bool popup_menu_open();
void hide_flyout();
void dismiss_flyout();
void update_flyout();
void cancel_flyout_timer();
void cancel_flyout_anim();
void schedule_flyout();
void set_hovered(bool hovered);
void free_flyout_surface();
bool present_flyout_surface();
void begin_flyout_fade(float target);
void tick_flyout_anim();
void cancel_hover_bg_anim();
void begin_hover_bg_fade(float target);
void tick_hover_bg_anim();
void snap_hover_bg(float value);

UINT flyout_delay_ms() {
  UINT ms = 400;
  if (!SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &ms, 0) || ms == 0) {
    ms = 400;
  }
  if (ms < 200) {
    ms = 200;
  } else if (ms > 2000) {
    ms = 2000;
  }
  return ms;
}

HWND find_tray() { return FindWindowW(L"Shell_TrayWnd", nullptr); }

HWND find_notify(HWND tray) {
  return FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
}

bool valid_rect(HWND hwnd, RECT* rc) {
  if (!hwnd || !GetWindowRect(hwnd, rc)) {
    return false;
  }
  return rc->right > rc->left && rc->bottom > rc->top;
}

bool system_uses_light_theme() {
  DWORD val = 1;
  DWORD size = sizeof(val);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                   L"Personalize",
                   L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &val,
                   &size) == ERROR_SUCCESS) {
    return val != 0;
  }
  return true;
}

void refresh_colors() {
  g_light_theme = system_uses_light_theme();
  g_fg = g_light_theme ? RGB(16, 16, 16) : RGB(255, 255, 255);
}

COLORREF color_for_level(MeterLevel level) {
  switch (level) {
    case MeterLevel::Warn:
      return g_light_theme ? RGB(184, 124, 0) : RGB(255, 208, 0);
    case MeterLevel::Danger:
      return g_light_theme ? RGB(196, 32, 32) : RGB(255, 72, 72);
    case MeterLevel::Normal:
    default:
      return g_fg;
  }
}

bool is_system_theme_change(UINT msg, LPARAM lp) {
  if (msg == WM_THEMECHANGED) {
    return true;
  }
  if (msg != WM_SETTINGCHANGE || lp == 0) {
    return false;
  }
  return _wcsicmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") ==
         0;
}

// Font metrics / shell chrome changes (accessibility text size, etc.).
bool is_shell_font_change(UINT msg, LPARAM lp) {
  if (msg == WM_THEMECHANGED) {
    return true;
  }
  if (msg != WM_SETTINGCHANGE) {
    return false;
  }
  if (lp == 0) {
    return true;
  }
  const auto* name = reinterpret_cast<const wchar_t*>(lp);
  return _wcsicmp(name, L"ImmersiveColorSet") == 0 ||
         _wcsicmp(name, L"WindowsThemeElement") == 0;
}

void apply_system_theme() {
  refresh_colors();
  text_render_reload_system_font();
  g_last_x = INT_MIN;
  present();
}

void apply_taskbar_font_from_config() {
  unsigned font_dip = kTaskbarFontDipAuto;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    font_dip = app().config.taskbar_font_dip;
  }
  text_render_set_font_dip(font_dip);
}

void apply_shell_font() {
  text_render_reload_system_font();
  g_last_x = INT_MIN;
  present();
}

int text_width(const wchar_t* text, bool tabular) {
  return text_render_width(text, tabular);
}

struct ColMetrics {
  int pad = 0;
  int label_w = 0;
  int gap = 0;
  int value_w = 0;
  int total() const { return pad + label_w + gap + value_w + pad; }
};

ColMetrics column_metrics() {
  static const wchar_t* kLabels[] = {L"Σ", L"▲", L"↓", L"●"};
  unsigned pad_px = kDefaultTaskbarPadPx;
  unsigned gap_px = kDefaultTaskbarGapPx;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    pad_px = app().config.taskbar_pad_px;
    gap_px = app().config.taskbar_gap_px;
  }
  ColMetrics m;
  m.pad = MulDiv(static_cast<int>(pad_px), g_dpi, 96);
  m.gap = MulDiv(static_cast<int>(gap_px), g_dpi, 96);
  for (const wchar_t* s : kLabels) {
    m.label_w = (std::max)(m.label_w, text_width(s, false));
  }
  each_taskbar_value_width_sample([&](const wchar_t* s) {
    m.value_w = (std::max)(m.value_w, text_width(s, true));
  });
  m.value_w += MulDiv(static_cast<int>(kTaskbarValueWidthSlopPx), g_dpi, 96);
  return m;
}

int compute_width() { return column_metrics().total(); }

std::vector<std::wstring> split_lines(const std::wstring& text) {
  std::vector<std::wstring> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t pos = text.find(L'\n', start);
    if (pos == std::wstring::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

float ease_out_cubic(float t) {
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float ease_in_cubic(float t) { return t * t * t; }

BYTE flyout_alpha_byte() {
  const float o = (std::max)(0.0f, (std::min)(1.0f, g_flyout_opacity));
  return static_cast<BYTE>(std::lround(o * 255.0f));
}

void free_flyout_surface() {
  if (g_flyout_surf.mem && g_flyout_surf.old_bmp) {
    SelectObject(g_flyout_surf.mem, g_flyout_surf.old_bmp);
  }
  if (g_flyout_surf.dib) {
    DeleteObject(g_flyout_surf.dib);
  }
  if (g_flyout_surf.mem) {
    DeleteDC(g_flyout_surf.mem);
  }
  g_flyout_surf = {};
}

// Must re-submit hdcSrc each frame: UpdateLayeredWindow(nullptr hdcSrc) does
// not reliably change SourceConstantAlpha on layered per-pixel surfaces.
bool present_flyout_surface() {
  if (!g_flyout || !IsWindow(g_flyout) || !g_flyout_surf.mem ||
      g_flyout_surf.width <= 0 || g_flyout_surf.height <= 0) {
    return false;
  }
  // PopIn: opacity + vertical slide. Dismiss: fade in place (no translation).
  float slide = 0.0f;
  if (g_flyout_vis == FlyoutVis::FadingIn) {
    slide = 1.0f - (std::max)(0.0f, (std::min)(1.0f, g_flyout_opacity));
  }
  const int pop_px = MulDiv(kFlyoutPopOffsetDip, g_dpi, 96);
  POINT pos = g_flyout_surf.pos;
  const int dy =
      static_cast<int>(std::lround(static_cast<float>(pop_px) * slide));
  // Tip above trigger slides up into place (starts lower / toward the bar).
  pos.y += g_flyout_above ? dy : -dy;

  POINT pt_src{0, 0};
  SIZE size{g_flyout_surf.width, g_flyout_surf.height};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = flyout_alpha_byte();
  blend.AlphaFormat = AC_SRC_ALPHA;
  HDC screen = GetDC(nullptr);
  const BOOL ok =
      UpdateLayeredWindow(g_flyout, screen, &pos, &size, g_flyout_surf.mem,
                          &pt_src, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  if (ok) {
    SetWindowPos(g_flyout, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  return ok != FALSE;
}

void cancel_flyout_anim() {
  if (g_hwnd) {
    KillTimer(g_hwnd, kFlyoutAnimTimer);
  }
}

void hide_flyout() {
  cancel_flyout_anim();
  g_flyout_opacity = 0.0f;
  g_flyout_vis = FlyoutVis::Hidden;
  free_flyout_surface();
  if (g_flyout && IsWindowVisible(g_flyout)) {
    ShowWindow(g_flyout, SW_HIDE);
  }
}

void begin_flyout_fade(float target) {
  target = target <= 0.0f ? 0.0f : 1.0f;
  if (std::fabs(g_flyout_opacity - target) < 0.001f) {
    cancel_flyout_anim();
    g_flyout_opacity = target;
    g_flyout_vis = target <= 0.0f ? FlyoutVis::Hidden : FlyoutVis::Shown;
    if (target <= 0.0f && g_flyout) {
      ShowWindow(g_flyout, SW_HIDE);
      free_flyout_surface();
    } else {
      present_flyout_surface();
    }
    return;
  }
  g_flyout_anim_from = g_flyout_opacity;
  g_flyout_anim_to = target;
  g_flyout_anim_start = GetTickCount64();
  g_flyout_vis = target > g_flyout_anim_from ? FlyoutVis::FadingIn
                                             : FlyoutVis::FadingOut;
  if (g_hwnd) {
    SetTimer(g_hwnd, kFlyoutAnimTimer, kFlyoutAnimIntervalMs, nullptr);
  }
  tick_flyout_anim();
}

void tick_flyout_anim() {
  if (g_flyout_vis != FlyoutVis::FadingIn &&
      g_flyout_vis != FlyoutVis::FadingOut) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  float t = static_cast<float>(now - g_flyout_anim_start) /
            static_cast<float>(kFlyoutFadeMs);
  if (t >= 1.0f) {
    t = 1.0f;
  }
  const bool fading_in = g_flyout_anim_to > g_flyout_anim_from;
  const float eased = fading_in ? ease_out_cubic(t) : ease_in_cubic(t);
  g_flyout_opacity =
      g_flyout_anim_from + (g_flyout_anim_to - g_flyout_anim_from) * eased;
  present_flyout_surface();
  if (t < 1.0f) {
    return;
  }
  const float target = g_flyout_anim_to;
  cancel_flyout_anim();
  g_flyout_opacity = target;
  if (g_flyout_opacity <= 0.0f) {
    g_flyout_vis = FlyoutVis::Hidden;
    g_flyout_opacity = 0.0f;
    free_flyout_surface();
    if (g_flyout) {
      ShowWindow(g_flyout, SW_HIDE);
    }
  } else {
    g_flyout_vis = FlyoutVis::Shown;
    g_flyout_opacity = 1.0f;
    present_flyout_surface();
  }
}

void dismiss_flyout() {
  if (g_flyout_vis == FlyoutVis::Hidden &&
      !(g_flyout && IsWindowVisible(g_flyout))) {
    return;
  }
  begin_flyout_fade(0.0f);
}

// Win11 ThemeShadow @ tooltip elevation: directional key shadow only.
// blur = Elevation, Y offset = Elevation * 0.5, X = 0; ambient opacity = 0.
// Light ~14% / Dark ~28% (Fluent low-elevation directional opacity).
void paint_flyout_shadow(const RECT& content, float radius_px) {
  const int blur = MulDiv(kFlyoutShadowElevDip, g_dpi, 96);
  const int y_off =
      MulDiv(kFlyoutShadowElevDip, g_dpi, 96) / 2;  // Elevation * 0.5
  const float strength = g_light_theme ? 0.14f : 0.28f;
  constexpr int kSteps = 10;
  for (int i = 1; i <= kSteps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSteps);
    const int expand =
        static_cast<int>(std::lround(static_cast<float>(blur) * t));
    const RECT rc{content.left - expand, content.top - expand + y_off,
                  content.right + expand, content.bottom + expand + y_off};
    const float falloff = 1.0f - t;
    const float alpha =
        strength * falloff * falloff / static_cast<float>(kSteps);
    text_render_fill_rounded_rect(
        rc, radius_px + static_cast<float>(expand), RGB(0, 0, 0), alpha);
  }
}

bool paint_flyout(HWND hwnd) {
  if (!hwnd || !text_render_init()) {
    return false;
  }
  const std::wstring tip =
      format_tooltip(app().copy_snap(), app().quota_kb());
  const auto lines = split_lines(tip);
  const int pad = MulDiv(14, g_dpi, 96);
  const int line_h = (std::max)(1, text_render_line_height());
  const int line_gap = MulDiv(2, g_dpi, 96);
  int text_w = 0;
  for (const auto& line : lines) {
    text_w = (std::max)(text_w, text_render_width(line.c_str(), false));
  }
  const int content_w = text_w + pad * 2;
  const int content_h =
      pad * 2 + static_cast<int>(lines.size()) * line_h +
      (std::max)(0, static_cast<int>(lines.size()) - 1) * line_gap;
  if (content_w < 8 || content_h < 8) {
    return false;
  }

  const int inset_l = MulDiv(kFlyoutShadowInsetL, g_dpi, 96);
  const int inset_t = MulDiv(kFlyoutShadowInsetT, g_dpi, 96);
  const int inset_r = MulDiv(kFlyoutShadowInsetR, g_dpi, 96);
  const int inset_b = MulDiv(kFlyoutShadowInsetB, g_dpi, 96);
  const int width = content_w + inset_l + inset_r;
  const int height = content_h + inset_t + inset_b;

  HDC screen = GetDC(nullptr);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib =
      CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    return false;
  }
  HGDIOBJ old_bmp = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) *
                           4);

  const RECT bounds{0, 0, width, height};
  const RECT content{inset_l, inset_t, inset_l + content_w,
                     inset_t + content_h};
  const float radius =
      static_cast<float>(MulDiv(kFlyoutCornerRadiusDip, g_dpi, 96));
  const COLORREF bg =
      g_light_theme ? RGB(252, 252, 252) : RGB(32, 32, 32);
  const COLORREF border =
      g_light_theme ? RGB(0, 0, 0) : RGB(255, 255, 255);
  const COLORREF fg = g_light_theme ? RGB(26, 26, 26) : RGB(255, 255, 255);
  if (text_render_begin(mem, bounds)) {
    paint_flyout_shadow(content, radius);
    // Fully opaque like the Win11 clock tip (not acrylic/translucent).
    text_render_fill_rounded_rect(content, radius, bg, 1.0f);
    text_render_draw_rounded_rect(content, radius, border, 0.10f,
                                  static_cast<float>(MulDiv(1, g_dpi, 96)));
    int y = content.top + pad;
    for (const auto& line : lines) {
      RECT rc{content.left + pad, y, content.right - pad, y + line_h};
      text_render_draw(line.c_str(), rc, fg, false, false);
      y += line_h + line_gap;
    }
    text_render_end();
  }

  RECT anchor{};
  if (!g_hwnd || !GetWindowRect(g_hwnd, &anchor)) {
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
    return false;
  }
  const int gap = MulDiv(8, g_dpi, 96);
  int screen_x =
      anchor.left + (anchor.right - anchor.left - content_w) / 2 - inset_l;
  int screen_y = anchor.top - gap - content_h - inset_t;
  bool above = true;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST),
                      &mi)) {
    const int margin = MulDiv(8, g_dpi, 96);
    if (screen_x + inset_l < mi.rcWork.left + margin) {
      screen_x = mi.rcWork.left + margin - inset_l;
    }
    if (screen_x + inset_l + content_w > mi.rcWork.right - margin) {
      screen_x = mi.rcWork.right - margin - content_w - inset_l;
    }
    if (screen_y + inset_t < mi.rcWork.top + margin) {
      screen_y = anchor.bottom + gap - inset_t;
      above = false;
    }
  }
  g_flyout_above = above;

  free_flyout_surface();
  g_flyout_surf.mem = mem;
  g_flyout_surf.dib = dib;
  g_flyout_surf.old_bmp = old_bmp;
  g_flyout_surf.width = width;
  g_flyout_surf.height = height;
  g_flyout_surf.pos = {screen_x, screen_y};
  return present_flyout_surface();
}

LRESULT CALLBACK flyout_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_flyout(HINSTANCE instance) {
  if (g_flyout && IsWindow(g_flyout)) {
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = flyout_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kFlyoutClass;
  RegisterClassExW(&wc);
  g_flyout = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
      kFlyoutClass, L"", WS_POPUP, 0, 0, 0, 0, g_hwnd, nullptr, instance,
      nullptr);
}

void update_flyout() {
  if (!g_hovered || g_flyout_pending || !g_hwnd || g_pause_updates ||
      popup_menu_open()) {
    hide_flyout();
    return;
  }
  ensure_flyout(app().instance);
  if (!g_flyout) {
    return;
  }
  const bool was_hidden = g_flyout_vis == FlyoutVis::Hidden ||
                          !IsWindowVisible(g_flyout);
  const bool was_fading_out = g_flyout_vis == FlyoutVis::FadingOut;
  if (was_hidden) {
    g_flyout_opacity = 0.0f;
  }
  if (!paint_flyout(g_flyout)) {
    return;
  }
  ShowWindow(g_flyout, SW_SHOWNOACTIVATE);
  if (was_hidden || was_fading_out) {
    begin_flyout_fade(1.0f);
  }
}

void cancel_flyout_timer() {
  g_flyout_pending = false;
  if (g_hwnd) {
    KillTimer(g_hwnd, kFlyoutTimer);
  }
}

void schedule_flyout() {
  if (!g_hwnd) {
    return;
  }
  cancel_flyout_timer();
  g_flyout_pending = true;
  hide_flyout();
  SetTimer(g_hwnd, kFlyoutTimer, flyout_delay_ms(), nullptr);
}

void cancel_hover_bg_anim() {
  g_hover_bg_animating = false;
  if (g_hwnd) {
    KillTimer(g_hwnd, kHoverBgAnimTimer);
  }
}

void snap_hover_bg(float value) {
  cancel_hover_bg_anim();
  g_hover_bg = value <= 0.0f ? 0.0f : 1.0f;
  g_hover_bg_from = g_hover_bg;
  g_hover_bg_to = g_hover_bg;
}

void tick_hover_bg_anim() {
  if (!g_hover_bg_animating) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  float t = static_cast<float>(now - g_hover_bg_start) /
            static_cast<float>(kHoverBgFadeMs);
  if (t >= 1.0f) {
    t = 1.0f;
  }
  // BrushTransition interpolates brushes linearly over its Duration.
  g_hover_bg = g_hover_bg_from + (g_hover_bg_to - g_hover_bg_from) * t;
  present();
  if (t < 1.0f) {
    return;
  }
  const float target = g_hover_bg_to;
  cancel_hover_bg_anim();
  g_hover_bg = target;
  present();
}

void begin_hover_bg_fade(float target) {
  target = target <= 0.0f ? 0.0f : 1.0f;
  if (std::fabs(g_hover_bg - target) < 0.001f) {
    snap_hover_bg(target);
    present();
    return;
  }
  g_hover_bg_from = g_hover_bg;
  g_hover_bg_to = target;
  g_hover_bg_start = GetTickCount64();
  g_hover_bg_animating = true;
  if (g_hwnd) {
    SetTimer(g_hwnd, kHoverBgAnimTimer, kFlyoutAnimIntervalMs, nullptr);
  }
  tick_hover_bg_anim();
}

void set_hovered(bool hovered) {
  if (g_hovered == hovered) {
    return;
  }
  g_hovered = hovered;
  begin_hover_bg_fade(hovered ? 1.0f : 0.0f);
  if (!hovered) {
    cancel_flyout_timer();
    dismiss_flyout();
  } else {
    schedule_flyout();
  }
}

void ensure_hit_testable(BYTE* bits, int width, int height) {
  const int count = width * height;
  BYTE* p = bits;
  for (int i = 0; i < count; ++i, p += 4) {
    if (p[3] == 0) {
      p[3] = 1;
    }
  }
}

void update_hit_region(int width, int height) {
  if (!g_hwnd) {
    return;
  }
  HRGN rgn = CreateRectRgn(0, 0, width, height);
  if (rgn) {
    SetWindowRgn(g_hwnd, rgn, TRUE);
  }
}

void request_exit() {
  if (g_user_exit) {
    return;
  }
  g_user_exit = true;
  if (g_host) {
    DestroyWindow(g_host);
    return;
  }
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
  }
}

void attach_tray_owner() {
  if (!g_hwnd) {
    g_tray_owner = nullptr;
    return;
  }
  HWND tray = find_tray();
  if (!tray) {
    return;
  }
  if (GetWindow(g_hwnd, GW_OWNER) != tray) {
    SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT,
                      reinterpret_cast<LONG_PTR>(tray));
  }
  g_tray_owner = GetWindow(g_hwnd, GW_OWNER);
}

bool class_is_shell(HWND hwnd) {
  wchar_t cls[64]{};
  if (!GetClassNameW(hwnd, cls, 64)) {
    return false;
  }
  return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

bool window_covers_monitor(HWND hwnd) {
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) {
    return false;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    return false;
  }
  const RECT& m = mi.rcMonitor;
  return wr.left <= m.left && wr.top <= m.top && wr.right >= m.right &&
         wr.bottom >= m.bottom;
}

bool fullscreen_app_active() {
  QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
  if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
    if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY ||
        state == QUNS_PRESENTATION_MODE) {
      return true;
    }
  }
  HWND fg = GetForegroundWindow();
  if (!fg || fg == g_hwnd || fg == g_host) {
    return false;
  }
  if (class_is_shell(fg)) {
    return false;
  }
  return window_covers_monitor(fg);
}

void hide_for_fullscreen() {
  if (g_hwnd && IsWindowVisible(g_hwnd)) {
    ShowWindow(g_hwnd, SW_HIDE);
  }
  cancel_flyout_timer();
  hide_flyout();
  snap_hover_bg(0.0f);
  g_hovered = false;
  g_tracking_mouse = false;
}

// Menus from any tray icon live in windows of the standard #32768 class. We
// can't get notified about other apps' menus the way g_pause_updates covers
// ours, so poll for one before touching the z-order.
bool popup_menu_open() {
  HWND menu = nullptr;
  while ((menu = FindWindowExW(nullptr, menu, L"#32768", nullptr)) != nullptr) {
    if (IsWindowVisible(menu)) {
      return true;
    }
  }
  return false;
}

bool present() {
  if (!g_hwnd || !text_render_init() || g_pause_updates || popup_menu_open()) {
    return false;
  }
  if (fullscreen_app_active()) {
    hide_for_fullscreen();
    return false;
  }
  if (!IsWindowVisible(g_hwnd)) {
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  }

  HWND tray = find_tray();
  RECT rc_tray{};
  if (!tray || !GetWindowRect(tray, &rc_tray)) {
    return false;
  }
  const int height = rc_tray.bottom - rc_tray.top;
  if (height < 8) {
    return false;
  }

  attach_tray_owner();

  HDC screen = GetDC(nullptr);
  const int width = compute_width();
  const int gap = MulDiv(6, g_dpi, 96);

  TaskbarSide side = TaskbarSide::Right;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    side = app().config.taskbar_side;
  }

  const int screen_y = rc_tray.top;
  int screen_x = 0;
  if (side == TaskbarSide::Left) {
    screen_x = rc_tray.left + gap;
  } else {
    HWND notify = find_notify(tray);
    RECT rc_notify{};
    if (valid_rect(notify, &rc_notify) && rc_notify.left != 0) {
      screen_x = rc_notify.left - gap - width;
    } else {
      screen_x = rc_tray.right - MulDiv(180, g_dpi, 96) - width;
    }
  }
  const int min_x = rc_tray.left + gap;
  const int max_x = rc_tray.right - gap - width;
  if (screen_x < min_x) {
    screen_x = min_x;
  } else if (max_x >= min_x && screen_x > max_x) {
    screen_x = max_x;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib =
      CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return false;
  }

  HGDIOBJ old_bmp = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) *
                           4);

  DisplaySnapshot snap;
  UsageMode mode;
  uint64_t quota = 0;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    snap = app().snap;
    mode = app().config.usage_mode;
    quota = quota_to_kb(app().config.quota_gb);
  }
  const auto cells = format_taskbar_cells(snap, mode, quota);
  const ColMetrics col = column_metrics();
  const int value_x = col.pad + col.label_w + col.gap;
  const COLORREF usage_fg =
      snap.have_usage ? color_for_level(usage_meter_level(snap.used_kb, quota))
                      : g_fg;
  const COLORREF speed_fg =
      snap.rate_valid
          ? color_for_level(speed_meter_level(snap.display_rate_kbps))
          : g_fg;

  const RECT bounds{0, 0, width, height};
  if (text_render_begin(mem, bounds)) {
    if (g_hover_bg > 0.001f) {
      // Match Win11 SystemTray OmniButton PointerOver:
      // OmniButtonBackgroundPointerOver -> ShellTaskbarItemFillColorSecondary
      // Light: #80FFFFFF, Dark: #0FFFFFFF (TaskbarResources.xbf)
      // CornerRadius: ControlLargeCornerRadius = 4
      // Fill crossfades via BrushTransition (ControlFasterAnimationDuration).
      const int inset_y = MulDiv(4, g_dpi, 96);
      RECT bg{0, inset_y, width, height - inset_y};
      if (bg.right > bg.left && bg.bottom > bg.top) {
        const float radius = static_cast<float>(MulDiv(4, g_dpi, 96));
        const COLORREF fill = RGB(255, 255, 255);
        const float base = g_light_theme ? (0x80 / 255.0f) : (0x0F / 255.0f);
        text_render_fill_rounded_rect(bg, radius, fill, base * g_hover_bg);
      }
    }
    auto draw_row = [&](const std::wstring& label, const std::wstring& value,
                        int y0, int y1, COLORREF fg) {
      RECT lc{col.pad, y0, col.pad + col.label_w, y1};
      RECT vc{value_x, y0, width - col.pad, y1};
      if (!label.empty()) {
        text_render_draw(label.c_str(), lc, fg, true, false);
      }
      text_render_draw(value.c_str(), vc, fg, false, true);
    };
    const int line_h = (std::max)(1, text_render_line_height());
    const int row_gap = MulDiv(kTaskbarRowGapPx, g_dpi, 96);
    const int block = line_h * 2 + row_gap;
    int top_y = (height - block) / 2;
    if (top_y < 0) {
      top_y = 0;
    }
    const int bottom_y = top_y + line_h + row_gap;
    draw_row(cells.top_label, cells.top_value, top_y, top_y + line_h, usage_fg);
    draw_row(cells.bottom_label, cells.bottom_value, bottom_y,
             bottom_y + line_h, speed_fg);
    text_render_end();
  }

  ensure_hit_testable(static_cast<BYTE*>(bits), width, height);

  POINT pt_dst{screen_x, screen_y};
  POINT pt_src{0, 0};
  SIZE size{width, height};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  const BOOL ok =
      UpdateLayeredWindow(g_hwnd, screen, &pt_dst, &size, mem, &pt_src, 0,
                          &blend, ULW_ALPHA);

  // Owned + TOPMOST keeps us above Shell_TrayWnd after other windows take
  // focus. Skip SHOWWINDOW; that used to flash the right-click menu.
  // NOOWNERZORDER is what stops the raise from dragging Shell_TrayWnd up with
  // us, which would put the taskbar over another icon's open menu.
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

  if (ok && (screen_x != g_last_x || screen_y != g_last_y || width != g_last_w ||
             height != g_last_h)) {
    g_last_x = screen_x;
    g_last_y = screen_y;
    g_last_w = width;
    g_last_h = height;
    update_hit_region(width, height);
  }
  if (ok && g_hovered && !g_flyout_pending && g_flyout &&
      IsWindowVisible(g_flyout) && g_flyout_vis == FlyoutVis::Shown) {
    update_flyout();
  }

  SelectObject(mem, old_bmp);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok != FALSE;
}

LRESULT CALLBACK display_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_hwnd = hwnd;
      g_dpi = GetDpiForWindow(hwnd);
      if (g_dpi == 0) {
        g_dpi = 96;
      }
      text_render_init();
      text_render_set_dpi(g_dpi);
      apply_taskbar_font_from_config();
      refresh_colors();
      SetTimer(hwnd, kPresentTimer, 200, nullptr);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_NCHITTEST:
      return HTCLIENT;
    case WM_MOUSEMOVE:
      if (!g_tracking_mouse) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        if (TrackMouseEvent(&tme)) {
          g_tracking_mouse = true;
        }
      }
      set_hovered(true);
      return 0;
    case WM_MOUSELEAVE:
      g_tracking_mouse = false;
      set_hovered(false);
      return 0;
    case WM_TIMER:
      if (wp == kPresentTimer) {
        refresh_colors();
        present();
      } else if (wp == kFlyoutTimer) {
        KillTimer(hwnd, kFlyoutTimer);
        g_flyout_pending = false;
        if (g_hovered) {
          update_flyout();
        }
      } else if (wp == kFlyoutAnimTimer) {
        tick_flyout_anim();
      } else if (wp == kHoverBgAnimTimer) {
        tick_hover_bg_anim();
      }
      return 0;
    case WM_RBUTTONUP: {
      set_hovered(false);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(hwnd, &pt);
      show_context_menu(hwnd, pt);
      return 0;
    }
    case WM_CONTEXTMENU: {
      set_hovered(false);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (pt.x == -1 && pt.y == -1) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        pt.x = rc.left;
        pt.y = rc.bottom;
      }
      show_context_menu(hwnd, pt);
      return 0;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
      if (is_system_theme_change(msg, lp)) {
        apply_system_theme();
        return 0;
      }
      if (is_shell_font_change(msg, lp)) {
        apply_shell_font();
        return 0;
      }
      break;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDM_USAGE_ABS:
        case IDM_USAGE_OVER:
        case IDM_USAGE_PCT:
        case IDM_SIDE_RIGHT:
        case IDM_SIDE_LEFT: {
          Config cfg;
          {
            std::lock_guard<std::mutex> lock(app().mu);
            switch (LOWORD(wp)) {
              case IDM_USAGE_ABS:
                app().config.usage_mode = UsageMode::Absolute;
                break;
              case IDM_USAGE_OVER:
                app().config.usage_mode = UsageMode::OverQuota;
                break;
              case IDM_USAGE_PCT:
                app().config.usage_mode = UsageMode::Percent;
                break;
              case IDM_SIDE_RIGHT:
                app().config.taskbar_side = TaskbarSide::Right;
                break;
              case IDM_SIDE_LEFT:
                app().config.taskbar_side = TaskbarSide::Left;
                break;
              default:
                break;
            }
            cfg = app().config;
          }
          save_config(cfg);
          g_last_x = INT_MIN;
          present();
          return 0;
        }
        case IDM_OPTIONS:
          show_options_dialog(g_host ? g_host : hwnd);
          return 0;
        case IDM_AUTOSTART:
          set_autostart(!is_autostart());
          return 0;
        case IDM_ABOUT:
          show_about_dialog(g_host ? g_host : hwnd);
          return 0;
        case IDM_EXIT:
          PostMessageW(g_host ? g_host : hwnd, WM_CLOSE, 0, 0);
          return 0;
        default:
          break;
      }
      break;
    case WM_DPICHANGED: {
      g_dpi = HIWORD(wp);
      if (g_dpi == 0) {
        g_dpi = 96;
      }
      text_render_set_dpi(g_dpi);
      present();
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, kPresentTimer);
      cancel_flyout_timer();
      cancel_flyout_anim();
      cancel_hover_bg_anim();
      hide_flyout();
      if (g_flyout) {
        DestroyWindow(g_flyout);
        g_flyout = nullptr;
      }
      text_render_shutdown();
      g_hwnd = nullptr;
      g_hovered = false;
      g_hover_bg = 0.0f;
      g_tracking_mouse = false;
      g_tray_owner = nullptr;
      g_last_x = INT_MIN;
      app().hwnd = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (app().taskbar_created_msg != 0 && msg == app().taskbar_created_msg) {
    reembed_taskbar_window();
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, kEmbedTimer, 1000, nullptr);
      return 0;
    case WM_TIMER:
      if (wp == kEmbedTimer) {
        reembed_taskbar_window();
      }
      return 0;
    case WM_USTB_RESHOW:
      if (g_hwnd && IsWindow(g_hwnd)) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        g_last_x = INT_MIN;
        present();
      } else {
        reembed_taskbar_window();
      }
      return 0;
    case WM_CLOSE:
      request_exit();
      return 0;
    case WM_DISPLAYCHANGE:
      g_last_x = INT_MIN;
      if (g_hwnd) {
        present();
      } else {
        reembed_taskbar_window();
      }
      return 0;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
      if (is_system_theme_change(msg, lp)) {
        apply_system_theme();
        return 0;
      }
      if (is_shell_font_change(msg, lp)) {
        apply_shell_font();
        return 0;
      }
      break;
    case WM_DESTROY:
      KillTimer(hwnd, kEmbedTimer);
      g_host = nullptr;
      if (!g_user_exit) {
        g_user_exit = true;
      }
      if (g_hwnd) {
        DestroyWindow(g_hwnd);
      }
      stop_poller();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool create_display_window(HINSTANCE instance, bool wait_for_tray) {
  if (g_hwnd && IsWindow(g_hwnd)) {
    attach_tray_owner();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    present();
    return true;
  }

  HWND tray = find_tray();
  if (wait_for_tray) {
    for (int i = 0; i < 20 && !tray; ++i) {
      Sleep(200);
      tray = find_tray();
    }
  } else if (!tray) {
    return false;
  }

  const DWORD ex =
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
  // Create as a real top-level layered window (this is what actually paints
  // on Win11). Then set the taskbar as owner so we stay above it. Do NOT
  // SetParent — Win11's XAML island hides taskbar children.
  g_hwnd = CreateWindowExW(ex, kWndClass, L"UstbTraffic", WS_POPUP, 0, 0, 96, 48,
                           nullptr, nullptr, instance, nullptr);
  if (!g_hwnd) {
    return false;
  }
  app().hwnd = g_hwnd;
  attach_tray_owner();
  present();
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  return true;
}

}  // namespace

HWND taskbar_owner_hwnd() { return g_host; }

int measure_taskbar_width(HDC, HFONT) { return compute_width(); }

bool create_taskbar_window(HINSTANCE instance) {
  WNDCLASSEXW host_wc{};
  host_wc.cbSize = sizeof(host_wc);
  host_wc.lpfnWndProc = host_proc;
  host_wc.hInstance = instance;
  host_wc.lpszClassName = kHostClass;
  RegisterClassExW(&host_wc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = display_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kWndClass;
  RegisterClassExW(&wc);

  g_host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass,
                           L"UstbTraffic", WS_POPUP, 0, 0, 0, 0, nullptr,
                           nullptr, instance, nullptr);
  if (!g_host) {
    return false;
  }
  return create_display_window(instance, true);
}

void destroy_taskbar_window() { request_exit(); }

void reembed_taskbar_window() {
  if (g_user_exit) {
    return;
  }
  HWND tray = find_tray();
  if (!g_hwnd || !IsWindow(g_hwnd)) {
    g_hwnd = nullptr;
    create_display_window(app().instance, false);
    return;
  }
  if (tray && GetWindow(g_hwnd, GW_OWNER) == tray && IsWindowVisible(g_hwnd)) {
    return;
  }
  if (g_hwnd && tray && GetWindow(g_hwnd, GW_OWNER) != tray) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    create_display_window(app().instance, false);
    return;
  }
  attach_tray_owner();
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  g_last_x = INT_MIN;
  present();
}

void pause_taskbar_updates(bool pause) {
  g_pause_updates = pause;
  if (!pause) {
    present();
  }
}

void invalidate_taskbar_layout() {
  apply_taskbar_font_from_config();
  g_last_x = INT_MIN;
  present();
}

}  // namespace ustb
