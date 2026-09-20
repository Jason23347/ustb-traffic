#include "parser.h"

#include <cctype>
#include <cstring>

namespace ustb {
namespace {

size_t skip_space(const std::string& s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                          s[i] == '\n')) {
    ++i;
  }
  return i;
}

size_t skip_space_and_quotes(const std::string& s, size_t i) {
  i = skip_space(s, i);
  while (i < s.size() && (s[i] == '\'' || s[i] == '"')) {
    ++i;
    i = skip_space(s, i);
  }
  return i;
}

std::string trim_copy(std::string v) {
  size_t a = 0;
  while (a < v.size() && (v[a] == ' ' || v[a] == '\t')) {
    ++a;
  }
  size_t b = v.size();
  while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t')) {
    --b;
  }
  return v.substr(a, b - a);
}

}  // namespace

std::optional<uint64_t> extract_u64_field(const std::string& html,
                                          const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return std::nullopt;
  }
  const size_t pos0 = html.find(key);
  if (pos0 == std::string::npos) {
    return std::nullopt;
  }
  size_t i = skip_space_and_quotes(html, pos0 + std::strlen(key));
  if (i >= html.size() || !std::isdigit(static_cast<unsigned char>(html[i]))) {
    return std::nullopt;
  }
  uint64_t v = 0;
  while (i < html.size() && std::isdigit(static_cast<unsigned char>(html[i]))) {
    const uint64_t d = static_cast<uint64_t>(html[i] - '0');
    if (v > (UINT64_MAX - d) / 10) {
      return std::nullopt;
    }
    v = v * 10 + d;
    ++i;
  }
  return v;
}

std::optional<std::string> extract_quoted_field(const std::string& html,
                                                const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return std::nullopt;
  }
  const size_t pos0 = html.find(key);
  if (pos0 == std::string::npos) {
    return std::nullopt;
  }
  size_t i = skip_space(html, pos0 + std::strlen(key));
  if (i >= html.size()) {
    return std::string();
  }
  char quote = 0;
  if (html[i] == '\'' || html[i] == '"') {
    quote = html[i];
    ++i;
  }
  const size_t start = i;
  while (i < html.size()) {
    const char c = html[i];
    if (quote != 0 && c == quote) {
      break;
    }
    if (quote == 0 && (c == ';' || c == '\n' || c == '\r')) {
      break;
    }
    ++i;
  }
  return trim_copy(html.substr(start, i - start));
}

PortalInfo parse_portal_html(const std::string& html) {
  PortalInfo info;
  if (const auto flow = extract_u64_field(html, "flow=")) {
    info.has_flow = true;
    info.flow_kb = *flow;
  }
  if (const auto nid = extract_quoted_field(html, "NID=")) {
    info.has_nid = true;
    info.nid = *nid;
  }
  if (const auto uid = extract_quoted_field(html, "uid=")) {
    info.uid = *uid;
  }
  if (const auto fee = extract_u64_field(html, "fee=")) {
    if (*fee <= UINT32_MAX) {
      info.fee = static_cast<uint32_t>(*fee);
    }
  }
  return info;
}

PortalInfo parse_zifuwu_dashboard(const std::string& html) {
  PortalInfo info;

  // Name from: <h4>朱帅成，您好！</h4> (UTF-8 or GBK page)
  {
    const size_t h4 = html.find("<h4");
    if (h4 != std::string::npos) {
      const size_t gt = html.find('>', h4);
      const size_t end = (gt == std::string::npos)
                             ? std::string::npos
                             : html.find("</h4>", gt + 1);
      if (gt != std::string::npos && end != std::string::npos && end > gt + 1) {
        std::string inner = html.substr(gt + 1, end - (gt + 1));
        std::string flat;
        flat.reserve(inner.size());
        for (unsigned char c : inner) {
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
          }
          flat.push_back(static_cast<char>(c));
        }
        auto cut_at = [&](const char* needle) -> bool {
          const size_t p = flat.find(needle);
          if (p == std::string::npos || p == 0) {
            return false;
          }
          info.nid = flat.substr(0, p);
          return true;
        };
        // UTF-8 "，" / "您好" then GBK "，"(A3 AC) / "您好"(C4 FA BA C3)
        if (!cut_at("\xEF\xBC\x8C") && !cut_at(",") &&
            !cut_at("\xE6\x82\xA8\xE5\xA5\xBD") && !cut_at("\xA3\xAC") &&
            !cut_at("\xC4\xFA\xBA\xC3")) {
          if (!flat.empty()) {
            info.nid = flat;
          }
        }
        while (!info.nid.empty()) {
          const unsigned char c =
              static_cast<unsigned char>(info.nid.back());
          if (c == '!' || c == '?' || c == '.' || c == ' ' || c == '\t') {
            info.nid.pop_back();
            continue;
          }
          // UTF-8 fullwidth "！"
          if (info.nid.size() >= 3 &&
              info.nid.compare(info.nid.size() - 3, 3, "\xEF\xBC\x81") == 0) {
            info.nid.resize(info.nid.size() - 3);
            continue;
          }
          break;
        }
        if (!info.nid.empty()) {
          info.has_nid = true;
        }
      }
    }
  }

  // Prefer the "已用" label; fall back to the amber #f0ad4e strong value.
  const char* markers[] = {"已用:", "已用：", "已用"};
  size_t anchor = std::string::npos;
  for (const char* m : markers) {
    anchor = html.find(m);
    if (anchor != std::string::npos) {
      break;
    }
  }
  if (anchor == std::string::npos) {
    // GBK "已用"
    anchor = html.find("\xD2\xD1\xD3\xC3");
  }
  if (anchor == std::string::npos) {
    anchor = html.find("#f0ad4e");
  }
  if (anchor == std::string::npos) {
    return info;
  }

  const size_t strong = html.find("<strong", anchor);
  if (strong == std::string::npos || strong > anchor + 400) {
    return info;
  }
  const size_t gt = html.find('>', strong);
  if (gt == std::string::npos) {
    return info;
  }
  size_t i = gt + 1;
  while (i < html.size() &&
         (html[i] == ' ' || html[i] == '\t' || html[i] == '\r' ||
          html[i] == '\n')) {
    ++i;
  }
  if (i >= html.size() || !std::isdigit(static_cast<unsigned char>(html[i]))) {
    return info;
  }
  uint64_t v = 0;
  while (i < html.size() && std::isdigit(static_cast<unsigned char>(html[i]))) {
    const uint64_t d = static_cast<uint64_t>(html[i] - '0');
    if (v > (UINT64_MAX - d) / 10) {
      return info;
    }
    v = v * 10 + d;
    ++i;
  }
  // Unit is M (MB) on the dashboard; FlowMonitor expects KB.
  char unit = 0;
  if (i < html.size()) {
    unit = html[i];
  }
  uint64_t flow_kb = v;
  if (unit == 'M' || unit == 'm') {
    if (v > UINT64_MAX / 1024) {
      return info;
    }
    flow_kb = v * 1024;
  } else if (unit == 'G' || unit == 'g') {
    if (v > UINT64_MAX / (1024ull * 1024ull)) {
      return info;
    }
    flow_kb = v * 1024ull * 1024ull;
  } else if (unit == 'K' || unit == 'k') {
    flow_kb = v;
  }

  info.has_flow = true;
  info.flow_kb = flow_kb;
  info.has_nid = true;
  if (info.nid.empty()) {
    info.nid = "zifuwu";
  }
  return info;
}

}  // namespace ustb
