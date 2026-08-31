#pragma once

#include <cstdint>
#include <string>

namespace ustb {

enum class UsageMode {
  Absolute = 0,
  OverQuota = 1,
  Percent = 2,
};

enum class TaskbarSide {
  Right = 0,
  Left = 1,
};

enum class MonitorState {
  Init,
  LoggedIn,
  NotLoggedIn,
  HttpError,
};

struct PortalInfo {
  bool has_flow = false;
  bool has_nid = false;
  uint64_t flow_kb = 0;
  std::string nid;
  std::string uid;
  uint32_t fee = 0;

  bool logged_in() const { return has_nid && !nid.empty(); }
};

struct DisplaySnapshot {
  MonitorState state = MonitorState::Init;
  uint64_t used_kb = 0;
  bool have_usage = false;
  double display_rate_kbps = 0;
  bool rate_valid = false;
  int fail_count = 0;
  std::wstring username;
  std::wstring nid;
  uint32_t fee = 0;
  std::wstring last_error;
  std::wstring last_success_text;
};

constexpr uint64_t kKb = 1;
constexpr uint64_t kMb = 1024;
constexpr uint64_t kGb = 1024 * 1024;
constexpr double kMaxRateKbps = 313.0 * 1024.0;
constexpr double kEmaAlpha = 0.4;
constexpr double kMinSampleDt = 0.2;
constexpr double kMaxSampleDt = 15.0;
constexpr int kFailHoldLimit = 3;
constexpr int kHttpTimeoutMs = 200;
constexpr int kBackoffCapMs = 30000;
constexpr unsigned kDefaultQuotaGb = 120;
constexpr unsigned kDefaultIntervalMs = 1000;
constexpr unsigned kMinIntervalMs = 500;
constexpr float kTaskbarFontPt = 8.0f;
constexpr unsigned kDefaultTaskbarPadPx = 2;
constexpr unsigned kDefaultTaskbarGapPx = 8;
constexpr unsigned kMaxTaskbarPadGapPx = 32;
constexpr unsigned kTaskbarValueWidthSlopPx = 2;

inline constexpr wchar_t kAppName[] = L"UstbTraffic";
inline constexpr wchar_t kAppVersion[] = L"1.0.0";
inline constexpr wchar_t kAppTagline[] =
    L"北京科技大学校园网流量任务栏监视器";
inline constexpr wchar_t kAppAuthor[] = L"Shuaicheng Zhu & Cursor";

}  // namespace ustb
