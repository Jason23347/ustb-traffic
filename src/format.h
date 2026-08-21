#pragma once

#include "types.h"

#include <string>

namespace ustb {

struct TaskbarCells {
  std::wstring top_label;
  std::wstring top_value;
  std::wstring bottom_label;
  std::wstring bottom_value;
};

std::wstring gbk_to_wide(const std::string& s);
std::wstring now_text();

std::wstring format_flow_kb(uint64_t kb);
std::wstring format_speed_kbps(double kbps);
std::wstring format_percent(uint64_t used_kb, uint64_t quota_kb);
std::wstring format_fee_yuan(uint32_t fee);

uint64_t quota_to_kb(unsigned quota_gb);
uint64_t flow_over_kb(uint64_t used_kb, uint64_t quota_kb);
uint64_t flow_left_kb(uint64_t used_kb, uint64_t quota_kb);

std::wstring format_usage_value(UsageMode mode, uint64_t used_kb,
                                uint64_t quota_kb);
TaskbarCells format_taskbar_cells(const DisplaySnapshot& snap, UsageMode mode,
                                  uint64_t quota_kb);
std::wstring format_tooltip(const DisplaySnapshot& snap, uint64_t quota_kb);

enum class MeterLevel { Normal, Warn, Danger };

MeterLevel usage_meter_level(uint64_t used_kb, uint64_t quota_kb);
MeterLevel speed_meter_level(double kbps);

}  // namespace ustb
