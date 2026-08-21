#include "autostart.h"

#include <windows.h>

#include <string>

namespace ustb {
namespace {

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"UstbTraffic";

std::wstring exe_path() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return path;
}

}  // namespace

bool is_autostart() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  wchar_t buf[MAX_PATH + 4]{};
  DWORD type = 0;
  DWORD size = sizeof(buf);
  const LONG st =
      RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                       &size);
  RegCloseKey(key);
  return st == ERROR_SUCCESS && type == REG_SZ && buf[0] != L'\0';
}

bool set_autostart(bool enable) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  LONG st;
  if (enable) {
    std::wstring cmd = L"\"";
    cmd += exe_path();
    cmd += L"\"";
    st = RegSetValueExW(key, kValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    st = RegDeleteValueW(key, kValueName);
    if (st == ERROR_FILE_NOT_FOUND) {
      st = ERROR_SUCCESS;
    }
  }
  RegCloseKey(key);
  return st == ERROR_SUCCESS;
}

}  // namespace ustb
