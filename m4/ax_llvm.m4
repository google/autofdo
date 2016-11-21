# ===========================================================================
#     Based on http://www.gnu.org/software/autoconf-archive/ax_llvm.html
#           Modifications by Diego Novillo <dnovillo@google.com>
# ===========================================================================
#
# SYNOPSIS
#
#   AX_LLVM([llvm-libs])
#
# DESCRIPTION
#
#   Test for the existance of llvm, and make sure that it can be linked with
#   the llvm-libs argument that is passed on to llvm-config i.e.:
#
#     llvm-config --libs <llvm-libs>
#
#   llvm-config will also include any libraries that are depended upon.
#
# LICENSE
#
#   Copyright (c) 2008 Andy Kitchen <agimbleinthewabe@gmail.com>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 12

AC_DEFUN([AX_LLVM],
[
  AC_ARG_WITH(
    [llvm],
    AS_HELP_STRING(
      [--with-llvm@<:@=PATH-TO-LLVM-CONFIG@:>@],
      [ use LLVM libraries (default is yes). It is possible to specify the
        full path to the llvm-config binary. If not given, llvm-config will be
        searched in your path.
      ]),
    [
      if test "$withval" = "no"; then
        want_llvm="no"
      elif test "$withval" = "yes"; then
        want_llvm="yes"
        ac_llvm_config_path=$(which llvm-config)
      else
        want_llvm="yes"
        ac_llvm_config_path="$withval"
      fi
    ],
    [want_llvm="yes"]
  )

  succeeded=no
  if test -z "$ac_llvm_config_path"; then
    ac_llvm_config_path=$(which llvm-config)
  fi

  if test "x$want_llvm" = "xyes"; then
    if test -f "$ac_llvm_config_path"; then
      LLVM_CXXFLAGS=$($ac_llvm_config_path --cppflags)
      shared_mode=$($ac_llvm_config_path --shared-mode)
      rpath=""
      if test "x$shared_mode" = "xstatic"; then
        LLVM_LDFLAGS="$($ac_llvm_config_path --ldflags) \
                      $($ac_llvm_config_path --libs $1) \
                      -ldl -lpthread -ltinfo"
      elif test "x$shared_mode" = "xshared"; then
        rpath="$($ac_llvm_config_path --libdir)"
        LLVM_LDFLAGS="-Wl,-rpath $rpath \
                      $($ac_llvm_config_path --ldflags) \
                      $($ac_llvm_config_path --libs $1)"
      fi

      AC_REQUIRE([AC_PROG_CXX])
      CXXFLAGS_SAVED="$CXXFLAGS"
      CXXFLAGS="$CXXFLAGS $LLVM_CXXFLAGS"
      export CXXFLAGS

      LDFLAGS_SAVED="$LDFLAGS"
      LDFLAGS="$LDFLAGS $LLVM_LDFLAGS"
      export LDFLAGS

      AC_CACHE_CHECK(
        whether we can compile and link with llvm([$1]),
        ax_cv_llvm,
        [
          AC_LANG_PUSH([C++])
          AC_LINK_IFELSE(
            [
              AC_LANG_PROGRAM(
                [[#include "llvm/ProfileData/SampleProf.h"]],
                [[
                  llvm::sampleprof::SampleRecord *R;
                  R = new llvm::sampleprof::SampleRecord();
                  return llvm::sampleprof::SPVersion();
                ]]
              )
            ],
            ax_cv_llvm=yes,
            ax_cv_llvm=no
          )
          AC_LANG_POP([C++])
        ]
      )

      if test "x$ax_cv_llvm" = "xyes"; then
        succeeded=yes
      fi
      CXXFLAGS="$CXXFLAGS_SAVED"
      LDFLAGS="$LDFLAGS_SAVED"
      if test "x$shared_mode" = "xstatic"; then
        AC_MSG_NOTICE([Using static LLVM libraries.])
      elif test "x$shared_mode" = "xshared"; then
        AC_MSG_NOTICE([Using shared LLVM libraries.  Setting -rpath to $rpath.])
      else
        AC_MSG_ERROR([Could not determine whether to use shared or static LLVM libraries])
      fi
    else
      succeeded=no
    fi
  fi

  if test "$succeeded" != "yes" ; then
    AC_MSG_WARN(
      [[could not detect the LLVM libraries. Support for LLVM profiles disabled.]]
    )
  else
    AC_SUBST(LLVM_CXXFLAGS)
    AC_SUBST(LLVM_LDFLAGS)
    AC_DEFINE(HAVE_LLVM,,[define if the LLVM library is available])
  fi
])
