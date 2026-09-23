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

bool contains_ci(const std::wstring& hay, const wchar_t* needle) {
  if (needle == nullptr || needle[0] == L'\0') {
    return false;
  }
  for (size_t i = 0; i < hay.size(); ++i) {
    size_t j = 0;
    while (needle[j] != L'\0') {
      const wchar_t a = hay[i + j];
      const wchar_t b = needle[j];
      if (i + j >= hay.size()) {
        break;
      }
      const wchar_t al =
          (a >= L'A' && a <= L'Z') ? static_cast<wchar_t>(a - L'A' + L'a') : a;
      const wchar_t bl =
          (b >= L'A' && b <= L'Z') ? static_cast<wchar_t>(b - L'A' + L'a') : b;
      if (al != bl) {
        break;
      }
      ++j;
    }
    if (needle[j] == L'\0') {
      return true;
    }
  }
  return false;
}

}  // namespace

void apply_traffic_source(Config& cfg) {
  switch (cfg.traffic_source) {
    case TrafficSource::Portal66:
      cfg.host = L"202.204.48.66";
      cfg.port = 80;
      cfg.path = L"/";
      cfg.use_https = false;
      break;
    case TrafficSource::Zifuwu:
      cfg.host = L"zifuwu.ustb.edu.cn";
      cfg.port = 443;
      cfg.path = L"/Self/dashboard";
      cfg.use_https = true;
      break;
    case TrafficSource::Hybrid:
      // Identity from portal 48.82; usage/fee via zifuwu fetch path.
      cfg.host = L"202.204.48.82";
      cfg.port = 80;
      cfg.path = L"/";
      cfg.use_https = false;
      break;
    case TrafficSource::Portal82:
    default:
      cfg.traffic_source = TrafficSource::Portal82;
      cfg.host = L"202.204.48.82";
      cfg.port = 80;
      cfg.path = L"/";
      cfg.use_https = false;
      break;
  }
}

const wchar_t* traffic_source_key(TrafficSource src) {
  switch (src) {
    case TrafficSource::Portal66:
      return L"portal66";
    case TrafficSource::Zifuwu:
      return L"zifuwu";
    case TrafficSource::Hybrid:
      return L"hybrid";
    case TrafficSource::Portal82:
    default:
      return L"portal82";
  }
}

const wchar_t* traffic_source_label(TrafficSource src) {
  switch (src) {
    case TrafficSource::Portal66:
      return L"202.204.48.66";
    case TrafficSource::Zifuwu:
      return L"https://zifuwu.ustb.edu.cn";
    case TrafficSource::Hybrid:
      return L"混合（登录页身份+自服务用量）";
    case TrafficSource::Portal82:
    default:
      return L"202.204.48.82";
  }
}

TrafficSource traffic_source_from_key(const wchar_t* key) {
  if (key == nullptr) {
    return TrafficSource::Portal82;
  }
  if (_wcsicmp(key, L"portal66") == 0) {
    return TrafficSource::Portal66;
  }
  if (_wcsicmp(key, L"zifuwu") == 0) {
    return TrafficSource::Zifuwu;
  }
  if (_wcsicmp(key, L"hybrid") == 0) {
    return TrafficSource::Hybrid;
  }
  return TrafficSource::Portal82;
}

TrafficSource traffic_source_from_legacy_host(const std::wstring& host) {
  if (contains_ci(host, L"zifuwu")) {
    return TrafficSource::Zifuwu;
  }
  if (contains_ci(host, L"202.204.48.66")) {
    return TrafficSource::Portal66;
  }
  return TrafficSource::Portal82;
}

const wchar_t* speed_source_key(SpeedSource src) {
  switch (src) {
    case SpeedSource::Portal66:
      return L"portal66";
    case SpeedSource::Portal82:
    default:
      return L"portal82";
  }
}

const wchar_t* speed_source_label(SpeedSource src) {
  switch (src) {
    case SpeedSource::Portal66:
      return L"202.204.48.66";
    case SpeedSource::Portal82:
    default:
      return L"202.204.48.82";
  }
}

SpeedSource speed_source_from_key(const wchar_t* key) {
  if (key != nullptr && _wcsicmp(key, L"portal66") == 0) {
    return SpeedSource::Portal66;
  }
  return SpeedSource::Portal82;
}

