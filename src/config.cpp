#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <iterator>

namespace ustb {
namespace {

unsigned parse_u(const wchar_t* s, unsigned fallback) {
  if (s == nullptr || s[0] == L'\0') {
    return fallback;
  }
  wchar_t* end = nullptr;
  const unsigned long v = wcstoul(s, &end, 10);
  if (end == s) {
    return fallback;
  }
  return static_cast<unsigned>(v);
}

}  // namespace

std::wstring config_dir() {
  wchar_t base[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, base))) {
    return L".";
  }
  std::wstring dir = base;
  dir += L"\\UstbTraffic";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring config_path() { return config_dir() + L"\\config.ini"; }

Config load_config() {
  Config cfg;
  const std::wstring path = config_path();
  wchar_t buf[256]{};

  GetPrivateProfileStringW(L"general", L"host", cfg.host.c_str(), buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.host = buf;

  GetPrivateProfileStringW(L"general", L"path", cfg.path.c_str(), buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.path = buf;
  if (cfg.path.empty() || cfg.path[0] != L'/') {
    cfg.path = L"/";
  }

  GetPrivateProfileStringW(L"general", L"port", L"80", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.port = static_cast<unsigned>(std::clamp(parse_u(buf, 80), 1u, 65535u));

  GetPrivateProfileStringW(L"general", L"interval_ms", L"1000", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.interval_ms =
      std::clamp(parse_u(buf, kDefaultIntervalMs), 500u, 60000u);

  GetPrivateProfileStringW(L"general", L"quota_gb", L"120", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.quota_gb = std::clamp(parse_u(buf, kDefaultQuotaGb), 1u, 10000u);

  GetPrivateProfileStringW(L"general", L"usage_mode", L"0", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  const unsigned mode = parse_u(buf, 0);
  if (mode <= 2) {
    cfg.usage_mode = static_cast<UsageMode>(mode);
  }
  return cfg;
}

void save_config(const Config& cfg) {
  const std::wstring path = config_path();
  WritePrivateProfileStringW(L"general", L"host", cfg.host.c_str(),
                             path.c_str());
  WritePrivateProfileStringW(L"general", L"path", cfg.path.c_str(),
                             path.c_str());

  wchar_t num[32];
  swprintf_s(num, L"%u", cfg.port);
  WritePrivateProfileStringW(L"general", L"port", num, path.c_str());
  swprintf_s(num, L"%u", cfg.interval_ms);
  WritePrivateProfileStringW(L"general", L"interval_ms", num, path.c_str());
  swprintf_s(num, L"%u", cfg.quota_gb);
  WritePrivateProfileStringW(L"general", L"quota_gb", num, path.c_str());
  swprintf_s(num, L"%u", static_cast<unsigned>(cfg.usage_mode));
  WritePrivateProfileStringW(L"general", L"usage_mode", num, path.c_str());
}

}  // namespace ustb
