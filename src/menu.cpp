#include "menu.h"

#include "app.h"
#include "autostart.h"
#include "config.h"
#include "taskbar_wnd.h"
#include "types.h"
#include "zifuwu.h"

#include <windows.h>

#include <algorithm>
#include <string>

namespace ustb {
namespace {

constexpr wchar_t kOptClass[] = L"UstbTrafficOptions";
constexpr int kOptClientW = 456;
constexpr int kOptClientH = 516;
constexpr int IDC_SOURCE = 2001;
constexpr int IDC_INTERVAL = 2002;
constexpr int IDC_QUOTA = 2003;
constexpr int IDC_PAD = 2004;
constexpr int IDC_GAP = 2005;
constexpr int IDC_FONT = 2006;
constexpr int IDC_USER = 2007;
constexpr int IDC_PASS = 2008;
constexpr int IDC_SPEED = 2009;
constexpr int IDC_HINT = 2010;
constexpr int IDC_UNIT_MS = 2011;
constexpr int IDC_UNIT_GB = 2012;
constexpr int IDC_UNIT_PX = 2013;
constexpr int IDC_UNIT_GAP = 2014;
constexpr int IDC_UNIT_FONT = 2015;
constexpr int IDC_OK = IDOK;
constexpr int IDC_CANCEL = IDCANCEL;

HWND g_opt = nullptr;
HFONT g_opt_font = nullptr;
int g_opt_dpi = 96;

int dpx(int px) { return MulDiv(px, g_opt_dpi, 96); }

HFONT make_ui_font() {
  return CreateFontW(-MulDiv(9, g_opt_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                     FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void apply_font(HWND hwnd) {
  if (hwnd && g_opt_font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_opt_font), TRUE);
  }
}

std::wstring get_text(HWND hwnd) {
  const int n = GetWindowTextLengthW(hwnd);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(hwnd, s.data(), n + 1);
  s.resize(wcslen(s.c_str()));
  return s;
}

TrafficSource selected_source(HWND hwnd) {
  const int sel =
      static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_SOURCE), CB_GETCURSEL,
                                    0, 0));
  if (sel == 1) {
    return TrafficSource::Portal66;
  }
  if (sel == 2) {
    return TrafficSource::Zifuwu;
  }
  if (sel == 3) {
    return TrafficSource::Hybrid;
  }
  return TrafficSource::Portal82;
}

SpeedSource selected_speed_source(HWND hwnd) {
  const int sel =
      static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_SPEED), CB_GETCURSEL,
                                    0, 0));
  if (sel == 1) {
    return SpeedSource::Portal66;
  }
  return SpeedSource::Portal82;
}

void sync_source_dependent(HWND hwnd) {
  const TrafficSource src = selected_source(hwnd);
  const bool need_cred = traffic_source_needs_zifuwu_cred(src);
  const bool need_speed = src == TrafficSource::Hybrid;
  EnableWindow(GetDlgItem(hwnd, IDC_USER), need_cred ? TRUE : FALSE);
  EnableWindow(GetDlgItem(hwnd, IDC_PASS), need_cred ? TRUE : FALSE);
  EnableWindow(GetDlgItem(hwnd, IDC_SPEED), need_speed ? TRUE : FALSE);
}