bool traffic_source_needs_zifuwu_cred(TrafficSource src) {
  return src == TrafficSource::Zifuwu || src == TrafficSource::Hybrid;
}

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
  wchar_t buf[512]{};

  GetPrivateProfileStringW(L"general", L"traffic_source", L"", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  if (buf[0] != L'\0') {
    cfg.traffic_source = traffic_source_from_key(buf);
  } else {
    GetPrivateProfileStringW(L"general", L"host", cfg.host.c_str(), buf,
                             static_cast<DWORD>(std::size(buf)), path.c_str());
    cfg.traffic_source = traffic_source_from_legacy_host(buf);
  }
  apply_traffic_source(cfg);

  GetPrivateProfileStringW(L"general", L"speed_source", L"portal82", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.speed_source = speed_source_from_key(buf);

  GetPrivateProfileStringW(L"general", L"username", L"", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.username = buf;

  GetPrivateProfileStringW(L"general", L"password", L"", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.password = buf;

  GetPrivateProfileStringW(L"general", L"interval_ms", L"1000", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.interval_ms =
      std::clamp(parse_u(buf, kDefaultIntervalMs), kMinIntervalMs, 60000u);

  GetPrivateProfileStringW(L"general", L"quota_gb", L"120", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.quota_gb = std::clamp(parse_u(buf, kDefaultQuotaGb), 1u, 10000u);

  GetPrivateProfileStringW(L"general", L"taskbar_pad_px", L"8", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.taskbar_pad_px = std::clamp(parse_u(buf, kDefaultTaskbarPadPx),
                                  kMinTaskbarPadPx, kMaxTaskbarPadGapPx);

  GetPrivateProfileStringW(L"general", L"taskbar_gap_px", L"8", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.taskbar_gap_px = std::clamp(parse_u(buf, kDefaultTaskbarGapPx), 0u,
                                  kMaxTaskbarPadGapPx);

  GetPrivateProfileStringW(L"general", L"taskbar_font_dip", L"0", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  const unsigned font_dip = parse_u(buf, kTaskbarFontDipAuto);
  if (font_dip == kTaskbarFontDipAuto) {
    cfg.taskbar_font_dip = kTaskbarFontDipAuto;
  } else {
    cfg.taskbar_font_dip =
        std::clamp(font_dip, kMinTaskbarFontDip, kMaxTaskbarFontDip);
  }

  GetPrivateProfileStringW(L"general", L"usage_mode", L"0", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  const unsigned mode = parse_u(buf, 0);
  if (mode <= 2) {
    cfg.usage_mode = static_cast<UsageMode>(mode);
  }

  GetPrivateProfileStringW(L"general", L"taskbar_side", L"0", buf,
                           static_cast<DWORD>(std::size(buf)), path.c_str());
  cfg.taskbar_side =
      parse_u(buf, 0) == 1 ? TaskbarSide::Left : TaskbarSide::Right;
  return cfg;
}

void save_config(const Config& cfg) {
  const std::wstring path = config_path();
  WritePrivateProfileStringW(L"general", L"traffic_source",
                             traffic_source_key(cfg.traffic_source),
                             path.c_str());
  WritePrivateProfileStringW(L"general", L"speed_source",
                             speed_source_key(cfg.speed_source), path.c_str());
  WritePrivateProfileStringW(L"general", L"host", cfg.host.c_str(),
                             path.c_str());
  WritePrivateProfileStringW(L"general", L"path", cfg.path.c_str(),
                             path.c_str());
  WritePrivateProfileStringW(L"general", L"username", cfg.username.c_str(),
                             path.c_str());
  WritePrivateProfileStringW(L"general", L"password", cfg.password.c_str(),
                             path.c_str());

  wchar_t num[32];
  swprintf_s(num, L"%u", cfg.port);
  WritePrivateProfileStringW(L"general", L"port", num, path.c_str());
  swprintf_s(num, L"%u", cfg.interval_ms);
  WritePrivateProfileStringW(L"general", L"interval_ms", num, path.c_str());
  swprintf_s(num, L"%u", cfg.quota_gb);
  WritePrivateProfileStringW(L"general", L"quota_gb", num, path.c_str());
  swprintf_s(num, L"%u", cfg.taskbar_pad_px);
  WritePrivateProfileStringW(L"general", L"taskbar_pad_px", num, path.c_str());
  swprintf_s(num, L"%u", cfg.taskbar_gap_px);
  WritePrivateProfileStringW(L"general", L"taskbar_gap_px", num, path.c_str());
  swprintf_s(num, L"%u", cfg.taskbar_font_dip);
  WritePrivateProfileStringW(L"general", L"taskbar_font_dip", num, path.c_str());
  swprintf_s(num, L"%u", static_cast<unsigned>(cfg.usage_mode));
  WritePrivateProfileStringW(L"general", L"usage_mode", num, path.c_str());
  swprintf_s(num, L"%u", static_cast<unsigned>(cfg.taskbar_side));
  WritePrivateProfileStringW(L"general", L"taskbar_side", num, path.c_str());
}

}  // namespace ustb
