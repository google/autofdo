// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_RUN_COMMAND_H_
#define CHROMIUMOS_WIDE_PROFILING_RUN_COMMAND_H_

#include <string>
#include <vector>

#include "chromiumos-wide-profiling/compat/string.h"

namespace quipper {

// Executes |command|. stderr is directed to /dev/null. If |output| is not null,
// stdout is stored in |output|, and to /dev/null otherwise. Returns true iff
// the command was successfully executed and returned a successful status.
bool RunCommand(const std::vector<string>& command,
                std::vector<char>* output);

}  // nampspace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_RUN_COMMAND_H_