LRESULT CALLBACK options_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_SOURCE:
          if (HIWORD(wp) == CBN_SELCHANGE) {
            sync_source_dependent(hwnd);
          }
          break;
        case IDC_OK: {
          const TrafficSource source = selected_source(hwnd);
          const SpeedSource speed = selected_speed_source(hwnd);
          std::wstring user = get_text(GetDlgItem(hwnd, IDC_USER));
          std::wstring pass = get_text(GetDlgItem(hwnd, IDC_PASS));
          std::wstring interval_s = get_text(GetDlgItem(hwnd, IDC_INTERVAL));
          std::wstring quota_s = get_text(GetDlgItem(hwnd, IDC_QUOTA));
          std::wstring pad_s = get_text(GetDlgItem(hwnd, IDC_PAD));
          std::wstring gap_s = get_text(GetDlgItem(hwnd, IDC_GAP));
          std::wstring font_s = get_text(GetDlgItem(hwnd, IDC_FONT));
          if (traffic_source_needs_zifuwu_cred(source) &&
              (user.empty() || pass.empty())) {
            MessageBoxW(hwnd, L"自服务/混合模式请填写学号和密码。",
                        L"UstbTraffic", MB_OK | MB_ICONWARNING);
            return 0;
          }
          unsigned interval = static_cast<unsigned>(
              wcstoul(interval_s.c_str(), nullptr, 10));
          unsigned quota =
              static_cast<unsigned>(wcstoul(quota_s.c_str(), nullptr, 10));
          unsigned pad =
              static_cast<unsigned>(wcstoul(pad_s.c_str(), nullptr, 10));
          unsigned gap =
              static_cast<unsigned>(wcstoul(gap_s.c_str(), nullptr, 10));
          unsigned font_dip =
              static_cast<unsigned>(wcstoul(font_s.c_str(), nullptr, 10));
          interval = std::clamp(interval, kMinIntervalMs, 60000u);
          quota = std::clamp(quota, 1u, 10000u);
          pad = std::clamp(pad, kMinTaskbarPadPx, kMaxTaskbarPadGapPx);
          gap = std::clamp(gap, 0u, kMaxTaskbarPadGapPx);
          if (font_dip != kTaskbarFontDipAuto) {
            font_dip =
                std::clamp(font_dip, kMinTaskbarFontDip, kMaxTaskbarFontDip);
          }
          Config cfg;
          {
            std::lock_guard<std::mutex> lock(app().mu);
            const bool cred_changed =
                app().config.username != user ||
                app().config.password != pass ||
                app().config.traffic_source != source;
            app().config.traffic_source = source;
            app().config.speed_source = speed;
            apply_traffic_source(app().config);
            app().config.username = std::move(user);
            app().config.password = std::move(pass);
            app().config.interval_ms = interval;
            app().config.quota_gb = quota;
            app().config.taskbar_pad_px = pad;
            app().config.taskbar_gap_px = gap;
            app().config.taskbar_font_dip = font_dip;
            cfg = app().config;
            if (cred_changed) {
              zifuwu_reset_session();
            }
          }
          save_config(cfg);
          invalidate_taskbar_layout();
          DestroyWindow(hwnd);
          return 0;
        }
        case IDC_CANCEL:
          DestroyWindow(hwnd);
          return 0;
        default:
          break;
      }
      break;
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      HWND ctl = reinterpret_cast<HWND>(lp);
      SetBkMode(dc, TRANSPARENT);
      const int id = GetDlgCtrlID(ctl);
      if (id == IDC_HINT || id == IDC_UNIT_MS || id == IDC_UNIT_GB ||
          id == IDC_UNIT_PX || id == IDC_UNIT_GAP || id == IDC_UNIT_FONT) {
        SetTextColor(dc, RGB(110, 110, 110));
      } else {
        SetTextColor(dc, RGB(32, 32, 32));
      }
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int y = rc.bottom - dpx(64);
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
      HGDIOBJ old = SelectObject(dc, pen);
      MoveToEx(dc, dpx(24), y, nullptr);
      LineTo(dc, rc.right - dpx(24), y);
      SelectObject(dc, old);
      DeleteObject(pen);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      g_opt = nullptr;
      if (g_opt_font) {
        DeleteObject(g_opt_font);
        g_opt_font = nullptr;
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND add_label(HWND parent, HINSTANCE inst, int id, const wchar_t* text, int x,
               int y, int w, int h) {
  HWND hwnd = CreateWindowExW(
      0, L"STATIC", text,
      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, x, y, w, h,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  apply_font(hwnd);
  return hwnd;
}

HWND add_edit(HWND parent, HINSTANCE inst, int id, const wchar_t* text, int x,
              int y, int w, int h, DWORD extra) {
  HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP |
                               extra,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           inst, nullptr);
  apply_font(e);
  return e;
}

HWND add_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
               int h) {
  HWND c = CreateWindowExW(
      0, L"COMBOBOX", L"",
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w,
      h + dpx(120), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      inst, nullptr);
  apply_font(c);
  return c;
}

HWND add_btn(HWND parent, HINSTANCE inst, int id, const wchar_t* text, int x,
             int y, int w, int h, DWORD extra) {
  HWND b = CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, x, y, w, h,
                           parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           inst, nullptr);
  apply_font(b);
  return b;
}

}  // namespace

