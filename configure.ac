#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([FULL-PACKAGE-NAME], [VERSION], [BUG-REPORT-ADDRESS])
AC_CONFIG_MACRO_DIR([m4])
AM_CONFIG_HEADER(glog/src/config.h)

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_RANLIB

AX_CXX_COMPILE_STDCXX_11
AM_INIT_AUTOMAKE

ac_cv_have_libgflags=1
AC_DEFINE(HAVE_LIB_GFLAGS, 1, [define if you have google gflags library])

# We only care about these two attributes.
if test x"$ac_cv___attribute__" = x"yes"; then
  ac_cv___attribute___noreturn="__attribute__ ((noreturn))"
  ac_cv___attribute___noinline="__attribute__ ((noinline))"
  ac_cv___attribute___printf_4_5="__attribute__((__format__ (__printf__, 4, 5)))"
else
  ac_cv___attribute___noreturn=
  ac_cv___attribute___noinline=
  ac_cv___attribute___printf_4_5=
fi

if test x"$ac_cv___builtin_expect" = x"yes"; then
  ac_cv_have___builtin_expect=1
else
  ac_cv_have___builtin_expect=0
fi

AC_CHECK_LIB([crypto], [MD5])
AC_CHECK_LIB([z], [gzopen])

AC_CHECK_HEADER(stdint.h, ac_cv_have_stdint_h=1, ac_cv_have_stdint_h=0)
AC_CHECK_HEADER(sys/types.h, ac_cv_have_systypes_h=1, ac_cv_have_systypes_h=0)
AC_CHECK_HEADER(inttypes.h, ac_cv_have_inttypes_h=1, ac_cv_have_inttypes_h=0)
AC_CHECK_HEADERS(unistd.h, ac_cv_have_unistd_h=1, ac_cv_have_unistd_h=0)
AC_CHECK_HEADERS(syscall.h)
AC_CHECK_HEADERS(sys/syscall.h)
# For backtrace with glibc.
AC_CHECK_HEADERS(execinfo.h)
# For backtrace with libunwind.
AC_CHECK_HEADERS(libunwind.h, ac_cv_have_libunwind_h=1, ac_cv_have_libunwind_h=0)
AC_CHECK_HEADERS(ucontext.h)
AC_CHECK_HEADERS(sys/utsname.h)
AC_CHECK_HEADERS(pwd.h)
AC_CHECK_HEADERS(syslog.h)
AC_CHECK_HEADERS(sys/time.h)
AC_CHECK_HEADERS(glob.h)
# For backtrace with gcc.
AC_CHECK_HEADERS(unwind.h)

AC_CHECK_SIZEOF(void *)

# These are the types I need.  We look for them in either stdint.h,
# sys/types.h, or inttypes.h, all of which are part of the default-includes.
AC_CHECK_TYPE(uint16_t, ac_cv_have_uint16_t=1, ac_cv_have_uint16_t=0)
AC_CHECK_TYPE(u_int16_t, ac_cv_have_u_int16_t=1, ac_cv_have_u_int16_t=0)
AC_CHECK_TYPE(__uint16, ac_cv_have___uint16=1, ac_cv_have___uint16=0)

AC_CXX_STL_NAMESPACE
AC_DEFINE_GOOGLE_NAMESPACE(google)

# These are what's needed by logging.h.in and raw_logging.h.in
AC_SUBST(ac_google_start_namespace)
AC_SUBST(ac_google_end_namespace)
AC_SUBST(ac_google_namespace)
AC_SUBST(ac_cv_cxx_using_operator)
AC_SUBST(ac_cv___attribute___noreturn)
AC_SUBST(ac_cv___attribute___noinline)
AC_SUBST(ac_cv___attribute___printf_4_5)
AC_SUBST(ac_cv_have___builtin_expect)
AC_SUBST(ac_cv_have_stdint_h)
AC_SUBST(ac_cv_have_systypes_h)
AC_SUBST(ac_cv_have_inttypes_h)
AC_SUBST(ac_cv_have_unistd_h)
AC_SUBST(ac_cv_have_uint16_t)
AC_SUBST(ac_cv_have_u_int16_t)
AC_SUBST(ac_cv_have___uint16)
AC_SUBST(ac_cv_have_libgflags)
AC_SUBST(GTEST_CFLAGS)
AC_SUBST(GMOCK_CFLAGS)
AC_SUBST(GTEST_LIBS)
AC_SUBST(GMOCK_LIBS)

AC_CONFIG_FILES([Makefile glog/src/glog/logging.h \
                 glog/src/glog/raw_logging.h glog/src/glog/vlog_is_on.h \
                 glog/src/glog/stl_logging.h])
AC_OUTPUT
