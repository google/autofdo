#ifndef AUTOFDO_STATUS_CONSUMER_REGISTRY_H_
#define AUTOFDO_STATUS_CONSUMER_REGISTRY_H_

#include <functional>
#include <memory>
#include <vector>

#include "status_provider.h"
#include "third_party/abseil/absl/base/thread_annotations.h"
#include "third_party/abseil/absl/synchronization/mutex.h"

namespace devtools_crosstool_autofdo {

// Defines the consumer interface.
class StatusConsumer {
 public:
  virtual ~StatusConsumer() = default;

  // Starts the consumer with `status_provider` and `status_provider` must
  // outlive the consumer.
  virtual void Start(StatusProvider &status_provider) = 0;

  // Stops the consumer.
  virtual void Stop() = 0;
};

class StatusConsumerRegistry {
 public:
  // Retrieve the global registry instance.
  static StatusConsumerRegistry &GetInstance();
  StatusConsumerRegistry() = default;

  StatusConsumerRegistry(const StatusConsumerRegistry &) = delete;
  StatusConsumerRegistry(StatusConsumerRegistry &&) = delete;
  StatusConsumerRegistry & operator=(const StatusConsumerRegistry &) = delete;
  StatusConsumerRegistry & operator=(StatusConsumerRegistry &&) = delete;

  // Starts status consumers with `provider`.
  void Start(StatusProvider &provider) ABSL_LOCKS_EXCLUDED(mutex_);
  // Stops status consumers by calling "Stop" on each consumer.
  void Stop() ABSL_LOCKS_EXCLUDED(mutex_);

  // Puts `consumer` on the registry and takes ownership. Consumers are deleted
  // when ~StatusConsumerRegistry is executed.
  void Register(std::unique_ptr<StatusConsumer> consumer)
      ABSL_LOCKS_EXCLUDED(mutex_);

 private:
  absl::Mutex mutex_;
  std::vector<std::unique_ptr<StatusConsumer>> consumers_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_STATUS_CONSUMER_REGISTRY_H_
