/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include <config.h>

#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <linux/input.h>

#include <librsvg/rsvg.h>

#include <gtk/gtk.h>
#include <glib.h>

#include <libratbag.h>
#include <libratbag-util.h>

#define clip(val_, min_, max_) min((max_), max((min_), (val_)))

enum options {
	OPT_VERBOSE,
	OPT_HELP,
};

enum cmd_flags {
	FLAG_VERBOSE = 1 << 0,
	FLAG_VERBOSE_RAW = 1 << 1,
};

struct window {
	GtkWidget *win;
	GtkWidget *area;
	int width, height; /* of window */

	RsvgHandle *svg_handle;

	struct ratbag_device *dev;
};

static int
error(const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "error: ");

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	return EXIT_FAILURE;
}

static void
usage(void)
{
	printf("Usage: %s [options] /sys/class/input/eventX\n"
	       "/path/to/device ..... Open the given device only\n",
		program_invocation_short_name);

	printf("\n"
	       "Options:\n"
	       "    --verbose[=raw] ....... Print debugging output, with protocol output if requested.\n"
	       "    --help .......... Print this help.\n");
}

static void
msg(const char *fmt, ...)
{
	va_list args;
	printf("info: ");

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static inline struct udev_device*
udev_device_from_path(struct udev *udev, const char *path)
{
	struct udev_device *udev_device;
	const char *event_node_prefix = "/dev/input/event";

	if (strncmp(path, event_node_prefix, strlen(event_node_prefix)) == 0) {
		struct stat st;
		if (stat(path, &st) == -1) {
			error("Failed to stat '%s': %s\n", path, strerror(errno));
			return NULL;
		}
		udev_device = udev_device_new_from_devnum(udev, 'c', st.st_rdev);

	} else {
		udev_device = udev_device_new_from_syspath(udev, path);
	}
	if (!udev_device) {
		error("Can't open '%s': %s\n", path, strerror(errno));
		return NULL;
	}

	return udev_device;
}

static struct ratbag_device *
ratbag_cmd_open_device(struct ratbag *ratbag, const char *path)
{
	struct ratbag_device *device;
	struct udev *udev;
	struct udev_device *udev_device;

	udev = udev_new();
	udev_device = udev_device_from_path(udev, path);
	if (!udev_device) {
		udev_unref(udev);
		return NULL;
	}

	device = ratbag_device_new_from_udev_device(ratbag, udev_device);

	udev_device_unref(udev_device);
	udev_unref(udev);

	return device;
}

static gboolean
draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct window *w = data;

	rsvg_handle_render_cairo(w->svg_handle, cr);

	return TRUE;
}

static void
window_init(struct window *w)
{
	RsvgDimensionData dim;

	rsvg_handle_get_dimensions(w->svg_handle, &dim);

	w->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_events(w->win, 0);
	gtk_window_set_title(GTK_WINDOW(w->win), "ratbag graphical configuring tool");
	gtk_window_set_default_size(GTK_WINDOW(w->win), dim.width, dim.height);
	gtk_window_set_resizable(GTK_WINDOW(w->win), TRUE);
	gtk_widget_realize(w->win);
	g_signal_connect(G_OBJECT(w->win), "delete-event", G_CALLBACK(gtk_main_quit), NULL);

	w->area = gtk_drawing_area_new();
	gtk_widget_set_events(w->area, 0);
	gtk_container_add(GTK_CONTAINER(w->win), w->area);
	g_signal_connect(G_OBJECT(w->area), "draw", G_CALLBACK(draw), w);
	gtk_widget_show_all(w->win);
}

static void
window_cleanup(struct window *w)
{
	w->dev = ratbag_device_unref(w->dev);
}

static int
open_restricted(const char *path, int flags, void *user_data)
{
	int fd = open(path, flags);

	if (fd < 0)
		error("Failed to open %s (%s)\n",
			path, strerror(errno));

	return fd < 0 ? -errno : fd;
}

static void
close_restricted(int fd, void *user_data)
{
	close(fd);
}

static const struct ratbag_interface interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

int
main(int argc, char *argv[])
{
	struct ratbag *ratbag;
	struct window w = {};
	uint32_t flags = 0;
	const char *path;
	GError *gerror = NULL;

	ratbag = ratbag_create_context(&interface, NULL);
	if (!ratbag) {
		error("Can't initialize ratbag\n");
		goto out;
	}

	while (1) {
		int c;
		int option_index = 0;
		static struct option opts[] = {
			{ "verbose", 2, 0, OPT_VERBOSE },
			{ "help", 0, 0, OPT_HELP },
		};

		c = getopt_long(argc, argv, "+h", opts, &option_index);
		if (c == -1)
			break;
		switch(c) {
		case 'h':
		case OPT_HELP:
			usage();
			goto out;
		case OPT_VERBOSE:
			if (optarg && streq(optarg, "raw"))
				flags |= FLAG_VERBOSE_RAW;
			else
				flags |= FLAG_VERBOSE;
			break;
		default:
			usage();
			return 1;
		}
	}

	if (optind >= argc) {
		usage();
		ratbag_unref(ratbag);
		return 1;
	}

	if (flags & FLAG_VERBOSE_RAW)
		ratbag_log_set_priority(ratbag, RATBAG_LOG_PRIORITY_RAW);
	else if (flags & FLAG_VERBOSE)
		ratbag_log_set_priority(ratbag, RATBAG_LOG_PRIORITY_DEBUG);

	path = argv[optind];

	w.dev = ratbag_cmd_open_device(ratbag, path);
	if (!w.dev) {
		error("Looks like '%s' is not supported\n", path);
		goto out;
	}

	rsvg_set_default_dpi (72.0);

	w.svg_handle = rsvg_handle_new_from_file("data/etekcity.svg", &gerror);
	if (gerror != NULL) {
		error("%s\n", gerror->message);
		goto out;
	}

	gtk_init(&argc, &argv);

	window_init(&w);

	gtk_main();

	window_cleanup(&w);
out:
	ratbag_unref(ratbag);

	return 0;
}
