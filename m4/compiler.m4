
AC_ARG_ENABLE(debug,
        AC_HELP_STRING([--enable-debug], [Enable debug mode build]),
        [],
        [enable_debug=no])


AS_IF([test "x$enable_debug" == xyes],
        [CFLAGS="-O0 -D_DEBUG $CFLAGS"],
        [CFLAGS="-O3 $CFLAGS"])
