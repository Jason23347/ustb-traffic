#include "zifuwu.h"

#include "md5.h"
#include "types.h"

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>
#include <vector>

namespace ustb {
namespace {

constexpr wchar_t kHost[] = L"zifuwu.ustb.edu.cn";
constexpr INTERNET_PORT kPort = 443;
constexpr wchar_t kLoginPath[] = L"/Self/login";
constexpr wchar_t kRandomCodePath[] = L"/Self/login/randomCode";
constexpr wchar_t kVerifyPath[] = L"/Self/login/verify";
constexpr wchar_t kDashboardPath[] = L"/Self/dashboard";
constexpr double kCookieRefreshSec = 2.0 * 60.0 * 60.0;

std::mutex g_mu;
std::string g_cookie;
double g_login_at = 0;
std::wstring g_login_user;

double now_sec() {
  static const LARGE_INTEGER freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::wstring winhttp_err(DWORD code) {
  wchar_t buf[64];
  swprintf_s(buf, L"WinHTTP 错误 %u", code);
  return buf;
}

std::string wide_to_utf8(const std::wstring& s) {
  if (s.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

void cookie_set(std::string& jar, const std::string& name,
                const std::string& value) {
  std::string rebuilt;
  size_t i = 0;
  bool replaced = false;
  while (i < jar.size()) {
    size_t start = i;
    while (i < jar.size() && jar[i] == ' ') {
      ++i;
    }
    size_t name_start = i;
    while (i < jar.size() && jar[i] != '=' && jar[i] != ';') {
      ++i;
    }
    const std::string key = jar.substr(name_start, i - name_start);
    if (i < jar.size() && jar[i] == '=') {
      ++i;
    }
    size_t val_start = i;
    while (i < jar.size() && jar[i] != ';') {
      ++i;
    }
    const std::string val = jar.substr(val_start, i - val_start);
    if (i < jar.size() && jar[i] == ';') {
      ++i;
    }
    if (key == name) {
      if (!rebuilt.empty()) {
        rebuilt += "; ";
      }
      rebuilt += name;
      rebuilt += '=';
      rebuilt += value;
      replaced = true;
    } else if (!key.empty()) {
      if (!rebuilt.empty()) {
        rebuilt += "; ";
      }
      rebuilt += key;
      rebuilt += '=';
      rebuilt += val;
    }
    (void)start;
  }
  if (!replaced) {
    if (!rebuilt.empty()) {
      rebuilt += "; ";
    }
    rebuilt += name;
    rebuilt += '=';
    rebuilt += value;
  }
  jar = std::move(rebuilt);
}

void absorb_set_cookie(HINTERNET request, std::string& jar) {
  DWORD raw_size = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                      &raw_size, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || raw_size == 0) {
    return;
  }
  std::vector<wchar_t> raw(raw_size / sizeof(wchar_t) + 1);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                           WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return;
  }
  const std::wstring headers(raw.data());
  const std::wstring prefix = L"Set-Cookie:";
  size_t pos = 0;
  while (pos < headers.size()) {
    size_t line_end = headers.find(L"\r\n", pos);
    if (line_end == std::wstring::npos) {
      line_end = headers.size();
    }
    std::wstring line = headers.substr(pos, line_end - pos);
    pos = line_end + 2;
    if (line.size() < prefix.size()) {
      continue;
    }
    bool match = true;
    for (size_t i = 0; i < prefix.size(); ++i) {
      const wchar_t a = line[i];
      const wchar_t b = prefix[i];
      const wchar_t al =
          (a >= L'A' && a <= L'Z') ? static_cast<wchar_t>(a - L'A' + L'a') : a;
      const wchar_t bl =
          (b >= L'A' && b <= L'Z') ? static_cast<wchar_t>(b - L'A' + L'a') : b;
      if (al != bl) {
        match = false;
        break;
      }
    }
    if (!match) {
      continue;
    }
    size_t p = prefix.size();
    while (p < line.size() && line[p] == L' ') {
      ++p;
    }
    size_t eq = line.find(L'=', p);
    if (eq == std::wstring::npos) {
      continue;
    }
    size_t semi = line.find(L';', eq + 1);
    std::wstring wname = line.substr(p, eq - p);
    std::wstring wval =
        line.substr(eq + 1, (semi == std::wstring::npos ? line.size() : semi) -
                                (eq + 1));
    while (!wname.empty() && wname.back() == L' ') {
      wname.pop_back();
    }
    const std::string name = wide_to_utf8(wname);
    const std::string value = wide_to_utf8(wval);
    if (!name.empty()) {
      cookie_set(jar, name, value);
    }
  }
}

bool read_body(HINTERNET request, std::string& body, std::wstring& err) {
  body.clear();
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request, &avail)) {
      err = winhttp_err(GetLastError());
      return false;
    }
    if (avail == 0) {
      break;
    }
    const size_t old = body.size();
    body.resize(old + avail);
    DWORD read = 0;
    if (!WinHttpReadData(request, body.data() + old, avail, &read)) {
      err = winhttp_err(GetLastError());
      return false;
    }
    body.resize(old + read);
    if (body.size() > 2 * 1024 * 1024) {
      err = L"响应过大";
      return false;
    }
  }
  return true;
}

