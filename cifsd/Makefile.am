## Makefile.am

AM_CPPFLAGS = -I$(top_srcdir)/include
AM_CFLAGS = -Wall
sbin_PROGRAMS = cifsd
cifsd_SOURCES = conv.c dcerpc.c pipecb.c netlink.c winreg.c cifsd.c netlink.h winreg.h $(top_srcdir)/include/cifsd.h
cifsd_LDADD = $(top_builddir)/lib/libcifsd.la
