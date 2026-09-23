#include "parser.h"

#include <cctype>
#include <cstring>
#include <string>

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

namespace {

// Dashboard cards:
//   <dt>155047<small class="unit">M</small></dt><dd>已用流量</dd>
//   <dt>19.94<small class="unit">元</small></dt><dd>账户余额</dd>
struct DtValue {
  uint64_t int_part = 0;
  uint32_t frac = 0;       // up to 4 digits, already scaled (19.94 → 9400)
  int frac_digits = 0;     // digits actually read before padding
  char unit = 0;           // letter after number, or from <small class="unit">
  bool ok = false;
};

size_t find_label(const std::string& html, const char* utf8_label,
                  const char* gbk_label) {
  size_t pos = html.find(utf8_label);
  if (pos == std::string::npos && gbk_label != nullptr) {
    pos = html.find(gbk_label);
  }
  return pos;
}

bool extract_dt_before_label(const std::string& html, size_t label_pos,
                             DtValue& out) {
  out = {};
  if (label_pos == std::string::npos) {
    return false;
  }
  const size_t search_start = label_pos > 800 ? label_pos - 800 : 0;
  size_t dt = html.rfind("<dt", label_pos);
  if (dt == std::string::npos) {
    dt = html.rfind("<DT", label_pos);
  }
  if (dt == std::string::npos || dt < search_start) {
    return false;
  }
  const size_t gt = html.find('>', dt);
  if (gt == std::string::npos || gt >= label_pos) {
    return false;
  }

  size_t i = gt + 1;
  auto skip_tag = [&]() {
    if (i < label_pos && html[i] == '<') {
      const size_t tag_open = i;
      while (i < label_pos && html[i] != '>') {
        ++i;
      }
      if (i < label_pos) {
        ++i;
      }
      // Unit letter may live inside <small class="unit">M</small>.
      std::string tag = html.substr(tag_open, i - tag_open);
      for (char& ch : tag) {
        if (ch >= 'A' && ch <= 'Z') {
          ch = static_cast<char>(ch - 'A' + 'a');
        }
      }
      if (out.unit == 0 &&
          (tag.find("unit") != std::string::npos ||
           tag.find("small") != std::string::npos)) {
        size_t j = i;
        while (j < label_pos &&
               (html[j] == ' ' || html[j] == '\t' || html[j] == '\r' ||
                html[j] == '\n')) {
          ++j;
        }
        if (j < label_pos) {
          const unsigned char c = static_cast<unsigned char>(html[j]);
          if (std::isalpha(c)) {
            out.unit = static_cast<char>(c);
          } else if (j + 2 < label_pos &&
                     html.compare(j, 3, "\xE5\x85\x83") == 0) {
            out.unit = 'Y';
          } else if (j + 1 < label_pos &&
                     html.compare(j, 2, "\xD4\xAA") == 0) {
            out.unit = 'Y';
          }
        }
      }
      return true;
    }
    return false;
  };

  auto is_digit = [](unsigned char c) {
    return std::isdigit(c) != 0;
  };
  // Fullwidth digits ０-９ are UTF-8 EF BC 90-99.
  auto read_digit = [&](size_t& at, uint64_t& dest) -> bool {
    if (at >= label_pos) {
      return false;
    }
    const unsigned char c = static_cast<unsigned char>(html[at]);
    if (is_digit(c)) {
      dest = dest * 10 + (c - '0');
      ++at;
      return true;
    }
    if (at + 2 < label_pos &&
        static_cast<unsigned char>(html[at]) == 0xEF &&
        static_cast<unsigned char>(html[at + 1]) == 0xBC &&
        static_cast<unsigned char>(html[at + 2]) >= 0x90 &&
        static_cast<unsigned char>(html[at + 2]) <= 0x99) {
      dest = dest * 10 + (html[at + 2] - 0x90);
      at += 3;
      return true;
    }
    return false;
  };
  auto is_dot = [&](size_t at) -> size_t {
    // returns byte length of decimal separator, or 0
    if (at >= label_pos) {
      return 0;
    }
    if (html[at] == '.' || html[at] == ',') {
      return 1;
    }
    // UTF-8 fullwidth ．
    if (at + 2 < label_pos &&
        html.compare(at, 3, "\xEF\xBC\x8E") == 0) {
      return 3;
    }
    return 0;
  };

  while (i < label_pos) {
    if (skip_tag()) {
      continue;
    }
    uint64_t probe = 0;
    size_t t = i;
    if (read_digit(t, probe)) {
      break;
    }
    ++i;
  }
  if (i >= label_pos) {
    return false;
  }

  while (i < label_pos) {
    size_t t = i;
    uint64_t next = out.int_part;
    if (!read_digit(t, next)) {
      break;
    }
    if (next < out.int_part) {
      return false;  // overflow
    }
    out.int_part = next;
    i = t;
  }
  if (const size_t dot_len = is_dot(i)) {
    i += dot_len;
    while (i < label_pos && out.frac_digits < 4) {
      size_t t = i;
      uint64_t dig = 0;
      if (!read_digit(t, dig)) {
        break;
      }
      // read_digit appends into dig starting from 0 → single digit value
      // but our read_digit does dest = dest*10 + d with dest starting 0, so dig is the digit
      out.frac = out.frac * 10u + static_cast<uint32_t>(dig);
      ++out.frac_digits;
      i = t;
    }
  }
  while (out.frac_digits < 4) {
    out.frac *= 10u;
    ++out.frac_digits;
  }

  while (i < label_pos) {
    if (skip_tag()) {
      continue;
    }
    if (html[i] == ' ' || html[i] == '\t' || html[i] == '\r' ||
        html[i] == '\n') {
      ++i;
      continue;
    }
    if (out.unit == 0 && std::isalpha(static_cast<unsigned char>(html[i]))) {
      out.unit = html[i];
    }
    break;
  }

  out.ok = true;
  return true;
}

void parse_zifuwu_fee(PortalInfo& info, const std::string& html) {
  auto apply = [&](const DtValue& v) -> bool {
    if (!v.ok) {
      return false;
    }
    if (v.int_part > (UINT32_MAX - v.frac) / 10000ull) {
      return false;
    }
    info.fee = static_cast<uint32_t>(v.int_part * 10000ull + v.frac);
    return true;
  };

  auto try_label = [&](const char* utf8, const char* gbk) -> bool {
    size_t pos = 0;
    for (;;) {
      size_t bal = html.find(utf8, pos);
      if (bal == std::string::npos && gbk != nullptr) {
        bal = html.find(gbk, pos);
      }
      if (bal == std::string::npos) {
        return false;
      }
      size_t dl_open = html.rfind("<dl", bal);
      if (dl_open == std::string::npos) {
        dl_open = html.rfind("<DL", bal);
      }
      size_t dl_close = html.find("</dl>", bal);
      if (dl_close == std::string::npos) {
        dl_close = html.find("</DL>", bal);
      }
      DtValue v;
      if (dl_open != std::string::npos && dl_close != std::string::npos &&
          bal < dl_close) {
        size_t dt = html.rfind("<dt", bal);
        if (dt == std::string::npos) {
          dt = html.rfind("<DT", bal);
        }
        if (dt != std::string::npos && dt >= dl_open &&
            extract_dt_before_label(html, bal, v) && apply(v)) {
          return true;
        }
      } else if (extract_dt_before_label(html, bal, v) && apply(v)) {
        return true;
      }
      pos = bal + 1;
    }
  };

  // Primary: same <dl> card as the live dashboard —
  //   <dt>19.94<small class="unit">元</small></dt><dd>账户余额</dd>
  // English UI variant: <dd>Balance</dd>
  // Do NOT use JSON leftMoney — that is package credit, not account balance.
  auto try_yuan_balance_card = [&]() -> bool {
    const char* yuan_u8 = "\xE5\x85\x83";
    const char* yuan_gbk = "\xD4\xAA";
    size_t pos = 0;
    for (;;) {
      size_t yuan = html.find(yuan_u8, pos);
      if (yuan == std::string::npos) {
        yuan = html.find(yuan_gbk, pos);
      }
      if (yuan == std::string::npos) {
        // English pages may omit 元 glyph; fall through to label search.
        break;
      }
      // Must be the card unit (<small class="unit">元), not "0.0006元/MB".
      const size_t small = html.rfind("<small", yuan);
      if (small == std::string::npos || yuan - small > 80) {
        pos = yuan + 1;
        continue;
      }
      const std::string small_tag = html.substr(small, yuan - small);
      if (small_tag.find("unit") == std::string::npos) {
        pos = yuan + 1;
        continue;
      }
      size_t dl_open = html.rfind("<dl", yuan);
      if (dl_open == std::string::npos) {
        dl_open = html.rfind("<DL", yuan);
      }
      size_t dl_close = html.find("</dl>", yuan);
      if (dl_close == std::string::npos) {
        dl_close = html.find("</DL>", yuan);
      }
      if (dl_open == std::string::npos || dl_close == std::string::npos ||
          yuan > dl_close) {
        pos = yuan + 1;
        continue;
      }
      const std::string card = html.substr(dl_open, dl_close - dl_open);
      const bool has_bal_label =
          card.find("余额") != std::string::npos ||
          card.find("\xD3\xE0\xB6\xEE") != std::string::npos ||
          card.find("Balance") != std::string::npos ||
          card.find("balance") != std::string::npos;
      if (!has_bal_label) {
        pos = yuan + 1;
        continue;
      }
      size_t label = card.find("账户余额");
      if (label == std::string::npos) {
        label = card.find("帐户余额");
      }
      if (label == std::string::npos) {
        label = card.find("余额");
      }
      if (label == std::string::npos) {
        label = card.find("\xD3\xE0\xB6\xEE");
      }
      if (label == std::string::npos) {
        label = card.find("Balance");
      }
      if (label == std::string::npos) {
        label = card.find("balance");
      }
      if (label == std::string::npos) {
        pos = yuan + 1;
        continue;
      }
      DtValue v;
      if (extract_dt_before_label(html, dl_open + label, v) && apply(v)) {
        return true;
      }
      pos = yuan + 1;
    }
    return false;
  };

  auto try_balance_dd_card = [&]() -> bool {
    // Match <dd>Balance</dd> / <dd>账户余额</dd> even if unit text is odd.
    const char* labels[] = {"账户余额", "帐户余额", "Balance", "balance",
                            "余额"};
    for (const char* lab : labels) {
      if (try_label(lab, nullptr)) {
        return true;
      }
    }
    return try_label("账户余额", "\xD5\xCB\xBB\xA7\xD3\xE0\xB6\xEE");
  };

  if (try_yuan_balance_card()) {
    return;
  }
  if (try_balance_dd_card()) {
    return;
  }
}

bool parse_zifuwu_flow_from_dl(PortalInfo& info, const std::string& html) {
  // Prefer exact card label; support Chinese and English UI.
  size_t label =
      find_label(html, "已用流量", "\xD2\xD1\xD3\xC3\xC1\xF7\xC1\xBF");
  if (label == std::string::npos) {
    label = html.find("Used Flow");
  }
  if (label == std::string::npos) {
    return false;
  }
  DtValue v;
  if (!extract_dt_before_label(html, label, v)) {
    return false;
  }
  uint64_t flow_kb = v.int_part;
  const char unit = v.unit;
  if (unit == 'M' || unit == 'm') {
    if (v.int_part > UINT64_MAX / 1024) {
      return false;
    }
    flow_kb = v.int_part * 1024;
  } else if (unit == 'G' || unit == 'g') {
    if (v.int_part > UINT64_MAX / (1024ull * 1024ull)) {
      return false;
    }
    flow_kb = v.int_part * 1024ull * 1024ull;
  } else if (unit == 'K' || unit == 'k' || unit == 0) {
    // Bare number without unit: treat as MB (dashboard cards use M).
    if (unit == 0) {
      if (v.int_part > UINT64_MAX / 1024) {
        return false;
      }
      flow_kb = v.int_part * 1024;
    }
  } else {
    return false;
  }
  info.has_flow = true;
  info.flow_kb = flow_kb;
  return true;
}

bool parse_zifuwu_flow_from_strong(PortalInfo& info, const std::string& html) {
  // Prefer colon forms so bare "已用" does not hit "已用流量" / comments first.
  const char* markers[] = {"已用:", "已用："};
  size_t anchor = std::string::npos;
  for (const char* m : markers) {
    anchor = html.find(m);
    if (anchor != std::string::npos) {
      break;
    }
  }
  if (anchor == std::string::npos) {
    // GBK "已用:" (D2 D1 D3 C3 3A)
    anchor = html.find("\xD2\xD1\xD3\xC3:");
  }
  if (anchor == std::string::npos) {
    return false;
  }

  const size_t strong = html.find("<strong", anchor);
  if (strong == std::string::npos || strong > anchor + 400) {
    return false;
  }
  const size_t gt = html.find('>', strong);
  if (gt == std::string::npos) {
    return false;
  }
  size_t i = gt + 1;
  while (i < html.size() &&
         (html[i] == ' ' || html[i] == '\t' || html[i] == '\r' ||
          html[i] == '\n')) {
    ++i;
  }
  if (i >= html.size() || !std::isdigit(static_cast<unsigned char>(html[i]))) {
    return false;
  }
  uint64_t v = 0;
  while (i < html.size() && std::isdigit(static_cast<unsigned char>(html[i]))) {
    const uint64_t d = static_cast<uint64_t>(html[i] - '0');
    if (v > (UINT64_MAX - d) / 10) {
      return false;
    }
    v = v * 10 + d;
    ++i;
  }
  char unit = 0;
  if (i < html.size()) {
    unit = html[i];
  }
  uint64_t flow_kb = v;
  if (unit == 'M' || unit == 'm') {
    if (v > UINT64_MAX / 1024) {
      return false;
    }
    flow_kb = v * 1024;
  } else if (unit == 'G' || unit == 'g') {
    if (v > UINT64_MAX / (1024ull * 1024ull)) {
      return false;
    }
    flow_kb = v * 1024ull * 1024ull;
  } else if (unit == 'K' || unit == 'k') {
    flow_kb = v;
  } else {
    return false;
  }

  info.has_flow = true;
  info.flow_kb = flow_kb;
  return true;
}

void append_utf8_codepoint(std::string& out, uint32_t cp) {
  if (cp < 0x80u) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

// Decode &#NNN; / &#xHH; / &nbsp; so label searches work on entity-encoded pages.
std::string decode_html_text_entities(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  for (size_t i = 0; i < html.size();) {
    if (html[i] == '&' && i + 3 < html.size() && html[i + 1] == '#') {
      size_t j = i + 2;
      uint32_t cp = 0;
      bool ok = false;
      if (j < html.size() && (html[j] == 'x' || html[j] == 'X')) {
        ++j;
        const size_t start = j;
        while (j < html.size() &&
               std::isxdigit(static_cast<unsigned char>(html[j]))) {
          const unsigned char c = static_cast<unsigned char>(html[j]);
          cp *= 16u;
          if (c >= '0' && c <= '9') {
            cp += static_cast<uint32_t>(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            cp += 10u + static_cast<uint32_t>(c - 'a');
          } else {
            cp += 10u + static_cast<uint32_t>(c - 'A');
          }
          ++j;
          if (cp > 0x10FFFFu) {
            break;
          }
        }
        ok = j > start && j < html.size() && html[j] == ';';
      } else {
        const size_t start = j;
        while (j < html.size() &&
               std::isdigit(static_cast<unsigned char>(html[j]))) {
          cp = cp * 10u + static_cast<uint32_t>(html[j] - '0');
          ++j;
          if (cp > 0x10FFFFu) {
            break;
          }
        }
        ok = j > start && j < html.size() && html[j] == ';';
      }
      if (ok) {
        append_utf8_codepoint(out, cp);
        i = j + 1;
        continue;
      }
    }
    if (html.compare(i, 6, "&nbsp;") == 0 ||
        html.compare(i, 6, "&NBSP;") == 0) {
      out.push_back(' ');
      i += 6;
      continue;
    }
    out.push_back(html[i]);
    ++i;
  }
  return out;
}

PortalInfo parse_zifuwu_dashboard_body(const std::string& html) {
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

  parse_zifuwu_fee(info, html);

  // Prefer top-of-page dl cards; fall back to progress-panel <strong>.
  if (!parse_zifuwu_flow_from_dl(info, html)) {
    parse_zifuwu_flow_from_strong(info, html);
  }

  if (info.has_flow) {
    info.has_nid = true;
    if (info.nid.empty()) {
      info.nid = "zifuwu";
    }
  }
  return info;
}

}  // namespace

PortalInfo parse_zifuwu_dashboard(const std::string& html) {
  // Always decode &#…; so label matches work when the server entity-encodes CJK.
  return parse_zifuwu_dashboard_body(decode_html_text_entities(html));
}

}  // namespace ustb
