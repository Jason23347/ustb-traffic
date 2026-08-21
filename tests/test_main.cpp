#include "debounce.h"
#include "format.h"
#include "parser.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failed = 0;

void expect(bool cond, const char* name) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_failed;
  } else {
    std::printf("ok   %s\n", name);
  }
}

}  // namespace

int main() {
  using namespace ustb;

  {
    const std::string html =
        "<html><script>\n"
        "flow='118609527 ';\n"
        "NID='abc123';\n"
        "uid='user';\n"
        "fee='42';\n"
        "</script></html>";
    const PortalInfo info = parse_portal_html(html);
    expect(info.has_flow, "parse has_flow");
    expect(info.flow_kb == 118609527ull, "parse flow spaces");
    expect(info.logged_in(), "parse logged in");
    expect(info.nid == "abc123", "parse nid");
    expect(info.uid == "user", "parse uid");
    expect(info.fee == 42, "parse fee");
  }

  {
    const std::string html = "flow=\"100\"; NID='';";
    const PortalInfo info = parse_portal_html(html);
    expect(info.has_flow && info.flow_kb == 100, "parse double quote flow");
    expect(info.has_nid && !info.logged_in(), "empty nid not logged in");
  }

  {
    const PortalInfo info = parse_portal_html("<html>no script vars</html>");
    expect(!info.has_flow && !info.logged_in(), "missing fields");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    info.has_flow = true;
    auto s1 = m.on_sample(1.0, 1000, info);
    expect(s1.state == MonitorState::LoggedIn, "first sample logged in");
    expect(s1.have_usage && s1.used_kb == 1000, "first sample usage");
    expect(!s1.rate_valid, "first sample no rate");

    auto s2 = m.on_sample(2.0, 1124, info);
    expect(s2.rate_valid, "second sample rate");
    expect(std::fabs(s2.display_rate_kbps - 124.0) < 0.01, "rate 124 KB/s");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    m.on_sample(1.0, 5000, info);
    auto keep = m.on_sample(2.0, 0, info);
    expect(keep.used_kb == 5000, "zero-flow bug keeps usage");
    expect(keep.state == MonitorState::LoggedIn, "zero-flow still logged in");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    m.on_sample(1.0, 8000, info);
    auto wrap = m.on_sample(2.0, 100, info);
    expect(wrap.used_kb == 100, "wrap updates usage");
    expect(!wrap.rate_valid, "wrap does not produce rate");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    m.on_sample(1.0, 100, info);
    auto sleep = m.on_sample(20.0, 5000, info);
    expect(sleep.used_kb == 5000, "large dt updates usage");
    expect(!sleep.rate_valid, "large dt no rate");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    m.on_sample(1.0, 100, info);
    auto spike = m.on_sample(2.0, 100 + static_cast<uint64_t>(kMaxRateKbps) + 10,
                             info);
    expect(spike.rate_valid, "spike still shows a rate");
    expect(spike.display_rate_kbps == 0.0, "spike filtered to 0");
  }

  {
    FlowMonitor m;
    auto n = m.on_not_logged_in();
    expect(n.state == MonitorState::NotLoggedIn, "not logged in state");
    auto lines = format_taskbar_cells(n, UsageMode::Absolute, quota_to_kb(120));
    expect(lines.top_value == L"状态" && lines.bottom_value == L"未登录",
           "not logged in text");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    m.on_sample(1.0, 2000, info);
    m.on_sample(2.0, 2100, info);
    DisplaySnapshot last;
    for (int i = 0; i < 4; ++i) {
      last = m.on_http_failure(L"timeout");
    }
    expect(last.fail_count == 4, "fail count");
    expect(last.state == MonitorState::HttpError, "http error after 3");
    auto lines = format_taskbar_cells(last, UsageMode::Absolute, quota_to_kb(120));
    expect(lines.top_value == L"状态" && lines.bottom_value == L"--",
           "error text --");
  }

  {
    const uint64_t used = 118609527ull;
    expect(format_flow_kb(used).find(L"GB") != std::wstring::npos,
           "format GB");
    auto abs = format_taskbar_cells(
        [] {
          DisplaySnapshot s;
          s.state = MonitorState::LoggedIn;
          s.have_usage = true;
          s.used_kb = 120 * kGb / 2;
          s.rate_valid = true;
          s.display_rate_kbps = 4.16;
          return s;
        }(),
        UsageMode::Percent, quota_to_kb(120));
    expect(abs.top_label == L"Σ", "usage glyph");
    expect(abs.top_value.find(L"%") != std::wstring::npos, "percent line");
    const uint64_t q = quota_to_kb(120);
    expect(format_percent(q, q) == L"100.0%", "percent 100");
    expect(format_percent(q + q / 100, q) == L"+1.0%", "percent over 100");
    expect(format_fee_yuan(499200) == L"49.92 元", "fee to yuan");
    DisplaySnapshot tip;
    tip.username = L"123456";
    tip.nid = L"张三";
    tip.fee = 499200;
    const std::wstring hover = format_tooltip(tip, quota_to_kb(120));
    expect(hover.find(L"学号: 123456") != std::wstring::npos, "tooltip student id");
    expect(hover.find(L"姓名: 张三") != std::wstring::npos, "tooltip name");
    expect(hover.find(L"余额: 49.92 元") != std::wstring::npos, "tooltip balance");
    expect(usage_meter_level(q * 69 / 100, q) == MeterLevel::Normal,
           "usage under 70");
    expect(usage_meter_level(q * 70 / 100, q) == MeterLevel::Warn,
           "usage 70 yellow");
    expect(usage_meter_level(q * 90 / 100, q) == MeterLevel::Danger,
           "usage 90 red");
    expect(speed_meter_level(2.0 * kMb) == MeterLevel::Normal, "speed 2MB white");
    expect(speed_meter_level(2.01 * kMb) == MeterLevel::Warn, "speed over 2MB");
    expect(speed_meter_level(8.01 * kMb) == MeterLevel::Danger, "speed over 8MB");
  }

  if (g_failed) {
    std::fprintf(stderr, "%d test(s) failed\n", g_failed);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
