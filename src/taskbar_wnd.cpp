#include "taskbar_wnd.h"

#include "app.h"
#include "autostart.h"
#include "format.h"
#include "menu.h"
#include "poller.h"

#include <commctrl.h>
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
HFONT g_font = nullptr;
int g_dpi = 96;
COLORREF g_fg = RGB(255, 255, 255);
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

HFONT make_font(int dpi) {
  return CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
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
  g_fg = system_uses_light_theme() ? RGB(16, 16, 16) : RGB(255, 255, 255);
}

int text_width(HDC hdc, const wchar_t* text) {
  SIZE sz{};
  GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
  return sz.cx;
}

struct ColMetrics {
  int pad = 0;
  int label_w = 0;
  int gap = 0;
  int value_w = 0;
  int total() const { return pad + label_w + gap + value_w + pad; }
};

ColMetrics column_metrics(HDC hdc) {
  static const wchar_t* kLabels[] = {L"Σ", L"▲", L"↓", L"●"};
  static const wchar_t* kValues[] = {
      L"999.99 GB", L"100.0%", L"999.99 MB/s", L"状态", L"未登录", L"--", L"…",
  };
  ColMetrics m;
  m.pad = MulDiv(2, g_dpi, 96);
  m.gap = MulDiv(4, g_dpi, 96);
  HGDIOBJ old = SelectObject(hdc, g_font);
  for (const wchar_t* s : kLabels) {
    m.label_w = (std::max)(m.label_w, text_width(hdc, s));
  }
  for (const wchar_t* s : kValues) {
    m.value_w = (std::max)(m.value_w, text_width(hdc, s));
  }
  SelectObject(hdc, old);
  return m;
}

int compute_width(HDC hdc) { return column_metrics(hdc).total(); }

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

void convert_white_mask_to_text(BYTE* bits, int count, COLORREF fg) {
  const int fr = GetRValue(fg);
  const int fgv = GetGValue(fg);
  const int fb = GetBValue(fg);
  for (int i = 0; i < count; ++i) {
    BYTE* p = bits + static_cast<size_t>(i) * 4;
    const int lum = (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
    if (lum <= 1) {
      // Alpha 0 is click-through on layered windows. Keep 1 so the
      // whole block can be right-clicked.
      p[0] = p[1] = p[2] = 0;
      p[3] = 1;
      continue;
    }
    p[0] = static_cast<BYTE>(fb * lum / 255);
    p[1] = static_cast<BYTE>(fgv * lum / 255);
    p[2] = static_cast<BYTE>(fr * lum / 255);
    p[3] = static_cast<BYTE>(lum);
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

bool present() {
  if (!g_hwnd || !g_font || g_pause_updates) {
    return false;
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
  const int width = compute_width(screen);
  const int gap = MulDiv(6, g_dpi, 96);

  HWND notify = find_notify(tray);
  RECT rc_notify{};
  int screen_x = 0;
  const int screen_y = rc_tray.top;
  if (valid_rect(notify, &rc_notify) && rc_notify.left != 0) {
    screen_x = rc_notify.left - gap - width;
  } else {
    screen_x = rc_tray.right - MulDiv(180, g_dpi, 96) - width;
  }
  if (screen_x < rc_tray.left + gap) {
    screen_x = rc_tray.left + gap;
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
  HGDIOBJ old_font = SelectObject(mem, g_font);
  std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) *
                           4);

  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, RGB(255, 255, 255));

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
  const ColMetrics col = column_metrics(mem);
  const int value_x = col.pad + col.label_w + col.gap;
  const DWORD dt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;

  auto draw_row = [&](const std::wstring& label, const std::wstring& value,
                      int y0, int y1) {
    RECT lc{col.pad, y0, col.pad + col.label_w, y1};
    RECT vc{value_x, y0, width - col.pad, y1};
    if (!label.empty()) {
      DrawTextW(mem, label.c_str(), -1, &lc, dt);
    }
    DrawTextW(mem, value.c_str(), -1, &vc, dt);
  };
  draw_row(cells.top_label, cells.top_value, 0, height / 2);
  draw_row(cells.bottom_label, cells.bottom_value, height / 2, height);
  GdiFlush();

  convert_white_mask_to_text(static_cast<BYTE*>(bits), width * height, g_fg);

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
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

  if (ok && (screen_x != g_last_x || screen_y != g_last_y || width != g_last_w ||
             height != g_last_h)) {
    g_last_x = screen_x;
    g_last_y = screen_y;
    g_last_w = width;
    g_last_h = height;
    update_tooltip_rect();
  }

  SelectObject(mem, old_font);
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
      if (g_font) {
        DeleteObject(g_font);
      }
      g_font = make_font(g_dpi);
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
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDM_USAGE_ABS:
        case IDM_USAGE_OVER:
        case IDM_USAGE_PCT: {
          Config cfg;
          {
            std::lock_guard<std::mutex> lock(app().mu);
            if (LOWORD(wp) == IDM_USAGE_ABS) {
              app().config.usage_mode = UsageMode::Absolute;
            } else if (LOWORD(wp) == IDM_USAGE_OVER) {
              app().config.usage_mode = UsageMode::OverQuota;
            } else {
              app().config.usage_mode = UsageMode::Percent;
            }
            cfg = app().config;
          }
          save_config(cfg);
          present();
          return 0;
        }
        case IDM_OPTIONS:
          show_options_dialog(g_host ? g_host : hwnd);
          return 0;
        case IDM_AUTOSTART:
          set_autostart(!is_autostart());
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
      if (g_font) {
        DeleteObject(g_font);
      }
      g_font = make_font(g_dpi);
      present();
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, kPresentTimer);
      if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
      }
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

int measure_taskbar_width(HDC hdc, HFONT font) {
  HFONT old = g_font;
  g_font = font;
  const int w = compute_width(hdc);
  g_font = old;
  return w;
}

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

}  // namespace ustb
