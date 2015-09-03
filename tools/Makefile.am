noinst_PROGRAMS =
bin_PROGRAMS = ratbag-command
noinst_LTLIBRARIES = libshared.la

AM_CPPFLAGS = -I$(top_srcdir)/include \
              -I$(top_srcdir)/src \
              -I$(top_builddir)/src  # for libratbag-version.h

libshared_la_SOURCES = \
		       shared.c \
		       shared.h
libshared_la_CFLAGS = $(LIBUDEV_LIBS) $(LIBEVDEV_CFLAGS)
libshared_la_LIBADD = $(LIBUDEV_CFLAGS) $(LIBEVDEV_LIBS)

ratbag_command_SOURCES = ratbag-command.c
ratbag_command_LDADD = ../src/libratbag.la libshared.la $(LIBUDEV_LIBS) $(LIBEVDEV_LIBS)
ratbag_command_LDFLAGS = -no-install
ratbag_command_CFLAGS = $(LIBUDEV_CFLAGS) $(LIBEVDEV_CFLAGS)

if BUILD_RATBAGGUI
noinst_PROGRAMS += ratbag-gui

ratbag_gui_SOURCES = ratbag-gui.c
ratbag_gui_LDADD = ../src/libratbag.la $(CAIRO_LIBS) $(GTK_LIBS) $(LIBUDEV_LIBS) $(LIBRSVG_LIBS) $(LIBXML2_LIBS)
ratbag_gui_CFLAGS = $(CAIRO_CFLAGS) $(GTK_CFLAGS) $(LIBUDEV_CFLAGS) $(LIBRSVG_CFLAGS) $(LIBXML2_CFLAGS)
ratbag_gui_LDFLAGS = -no-install
endif
