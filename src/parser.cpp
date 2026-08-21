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

}  // namespace ustb
