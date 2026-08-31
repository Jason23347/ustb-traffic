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
#include <climits>
#include <cstring>
#include <mutex>
#include <string>

namespace ustb {
namespace {

constexpr wchar_t kHostClass[] = L"UstbTrafficHost";
constexpr wchar_t kWndClass[] = L"UstbTrafficTaskbar";
constexpr UINT kPresentTimer = 1;
constexpr UINT kEmbedTimer = 2;

HWND g_host = nullptr;
HWND g_hwnd = nullptr;
HWND g_tip = nullptr;
HWND g_tray_owner = nullptr;
int g_dpi = 96;
COLORREF g_fg = RGB(255, 255, 255);
bool g_light_theme = false;
bool g_pause_updates = false;
bool g_user_exit = false;
int g_last_x = INT_MIN;
int g_last_y = INT_MIN;
int g_last_w = 0;
int g_last_h = 0;

bool create_display_window(HINSTANCE instance, bool wait_for_tray);
void attach_tray_owner();
bool present();

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

void apply_system_theme() {
  refresh_colors();
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
  static const wchar_t* kValues[] = {
      L"999.99 GB", L"100.0%", L"+99.9%", L"999.99 MB/s", L"状态", L"未登录",
      L"--", L"…",
  };
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
  for (const wchar_t* s : kValues) {
    m.value_w = (std::max)(m.value_w, text_width(s, true));
  }
  return m;
}

int compute_width() { return column_metrics().total(); }

void update_tooltip_rect() {
  if (!g_tip || !g_hwnd) {
    return;
  }
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = g_hwnd;
  ti.uId = 1;
  GetClientRect(g_hwnd, &ti.rect);
  SendMessageW(g_tip, TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
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
    auto draw_row = [&](const std::wstring& label, const std::wstring& value,
                        int y0, int y1, COLORREF fg) {
      RECT lc{col.pad, y0, col.pad + col.label_w, y1};
      RECT vc{value_x, y0, width - col.pad, y1};
      if (!label.empty()) {
        text_render_draw(label.c_str(), lc, fg, true, false);
      }
      text_render_draw(value.c_str(), vc, fg, false, true);
    };
    draw_row(cells.top_label, cells.top_value, 0, height / 2, usage_fg);
    draw_row(cells.bottom_label, cells.bottom_value, height / 2, height,
             speed_fg);
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
    update_tooltip_rect();
  }

  SelectObject(mem, old_bmp);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok != FALSE;
}

void create_tooltip(HWND hwnd, HINSTANCE instance) {
  g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                          WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                          hwnd, nullptr, instance, nullptr);
  if (!g_tip) {
    return;
  }
  SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = hwnd;
  ti.uId = 1;
  ti.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
  GetClientRect(hwnd, &ti.rect);
  ti.lpszText = LPSTR_TEXTCALLBACK;
  SendMessageW(g_tip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
  SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 360);
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
      refresh_colors();
      create_tooltip(hwnd, app().instance);
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
    case WM_TIMER:
      if (wp == kPresentTimer) {
        refresh_colors();
        present();
      }
      return 0;
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(hwnd, &pt);
      show_context_menu(hwnd, pt);
      return 0;
    }
    case WM_CONTEXTMENU: {
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
    case WM_NOTIFY: {
      auto* hdr = reinterpret_cast<NMHDR*>(lp);
      if (hdr && hdr->code == TTN_GETDISPINFO) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
        static wchar_t tip[1024];
        const std::wstring text =
            format_tooltip(app().copy_snap(), app().quota_kb());
        wcsncpy_s(tip, text.c_str(), _TRUNCATE);
        info->lpszText = tip;
        return 0;
      }
      break;
    }
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
      text_render_shutdown();
      g_hwnd = nullptr;
      g_tip = nullptr;
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
  g_last_x = INT_MIN;
  present();
}

}  // namespace ustb
