#!/usr/grte/v3/bin/python2.7
# Author: sque@google.com (Simon Que)
#
"""Copies files from Chrome OS CWP repo to google3 CWP repo.
"""

import logging
import os
import shutil
import subprocess
import tempfile

import google3

from google3.pyglib import app
from google3.pyglib import flags

FLAGS = flags.FLAGS

# Chrome OS CWP repo URL.
# TODO(sque): move to a data file.
GERRIT_URL = ("https://chromium.googlesource.com/chromiumos/platform2")
CWP_PLATFORM2_PATH = "chromiumos-wide-profiling"

flags.DEFINE_string("dest_path", "third_party/quipper",
                    "Path to google3 quipper repo")

flags.DEFINE_string("source_path", "",
                    "Path to upstream quipper directory if it already "
                    "exists on the local filesystem")


class QuipperImporter(object):
  """A class to sync quipper.

  Quipper is an opensource project that lives upstream. It is also used within
  Google. This class helps bring in upstream changes to google3.
  """

  # These are files not to copy.
  # TODO(sque): move to a data file.
  BLACKLIST = [
      "OWNERS",
      "common.mk",
      "quipper_proto.h",
      "quipper_string.h",
      "quipper_test.h",
      "test_utils_defs.cc",
  ]

  def __init__(self, dest_dir, source_dir=None):
    self._dest_dir = dest_dir
    self._source_dir = source_dir

  def _GetHeadCommit(self, repository):
    return RunCommand(("git", "rev-parse", "HEAD"), cwd=repository)

  def _CloneUpstream(self):
    clone_dir = tempfile.mkdtemp()
    RunCommand(("git", "clone", GERRIT_URL, clone_dir))
    print "Synced to", self._GetHeadCommit(clone_dir)
    return clone_dir

  def _ReplaceIncludePath(self, file_path):
    with open(file_path, "r") as in_file:
      lines = in_file.readlines()

    with open(file_path, "w") as out_file:
      for line in lines:
        out_file.write(
            line.replace("#include \"%s/" % CWP_PLATFORM2_PATH, "#include \""))

  def _InsertExternalTestPathsDefine(self, file_path):
    with open(file_path, "r") as in_file:
      lines = in_file.readlines()

    # Write the #define after the copyright notice.
    with open(file_path, "w") as out_file:
      i = iter(lines)
      in_copyright = False
      for line in i:
        if not in_copyright and line.startswith("// Copyright"):
          in_copyright = True
        elif in_copyright and not line.startswith("//"):
          out_file.write("#define QUIPPER_EXTERNAL_TEST_PATHS  "
                         "// inserted by sync_quipper.py\n")
          out_file.write(line)
          break
        out_file.write(line)
      # copy out the rest of the file
      for line in i:
        out_file.write(line)

  def Import(self):
    """Imports code from upstream to google3.

    If no source directory is given, it clones the upstream repository and uses
    that as the input source.
    """
    if not self._source_dir:
      clone_dir = self._CloneUpstream()
    else:
      clone_dir = self._source_dir
      print "Source path is synced to", self._GetHeadCommit(clone_dir)

    # Now that CWP code has been moved to platform2, append the CWP directory to
    # the cloned path.
    clone_dir = os.path.join(clone_dir, CWP_PLATFORM2_PATH)

    try:
      # Iterate through the files.
      out = subprocess.check_output(("git", "ls-files", "-c"), cwd=clone_dir)
      for path in out.splitlines():
        dest_file = os.path.join(FLAGS.dest_path, path)
        # Create directories that don't yet exist in the destination path.
        dest_dir = os.path.join(FLAGS.dest_path, os.path.dirname(path))
        if not os.path.isdir(dest_dir):
          os.makedirs(dest_dir, 0755)
        if path in self.BLACKLIST:
          continue
        shutil.copy(os.path.join(clone_dir, path), dest_file)
        if any(path.endswith(suffix) for suffix in (".cc", ".h")):
          self._ReplaceIncludePath(dest_file)
        if path == "test_utils.cc":
          self._InsertExternalTestPathsDefine(dest_file)
    finally:
      # Clean up the temporary directory if it was a temporary one.
      if not self._source_dir:
        shutil.rmtree(clone_dir)


def RunCommand(command_args, cwd=None):
  """Function that runs a command and returns stdout.

  Args:
    command_args: an array of the command arguments.
    cwd: Run command from this working directory.

  Returns:
    None
  Raises:
    subprocess.CalledProcessError: if command returns non-zero status.
  """

  logging.info("Running command: %s", command_args)
  p = subprocess.Popen(command_args, stdout=subprocess.PIPE, cwd=cwd)
  stdout = p.communicate()[0]
  if p.returncode != 0:
    # Emulate check_output
    raise subprocess.CalledProcessError(
        p.returncode, command_args, output=stdout)
  return stdout.strip()


def main(_):
  importer = QuipperImporter(FLAGS.dest_path, FLAGS.source_path)
  importer.Import()


# COV_NF_START
if __name__ == "__main__":
  app.run()
# COV_NF_END
