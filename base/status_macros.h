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

#ifndef STATUS_MACROS_H_
#define STATUS_MACROS_H_

#if not defined(CONCAT)
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#endif

// Evaluates `exp`, which must evaluate to an `absl::StatusOr<T>`. If `exp`
// evaluates to a non-ok status, exits the current function by returning that
// status. Otherwise, stores `exp.value()` into `lhs`, which must be an lvalue.
#if not defined(ASSIGN_OR_RETURN)
#define ASSIGN_OR_RETURN(lhs, exp)                      \
  auto CONCAT(_status_or_value, __LINE__) = (exp);      \
  if (!(CONCAT(_status_or_value, __LINE__)).ok())       \
    return CONCAT(_status_or_value, __LINE__).status(); \
  lhs = std::move(CONCAT(_status_or_value, __LINE__).value());
#endif

// Evaluates `exp`, which must evaluate to an `absl::Status` or
// `absl::StatusOr`. If `exp` evaluates to a non-ok status, exits the current
// function by returning that status.
#if not defined(RETURN_IF_ERROR)
#define RETURN_IF_ERROR(exp)              \
  auto CONCAT(_status, __LINE__) = (exp); \
  if (!(CONCAT(_status, __LINE__)).ok()) return CONCAT(_status, __LINE__);
#endif

#endif  // STATUS_MACROS_H_
