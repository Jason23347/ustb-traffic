#include "poller.h"

#include "app.h"
#include "parser.h"
#include "types.h"

#include <windows.h>
#include <winhttp.h>

#include <thread>

namespace ustb {
namespace {

HANDLE g_stop = nullptr;
std::thread g_thread;

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

void poller_loop() {
  while (app().running.load()) {
    Config cfg;
    {
      std::lock_guard<std::mutex> lock(app().mu);
      cfg = app().config;
    }

    std::string body;
    std::wstring err;
    DisplaySnapshot snap;
    if (!http_get(cfg, body, err)) {
      std::lock_guard<std::mutex> lock(app().mu);
      snap = app().monitor.on_http_failure(err.c_str());
      app().snap = snap;
    } else {
      const PortalInfo info = parse_portal_html(body);
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

    if (app().hwnd) {
      InvalidateRect(app().hwnd, nullptr, FALSE);
    }

    int wait_ms = static_cast<int>(cfg.interval_ms);
    if (snap.fail_count > 0) {
      wait_ms = backoff_ms(snap.fail_count, cfg.interval_ms);
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
}

}  // namespace ustb
