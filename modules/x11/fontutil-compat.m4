AC_DEFUN([XORG_FONT_MACROS_VERSION],[])

dnl Real font-util leaves FONTROOTDIR as an unexpanded '${datadir}/...'
dnl string for late (make-time) resolution. Consumers here instead feed it
dnl straight into AC_DEFINE_DIR (m4/ac_define_dir.m4), which only ever
dnl `eval`s twice ("In Autoconf 2.60, ${datadir} refers to ${datarootdir},
dnl which in turn refers to ${prefix}. Thus we have to use `eval' twice.")
dnl -- exactly enough for a 2-hop chain, not the extra datadir/datarootdir
dnl hops a deferred FONTROOTDIR adds on top. Resolve datarootdir/prefix
dnl immediately (same double-eval trick) so FONTROOTDIR is already a plain
dnl path, keeping the remaining chain within AC_DEFINE_DIR's 2 evals.
AC_DEFUN([XORG_FONTROOTDIR],[
eval FONTROOTDIR="\"${datarootdir}/fonts/X11\""
eval FONTROOTDIR="\"$FONTROOTDIR\""
AC_SUBST([FONTROOTDIR])
])

AC_DEFUN([XORG_FONTSUBDIR],[
$1='${FONTROOTDIR}/$3'
AC_SUBST([$1])
])