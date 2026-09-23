#include "poller.h"

#include "app.h"
#include "config.h"
#include "parser.h"
#include "types.h"
#include "zifuwu.h"

#include <windows.h>
#include <winhttp.h>

#include <thread>

namespace ustb {
namespace {

HANDLE g_stop = nullptr;
std::thread g_thread;
TrafficSource g_last_source = TrafficSource::Portal82;
std::wstring g_last_user;

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

int backoff_ms(int fail_count, unsigned base_ms) {
  if (fail_count <= 0) {
    return static_cast<int>(base_ms);
  }
  unsigned long long v = base_ms;
  for (int i = 0; i < fail_count && v < kBackoffCapMs; ++i) {
    v *= 2;
  }
  if (v > kBackoffCapMs) {
    v = kBackoffCapMs;
  }
  return static_cast<int>(v);
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

bool http_get(const Config& cfg, std::string& body, std::wstring& err) {
  body.clear();
  HINTERNET session = WinHttpOpen(L"UstbTraffic/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0);
  if (!session) {
    err = winhttp_err(GetLastError());
    return false;
  }

  WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs,
                     kHttpTimeoutMs);

  HINTERNET connect =
      WinHttpConnect(session, cfg.host.c_str(),
                     static_cast<INTERNET_PORT>(cfg.port), 0);
  if (!connect) {
    err = winhttp_err(GetLastError());
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", cfg.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!request) {
    err = winhttp_err(GetLastError());
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
    err = winhttp_err(GetLastError());
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                      WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    wchar_t buf[48];
    swprintf_s(buf, L"HTTP %u", status);
    err = buf;
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request, &avail)) {
      err = winhttp_err(GetLastError());
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
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
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
    body.resize(old + read);
    if (body.size() > 1024 * 1024) {
      err = L"响应过大";
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return true;
}

void maybe_reset_zifuwu(const Config& cfg) {
  const bool uses_zifuwu = traffic_source_needs_zifuwu_cred(cfg.traffic_source);
  if (cfg.traffic_source != g_last_source ||
      (uses_zifuwu && cfg.username != g_last_user)) {
    zifuwu_reset_session();
  }
  g_last_source = cfg.traffic_source;
  if (uses_zifuwu) {
    g_last_user = cfg.username;
  } else {
    g_last_user.clear();
  }
}

void apply_portal_info(PortalInfo info, DisplaySnapshot& snap) {
  std::lock_guard<std::mutex> lock(app().mu);
  if (!info.logged_in() || !info.has_flow) {
    if (!info.logged_in()) {
      snap = app().monitor.on_not_logged_in();
    } else {
      snap = app().monitor.on_http_failure(L"无法解析 flow");
    }
  } else {
    snap = app().monitor.on_sample(now_sec(), info.flow_kb, info);
  }
  app().snap = snap;
}

void apply_hybrid_sample(const PortalInfo& identity, uint64_t rate_kb,
                         const PortalInfo& zifuwu, DisplaySnapshot& snap) {
  PortalInfo merged;
  merged.has_nid = identity.has_nid;
  merged.nid = identity.nid;
  merged.uid = identity.uid;
  merged.has_flow = zifuwu.has_flow;
  merged.flow_kb = zifuwu.flow_kb;
  merged.fee = zifuwu.fee;

  std::lock_guard<std::mutex> lock(app().mu);
  snap = app().monitor.on_sample(now_sec(), zifuwu.flow_kb, rate_kb, merged);
  app().snap = snap;
}

bool http_get_portal_host(const wchar_t* host, std::string& body,
                          std::wstring& err) {
  Config portal_cfg;
  portal_cfg.host = host;
  portal_cfg.port = 80;
  portal_cfg.path = L"/";
  portal_cfg.use_https = false;
  return http_get(portal_cfg, body, err);
}

void poll_hybrid(const Config& cfg, DisplaySnapshot& snap) {
  std::string identity_body;
  std::wstring identity_err;
  if (!http_get_portal_host(L"202.204.48.82", identity_body, identity_err)) {
    std::lock_guard<std::mutex> lock(app().mu);
    snap = app().monitor.on_http_failure(identity_err.c_str());
    app().snap = snap;
    return;
  }

  const PortalInfo identity = parse_portal_html(identity_body);
  if (!identity.logged_in()) {
    std::lock_guard<std::mutex> lock(app().mu);
    snap = app().monitor.on_not_logged_in();
    app().snap = snap;
    return;
  }

  uint64_t rate_kb = 0;
  if (cfg.speed_source == SpeedSource::Portal66) {
    std::string speed_body;
    std::wstring speed_err;
    if (!http_get_portal_host(L"202.204.48.66", speed_body, speed_err)) {
      std::lock_guard<std::mutex> lock(app().mu);
      snap = app().monitor.on_http_failure(speed_err.c_str());
      app().snap = snap;
      return;
    }
    const PortalInfo speed_portal = parse_portal_html(speed_body);
    if (!speed_portal.has_flow) {
      std::lock_guard<std::mutex> lock(app().mu);
      snap = app().monitor.on_http_failure(L"无法解析 48.66 flow（速率源）");
      app().snap = snap;
      return;
    }
    rate_kb = speed_portal.flow_kb;
  } else {
    if (!identity.has_flow) {
      std::lock_guard<std::mutex> lock(app().mu);
      snap = app().monitor.on_http_failure(L"无法解析 48.82 flow（速率源）");
      app().snap = snap;
      return;
    }
    rate_kb = identity.flow_kb;
  }

  const ZifuwuFetchResult fetched = zifuwu_fetch_dashboard(cfg);
  if (!fetched.ok) {
    std::lock_guard<std::mutex> lock(app().mu);
    snap = app().monitor.on_http_failure(
        fetched.err.empty() ? L"自服务请求失败" : fetched.err.c_str());
    app().snap = snap;
    return;
  }

  const PortalInfo zifuwu = parse_zifuwu_dashboard(fetched.body);
  if (!zifuwu.has_flow) {
    zifuwu_reset_session();
    std::lock_guard<std::mutex> lock(app().mu);
    snap = app().monitor.on_http_failure(L"无法解析已用流量");
    app().snap = snap;
    return;
  }

  apply_hybrid_sample(identity, rate_kb, zifuwu, snap);
}

void poller_loop() {
  while (app().running.load()) {
    const double loop_start = now_sec();

    Config cfg;
    {
      std::lock_guard<std::mutex> lock(app().mu);
      cfg = app().config;
    }
    maybe_reset_zifuwu(cfg);

    DisplaySnapshot snap;
    if (cfg.traffic_source == TrafficSource::Hybrid) {
      poll_hybrid(cfg, snap);
    } else if (cfg.traffic_source == TrafficSource::Zifuwu) {
      const ZifuwuFetchResult fetched = zifuwu_fetch_dashboard(cfg);
      if (!fetched.ok) {
        std::lock_guard<std::mutex> lock(app().mu);
        snap = app().monitor.on_http_failure(
            fetched.err.empty() ? L"自服务请求失败" : fetched.err.c_str());
        app().snap = snap;
      } else {
        PortalInfo info = parse_zifuwu_dashboard(fetched.body);
        if (!info.has_flow) {
          zifuwu_reset_session();
          std::lock_guard<std::mutex> lock(app().mu);
          snap = app().monitor.on_http_failure(L"无法解析已用流量");
          app().snap = snap;
        } else {
          // uid → tooltip「用户」(学号); nid →「姓名」
          info.uid = wide_to_utf8(cfg.username);
          apply_portal_info(info, snap);
        }
      }
    } else {
      std::string body;
      std::wstring err;
      if (!http_get(cfg, body, err)) {
        std::lock_guard<std::mutex> lock(app().mu);
        snap = app().monitor.on_http_failure(err.c_str());
        app().snap = snap;
      } else {
        apply_portal_info(parse_portal_html(body), snap);
      }
    }

    if (app().hwnd) {
      InvalidateRect(app().hwnd, nullptr, FALSE);
    }

    int target_ms = static_cast<int>(cfg.interval_ms);
    if (snap.fail_count > 0) {
      target_ms = backoff_ms(snap.fail_count, cfg.interval_ms);
    }
    int wait_ms =
        target_ms - static_cast<int>((now_sec() - loop_start) * 1000.0);
    if (wait_ms < 0) {
      wait_ms = 0;
    }
    const DWORD wr = WaitForSingleObject(g_stop, static_cast<DWORD>(wait_ms));
    if (wr == WAIT_OBJECT_0) {
      break;
    }
  }
}

}  // namespace

void start_poller() {
  if (g_thread.joinable()) {
    return;
  }
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_thread = std::thread(poller_loop);
}

void stop_poller() {
  app().running = false;
  if (g_stop) {
    SetEvent(g_stop);
  }
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (g_stop) {
    CloseHandle(g_stop);
    g_stop = nullptr;
  }
  zifuwu_reset_session();
}

}  // namespace ustb
