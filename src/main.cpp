#include "app.h"
#include "config.h"
#include "poller.h"
#include "taskbar_wnd.h"

#include <commctrl.h>
#include <windows.h>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\UstbTrafficSingleton";

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(L"UstbTrafficHost", nullptr);
    if (existing) {
      PostMessageW(existing, ustb::WM_USTB_RESHOW, 0, 0);
    }
    CloseHandle(mutex);
    return 0;
  }

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  ustb::app().instance = instance;
  ustb::app().taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
  ustb::app().config = ustb::load_config();

  if (!ustb::create_taskbar_window(instance)) {
    MessageBoxW(nullptr, L"无法创建任务栏窗口。", L"UstbTraffic",
                MB_OK | MB_ICONERROR);
    if (mutex) {
      CloseHandle(mutex);
    }
    return 1;
  }

  ustb::start_poller();

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  ustb::stop_poller();
  if (mutex) {
    CloseHandle(mutex);
  }
  return static_cast<int>(msg.wParam);
}
