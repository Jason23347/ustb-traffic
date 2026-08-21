#include "debounce.h"

#include "format.h"

namespace ustb {

void FlowMonitor::remember_identity(const PortalInfo& info) {
  snap_.nid = gbk_to_wide(info.nid);
  snap_.username = gbk_to_wide(info.uid);
  snap_.fee = info.fee;
}

void FlowMonitor::push_rate(double rate_kbps) {
  rates_[static_cast<size_t>(rate_i_)] = rate_kbps;
  rate_i_ = (rate_i_ + 1) % static_cast<int>(rates_.size());
  if (rate_n_ < static_cast<int>(rates_.size())) {
    ++rate_n_;
  }
  const double avg = window_average();
  if (!have_ema_) {
    ema_ = avg;
    have_ema_ = true;
  } else {
    ema_ = kEmaAlpha * avg + (1.0 - kEmaAlpha) * ema_;
  }
  snap_.display_rate_kbps = ema_;
  snap_.rate_valid = true;
}

double FlowMonitor::window_average() const {
  if (rate_n_ <= 0) {
    return 0;
  }
  double sum = 0;
  for (int i = 0; i < rate_n_; ++i) {
    sum += rates_[static_cast<size_t>(i)];
  }
  return sum / static_cast<double>(rate_n_);
}

DisplaySnapshot FlowMonitor::on_http_failure(const wchar_t* err) {
  ++snap_.fail_count;
  snap_.last_error = err ? err : L"HTTP 失败";
  if (snap_.fail_count > kFailHoldLimit) {
    snap_.state = MonitorState::HttpError;
    snap_.rate_valid = false;
  } else if (snap_.state == MonitorState::Init) {
    snap_.state = MonitorState::HttpError;
  }
  baseline_only_ = true;
  have_prev_ = false;
  return snap_;
}

DisplaySnapshot FlowMonitor::on_not_logged_in() {
  snap_.fail_count = 0;
  snap_.state = MonitorState::NotLoggedIn;
  snap_.rate_valid = false;
  snap_.have_usage = false;
  snap_.last_error.clear();
  baseline_only_ = true;
  have_prev_ = false;
  return snap_;
}

DisplaySnapshot FlowMonitor::on_sample(double t_sec, uint64_t flow_kb,
                                       const PortalInfo& info) {
  snap_.fail_count = 0;
  snap_.last_error.clear();
  remember_identity(info);
  snap_.last_success_text = now_text();

  if (flow_kb == 0 && snap_.have_usage && snap_.used_kb > 0) {
    snap_.state = MonitorState::LoggedIn;
    return snap_;
  }

  snap_.used_kb = flow_kb;
  snap_.have_usage = true;
  snap_.state = MonitorState::LoggedIn;

  if (!have_prev_ || baseline_only_) {
    prev_t_ = t_sec;
    prev_c_ = flow_kb;
    have_prev_ = true;
    baseline_only_ = false;
    return snap_;
  }

  const double dt = t_sec - prev_t_;
  if (dt < kMinSampleDt) {
    return snap_;
  }
  if (dt > kMaxSampleDt) {
    prev_t_ = t_sec;
    prev_c_ = flow_kb;
    snap_.rate_valid = false;
    return snap_;
  }
  if (flow_kb < prev_c_) {
    prev_t_ = t_sec;
    prev_c_ = flow_kb;
    snap_.rate_valid = false;
    return snap_;
  }

  const double inst = static_cast<double>(flow_kb - prev_c_) / dt;
  prev_t_ = t_sec;
  prev_c_ = flow_kb;

  if (inst > kMaxRateKbps) {
    snap_.display_rate_kbps = 0;
    snap_.rate_valid = true;
    return snap_;
  }

  push_rate(inst);
  return snap_;
}

}  // namespace ustb
