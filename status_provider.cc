#include "status_provider.h"

#include <string>

#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {
std::string MultiStatusProvider::GetJob() const {
  StatusProvider *const sp = FindWorkingStatusProvider();
  if (sp == nullptr)
    return job_;
  return absl::StrFormat("%s - %s", job_, sp->GetJob());
}

// Get current status, the status that has its progress < 100.
int MultiStatusProvider::GetProgress() const {
  // No sub status is added, return 100 (to avoid div by 0 error)
  if (total_weight_ == 0) return 100;
  int i = 0, n_status = weighted_statuses_.size(), finished_weight = 0;
  while (i < n_status && weighted_statuses_[i].status_provider->IsDone()) {
    finished_weight += weighted_statuses_[i].weight;
    ++i;
  }
  if (i == n_status) return 100;
  int w = weighted_statuses_[i].weight;
  int p = weighted_statuses_[i].status_provider->GetProgress();
  return (finished_weight + static_cast<float>(w) * p / 100) * 100 /
         total_weight_;
}
}  // namespace devtools_crosstool_autofdo
