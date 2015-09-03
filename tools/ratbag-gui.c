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
#include "shared.h"

#include <cairo.h>

#include <librsvg/rsvg.h>
#include <libxml/parser.h>

#include <gtk/gtk.h>
#include <glib.h>

#include <libratbag.h>
#include <libratbag-util.h>

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

	const char *path;
	struct ratbag_device *dev;
	struct ratbag_profile *current_profile;
	char svg_path[256];
};

static int
update_svg_from_device(struct window *w, int update);

static struct ratbag *ratbag;

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

static inline int
path_exists(const char *filename)
{
	struct stat st;
	return !stat(filename, &st);
}

static gboolean
draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct window *w = data;

	rsvg_handle_render_cairo(w->svg_handle, cr);

	return TRUE;
}

static gboolean
button_release_event(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct window *w = data;

	update_svg_from_device(w, 1);

	gtk_widget_queue_draw (w->win);

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
	g_signal_connect(G_OBJECT(w->win), "button-release-event", G_CALLBACK(button_release_event), w);

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
	w->current_profile = ratbag_profile_unref(w->current_profile);
	if (w->svg_handle)
		g_object_unref(w->svg_handle);
	w->svg_handle = NULL;
}

static void
update_svg_text_from_device(struct window *w, xmlNode *node)
{
	xmlChar *content = NULL;
	int index;

	content = xmlNodeGetContent(node);

	if (!strncasecmp(content, "button", 6)) {
		struct ratbag_button *button;
		index = atoi(content + 6);
		button = ratbag_profile_get_button_by_index(w->current_profile,
							    index);
		if (!button) {
			xmlNodeSetContent(node, "XXXXXXXX");
			goto out;
		}

		xmlNodeSetContent(node, button_action_to_str(button));

		ratbag_button_unref(button);
		goto out;
	}

	if (!strncasecmp(content, "resolution", 10)) {
		struct ratbag_resolution *resolution;
		char buf[64];
		index = atoi(content + 10);
		resolution = ratbag_profile_get_resolution(w->current_profile,
							   index);
		if (!resolution) {
			xmlNodeSetContent(node, "YYYYYYYY");
			goto out;
		}

		snprintf(buf, sizeof(buf), "%d: %d dpi", index,
			 ratbag_resolution_get_dpi(resolution));
		xmlNodeSetContent(node, buf);

		ratbag_resolution_unref(resolution);
		goto out;
	}

out:
	xmlFree(content);
}

static void
update_svg_node_from_device(struct window *w, xmlNode *node)
{
	xmlNode *cur_node = NULL;

	for (cur_node = node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			if (!strcmp(cur_node->name, "text"))
				update_svg_text_from_device(w, cur_node);
		}

		update_svg_node_from_device(w, cur_node->children);
	}
}

static int
update_svg_from_device(struct window *w, int update)
{
	unsigned int num_profiles, i;
	xmlNode *root_element = NULL;
	xmlChar *xmlbuff = NULL;
	xmlDocPtr doc = NULL;
	GError *gerror = NULL;
	int buffersize;
	int rc = -EINVAL;

	/* FIXME: libratbag should not require to requery the device */
	if (update) {
		w->dev = ratbag_device_unref(w->dev);
		w->dev = ratbag_cmd_open_device(ratbag, w->path);
	}

	w->current_profile = ratbag_profile_unref(w->current_profile);

	num_profiles = ratbag_device_get_num_profiles(w->dev);
	for (i = 0; i < num_profiles; i++) {
		struct ratbag_profile *profile;

		profile = ratbag_device_get_profile_by_index(w->dev, i);
		if (ratbag_profile_is_active(profile)) {
			w->current_profile = profile;
			break;
		}

		ratbag_profile_unref(profile);
	}

	if (!w->current_profile) {
		error("Unable to retrieve the current profile\n");
		goto out;
	}

	doc = xmlReadFile(w->svg_path, NULL, 0);
	if (!doc) {
		error("unable to parse '%s'\n", w->svg_path);
		goto out;
	}

	root_element = xmlDocGetRootElement(doc);

	update_svg_node_from_device(w, root_element);

	xmlDocDumpFormatMemory(doc, &xmlbuff, &buffersize, 1);

	if (w->svg_handle)
		g_object_unref(w->svg_handle);
	w->svg_handle = rsvg_handle_new_from_data(xmlbuff, buffersize, &gerror);
	if (gerror != NULL) {
		error("%s\n", gerror->message);
		goto out;
	}

	rc = 0;

out:
	if (doc)
		xmlFreeDoc(doc);
	xmlCleanupParser();
	xmlFree(xmlbuff);
	return rc;
}

int
main(int argc, char *argv[])
{
	struct window w = {};
	uint32_t flags = 0;
	const char *svg_filename;

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

	w.path = argv[optind];

	w.dev = ratbag_cmd_open_device(ratbag, w.path);
	if (!w.dev) {
		error("Looks like '%s' is not supported\n", w.path);
		goto out;
	}

	svg_filename = ratbag_device_get_svg_name(w.dev);
	if (!svg_filename) {
		error("Looks like '%s' has no graphics associated\n", w.path);
		goto out;
	}

	rsvg_set_default_dpi (72.0);

	snprintf(w.svg_path, sizeof(w.svg_path), "data/%s", svg_filename);

	if (!path_exists(w.svg_path))
		snprintf(w.svg_path, sizeof(w.svg_path), "../data/%s", svg_filename);

	if (!path_exists(w.svg_path)) {
		error("Unable to find '%s'\n", svg_filename);
		goto out;
	}

	if (update_svg_from_device(&w, 0))
		goto out;

	gtk_init(&argc, &argv);

	window_init(&w);

	gtk_main();

out:
	window_cleanup(&w);
	ratbag_unref(ratbag);

	return 0;
}