void show_context_menu(HWND hwnd, POINT screen) {
  HMENU menu = CreatePopupMenu();
  HMENU usage = CreatePopupMenu();
  HMENU side = CreatePopupMenu();
  UsageMode mode;
  TaskbarSide taskbar_side;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    mode = app().config.usage_mode;
    taskbar_side = app().config.taskbar_side;
  }
  auto radio = [&](HMENU dest, UINT id, const wchar_t* label, bool checked) {
    UINT f = MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(dest, f, id, label);
  };
  radio(usage, IDM_USAGE_ABS, L"绝对用量", mode == UsageMode::Absolute);
  radio(usage, IDM_USAGE_OVER, L"超出用量", mode == UsageMode::OverQuota);
  radio(usage, IDM_USAGE_PCT, L"百分比用量", mode == UsageMode::Percent);
  radio(side, IDM_SIDE_RIGHT, L"靠右（托盘左侧）",
        taskbar_side == TaskbarSide::Right);
  radio(side, IDM_SIDE_LEFT, L"靠左（任务栏最左）",
        taskbar_side == TaskbarSide::Left);
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(usage), L"用量显示");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(side), L"显示位置");
  AppendMenuW(menu, MF_STRING, IDM_OPTIONS, L"选项...");
  AppendMenuW(menu, MF_STRING | (is_autostart() ? MF_CHECKED : MF_UNCHECKED),
              IDM_AUTOSTART, L"开机自启");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于...");
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

  pause_taskbar_updates(true);
  SetForegroundWindow(hwnd);
  const UINT cmd = TrackPopupMenu(
      menu,
      TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
      screen.x, screen.y, 0, hwnd, nullptr);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd != 0) {
    SendMessageW(hwnd, WM_COMMAND, cmd, 0);
  }
  pause_taskbar_updates(false);
}

