/*
 Copyright 2024 Google LLC

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      https://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

#ifndef STATUS_MATCHERS_H_
#define STATUS_MATCHERS_H_

#include "gmock/gmock.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"

#if not defined(CONCAT)
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#endif  // CONCAT

#ifndef GTEST_HAS_STATUS_MATCHERS
namespace testing {
namespace status {
namespace status_internal {

inline const absl::Status& GetStatus(const ::absl::Status& status) {
  return status;
}

template <typename T>
inline const absl::Status& GetStatus(const ::absl::StatusOr<T>& status) {
  return status.status();
}

// Monomorphic implementation of matcher IsOkAndHolds(m).  StatusOrType is a
// reference to StatusOr<T>.
template <typename StatusOrType>
class IsOkAndHoldsMatcherImpl : public testing::MatcherInterface<StatusOrType> {
 public:
  typedef
      typename std::remove_reference<StatusOrType>::type::value_type value_type;

  template <typename InnerMatcher>
  explicit IsOkAndHoldsMatcherImpl(InnerMatcher&& inner_matcher)
      : inner_matcher_(testing::SafeMatcherCast<const value_type&>(
            std::forward<InnerMatcher>(inner_matcher))) {}

  void DescribeTo(std::ostream* os) const override {
    *os << "is OK and has a value that ";
    inner_matcher_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const override {
    *os << "isn't OK or has a value that ";
    inner_matcher_.DescribeNegationTo(os);
  }

  bool MatchAndExplain(
      StatusOrType actual_value,
      testing::MatchResultListener* result_listener) const override {
    if (!actual_value.ok()) {
      *result_listener << "which has status " << actual_value.status();
      return false;
    }

    testing::StringMatchResultListener inner_listener;
    const bool matches =
        inner_matcher_.MatchAndExplain(*actual_value, &inner_listener);
    const std::string inner_explanation = inner_listener.str();
    if (!inner_explanation.empty()) {
      *result_listener << "which contains value "
                       << testing::PrintToString(*actual_value) << ", "
                       << inner_explanation;
    }
    return matches;
  }

 private:
  const testing::Matcher<const value_type&> inner_matcher_;
};

// Implements IsOkAndHolds(m) as a polymorphic matcher.
template <typename InnerMatcher>
class IsOkAndHoldsMatcher {
 public:
  explicit IsOkAndHoldsMatcher(InnerMatcher inner_matcher)
      : inner_matcher_(std::move(inner_matcher)) {}

  // Converts this polymorphic matcher to a monomorphic matcher of the
  // given type.  StatusOrType can be either StatusOr<T> or a
  // reference to StatusOr<T>.
  template <typename StatusOrType>
  operator testing::Matcher<StatusOrType>() const {  // NOLINT
    return testing::Matcher<StatusOrType>(
        new IsOkAndHoldsMatcherImpl<const StatusOrType&>(inner_matcher_));
  }

 private:
  const InnerMatcher inner_matcher_;
};

////////////////////////////////////////////////////////////
// Implementation of StatusIs().
//
// StatusIs() is a polymorphic matcher. This class is the common
// implementation of it shared by all types T where StatusIs() can be used as
// a Matcher<T>.

class StatusIsMatcherCommonImpl {
 public:
  StatusIsMatcherCommonImpl(
      testing::Matcher<const absl::StatusCode> code_matcher,
      testing::Matcher<const std::string&> message_matcher)
      : code_matcher_(std::move(code_matcher)),
        message_matcher_(std::move(message_matcher)) {}

  void DescribeTo(std::ostream* os) const {
    *os << "has a status code that ";
    code_matcher_.DescribeTo(os);
    *os << ", and has an error message that ";
    message_matcher_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const {
    *os << "has a status code that ";
    code_matcher_.DescribeNegationTo(os);
    *os << ", and has an error message that ";
    message_matcher_.DescribeNegationTo(os);
  }

  bool MatchAndExplain(const absl::Status& status,
                       testing::MatchResultListener* result_listener) const {
    testing::StringMatchResultListener inner_listener;
    inner_listener.Clear();
    if (!code_matcher_.MatchAndExplain(
            static_cast<absl::StatusCode>(status.code()), &inner_listener)) {
      *result_listener << (inner_listener.str().empty()
                               ? "whose status code is wrong"
                               : "which has a status code " +
                                     inner_listener.str());
      return false;
    }

    if (!message_matcher_.Matches(std::string(status.message()))) {
      *result_listener << "whose error message is wrong";
      return false;
    }

    return true;
  }

 private:
  const testing::Matcher<const absl::StatusCode> code_matcher_;
  const testing::Matcher<const std::string&> message_matcher_;
};

// Monomorphic implementation of matcher StatusIs() for a given type T. T can
// be Status, StatusOr<>, or a reference to either of them.
template <typename T>
class MonoStatusIsMatcherImpl : public testing::MatcherInterface<T> {
 public:
  explicit MonoStatusIsMatcherImpl(StatusIsMatcherCommonImpl common_impl)
      : common_impl_(std::move(common_impl)) {}

  void DescribeTo(std::ostream* os) const override {
    common_impl_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const override {
    common_impl_.DescribeNegationTo(os);
  }

  bool MatchAndExplain(
      T actual_value,
      testing::MatchResultListener* result_listener) const override {
    return common_impl_.MatchAndExplain(GetStatus(actual_value),
                                        result_listener);
  }

 private:
  StatusIsMatcherCommonImpl common_impl_;
};

// Implements StatusIs() as a polymorphic matcher.
class StatusIsMatcher {
 public:
  StatusIsMatcher(testing::Matcher<const absl::StatusCode> code_matcher,
                  testing::Matcher<const std::string&> message_matcher)
      : common_impl_(
            testing::MatcherCast<const absl::StatusCode>(code_matcher),
            testing::MatcherCast<const std::string&>(message_matcher)) {}

  // Converts this polymorphic matcher to a monomorphic matcher of the given
  // type. T can be StatusOr<>, Status, or a reference to either of them.
  template <typename T>
  operator testing::Matcher<T>() const {  // NOLINT
    return testing::MakeMatcher(new MonoStatusIsMatcherImpl<T>(common_impl_));
  }

 private:
  const StatusIsMatcherCommonImpl common_impl_;
};

// Monomorphic implementation of matcher IsOk() for a given type T.
// T can be Status, StatusOr<>, or a reference to either of them.
template <typename T>
class MonoIsOkMatcherImpl : public testing::MatcherInterface<T> {
 public:
  void DescribeTo(std::ostream* os) const override { *os << "is OK"; }
  void DescribeNegationTo(std::ostream* os) const override {
    *os << "is not OK";
  }
  bool MatchAndExplain(T actual_value,
                       testing::MatchResultListener*) const override {
    return GetStatus(actual_value).ok();
  }
};

// Implements IsOk() as a polymorphic matcher.
class IsOkMatcher {
 public:
  template <typename T>
  operator testing::Matcher<T>() const {  // NOLINT
    return testing::Matcher<T>(new MonoIsOkMatcherImpl<T>());
  }
};
}  // namespace status_internal

// Returns a gMock matcher that matches a StatusOr<> whose status is
// OK and whose value matches the inner matcher.
template <typename InnerMatcher>
status_internal::IsOkAndHoldsMatcher<typename std::decay<InnerMatcher>::type>
IsOkAndHolds(InnerMatcher&& inner_matcher) {
  return status_internal::IsOkAndHoldsMatcher<
      typename std::decay<InnerMatcher>::type>(
      std::forward<InnerMatcher>(inner_matcher));
}

// Returns a matcher that matches a Status or StatusOr<> whose status code
// matches code_matcher, and whose error message matches message_matcher.
template <typename CodeMatcher, typename MessageMatcher>
status_internal::StatusIsMatcher StatusIs(CodeMatcher code_matcher,
                                          MessageMatcher message_matcher) {
  return status_internal::StatusIsMatcher(std::move(code_matcher),
                                          std::move(message_matcher));
}

template <typename CodeMatcher>
status_internal::StatusIsMatcher StatusIs(CodeMatcher code_matcher) {
  return StatusIs(std::move(code_matcher), testing::_);
}

// Returns a gMock matcher that matches a Status or StatusOr<> which is OK.
inline status_internal::IsOkMatcher IsOk() {
  return status_internal::IsOkMatcher();
}

// Macros for testing the results of functions that return absl::Status or
// absl::StatusOr<T> (for any type T).
#ifndef EXPECT_OK
#define EXPECT_OK(expression) EXPECT_THAT(expression, testing::status::IsOk())
#endif  // EXPECT_OK

#ifndef ASSERT_OK
#define ASSERT_OK(expression) ASSERT_THAT(expression, testing::status::IsOk())
#endif  // ASSERT_OK

#ifndef ASSERT_OK_AND_ASSIGN
#define ASSERT_OK_AND_ASSIGN(lhs, exp)             \
  auto CONCAT(_status_or_value, __LINE__) = (exp); \
  ASSERT_OK(CONCAT(_status_or_value, __LINE__));   \
  lhs = std::move(CONCAT(_status_or_value, __LINE__).value());
#endif

}  // namespace status
}  // namespace testing
#endif  // GTEST_HAS_STATUS_MATCHERS
#endif  // STATUS_MATCHERS_H_
