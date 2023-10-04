#ifndef AUTOFDO_STATUS_PROVIDER_H_
#define AUTOFDO_STATUS_PROVIDER_H_

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

// Defines an interface that provides status upon request.
class StatusProvider {
 public:
  virtual ~StatusProvider() = default;
  // Returns a descriptive string for the job this status represents.
  virtual std::string GetJob() const = 0;
  // Returns the percentage done, in the range of [0, 100].
  virtual int GetProgress() const = 0;
  virtual bool IsDone() const = 0;
  virtual void SetDone() = 0;
};

// Defines a default status provider that represents a single job.
class DefaultStatusProvider : public StatusProvider {
 public:
  explicit DefaultStatusProvider(absl::string_view job)
      : job_(job), progress_(0) {}

  DefaultStatusProvider(const DefaultStatusProvider &&) = delete;
  DefaultStatusProvider(DefaultStatusProvider &&) = delete;
  DefaultStatusProvider & operator=(const DefaultStatusProvider &) = delete;
  DefaultStatusProvider & operator=(DefaultStatusProvider &&) = delete;

  std::string GetJob() const override { return job_; }
  int GetProgress() const override { return progress_; }
  bool IsDone() const override { return progress_ >= 100; }

  void SetProgress(int p) { progress_ = p; }
  void SetDone() override { SetProgress(100); }

 private:
  const std::string job_;
  int progress_;
};

// MultiStatusProvider reports status based on multiple sub-statuses. Suppose
// we have the following 3 jobs with different weight.
//
//   Job_1, weight=10
//   Job_2, weight=20
//   Job_3, weight=30
//
// When job_1 is done, job_2 is at progress=10, then MultiStatusProvider is at
// progress 20%. 20% is calculated as: (10 + 20 * 10%) / (10 + 20 + 30).
//
// More than 1 level of nest is also possible.
class MultiStatusProvider : public StatusProvider {
 public:
  struct WeightedStatusProvider {
    int weight;
    std::unique_ptr<StatusProvider> status_provider;
  };
  explicit MultiStatusProvider(absl::string_view job) : job_(job) {}

  MultiStatusProvider(const MultiStatusProvider &) = default;
  MultiStatusProvider(MultiStatusProvider &&) = default;
  MultiStatusProvider &operator=(const MultiStatusProvider &) = default;
  MultiStatusProvider &operator=(MultiStatusProvider &&) = default;

  std::string GetJob() const override;
  int GetProgress() const override;
  bool IsDone() const override {
    return FindWorkingStatusProvider() == nullptr;
  }

  void SetDone() override {
    for (auto &p : weighted_statuses_) p.status_provider->SetDone();
  }

  // Adds a sub status provider. The new status provider is owned by "this"
  // instance. Note, a MutiStatusProvider is also allowed here.
  void AddStatusProvider(int weight,
                         std::unique_ptr<StatusProvider> sub_provider) {
    weighted_statuses_.push_back({weight, std::move(sub_provider)});
    total_weight_ += weight;
  }

 private:
  // Returns the first StatusProvider that is still avtive (IsDone() == false).
  // When all sub status providers are done, this returns nullptr.
  StatusProvider *const FindWorkingStatusProvider() const {
    auto i = absl::c_find_if(weighted_statuses_,
                             [](const WeightedStatusProvider &p) {
                               return !p.status_provider->IsDone();
                             });
    if (i == weighted_statuses_.end()) return nullptr;
    return i->status_provider.get();
  }

  std::string job_;
  std::vector<WeightedStatusProvider> weighted_statuses_;
  int total_weight_ = 0;
};
}  // namespace devtools_crosstool_autofdo



#endif  // AUTOFDO_STATUS_PROVIDER_H_
