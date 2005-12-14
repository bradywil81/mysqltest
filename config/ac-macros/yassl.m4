AC_CONFIG_FILES(extra/yassl/Makefile dnl
extra/yassl/taocrypt/Makefile dnl
extra/yassl/taocrypt/src/Makefile dnl
extra/yassl/src/Makefile)

AC_DEFUN([MYSQL_CHECK_YASSL], [
  AC_MSG_CHECKING(for yaSSL)
  AC_ARG_WITH([yassl], [  --with-yassl          Include the yaSSL support],,)

  if test "$with_yassl" = "yes"
  then
    if test "$openssl" != "no"
    then
      AC_MSG_ERROR([Cannot configure MySQL to use yaSSL and OpenSSL simultaneously.])
    fi
    AC_MSG_RESULT([using bundled yaSSL])
    yassl_dir="extra/yassl"
    yassl_libs="-L\$(top_srcdir)/extra/yassl/src -lyassl -L\$(top_srcdir)/extra/yassl/taocrypt/src -ltaocrypt"
    yassl_includes="-I\$(top_srcdir)/extra/yassl/include"
    yassl_libs_with_path="\$(top_srcdir)/extra/yassl/src/libyassl.la \$(top_srcdir)/extra/yassl/taocrypt/src/libtaocrypt.la"
    AC_DEFINE([HAVE_OPENSSL], [1], [Defined by configure. Using yaSSL for OpenSSL emulation.])
    AC_DEFINE([HAVE_YASSL], [1], [Defined by configure. Using yaSSL for OpenSSL emulation.])
    # System specific checks
    yassl_integer_extra_cxxflags=""
    case $host_cpu--$CXX_VERSION in
        sparc*--*Sun*C++*5.6*)
	# Disable inlining when compiling taocrypt/src/
	yassl_taocrypt_extra_cxxflags="+d"
        AC_MSG_NOTICE([disabling inlining for yassl/taocrypt/src/])
        ;;
    esac
    AC_SUBST([yassl_taocrypt_extra_cxxflags])

  else
    yassl_dir=""
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(yassl_libs)
  AC_SUBST(yassl_libs_with_path)
  AC_SUBST(yassl_includes)
  AC_SUBST(yassl_dir)
  AM_CONDITIONAL([HAVE_YASSL], [ test "with_yassl" = "yes" ])
])