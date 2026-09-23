#include "debounce.h"
#include "format.h"
#include "md5.h"
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
    const std::string html =
        "<div class=\"thumbnail\">"
        "<div class=\"caption\">"
        "<h4>朱帅成，\n                            您好！</h4>"
        "<p><label>防伪信息：</label></p>"
        "</div></div>"
        "<div class=\"row text-center\" style=\"margin-top: 15px;\">"
        "<div class=\"col-xs-4\"><small>"
        "<span style=\"color: #337ab7;\">总量:</span> "
        "<strong style=\"color: #337ab7; font-size: 16px;\">122880M</strong>"
        "</small></div>"
        "<div class=\"col-xs-4\"><small>"
        "<span style=\"color: #f0ad4e;\">已用:</span> "
        "<strong style=\"color: #f0ad4e; font-size: 16px;\">141340M</strong>"
        "</small></div>"
        "<div class=\"col-xs-4\"><small>"
        "<span style=\"color: #5cb85c;\">剩余:</span> "
        "<strong style=\"color: #5cb85c; font-size: 16px;\">0M</strong>"
        "</small></div></div>"
        "<div class=\"col-md-3 col-xs-6 col-xxxs-12\">"
        "<dl>"
        "<dt>\n                                                    19.94\n"
        "                                                    <small class=\"unit\">\n"
        "                                                        元</small>\n"
        "                                                </dt>"
        "<dd>账户余额</dd>"
        "</dl>"
        "</div>";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.has_flow, "zifuwu has_flow");
    expect(info.logged_in(), "zifuwu logged in");
    expect(info.flow_kb == 141340ull * 1024ull, "zifuwu used MB to KB");
    expect(info.nid == "朱帅成", "zifuwu name from h4");
    expect(info.fee == 199400u, "zifuwu fee yuan*10000");
  }

  {
    // Live dashboard cards (dl/dt/dd) + progress panel; must prefer 已用流量 card
    // over #f0ad4e progress-bar / 总量 strong.
    const std::string html =
        "<div class=\"col-xs-12 user-info1\">"
        "<div class=\"col-md-3 col-xs-6 col-xxxs-12\"><dl>"
        "<dt>\n                                                    155047\n"
        "                                                    <small class=\"unit\">M</small>\n"
        "                                                </dt>"
        "<dd>已用流量</dd></dl></div>"
        "<div class=\"col-md-3 col-xs-6 col-xxxs-12\"><dl>"
        "<dt>33233<small class=\"unit\">M</small></dt>"
        "<dd>可用流量=免费流量+资金可使用流量</dd></dl></div>"
        "<div class=\"col-md-3 col-xs-6 col-xxxs-12\"><dl>"
        "<dt>未设置</dt><dd>消费保护</dd></dl></div>"
        "<div class=\"col-md-3 col-xs-6 col-xxxs-12\"><dl>"
        "<dt>\n                                                    19.94\n"
        "                                                    <small class=\"unit\">\n"
        "                                                        元</small>\n"
        "                                                </dt>"
        "<dd>账户余额</dd></dl></div>"
        "<div class=\"progress-bar\" style=\"background-color: #f0ad4e;\"></div>"
        "<div class=\"progress-bar\" style=\"background-color: #5cb85c;\"></div>"
        "<span style=\"color: #337ab7;\">总量:</span> "
        "<strong style=\"color: #337ab7; font-size: 16px;\">122880M</strong>"
        "<span style=\"color: #f0ad4e;\">已用:</span> "
        "<strong style=\"color: #f0ad4e; font-size: 16px;\">155047M</strong>"
        "<span style=\"color: #5cb85c;\">剩余:</span> "
        "<strong style=\"color: #5cb85c; font-size: 16px;\">0M</strong>"
        "</div>";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.has_flow, "card has_flow");
    expect(info.flow_kb == 155047ull * 1024ull, "card used 155047M");
    expect(info.fee == 199400u, "card fee 19.94");
  }

  {
    // Entity-encoded labels (as sometimes emitted by Java backends).
    const std::string html =
        "<dl><dt>19.94<small class=\"unit\">&#20803;</small></dt>"
        "<dd>&#x8D26;&#x6237;&#x4F59;&#x989D;</dd></dl>"
        "<dl><dt>155047<small class=\"unit\">M</small></dt>"
        "<dd>&#x5DF2;&#x7528;&#x6D41;&#x91CF;</dd></dl>";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.fee == 199400u, "entity fee 19.94");
    expect(info.has_flow && info.flow_kb == 155047ull * 1024ull,
           "entity used flow");
  }

  {
    // English UI (Accept-Language / server locale) uses Balance / Used Flow.
    const std::string html =
        "<div class=\"col-xs-12 user-info1\">"
        "<div class=\"col-md-3\"><dl>"
        "<dt>155179<small class=\"unit\">M</small></dt>"
        "<dd>Used Flow</dd></dl></div>"
        "<div class=\"col-md-3\"><dl>"
        "<dt>19.86<small class=\"unit\">Yuan</small></dt>"
        "<dd>Balance</dd></dl></div></div>";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.has_flow && info.flow_kb == 155179ull * 1024ull,
           "english Used Flow");
    expect(info.fee == 198600u, "english Balance 19.86");
  }

  {
    // JSON leftMoney is package credit — must NOT override 账户余额 card.
    const std::string html =
        "{\"leftFlow\":0,\"leftMoney\":21.44,\"leftTime\":0}"
        "<div class=\"col-md-3\"><dl>"
        "<dt>19.94<small class=\"unit\">\n元</small></dt>"
        "<dd>账户余额</dd></dl></div>";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.fee == 199400u, "prefer card 19.94 over leftMoney 21.44");
  }

  {
    const std::string html =
        "{\"leftFlow\":0,\"leftMoney\":21.44,\"leftTime\":0}";
    const PortalInfo info = parse_zifuwu_dashboard(html);
    expect(info.fee == 0u, "leftMoney alone is not 账户余额");
  }

  {
    FlowMonitor m;
    PortalInfo info;
    info.has_nid = true;
    info.nid = "n";
    info.has_flow = true;
    info.uid = "u";
    auto s1 = m.on_sample(1.0, 5000, 1000, info);
    expect(s1.have_usage && s1.used_kb == 5000, "split usage first");
    expect(!s1.rate_valid, "split rate baseline");
    auto s2 = m.on_sample(2.0, 5000, 1124, info);
    expect(s2.used_kb == 5000, "split usage unchanged");
    expect(s2.rate_valid, "split rate from rate_kb");
    expect(std::fabs(s2.display_rate_kbps - 124.0) < 1.0, "split rate value");
  }

  {
    expect(md5_hex("password") == "5f4dcc3b5aa765d61d8327deb882cf99",
           "md5 password");
    expect(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e", "md5 empty");
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
