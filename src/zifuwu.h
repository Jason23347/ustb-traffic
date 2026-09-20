#pragma once

#include "config.h"
#include "types.h"

#include <string>

namespace ustb {

struct ZifuwuFetchResult {
  bool ok = false;
  std::string body;
  std::wstring err;
};

// Fetch dashboard HTML, logging in when cookie is missing or older than 2h.
ZifuwuFetchResult zifuwu_fetch_dashboard(const Config& cfg);

// Drop in-memory session (e.g. when switching source or credentials).
void zifuwu_reset_session();

}  // namespace ustb
