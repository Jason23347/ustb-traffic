#include "app.h"

#include "format.h"

namespace ustb {

App& app() {
  static App g;
  return g;
}

DisplaySnapshot App::copy_snap() {
  std::lock_guard<std::mutex> lock(mu);
  return snap;
}

void App::publish(const DisplaySnapshot& s) {
  std::lock_guard<std::mutex> lock(mu);
  snap = s;
}

uint64_t App::quota_kb() {
  std::lock_guard<std::mutex> lock(mu);
  return quota_to_kb(config.quota_gb);
}

}  // namespace ustb
