#pragma once

#include "config.h"
#include "debounce.h"
#include "types.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace ustb {

struct App {
  Config config;
  std::mutex mu;
  DisplaySnapshot snap;
  FlowMonitor monitor;
  HWND hwnd = nullptr;
  HINSTANCE instance = nullptr;
  std::atomic<bool> running{true};
  UINT taskbar_created_msg = 0;

  DisplaySnapshot copy_snap();
  void publish(const DisplaySnapshot& s);
  uint64_t quota_kb();
};

App& app();

}  // namespace ustb
