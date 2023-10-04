#include "status_consumer_registry.h"

#include <memory>
#include <utility>

#include "status_provider.h"
#include "third_party/abseil/absl/synchronization/mutex.h"

namespace devtools_crosstool_autofdo {

StatusConsumerRegistry &StatusConsumerRegistry::GetInstance() {
  static StatusConsumerRegistry *registry = new StatusConsumerRegistry();
  return *registry;
}

void StatusConsumerRegistry::Start(StatusProvider &provider) {
  absl::MutexLock lock(&mutex_);
  for (auto &consumer : consumers_) consumer->Start(provider);
}

void StatusConsumerRegistry::Stop() {
  absl::MutexLock lock(&mutex_);
  for (auto &consumer : consumers_) consumer->Stop();
}

void StatusConsumerRegistry::Register(
    std::unique_ptr<StatusConsumer> consumer) {
  absl::MutexLock lock(&mutex_);
  consumers_.push_back(std::move(consumer));
}

}  // namespace devtools_crosstool_autofdo
