#ifndef AUTOFDO_MATH_UTIL_H_
#define AUTOFDO_MATH_UTIL_H_

#include <algorithm>
#include <cstdio>
#include <limits>

namespace MathUtil {
constexpr double epsilon = 64 * std::numeric_limits<double>::epsilon();
bool AlmostEquals(double a, double b){
  if (fabs(a) <= std::numeric_limits<double>::epsilon() &&
      fabs(b) <= std::numeric_limits<double>::epsilon())
    return true;
  double diff = fabs(a - b);
  if (diff < epsilon) return true;
  double max = std::max(fabs(a), fabs(b));
  return fabs(a - b) < max * epsilon;
}
}  // namespace MathUtil

#endif  // AUTOFDO_MATH_UTIL_H_