std::wstring ascii_to_wide(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

bool parse_location(const std::wstring& location, std::wstring& path_out) {
  if (location.empty()) {
    return false;
  }
  // Absolute URL → take path+query; relative → as-is.
  const size_t scheme = location.find(L"://");
  if (scheme != std::wstring::npos) {
    const size_t slash = location.find(L'/', scheme + 3);
    if (slash == std::wstring::npos) {
      path_out = L"/";
    } else {
      path_out = location.substr(slash);
    }
  } else if (location[0] == L'/') {
    path_out = location;
  } else {
    path_out = L"/";
    path_out += location;
  }
  return true;
}

bool query_location(HINTERNET request, std::wstring& location) {
  location.clear();
  DWORD size = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                      WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                      &size, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    return false;
  }
  std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                           WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return false;
  }
  location.assign(buf.data());
  return !location.empty();
}

bool http_exchange(const wchar_t* method, const wchar_t* path,
                   const std::string* post_body, std::string& cookie_jar,
                   bool follow_redirect, std::string& body, std::wstring& err,
                   DWORD* out_status, std::wstring* out_location = nullptr) {
  body.clear();
  if (out_status) {
    *out_status = 0;
  }
  if (out_location) {
    out_location->clear();
  }

  HINTERNET session = WinHttpOpen(L"UstbTraffic/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0);
  if (!session) {
    err = winhttp_err(GetLastError());
    return false;
  }
  WinHttpSetTimeouts(session, kZifuwuHttpTimeoutMs, kZifuwuHttpTimeoutMs,
                     kZifuwuHttpTimeoutMs, kZifuwuHttpTimeoutMs);

  HINTERNET connect = WinHttpConnect(session, kHost, kPort, 0);
  if (!connect) {
    err = winhttp_err(GetLastError());
    WinHttpCloseHandle(session);
    return false;
  }

  // Never let WinHTTP auto-follow: JSESSIONID is set on the 302 to
  // /Self/login and would be lost if we only read the final response.
  std::wstring cur_method = method;
  std::wstring cur_path = path;
  const std::string* cur_post = post_body;
  constexpr int kMaxRedirects = 8;

  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    HINTERNET request = WinHttpOpenRequest(
        connect, cur_method.c_str(), cur_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
      err = winhttp_err(GetLastError());
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }

    DWORD redir_never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redir_never,
                     sizeof(redir_never));

    std::wstring extra_headers;
    if (!cookie_jar.empty()) {
      extra_headers = L"Cookie: ";
      extra_headers += ascii_to_wide(cookie_jar);
      extra_headers += L"\r\n";
    }
    // Site JS uses ctx under /Self/; referer helps session binding.
    extra_headers += L"Referer: https://zifuwu.ustb.edu.cn/Self/login/\r\n";
    if (cur_post) {
      extra_headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
    }

    BOOL sent = FALSE;
    if (cur_post) {
      sent = WinHttpSendRequest(
          request,
          extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                : extra_headers.c_str(),
          extra_headers.empty() ? 0 : static_cast<DWORD>(-1),
          const_cast<char*>(cur_post->data()),
          static_cast<DWORD>(cur_post->size()),
          static_cast<DWORD>(cur_post->size()), 0);
    } else {
      sent = WinHttpSendRequest(
          request,
          extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                : extra_headers.c_str(),
          extra_headers.empty() ? 0 : static_cast<DWORD>(-1),
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
      err = winhttp_err(GetLastError());
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }

    absorb_set_cookie(request, cookie_jar);

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (out_status) {
      *out_status = status;
    }

    std::wstring location;
    const bool is_redirect =
        status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308;
    if (is_redirect) {
      query_location(request, location);
      if (out_location) {
        *out_location = location;
      }
    }

    if (is_redirect && follow_redirect && hop < kMaxRedirects) {
      if (location.empty() || !parse_location(location, cur_path)) {
        err = L"重定向缺少 Location";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
      }
      // POST → GET on 302/303 (common for login verify / session bounce).
      if (status == 302 || status == 303) {
        cur_method = L"GET";
        cur_post = nullptr;
      }
      // Drain body before next hop.
      std::string discard;
      std::wstring discard_err;
      read_body(request, discard, discard_err);
      WinHttpCloseHandle(request);
      continue;
    }

    if (!read_body(request, body, err)) {
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (status != 200 && status != 302) {
      wchar_t buf[48];
      swprintf_s(buf, L"HTTP %u", status);
      err = buf;
      return false;
    }
    return true;
  }

  err = L"重定向过多";
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return false;
}

bool extract_checkcode(const std::string& html, std::string& code) {
  // Match name="checkcode" ... value="xxxx" or value='xxxx' near checkcode.
  const char* keys[] = {"name=\"checkcode\"", "name='checkcode'",
                        "id=\"checkcode\"", "id='checkcode'"};
  size_t pos = std::string::npos;
  for (const char* key : keys) {
    pos = html.find(key);
    if (pos != std::string::npos) {
      break;
    }
  }
  // Fallback: first value="dddd" after checkcode text
  if (pos == std::string::npos) {
    pos = html.find("checkcode");
  }
  if (pos == std::string::npos) {
    return false;
  }
  const size_t value_pos = html.find("value", pos);
  if (value_pos == std::string::npos || value_pos > pos + 200) {
    return false;
  }
  size_t i = value_pos + 5;
  while (i < html.size() && (html[i] == ' ' || html[i] == '=')) {
    ++i;
  }
  if (i >= html.size()) {
    return false;
  }
  char quote = 0;
  if (html[i] == '\'' || html[i] == '"') {
    quote = html[i++];
  }
  const size_t start = i;
  while (i < html.size()) {
    if (quote != 0 && html[i] == quote) {
      break;
    }
    if (quote == 0 && (html[i] == ' ' || html[i] == '>' || html[i] == '/')) {
      break;
    }
    ++i;
  }
  code = html.substr(start, i - start);
  return !code.empty();
}

bool do_login(const Config& cfg, std::string& cookie_jar, std::wstring& err) {
  cookie_jar.clear();
  std::string body;
  DWORD status = 0;

  // 1) GET login page — obtain JSESSIONID (via 302) + hidden checkcode
  if (!http_exchange(L"GET", kLoginPath, nullptr, cookie_jar, true, body, err,
                     &status)) {
    return false;
  }
  if (cookie_jar.find("JSESSIONID=") == std::string::npos) {
    err = L"未获得会话 Cookie";
    return false;
  }
  std::string checkcode;
  if (!extract_checkcode(body, checkcode)) {
    // Match ustb-cli: first value="..."
    const size_t v = body.find("value=");
    if (v != std::string::npos) {
      size_t i = v + 6;
      char q = 0;
      if (i < body.size() && (body[i] == '"' || body[i] == '\'')) {
        q = body[i++];
      }
      const size_t start = i;
      while (i < body.size() && body[i] != q && body[i] != ' ' &&
             body[i] != '>') {
        ++i;
      }
      checkcode = body.substr(start, i - start);
    }
  }
  if (checkcode.empty()) {
    err = L"无法解析验证码字段";
    return false;
  }

  // 2) GET captcha once — required to arm the session; body unused
  {
    wchar_t captcha_path[128];
    swprintf_s(captcha_path, L"%s?t=%lu", kRandomCodePath,
               static_cast<unsigned long>(GetTickCount()));
    std::string captcha_body;
    if (!http_exchange(L"GET", captcha_path, nullptr, cookie_jar, true,
                       captcha_body, err, &status)) {
      return false;
    }
    (void)captcha_body;
  }

  const std::string user = wide_to_utf8(cfg.username);
  const std::string pass = wide_to_utf8(cfg.password);
  if (user.empty() || pass.empty()) {
    err = L"请填写学号和密码";
    return false;
  }
  const std::string pass_md5 = md5_hex(pass);
  if (pass_md5.size() != 32) {
    err = L"MD5 计算失败";
    return false;
  }

  // Same fields as ustb-cli devices_login / browser form (password = MD5)
  // Do not URL-encode alphanumerics; keep parity with ustb-cli raw POST body.
  std::string form;
  form.reserve(256);
  form += "foo=";
  form += user;
  form += "&bar=";
  form += pass;
  form += "&checkcode=";
  form += checkcode;
  form += "&account=";
  form += user;
  form += "&password=";
  form += pass_md5;
  form += "&code=";

  // 3) POST verify — do not follow redirects.
  // Note: both success and failure may 302 to /Self/login; do not judge by
  // Location. Caller confirms by fetching /Self/dashboard.
  std::wstring location;
  if (!http_exchange(L"POST", kVerifyPath, &form, cookie_jar, false, body, err,
                     &status, &location)) {
    return false;
  }
  (void)location;
  if (cookie_jar.find("JSESSIONID=") == std::string::npos) {
    err = L"登录未返回 Cookie";
    return false;
  }
  // Only treat an HTML login form in the POST body as an immediate failure.
  if (body.find("name=\"checkcode\"") != std::string::npos ||
      body.find("name='checkcode'") != std::string::npos) {
    err = L"登录失败（账号或密码错误）";
    return false;
  }
  return true;
}

bool session_valid_locked(const Config& cfg) {
  if (g_cookie.empty() || g_login_at <= 0) {
    return false;
  }
  if (g_login_user != cfg.username) {
    return false;
  }
  return (now_sec() - g_login_at) < kCookieRefreshSec;
}

}  // namespace

