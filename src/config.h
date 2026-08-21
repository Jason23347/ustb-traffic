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
  UsageMode usage_mode = UsageMode::Absolute;
};

std::wstring config_dir();
std::wstring config_path();
Config load_config();
void save_config(const Config& cfg);

}  // namespace ustb
