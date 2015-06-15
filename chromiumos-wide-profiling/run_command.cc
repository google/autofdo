// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/run_command.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "base/logging.h"

#include "chromiumos-wide-profiling/compat/string.h"

namespace quipper {

bool RunCommand(const std::vector<string>& command,
                std::vector<char>* output) {
  std::vector<char *> c_str_cmd;
  c_str_cmd.reserve(command.size() + 1);
  for (const auto& c : command) {
    // This cast is safe: POSIX states that exec shall not modify argv nor the
    // strings pointed to by argv.
    c_str_cmd.push_back(const_cast<char *>(c.c_str()));
  }
  c_str_cmd.push_back(nullptr);

  // Create pipe for stdout:
  int pipefd[2];
  if (output) {
    if (pipe(pipefd)) {
      PLOG(ERROR) << "pipe";
      return false;
    }
  }

  const pid_t child = fork();
  if (child == 0) {
    if (output) {
      if (close(pipefd[0]) < 0) {
        PLOG(FATAL) << "close read end of pipe";
      }
    }

    int devnull_fd = open("/dev/null", O_WRONLY);
    if (devnull_fd < 0) {
      PLOG(FATAL) << "open /dev/null";
    }

    int ret;
    ret = dup2(output ? pipefd[1] : devnull_fd, 1);
    if (ret < 0) {
      PLOG(FATAL) << "dup2 stdout";
    }

    ret = dup2(devnull_fd, 2);
    if (ret < 0) {
      PLOG(FATAL) << "dup2 stderr";
    }

    if (close(devnull_fd) < 0) {
      PLOG(FATAL) << "close /dev/null";
    }

    execvp(c_str_cmd[0], c_str_cmd.data());
    // No point logging an error, since it can't be seen.
    std::exit(EXIT_FAILURE);
  }

  // Read stdout from pipe.
  if (output) {
    if (close(pipefd[1]) < 0) {
      PLOG(FATAL) << "close write end of pipe";
    }
    static const int kReadSize = 4096;
    ssize_t read_sz;
    size_t read_off = output->size();
    do {
      output->resize(read_off + kReadSize);
      read_sz = read(pipefd[0], output->data() + read_off, kReadSize);
      if (read_sz < 0) {
        PLOG(FATAL) << "read";
        break;
      }
      read_off += read_sz;
    } while (read_sz > 0);
    output->resize(read_off);
  }

  // Wait for child.
  int exit_status;
  waitpid(child, &exit_status, 0);
  return WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0;
}

}  // namespace quipper