void zifuwu_reset_session() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_cookie.clear();
  g_login_at = 0;
  g_login_user.clear();
}

ZifuwuFetchResult zifuwu_fetch_dashboard(const Config& cfg) {
  ZifuwuFetchResult result;
  if (cfg.username.empty() || cfg.password.empty()) {
    result.err = L"请填写学号和密码";
    return result;
  }

  std::string cookie;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (session_valid_locked(cfg)) {
      cookie = g_cookie;
    }
  }

  if (cookie.empty()) {
    std::wstring err;
    std::string jar;
    if (!do_login(cfg, jar, err)) {
      result.err = err.empty() ? L"登录失败" : err;
      zifuwu_reset_session();
      return result;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_cookie = jar;
    g_login_at = now_sec();
    g_login_user = cfg.username;
    cookie = jar;
  }

  std::string body;
  std::wstring err;
  DWORD status = 0;
  std::string jar = cookie;
  if (!http_exchange(L"GET", kDashboardPath, nullptr, jar, true, body, err,
                     &status)) {
    result.err = err.empty() ? L"获取 dashboard 失败" : err;
    zifuwu_reset_session();
    return result;
  }

  // Update cookies from dashboard response.
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!jar.empty()) {
      g_cookie = jar;
    }
  }

  // Login page bounced back (no usage block).
  if (body.find("已用") == std::string::npos) {
    zifuwu_reset_session();
    if (body.find("name=\"checkcode\"") != std::string::npos ||
        body.find("/Self/login") != std::string::npos) {
      result.err = L"登录失败（账号或密码错误）";
    } else {
      result.err = L"无法解析已用流量";
    }
    return result;
  }

  result.ok = true;
  result.body = std::move(body);
  return result;
}

}  // namespace ustb