void show_options_dialog(HWND parent) {
  if (g_opt) {
    SetForegroundWindow(g_opt);
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = options_proc;
  wc.hInstance = app().instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kOptClass;
  RegisterClassExW(&wc);

  Config cfg;
  {
    std::lock_guard<std::mutex> lock(app().mu);
    cfg = app().config;
  }

  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

  const DWORD style = WS_CAPTION | WS_SYSMENU;
  const DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
  RECT wr{0, 0, kOptClientW, kOptClientH};
  AdjustWindowRectEx(&wr, style, FALSE, ex);
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;
  const int x = wa.left + (wa.right - wa.left - win_w) / 2;
  const int y = wa.top + (wa.bottom - wa.top - win_h) / 2;

  g_opt = CreateWindowExW(ex, kOptClass, L"UstbTraffic 选项", style, x, y, win_w,
                          win_h, parent, nullptr, app().instance, nullptr);
  if (!g_opt) {
    return;
  }
  g_opt_dpi = GetDpiForWindow(g_opt);
  if (g_opt_dpi == 0) {
    g_opt_dpi = 96;
  }
  wr = {0, 0, dpx(kOptClientW), dpx(kOptClientH)};
  AdjustWindowRectEx(&wr, style, FALSE, ex);
  SetWindowPos(g_opt, nullptr, x, y, wr.right - wr.left, wr.bottom - wr.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
  g_opt_font = make_ui_font();

  const int label_x = dpx(24);
  const int label_w = dpx(72);
  const int edit_x = dpx(108);
  const int row_h = dpx(28);
  const int field_w = dpx(kOptClientW) - edit_x - dpx(24);
  const int num_w = dpx(140);
  const int unit_x = edit_x + num_w + dpx(10);

  add_label(g_opt, app().instance, 0, L"流量源", label_x, dpx(22), label_w,
            row_h);
  HWND source = add_combo(g_opt, app().instance, IDC_SOURCE, edit_x, dpx(22),
                          field_w, row_h);
  SendMessageW(source, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(traffic_source_label(TrafficSource::Portal82)));
  SendMessageW(source, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(traffic_source_label(TrafficSource::Portal66)));
  SendMessageW(source, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(traffic_source_label(TrafficSource::Zifuwu)));
  SendMessageW(source, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(traffic_source_label(TrafficSource::Hybrid)));
  int sel = 0;
  if (cfg.traffic_source == TrafficSource::Portal66) {
    sel = 1;
  } else if (cfg.traffic_source == TrafficSource::Zifuwu) {
    sel = 2;
  } else if (cfg.traffic_source == TrafficSource::Hybrid) {
    sel = 3;
  }
  SendMessageW(source, CB_SETCURSEL, sel, 0);

  add_label(g_opt, app().instance, 0, L"学号", label_x, dpx(62), label_w, row_h);
  add_edit(g_opt, app().instance, IDC_USER, cfg.username.c_str(), edit_x,
           dpx(62), field_w, row_h, 0);
  add_label(g_opt, app().instance, 0, L"密码", label_x, dpx(102), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_PASS, cfg.password.c_str(), edit_x,
           dpx(102), field_w, row_h, ES_PASSWORD);

  add_label(g_opt, app().instance, 0, L"速率源", label_x, dpx(142), label_w,
            row_h);
  HWND speed = add_combo(g_opt, app().instance, IDC_SPEED, edit_x, dpx(142),
                         field_w, row_h);
  SendMessageW(speed, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(speed_source_label(SpeedSource::Portal82)));
  SendMessageW(speed, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(speed_source_label(SpeedSource::Portal66)));
  SendMessageW(speed, CB_SETCURSEL,
               cfg.speed_source == SpeedSource::Portal66 ? 1 : 0, 0);

  sync_source_dependent(g_opt);

  wchar_t num[32];
  swprintf_s(num, L"%u", cfg.interval_ms);
  add_label(g_opt, app().instance, 0, L"轮询间隔", label_x, dpx(182), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_INTERVAL, num, edit_x, dpx(182), num_w,
           row_h, ES_NUMBER);
  add_label(g_opt, app().instance, IDC_UNIT_MS, L"ms", unit_x, dpx(182), dpx(36),
            row_h);

  swprintf_s(num, L"%u", cfg.quota_gb);
  add_label(g_opt, app().instance, 0, L"免费额度", label_x, dpx(222), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_QUOTA, num, edit_x, dpx(222), num_w, row_h,
           ES_NUMBER);
  add_label(g_opt, app().instance, IDC_UNIT_GB, L"GB", unit_x, dpx(222), dpx(36),
            row_h);

  swprintf_s(num, L"%u", cfg.taskbar_pad_px);
  add_label(g_opt, app().instance, 0, L"边距", label_x, dpx(262), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_PAD, num, edit_x, dpx(262), num_w, row_h,
           ES_NUMBER);
  add_label(g_opt, app().instance, IDC_UNIT_PX, L"px", unit_x, dpx(262), dpx(36),
            row_h);

  swprintf_s(num, L"%u", cfg.taskbar_gap_px);
  add_label(g_opt, app().instance, 0, L"列间距", label_x, dpx(302), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_GAP, num, edit_x, dpx(302), num_w, row_h,
           ES_NUMBER);
  add_label(g_opt, app().instance, IDC_UNIT_GAP, L"px", unit_x, dpx(302), dpx(36),
            row_h);

  swprintf_s(num, L"%u", cfg.taskbar_font_dip);
  add_label(g_opt, app().instance, 0, L"字体大小", label_x, dpx(342), label_w,
            row_h);
  add_edit(g_opt, app().instance, IDC_FONT, num, edit_x, dpx(342), num_w, row_h,
           ES_NUMBER);
  add_label(g_opt, app().instance, IDC_UNIT_FONT, L"DIP", unit_x, dpx(342),
            dpx(48), row_h);

  add_label(g_opt, app().instance, IDC_HINT,
            L"自服务/混合需学号密码；混合在门户未登录时显示未登录；字体 0=自动",
            label_x, dpx(388), dpx(kOptClientW) - dpx(48), dpx(22));

  const int btn_w = dpx(88);
  const int btn_h = dpx(32);
  const int btn_y = dpx(kOptClientH) - dpx(24) - btn_h;
  add_btn(g_opt, app().instance, IDC_OK, L"确定",
          dpx(kOptClientW) - dpx(24) - btn_w * 2 - dpx(12), btn_y, btn_w, btn_h,
          BS_DEFPUSHBUTTON);
  add_btn(g_opt, app().instance, IDC_CANCEL, L"取消",
          dpx(kOptClientW) - dpx(24) - btn_w, btn_y, btn_w, btn_h, 0);
  ShowWindow(g_opt, SW_SHOW);
  SetForegroundWindow(g_opt);
}

void show_about_dialog(HWND parent) {
  wchar_t text[512];
  swprintf_s(text, L"%s\nv%s\n\n%s\n\n作者：%s", kAppName, kAppVersion,
             kAppTagline, kAppAuthor);
  wchar_t title[64];
  swprintf_s(title, L"关于 %s", kAppName);
  MessageBoxW(parent, text, title, MB_OK | MB_ICONINFORMATION);
}

}  // namespace ustb
