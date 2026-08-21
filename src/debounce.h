#pragma once

#include "types.h"

#include <array>

namespace ustb {

class FlowMonitor {
 public:
  DisplaySnapshot on_http_failure(const wchar_t* err);
  DisplaySnapshot on_not_logged_in();
  DisplaySnapshot on_sample(double t_sec, uint64_t flow_kb, const PortalInfo& info);
  DisplaySnapshot current() const { return snap_; }

 private:
  void remember_identity(const PortalInfo& info);
  void push_rate(double rate_kbps);
  double window_average() const;

  DisplaySnapshot snap_;
  bool have_prev_ = false;
  double prev_t_ = 0;
  uint64_t prev_c_ = 0;
  bool baseline_only_ = true;
  std::array<double, 3> rates_{};
  int rate_n_ = 0;
  int rate_i_ = 0;
  double ema_ = 0;
  bool have_ema_ = false;
};

}  // namespace ustb
