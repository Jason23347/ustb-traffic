#include "format.h"

#include <windows.h>

#include <cstdio>

namespace ustb {

std::wstring gbk_to_wide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  auto convert = [&](UINT cp) -> std::wstring {
    const int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) {
      return {};
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), out.data(),
                        n);
    return out;
  };
  std::wstring w = convert(936);
  if (w.empty()) {
    w = convert(CP_ACP);
  }
  return w;
}

std::wstring now_text() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[32];
  swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  return buf;
}

uint64_t quota_to_kb(unsigned quota_gb) {
  return static_cast<uint64_t>(quota_gb) * kGb;
}

uint64_t flow_over_kb(uint64_t used_kb, uint64_t quota_kb) {
  return used_kb > quota_kb ? used_kb - quota_kb : 0;
}

uint64_t flow_left_kb(uint64_t used_kb, uint64_t quota_kb) {
  return used_kb < quota_kb ? quota_kb - used_kb : 0;
}

std::wstring format_flow_kb(uint64_t kb) {
  wchar_t buf[48];
  if (kb < kMb) {
    swprintf_s(buf, L"%.2f KB", static_cast<double>(kb));
  } else if (kb < kGb) {
    swprintf_s(buf, L"%.2f MB", static_cast<double>(kb) / static_cast<double>(kMb));
  } else if (kb < kGb * 1024ull) {
    swprintf_s(buf, L"%.2f GB", static_cast<double>(kb) / static_cast<double>(kGb));
  } else {
    swprintf_s(buf, L"%.2f TB",
               static_cast<double>(kb) / static_cast<double>(kGb * 1024ull));
  }
  return buf;
}

std::wstring format_speed_kbps(double kbps) {
  if (kbps < 0) {
    kbps = 0;
  }
  wchar_t buf[48];
  if (kbps < static_cast<double>(kMb)) {
    swprintf_s(buf, L"%.2f KB/s", kbps);
  } else {
    swprintf_s(buf, L"%.2f MB/s", kbps / static_cast<double>(kMb));
  }
  return buf;
}

std::wstring format_fee_yuan(uint32_t fee) {
  wchar_t buf[48];
  swprintf_s(buf, L"%.2f 元", static_cast<double>(fee) / 10000.0);
  return buf;
}

std::wstring format_percent(uint64_t used_kb, uint64_t quota_kb) {
  wchar_t buf[48];
  if (quota_kb == 0) {
    return L"--";
  }
  const double pct =
      100.0 * static_cast<double>(used_kb) / static_cast<double>(quota_kb);
  swprintf_s(buf, L"%.1f%%", pct);
  return buf;
}

std::wstring format_usage_value(UsageMode mode, uint64_t used_kb,
                                uint64_t quota_kb) {
  switch (mode) {
    case UsageMode::OverQuota:
      return format_flow_kb(flow_over_kb(used_kb, quota_kb));
    case UsageMode::Percent:
      return format_percent(used_kb, quota_kb);
    case UsageMode::Absolute:
    default:
      return format_flow_kb(used_kb);
  }
}

static const wchar_t* usage_label_glyph(UsageMode mode) {
  // Σ 累计用量；▲ 超出额度。和下行 ↓ 一样用单个几何符号。
  return (mode == UsageMode::OverQuota) ? L"▲" : L"Σ";
}

TaskbarCells format_taskbar_cells(const DisplaySnapshot& snap, UsageMode mode,
                                  uint64_t quota_kb) {
  if (snap.state == MonitorState::Init) {
    return {L"Σ", L"…", L"↓", L"…"};
  }
  if (snap.state == MonitorState::NotLoggedIn) {
    return {L"●", L"状态", L"", L"未登录"};
  }
  if (snap.state == MonitorState::HttpError &&
      snap.fail_count > kFailHoldLimit) {
    return {L"●", L"状态", L"", L"--"};
  }

  TaskbarCells cells;
  cells.top_label = usage_label_glyph(mode);
  cells.top_value = snap.have_usage
                        ? format_usage_value(mode, snap.used_kb, quota_kb)
                        : L"…";
  cells.bottom_label = L"↓";
  cells.bottom_value =
      snap.rate_valid ? format_speed_kbps(snap.display_rate_kbps) : L"…";
  return cells;
}

MeterLevel usage_meter_level(uint64_t used_kb, uint64_t quota_kb) {
  if (quota_kb == 0) {
    return MeterLevel::Normal;
  }
  const double pct =
      100.0 * static_cast<double>(used_kb) / static_cast<double>(quota_kb);
  if (pct >= 90.0) {
    return MeterLevel::Danger;
  }
  if (pct >= 70.0) {
    return MeterLevel::Warn;
  }
  return MeterLevel::Normal;
}

MeterLevel speed_meter_level(double kbps) {
  const double mbps = kbps / static_cast<double>(kMb);
  if (mbps > 8.0) {
    return MeterLevel::Danger;
  }
  if (mbps > 2.0) {
    return MeterLevel::Warn;
  }
  return MeterLevel::Normal;
}

std::wstring format_tooltip(const DisplaySnapshot& snap, uint64_t quota_kb) {
  std::wstring s;
  auto line = [&](const wchar_t* k, const std::wstring& v) {
    if (!s.empty()) {
      s += L"\n";
    }
    s += k;
    s += v;
  };
  line(L"学号: ", snap.username.empty() ? L"(未知)" : snap.username);
  line(L"姓名: ", snap.nid.empty() ? L"(无)" : snap.nid);
  if (snap.have_usage) {
    line(L"已用: ", format_flow_kb(snap.used_kb));
    line(L"剩余: ", format_flow_kb(flow_left_kb(snap.used_kb, quota_kb)));
    line(L"超出: ", format_flow_kb(flow_over_kb(snap.used_kb, quota_kb)));
  }
  line(L"余额: ", format_fee_yuan(snap.fee));
  line(L"更新: ", snap.last_success_text.empty() ? L"(无)" : snap.last_success_text);
  if (!snap.last_error.empty()) {
    line(L"错误: ", snap.last_error);
  }
  if (snap.state == MonitorState::NotLoggedIn) {
    line(L"状态: ", L"未登录");
  }
  return s;
}

}  // namespace ustb
