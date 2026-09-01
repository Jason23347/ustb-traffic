#pragma once

#include "types.h"

#include <string>

namespace ustb {

struct Config {
  std::wstring host = L"202.204.48.82";
  unsigned port = 80;
  std::wstring path = L"/";
  unsigned interval_ms = kDefaultIntervalMs;
  unsigned quota_gb = kDefaultQuotaGb;
  unsigned taskbar_pad_px = kDefaultTaskbarPadPx;
  unsigned taskbar_gap_px = kDefaultTaskbarGapPx;
  unsigned taskbar_font_dip = kTaskbarFontDipAuto;
  UsageMode usage_mode = UsageMode::Absolute;
  TaskbarSide taskbar_side = TaskbarSide::Right;
};

std::wstring config_dir();
std::wstring config_path();
Config load_config();
void save_config(const Config& cfg);

}  // namespace ustb
