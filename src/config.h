#pragma once

#include "types.h"

#include <string>

namespace ustb {

enum class TrafficSource {
  Portal82 = 0,
  Portal66 = 1,
  Zifuwu = 2,
};

struct Config {
  TrafficSource traffic_source = TrafficSource::Portal82;
  std::wstring host = L"202.204.48.82";
  unsigned port = 80;
  std::wstring path = L"/";
  bool use_https = false;
  std::wstring username;
  std::wstring password;
  unsigned interval_ms = kDefaultIntervalMs;
  unsigned quota_gb = kDefaultQuotaGb;
  unsigned taskbar_pad_px = kDefaultTaskbarPadPx;
  unsigned taskbar_gap_px = kDefaultTaskbarGapPx;
  unsigned taskbar_font_dip = kTaskbarFontDipAuto;
  UsageMode usage_mode = UsageMode::Absolute;
  TaskbarSide taskbar_side = TaskbarSide::Right;
};

void apply_traffic_source(Config& cfg);
const wchar_t* traffic_source_key(TrafficSource src);
const wchar_t* traffic_source_label(TrafficSource src);
TrafficSource traffic_source_from_key(const wchar_t* key);
TrafficSource traffic_source_from_legacy_host(const std::wstring& host);

std::wstring config_dir();
std::wstring config_path();
Config load_config();
void save_config(const Config& cfg);

}  // namespace ustb
