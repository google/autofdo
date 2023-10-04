#ifndef AUTOFDO_STATUS_MACROS_H
#define AUTOFDO_STATUS_MACROS_H

// This file defines the ASSIGN_OR_RETURN macro for abseil::status.

#define CONCAT(X,Y) __CONCAT(X,Y)

#define ASSIGN_OR_RETURN(lhs, exp)              \
  auto CONCAT(_status_or_value,__LINE__) = (exp);      \
  if (!CONCAT(_status_or_value,__LINE__).ok())         \
    return CONCAT(_status_or_value,__LINE__).status(); \
  lhs = std::move(CONCAT(_status_or_value,__LINE__).value());

#endif  // AUTOFDO_STATUS_MACROS_H
