/*
 * mdpreview.c - Markdown preview for Geany, rendered with MuPDF
 *
 * Copyright 2026 Matti Palmström <sjukfan@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Note: MuPDF is licensed under the GNU AGPL v3. A binary of this plugin
 * that links MuPDF is therefore distributed under the terms of the AGPL v3.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* Fallbacks for the standalone build (no config.h) */
#ifndef GETTEXT_PACKAGE
# define GETTEXT_PACKAGE "geany-plugins"
#endif
#ifndef LOCALEDIR
# define LOCALEDIR ""
#endif

#include <geanyplugin.h>
#include <mupdf/fitz.h>
#include <string.h>
#include <math.h>

#if FZ_VERSION_MAJOR == 1 && FZ_VERSION_MINOR < 28
# error "MuPDF 1.28 or newer is required (it added the Markdown reader)"
#endif
#if !FZ_ENABLE_MD
# error "MuPDF was built without Markdown support (FZ_ENABLE_MD=0)"
#endif

#define PLUGIN_NAME     "mdpreview"
#define PREVIEW_LABEL   _("Markdown")

/* Layout units are points (1/72 inch). They are converted to screen pixels
 * with the desktop's font DPI (Xft.dpi / text scaling factor), the same
 * setting that scales the rest of Geany's text. 96 dpi = 100 %. HiDPI
 * integer scaling (GDK_SCALE=2) is handled separately when drawing. */
#define DEFAULT_DPI     96.0
/* "Endless" page: lay the document out on one very tall page. */
#define LAYOUT_HEIGHT   1.0e6f
#define UPDATE_DELAY_MS 250
#define MAX_SEL_QUADS   4096

/* Always applied before the user's stylesheet: small margins suit a sidebar. */
static const char *base_css =
	"@page{margin:0}"
	"body{margin:0.8em}"
	"pre{white-space:pre-wrap}";

static const char *css_template =
	"/* Markdown Preview stylesheet\n"
	" *\n"
	" * This is applied on top of MuPDF's built-in Markdown style.\n"
	" * Save the file in Geany and the preview updates.\n"
	" *\n"
	" * MuPDF understands most of CSS 2.1 (fonts, colours, margins, padding,\n"
	" * borders, tables, lists, page-break-*), but not flexbox/grid or JS.\n"
	" * Remove the comment markers around a rule to try it.\n"
	" */\n"
	"\n"
	"/* body { font-family: serif; } */\n"
	"/* h1, h2 { color: #24292f; } */\n"
	"/* a:link { color: #0969da; text-decoration: none; } */\n"
	"/*   (use a:link, not a: headings are wrapped in <a id=...> too) */\n"
	"/* pre, code { background-color: #f6f8fa; } */\n"
	"/* pre { white-space: pre; }     (don't wrap code, scroll instead) */\n"
	"\n"
	"/* Dark preview: also set the background colour in the plugin preferences.\n"
	"body { color: #d0d0d0; }\n"
	"a:link { color: #6cb6ff; }\n"
	"pre, code { background-color: #2d2d2d; }\n"
	"blockquote { color: #a0a0a0; }\n"
	"h1, h2 { border-bottom-color: #555; }\n"
	"tr:nth-child(even) { background-color: #2a2a2a; }\n"
	"*/\n";

enum { POS_SIDEBAR = 0, POS_MSGWIN = 1 };

typedef struct
{
	GeanyPlugin *plugin;
	fz_context *ctx;

	GtkWidget *scrolled;
	GtkWidget *darea;

	/* rendered state */
	fz_document *doc;
	fz_page *page;
	fz_display_list *list;
	fz_link *links;
	fz_stext_page *stext;
	float layout_w;        /* width the document is laid out at (pt) */
	float content_w;       /* actual ink width (may exceed layout_w) */
	float content_h;
	gchar *base_dir;       /* directory of the shown file (locale encoding) */

	/* updating */
	guint update_source;
	guint relayout_source;
	gboolean dirty;        /* update wanted while hidden */
	gint shown_doc_id;     /* GeanyDocument id, -1 = none */
	GHashTable *scroll_pos;/* doc id -> saved vertical position */
	gboolean scroll_pending;
	gdouble scroll_value;  /* absolute (px) target, or fraction if below */
	gboolean scroll_is_frac;
	gchar *pending_anchor; /* "#id" to jump to after next update */
	guint scroll_clear_source;

	/* selection / mouse */
	gboolean selecting;
	gboolean click_is_multi;
	gdouble press_x, press_y;
	fz_point sel_a, sel_b;     /* as dragged */
	fz_point sel_ca, sel_cb;   /* after snapping, used for copying */
	int sel_mode;
	fz_quad *quads;
	int n_quads;
	fz_link *hover;

	/* configuration */
	gchar *conf_file;
	gchar *css_file;
	gint position;
	gdouble font_size;
	GdkRGBA background;
	gchar *css;            /* base_css + user stylesheet */

	/* preferences dialog widgets */
	GtkWidget *pref_pos, *pref_size, *pref_bg;
} MdPreview;

static MdPreview md;

static void schedule_update(guint delay_ms);
static void load_css(void);
static void do_layout(void);


/* ------------------------------------------------------------------ */
/* MuPDF helpers                                                       */
/* ------------------------------------------------------------------ */

static void on_fz_message(void *user, const char *message)
{
	g_debug("MuPDF: %s", message);
}

/* An fz_archive that reads files relative to the Markdown file's folder.
 * MuPDF's own fz_open_directory() refuses "../" paths, which are common in
 * Markdown (e.g. ../images/foo.png). MuPDF hands us names like "img/a.png"
 * or "../shared/b.png"; absolute paths arrive with the leading "/" removed,
 * so those are tried as a fallback. */
typedef struct
{
	fz_archive super;
	char *base;
} local_dir;

static char *local_resolve(fz_context *ctx, fz_archive *arch, const char *name)
{
	local_dir *dir = (local_dir *) arch;
	char *path;

	if (g_path_is_absolute(name))
		path = g_strdup(name);
	else
	{
		path = g_build_filename(dir->base, name, NULL);
		if (!g_file_test(path, G_FILE_TEST_EXISTS))
		{
			char *abs = g_strconcat(G_DIR_SEPARATOR_S, name, NULL);
			if (g_file_test(abs, G_FILE_TEST_EXISTS))
			{
				g_free(path);
				path = abs;
			}
			else
				g_free(abs);
		}
	}
	return path;
}

static int local_has_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	char *path = local_resolve(ctx, arch, name);
	int ok = g_file_test(path, G_FILE_TEST_IS_REGULAR);
	g_free(path);
	return ok;
}

static fz_buffer *local_read_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	char *path = local_resolve(ctx, arch, name);
	fz_buffer *buf = NULL;
	fz_try(ctx)
		buf = fz_try_read_file(ctx, path);
	fz_always(ctx)
		g_free(path);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return buf;
}

static fz_stream *local_open_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	char *path = local_resolve(ctx, arch, name);
	fz_stream *stm = NULL;
	fz_try(ctx)
		stm = fz_try_open_file(ctx, path);
	fz_always(ctx)
		g_free(path);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return stm;
}

static void local_drop(fz_context *ctx, fz_archive *arch)
{
	g_free(((local_dir *) arch)->base);
}

static fz_archive *new_local_dir(fz_context *ctx, const char *base)
{
	local_dir *dir = fz_new_derived_archive(ctx, NULL, local_dir);
	dir->super.format = "mdpreview-dir";
	dir->super.has_entry = local_has_entry;
	dir->super.read_entry = local_read_entry;
	dir->super.open_entry = local_open_entry;
	dir->super.drop_archive = local_drop;
	dir->base = g_strdup(base);
	return &dir->super;
}


/* ------------------------------------------------------------------ */
/* Document state                                                      */
/* ------------------------------------------------------------------ */

static void clear_selection(void)
{
	md.n_quads = 0;
	md.selecting = FALSE;
}

static void drop_layout(void)
{
	clear_selection();
	md.hover = NULL;
	fz_drop_stext_page(md.ctx, md.stext);
	md.stext = NULL;
	fz_drop_link(md.ctx, md.links);
	md.links = NULL;
	fz_drop_display_list(md.ctx, md.list);
	md.list = NULL;
	fz_drop_page(md.ctx, md.page);
	md.page = NULL;
}

static void drop_document(void)
{
	drop_layout();
	fz_drop_document(md.ctx, md.doc);
	md.doc = NULL;
}

static gdouble view_scale(void)
{
	GdkScreen *screen = md.darea ? gtk_widget_get_screen(md.darea) : gdk_screen_get_default();
	gdouble dpi = screen ? gdk_screen_get_resolution(screen) : -1;

	if (dpi <= 0)
		dpi = DEFAULT_DPI;
	return dpi / 72.0;
}

static gint view_width_px(void)
{
	GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(md.scrolled));
	gint w = gtk_widget_get_allocated_width(viewport ? viewport : md.scrolled);
	return w > 50 ? w : 300;
}

static GtkAdjustment *vadj(void)
{
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(md.scrolled));
}

/* Lay the current document out at the widget's width and record the result. */
static void do_layout(void)
{
	fz_context *ctx = md.ctx;
	fz_rect ink = fz_empty_rect;
	fz_device *bbox = NULL;
	gdouble s = view_scale();

	drop_layout();

	/* forget the hovered link: it belongs to the old layout */
	gtk_widget_set_tooltip_text(md.darea, NULL);
	if (gtk_widget_get_realized(md.darea))
		gdk_window_set_cursor(gtk_widget_get_window(md.darea), NULL);

	if (!md.doc)
		return;

	md.layout_w = (float) (view_width_px() / s);

	fz_var(bbox);
	fz_try(ctx)
	{
		fz_layout_document(ctx, md.doc, md.layout_w, LAYOUT_HEIGHT, (float) md.font_size);
		md.page = fz_load_page(ctx, md.doc, 0);
		md.list = fz_new_display_list_from_page(ctx, md.page);
		bbox = fz_new_bbox_device(ctx, &ink);
		fz_run_display_list(ctx, md.list, bbox, fz_identity, fz_infinite_rect, NULL);
		fz_close_device(ctx, bbox);
		md.links = fz_load_links(ctx, md.page);
	}
	fz_always(ctx)
		fz_drop_device(ctx, bbox);
	fz_catch(ctx)
	{
		g_warning("Markdown preview: layout failed: %s", fz_caught_message(ctx));
		drop_layout();
	}

	if (fz_is_empty_rect(ink))
		ink = fz_make_rect(0, 0, 0, 0);
	md.content_w = MAX(md.layout_w, ink.x1);
	md.content_h = ink.y1 + (float) md.font_size;

	/* Only request a width when something sticks out (wide table, long
	 * word); then a horizontal scrollbar appears instead of clipping. */
	gtk_widget_set_size_request(md.darea,
		md.content_w > md.layout_w + 1 ? (gint) ceil(md.content_w * s) : -1,
		(gint) ceil(md.content_h * s));
	gtk_widget_queue_draw(md.darea);
}

/* Parse a buffer with MuPDF. magic is "text/markdown" or "text/html". */
static gboolean load_buffer(const char *data, size_t len, const char *magic, const gchar *dir)
{
	fz_context *ctx = md.ctx;
	fz_buffer *buf = NULL;
	fz_archive *arc = NULL;
	fz_stream *stm = NULL;
	gboolean ok = TRUE;

	drop_document();

	fz_var(buf);
	fz_var(arc);
	fz_var(stm);
	fz_try(ctx)
	{
		buf = fz_new_buffer(ctx, len + 1);
		fz_append_data(ctx, buf, data, len);
		/* MuPDF's Markdown reader (md.c) drops the last byte of its input,
		 * so make sure that byte is a harmless newline. */
		fz_append_byte(ctx, buf, '\n');
		if (dir)
			arc = new_local_dir(ctx, dir);
		stm = fz_open_buffer(ctx, buf);
		md.doc = fz_open_document_with_stream_and_dir(ctx, magic, stm, arc);
		fz_style_document(ctx, md.doc, 1, md.css);
	}
	fz_always(ctx)
	{
		fz_drop_stream(ctx, stm);
		fz_drop_archive(ctx, arc);
		fz_drop_buffer(ctx, buf);
	}
	fz_catch(ctx)
	{
		g_warning("Markdown preview: cannot open document: %s", fz_caught_message(ctx));
		drop_document();
		ok = FALSE;
	}
	return ok;
}

static void show_message(const gchar *msg)
{
	gchar *esc = g_markup_escape_text(msg, -1);
	gchar *html = g_strdup_printf(
		"<html><body><p style=\"font-family:sans-serif;color:gray;text-align:center;margin-top:2em\">%s</p></body></html>",
		esc);
	load_buffer(html, strlen(html), "text/html", NULL);
	g_free(html);
	g_free(esc);
}

static gboolean is_markdown_doc(GeanyDocument *doc)
{
	return DOC_VALID(doc) && doc->file_type &&
		doc->file_type->id == GEANY_FILETYPES_MARKDOWN;
}


/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

static gboolean on_scroll_clear(gpointer data)
{
	md.scroll_pending = FALSE;
	md.scroll_clear_source = 0;
	return G_SOURCE_REMOVE;
}

/* The adjustment only learns the new height after GTK has re-allocated
 * the widget, so the wanted position is kept "pending" and applied again
 * whenever the adjustment changes (for a short while). */
static void apply_pending_scroll(void)
{
	GtkAdjustment *adj = vadj();
	gdouble upper, target;

	if (!md.scroll_pending)
		return;
	upper = gtk_adjustment_get_upper(adj);
	target = md.scroll_is_frac ? md.scroll_value * upper : md.scroll_value;
	gtk_adjustment_set_value(adj, target);
}

static void request_scroll(gdouble value, gboolean is_frac)
{
	md.scroll_pending = TRUE;
	md.scroll_value = value;
	md.scroll_is_frac = is_frac;
	apply_pending_scroll();
	if (md.scroll_clear_source)
		g_source_remove(md.scroll_clear_source);
	md.scroll_clear_source = g_timeout_add(500, on_scroll_clear, NULL);
}

static void on_vadj_changed(GtkAdjustment *adj, gpointer data)
{
	apply_pending_scroll();
}

static gboolean scroll_to_anchor(const gchar *anchor)
{
	float x = 0, y = 0;
	fz_location loc;

	if (!md.doc || !anchor || !*anchor)
		return FALSE;

	fz_var(loc);
	fz_try(md.ctx)
		loc = fz_resolve_link(md.ctx, md.doc, anchor, &x, &y);
	fz_catch(md.ctx)
		loc.page = -1;

	if (loc.page != 0)
		return FALSE;
	request_scroll(MAX(0, y * view_scale() - 4), FALSE);
	return TRUE;
}


/* ------------------------------------------------------------------ */
/* Updating from Geany                                                 */
/* ------------------------------------------------------------------ */

static void update_now(void)
{
	GeanyDocument *doc = document_get_current();
	gint new_id = DOC_VALID(doc) ? (gint) doc->id : -1;
	gboolean switched = (new_id != md.shown_doc_id);

	if (!gtk_widget_get_mapped(md.darea))
	{
		/* Hidden (other sidebar tab is shown): do the work when mapped. */
		md.dirty = TRUE;
		return;
	}
	md.dirty = FALSE;

	if (switched && md.shown_doc_id >= 0)
	{
		gdouble *pos = g_new(gdouble, 1);
		*pos = gtk_adjustment_get_value(vadj());
		g_hash_table_insert(md.scroll_pos, GINT_TO_POINTER(md.shown_doc_id), pos);
	}

	g_free(md.base_dir);
	md.base_dir = NULL;

	if (is_markdown_doc(doc))
	{
		ScintillaObject *sci = doc->editor->sci;
		const char *text = (const char *) scintilla_send_message(sci, SCI_GETCHARACTERPOINTER, 0, 0);
		size_t len = (size_t) sci_get_length(sci);

		if (doc->real_path)
			md.base_dir = g_path_get_dirname(doc->real_path);
		else if (doc->file_name)
		{
			gchar *locale = utils_get_locale_from_utf8(doc->file_name);
			md.base_dir = g_path_get_dirname(locale);
			g_free(locale);
		}
		else
			md.base_dir = g_get_current_dir();

		if (!load_buffer(text, len, "text/markdown", md.base_dir))
			show_message(_("This Markdown document could not be rendered."));
	}
	else
		show_message(_("The current document is not a Markdown file."));

	do_layout();
	md.shown_doc_id = new_id;

	if (md.pending_anchor)
	{
		scroll_to_anchor(md.pending_anchor);
		g_free(md.pending_anchor);
		md.pending_anchor = NULL;
	}
	else if (switched)
	{
		gdouble *saved = g_hash_table_lookup(md.scroll_pos, GINT_TO_POINTER(new_id));
		request_scroll(saved ? *saved : 0, FALSE);
	}
}

static gboolean on_update_timeout(gpointer data)
{
	md.update_source = 0;
	update_now();
	return G_SOURCE_REMOVE;
}

static void schedule_update(guint delay_ms)
{
	if (md.update_source)
		g_source_remove(md.update_source);
	if (delay_ms == 0)
		md.update_source = g_idle_add(on_update_timeout, NULL);
	else
		md.update_source = g_timeout_add(delay_ms, on_update_timeout, NULL);
}

static gboolean on_editor_notify(GObject *obj, GeanyEditor *editor,
	SCNotification *nt, gpointer data)
{
	if (nt->nmhdr.code == SCN_MODIFIED && nt->length > 0 &&
		(nt->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) &&
		editor->document == document_get_current())
	{
		schedule_update(UPDATE_DELAY_MS);
	}
	return FALSE;
}

static void on_document_signal(GObject *obj, GeanyDocument *doc, gpointer data)
{
	schedule_update(0);
}

static void on_document_filetype_set(GObject *obj, GeanyDocument *doc,
	GeanyFiletype *old, gpointer data)
{
	schedule_update(0);
}

static void on_document_close(GObject *obj, GeanyDocument *doc, gpointer data)
{
	g_hash_table_remove(md.scroll_pos, GINT_TO_POINTER((gint) doc->id));
	if ((gint) doc->id == md.shown_doc_id)
		md.shown_doc_id = -1;
}

static void on_darea_map(GtkWidget *w, gpointer data)
{
	if (md.dirty)
		schedule_update(0);
}

/* Re-flow when the width changes. */
static gboolean on_relayout_idle(gpointer data)
{
	GtkAdjustment *adj = vadj();
	gdouble upper = gtk_adjustment_get_upper(adj);
	gdouble frac = upper > 0 ? gtk_adjustment_get_value(adj) / upper : 0;

	md.relayout_source = 0;
	if (!md.doc)
		return G_SOURCE_REMOVE;
	do_layout();
	request_scroll(frac, TRUE);
	return G_SOURCE_REMOVE;
}

/* Font DPI / text scaling changed in the desktop settings */
static void on_resolution_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	if (md.doc && !md.relayout_source)
		md.relayout_source = g_idle_add(on_relayout_idle, NULL);
}

/* Moved to a monitor with another integer scale (GDK_SCALE) */
static void on_scale_factor_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	gtk_widget_queue_draw(md.darea);
}

static void on_viewport_size_allocate(GtkWidget *w, GdkRectangle *alloc, gpointer data)
{
	float new_w = (float) (alloc->width / view_scale());

	if (md.doc && alloc->width > 50 && fabsf(new_w - md.layout_w) >= 0.5f && !md.relayout_source)
		md.relayout_source = g_idle_add(on_relayout_idle, NULL);
}


/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	fz_context *ctx = md.ctx;
	GdkRectangle clip;
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	gint sf;
	gdouble s;
	fz_irect ib;

	gdk_cairo_set_source_rgba(cr, &md.background);
	cairo_paint(cr);

	if (!md.list || !gdk_cairo_get_clip_rectangle(cr, &clip))
		return TRUE;

	sf = gtk_widget_get_scale_factor(widget);
	s = view_scale();
	ib.x0 = clip.x * sf;
	ib.y0 = clip.y * sf;
	ib.x1 = (clip.x + clip.width) * sf;
	ib.y1 = (clip.y + clip.height) * sf;

	fz_var(pix);
	fz_var(dev);
	fz_try(ctx)
	{
		cairo_surface_t *surf;

		/* BGRA with alpha == cairo's ARGB32 on little-endian machines */
		pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), ib, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		dev = fz_new_draw_device(ctx, fz_identity, pix);
		fz_run_display_list(ctx, md.list, dev, fz_scale((float) (s * sf), (float) (s * sf)),
			fz_rect_from_irect(ib), NULL);
		fz_close_device(ctx, dev);

		surf = cairo_image_surface_create_for_data(fz_pixmap_samples(ctx, pix),
			CAIRO_FORMAT_ARGB32, fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix),
			(int) fz_pixmap_stride(ctx, pix));
		cairo_surface_set_device_scale(surf, sf, sf);
		cairo_set_source_surface(cr, surf, clip.x, clip.y);
		cairo_paint(cr);
		cairo_surface_destroy(surf);
	}
	fz_always(ctx)
	{
		fz_drop_device(ctx, dev);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx)
		g_warning("Markdown preview: drawing failed: %s", fz_caught_message(ctx));

	/* selection highlight */
	if (md.n_quads > 0)
	{
		GtkStyleContext *sc = gtk_widget_get_style_context(widget);
		GdkRGBA sel = { 0.2, 0.5, 1.0, 0.35 };
		GdkRGBA themed;
		int i;

		if (gtk_style_context_lookup_color(sc, "theme_selected_bg_color", &themed))
		{
			sel = themed;
			sel.alpha = 0.4;
		}
		gdk_cairo_set_source_rgba(cr, &sel);
		for (i = 0; i < md.n_quads; i++)
		{
			fz_quad q = md.quads[i];
			cairo_move_to(cr, q.ul.x * s, q.ul.y * s);
			cairo_line_to(cr, q.ur.x * s, q.ur.y * s);
			cairo_line_to(cr, q.lr.x * s, q.lr.y * s);
			cairo_line_to(cr, q.ll.x * s, q.ll.y * s);
			cairo_close_path(cr);
		}
		cairo_fill(cr);
	}
	return TRUE;
}


/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

static gboolean ensure_stext(void)
{
	if (md.stext)
		return TRUE;
	if (!md.list)
		return FALSE;
	fz_try(md.ctx)
		md.stext = fz_new_stext_page_from_display_list(md.ctx, md.list, NULL);
	fz_catch(md.ctx)
		md.stext = NULL;
	return md.stext != NULL;
}

static void update_selection(void)
{
	if (!ensure_stext())
		return;
	md.sel_ca = md.sel_a;
	md.sel_cb = md.sel_b;
	fz_try(md.ctx)
	{
		if (md.sel_mode != FZ_SELECT_CHARS)
			fz_snap_selection(md.ctx, md.stext, &md.sel_ca, &md.sel_cb, md.sel_mode);
		md.n_quads = fz_highlight_selection(md.ctx, md.stext, md.sel_ca, md.sel_cb,
			md.quads, MAX_SEL_QUADS);
	}
	fz_catch(md.ctx)
		md.n_quads = 0;
	gtk_widget_queue_draw(md.darea);
}

/* Copying text.
 *
 * fz_copy_selection() ends every visual line with a newline, so text that was
 * merely wrapped by the layout would be copied with hard line breaks. Work
 * out which line ends are soft wraps, mark the last character of those lines
 * while MuPDF copies, and then turn "marked character + newline" back into
 * the character followed by a space (or nothing, after a hyphen or slash,
 * where the layout breaks inside a word, or between CJK characters).
 *
 * A line end counts as a soft wrap when the next line is in the same text
 * block, directly below, does not start with a list marker, and its first
 * word would not have fitted on the line before it (i.e. the layout really
 * had to wrap there). For ordinary text "fits" is measured against the
 * widest line of the block. For code blocks (pre-wrap, monospace) that does
 * not work, since a short block's longest line is often a hard break, so
 * there the real right edge is used: the layout width minus the left indent
 * of the code. Anything unclear keeps the newline, so the worst case is the
 * old behaviour. */
#define WRAP_MARK_BASE  0xF0000   /* Unicode plane-15 private use area */
#define WRAP_MARK_MAX   0x8000

typedef struct { fz_stext_char *ch; int c; } wrap_mark;

static gboolean is_mono(fz_stext_char *ch)
{
	return ch && ch->font && fz_font_is_monospaced(md.ctx, ch->font);
}

static gboolean is_list_marker(fz_stext_char *ch)
{
	int c = ch->c;
	if (c == 0x2022 || c == 0x25E6 || c == 0x25AA || c == 0x2023 || c == 0x2219)
		return TRUE;
	if (c >= '0' && c <= '9')
	{
		while (ch && ch->c >= '0' && ch->c <= '9')
			ch = ch->next;
		return ch && (ch->c == '.' || ch->c == ')') && ch->next && ch->next->c == ' ';
	}
	return FALSE;
}

/* right: right edge for ordinary text; mono_right: for code, or 0 if unknown */
static gboolean is_soft_wrap(fz_stext_line *a, fz_stext_line *b, float right, float mono_right)
{
	fz_stext_char *last = a->last_char, *first = b->first_char, *w, *wend;
	float size, gap, width, limit;

	if (!last || !first || a->wmode || b->wmode ||
	    fabsf(a->dir.y) > 0.01f || fabsf(b->dir.y) > 0.01f)
		return FALSE;
	size = last->size;
	/* the next line must be below, not beside (table cells) */
	if (first->origin.y < last->origin.y + 0.5f * size)
		return FALSE;
	/* hanging indent is fine, but never a step back to the left */
	if (b->bbox.x0 < a->bbox.x0 - 0.5f * size)
		return FALSE;
	if (is_list_marker(first))
		return FALSE;

	limit = right;
	gap = 0.25f * size;
	if (is_mono(last) && is_mono(first))
	{
		if (mono_right <= 0)
			return FALSE;
		limit = mono_right;
		gap = fabsf(last->quad.ur.x - last->quad.ul.x);   /* one character cell */
	}
	/* a wrap right after a hyphen or slash had no space in between */
	if (last->c == '-' || last->c == '/')
		gap = 0;
	/* the first chunk of the next line: up to a space, or just after a
	 * hyphen or slash, which MuPDF also breaks at */
	for (w = first, wend = first; w; w = w->next)
	{
		if (w->c == ' ')
			break;
		wend = w;
		if (w->c == '-' || w->c == '/')
			break;
	}
	width = fmaxf(wend->quad.ur.x, wend->quad.lr.x) - fminf(first->quad.ul.x, first->quad.ll.x);
	return a->bbox.x1 + gap + width > limit - 1.0f;
}

static gchar *selection_text(void)
{
	char *text = NULL;
	gchar *ret = NULL;
	GArray *marks;
	fz_stext_block *block;
	fz_stext_line *line;
	float mono_x0 = 0, mono_right = 0;
	guint i;

	if (md.n_quads == 0 || !ensure_stext())
		return NULL;

	/* left indent of code blocks: the smallest x0 of a purely monospace line */
	for (block = md.stext->first_block; block; block = block->next)
	{
		if (block->type != FZ_STEXT_BLOCK_TEXT)
			continue;
		for (line = block->u.t.first_line; line; line = line->next)
			if (is_mono(line->first_char) && is_mono(line->last_char) &&
			    (mono_x0 == 0 || line->bbox.x0 < mono_x0))
				mono_x0 = line->bbox.x0;
	}
	if (mono_x0 > 0)
		mono_right = md.layout_w - mono_x0;

	marks = g_array_new(FALSE, FALSE, sizeof(wrap_mark));
	for (block = md.stext->first_block; block; block = block->next)
	{
		float right = 0;
		if (block->type != FZ_STEXT_BLOCK_TEXT)
			continue;
		for (line = block->u.t.first_line; line; line = line->next)
			right = fmaxf(right, line->bbox.x1);
		for (line = block->u.t.first_line; line && line->next; line = line->next)
		{
			wrap_mark m;
			m.ch = line->last_char;
			if (m.ch && m.ch->c > 32 && m.ch->c < WRAP_MARK_MAX - 16 &&
			    is_soft_wrap(line, line->next, right, mono_right))
			{
				m.c = m.ch->c;
				m.ch->c = WRAP_MARK_BASE + m.c;
				g_array_append_val(marks, m);
			}
		}
	}

	fz_try(md.ctx)
		text = fz_copy_selection(md.ctx, md.stext, md.sel_ca, md.sel_cb, 0);
	fz_catch(md.ctx)
		text = NULL;

	for (i = 0; i < marks->len; i++)
	{
		wrap_mark *m = &g_array_index(marks, wrap_mark, i);
		m->ch->c = m->c;
	}
	g_array_free(marks, TRUE);

	if (text && *text)
	{
		GString *out = g_string_sized_new(strlen(text));
		const gchar *p = text;

		while (*p)
		{
			gunichar u = g_utf8_get_char(p);
			p = g_utf8_next_char(p);
			if (u >= WRAP_MARK_BASE && u < WRAP_MARK_BASE + WRAP_MARK_MAX)
			{
				u -= WRAP_MARK_BASE;
				g_string_append_unichar(out, u);
				if (*p == '\n')
				{
					p++;
					/* no space after a hyphen or slash, or between CJK characters */
					if (u != '-' && u != '/' && u != 0x2010 && u != 0x2011 && u != 0x2013 &&
					    !(u >= 0x2E80 && u <= 0xD7AF) && !(u >= 0xF900 && u <= 0xFAFF) &&
					    !(u >= 0xFF00 && u <= 0xFFEF))
						g_string_append_c(out, ' ');
				}
			}
			else
				g_string_append_unichar(out, u);
		}
		ret = g_string_free(out, FALSE);
	}
	fz_free(md.ctx, text);
	return ret;
}

static void copy_selection(GdkAtom which)
{
	gchar *text = selection_text();
	if (text)
	{
		gtk_clipboard_set_text(gtk_clipboard_get(which), text, -1);
		g_free(text);
	}
}

static void select_all(void)
{
	md.sel_mode = FZ_SELECT_CHARS;
	md.sel_a = fz_make_point(0, 0);
	md.sel_b = fz_make_point(md.content_w, md.content_h);
	update_selection();
	copy_selection(GDK_SELECTION_PRIMARY);
}


/* ------------------------------------------------------------------ */
/* Links                                                               */
/* ------------------------------------------------------------------ */

static fz_point widget_to_doc(gdouble x, gdouble y)
{
	gdouble s = view_scale();
	return fz_make_point((float) (x / s), (float) (y / s));
}

static fz_link *link_at(gdouble x, gdouble y)
{
	fz_point p = widget_to_doc(x, y);
	fz_link *l;

	for (l = md.links; l; l = l->next)
		if (fz_is_point_inside_rect(p, l->rect))
			return l;
	return NULL;
}

static gboolean is_markdown_name(const gchar *path)
{
	static const gchar *exts[] = { ".md", ".markdown", ".mdown", ".mkd", ".mkdn", ".mdwn", ".mdtxt", NULL };
	gchar *lower = g_ascii_strdown(path, -1);
	gboolean ret = FALSE;
	int i;

	for (i = 0; exts[i] && !ret; i++)
		ret = g_str_has_suffix(lower, exts[i]);
	g_free(lower);
	return ret;
}

static void open_local_file(const gchar *path, const gchar *anchor)
{
	if (!g_file_test(path, G_FILE_TEST_EXISTS))
	{
		gchar *utf8 = utils_get_utf8_from_locale(path);
		ui_set_statusbar(TRUE, _("Markdown preview: file not found: %s"), utf8);
		g_free(utf8);
		return;
	}

	if (is_markdown_name(path))
	{
		GeanyDocument *cur = document_get_current();
		GeanyDocument *doc;

		g_free(md.pending_anchor);
		md.pending_anchor = (anchor && anchor[1]) ? g_strdup(anchor) : NULL;

		doc = document_open_file(path, FALSE, NULL, NULL);
		if (doc && doc == cur)
		{
			/* Link to the file that is already shown */
			if (md.pending_anchor)
				scroll_to_anchor(md.pending_anchor);
			g_free(md.pending_anchor);
			md.pending_anchor = NULL;
		}
		else if (doc && !is_markdown_doc(doc))
		{
			g_free(md.pending_anchor);
			md.pending_anchor = NULL;
		}
	}
	else
	{
		/* Images, PDFs, folders, ...: hand over to the desktop. */
		gchar *uri = g_filename_to_uri(path, NULL, NULL);
		if (uri && !g_app_info_launch_default_for_uri(uri, NULL, NULL))
			utils_open_browser(uri);
		g_free(uri);
	}
}

static void activate_link(const char *uri)
{
	if (uri[0] == '#')
	{
		scroll_to_anchor(uri);
		return;
	}

	if (g_str_has_prefix(uri, "file:"))
	{
		gchar *hash = strchr(uri, '#');
		gchar *u = hash ? g_strndup(uri, hash - uri) : g_strdup(uri);
		gchar *path = g_filename_from_uri(u, NULL, NULL);
		if (path)
			open_local_file(path, hash);
		g_free(path);
		g_free(u);
		return;
	}

	if (fz_is_external_link(md.ctx, uri))
	{
		utils_open_browser(uri);
		return;
	}

	/* Relative link like "other.md", "../docs/x.md#usage" */
	{
		const gchar *hash = strchr(uri, '#');
		gchar *rel = hash ? g_strndup(uri, hash - uri) : g_strdup(uri);
		gchar *path = NULL;

		if (g_path_is_absolute(rel))
			path = g_strdup(rel);
		else
		{
			path = g_build_filename(md.base_dir ? md.base_dir : ".", rel, NULL);
			if (!g_file_test(path, G_FILE_TEST_EXISTS))
			{
				/* MuPDF strips the leading "/" from absolute links */
				gchar *abs = g_strconcat(G_DIR_SEPARATOR_S, rel, NULL);
				if (g_file_test(abs, G_FILE_TEST_EXISTS))
				{
					g_free(path);
					path = abs;
				}
				else
					g_free(abs);
			}
		}
		open_local_file(path, hash);
		g_free(path);
		g_free(rel);
	}
}


/* ------------------------------------------------------------------ */
/* Mouse and keyboard                                                  */
/* ------------------------------------------------------------------ */

static void on_menu_copy(GtkMenuItem *item, gpointer data)
{
	copy_selection(GDK_SELECTION_CLIPBOARD);
}

static void on_menu_select_all(GtkMenuItem *item, gpointer data)
{
	select_all();
}

static void on_menu_copy_link(GtkMenuItem *item, gpointer uri)
{
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), uri, -1);
}

/* Re-render from the editor text. Also re-reads the stylesheet, and since
 * the document is opened again, images changed on disk are picked up. */
static void on_menu_reload(GtkMenuItem *item, gpointer data)
{
	load_css();
	schedule_update(0);
}

/* GTK emits "deactivate" BEFORE the clicked item's "activate". Destroying
 * the menu right away would delete the item before its action ran, so the
 * menu is destroyed from an idle callback instead. */
static gboolean destroy_menu_idle(gpointer menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu));
	g_object_unref(menu);
	return G_SOURCE_REMOVE;
}

static void on_menu_deactivate(GtkMenuShell *menu, gpointer data)
{
	g_idle_add(destroy_menu_idle, g_object_ref(menu));
}

static void show_context_menu(GdkEventButton *ev)
{
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *item;
	fz_link *link = link_at(ev->x, ev->y);

	item = gtk_menu_item_new_with_mnemonic(_("_Copy"));
	gtk_widget_set_sensitive(item, md.n_quads > 0);
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_copy), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = gtk_menu_item_new_with_mnemonic(_("Select _All"));
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_select_all), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	if (link)
	{
		item = gtk_menu_item_new_with_mnemonic(_("Copy _Link Address"));
		g_signal_connect_data(item, "activate", G_CALLBACK(on_menu_copy_link),
			g_strdup(link->uri), (GClosureNotify) g_free, 0);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	item = gtk_menu_item_new_with_mnemonic(_("_Reload Preview"));
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_reload), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	gtk_widget_show_all(menu);
	g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) ev);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	gtk_widget_grab_focus(w);

	if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS)
	{
		show_context_menu(ev);
		return TRUE;
	}
	if (ev->button != 1)
		return FALSE;

	md.sel_a = md.sel_b = widget_to_doc(ev->x, ev->y);
	if (ev->type == GDK_2BUTTON_PRESS || ev->type == GDK_3BUTTON_PRESS)
	{
		md.click_is_multi = TRUE;
		md.selecting = FALSE;
		md.sel_mode = ev->type == GDK_2BUTTON_PRESS ? FZ_SELECT_WORDS : FZ_SELECT_LINES;
		update_selection();
		copy_selection(GDK_SELECTION_PRIMARY);
		return TRUE;
	}

	md.click_is_multi = FALSE;
	md.selecting = TRUE;
	md.sel_mode = FZ_SELECT_CHARS;
	md.press_x = ev->x;
	md.press_y = ev->y;
	if (md.n_quads)
	{
		md.n_quads = 0;
		gtk_widget_queue_draw(w);
	}
	return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
	if (md.selecting && (ev->state & GDK_BUTTON1_MASK))
	{
		md.sel_b = widget_to_doc(ev->x, ev->y);
		update_selection();
	}
	else
	{
		fz_link *link = link_at(ev->x, ev->y);
		if (link != md.hover)
		{
			GdkWindow *win = gtk_widget_get_window(w);
			GdkCursor *cur = link ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer") : NULL;
			gdk_window_set_cursor(win, cur);
			if (cur)
				g_object_unref(cur);
			gtk_widget_set_tooltip_text(w, link ? link->uri : NULL);
			md.hover = link;
		}
	}
	gdk_event_request_motions(ev);
	return TRUE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	gboolean was_selecting = md.selecting;

	if (ev->button != 1)
		return FALSE;
	md.selecting = FALSE;

	if (md.click_is_multi || !was_selecting)
		return TRUE;

	if (fabs(ev->x - md.press_x) < 4 && fabs(ev->y - md.press_y) < 4)
	{
		/* a click, not a drag */
		fz_link *link = link_at(ev->x, ev->y);
		md.n_quads = 0;
		gtk_widget_queue_draw(w);
		if (link)
		{
			gchar *uri = g_strdup(link->uri); /* activating may reload */
			activate_link(uri);
			g_free(uri);
		}
	}
	else
		copy_selection(GDK_SELECTION_PRIMARY);
	return TRUE;
}

/* Geany's own keybindings see Ctrl+C before any widget does, so catch it
 * through the plugin "key-press" signal while the preview has focus. */
static gboolean on_key_press(GObject *obj, GdkEventKey *ev, gpointer data)
{
	guint mods = ev->state & gtk_accelerator_get_default_mod_mask();

	if (!gtk_widget_has_focus(md.darea) || mods != GDK_CONTROL_MASK)
		return FALSE;

	switch (gdk_keyval_to_lower(ev->keyval))
	{
		case GDK_KEY_c:
		case GDK_KEY_Insert:
			copy_selection(GDK_SELECTION_CLIPBOARD);
			return TRUE;
		case GDK_KEY_a:
			select_all();
			return TRUE;
	}
	return FALSE;
}


/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void load_css(void)
{
	gchar *user = NULL;

	g_free(md.css);
	if (g_file_get_contents(md.css_file, &user, NULL, NULL))
		md.css = g_strconcat(base_css, "\n", user, NULL);
	else
		md.css = g_strdup(base_css);
	g_free(user);
}

static void load_config(void)
{
	GKeyFile *kf = g_key_file_new();
	gchar *bg;

	md.position = POS_SIDEBAR;
	md.font_size = 11.0;
	gdk_rgba_parse(&md.background, "#ffffff");

	if (g_key_file_load_from_file(kf, md.conf_file, G_KEY_FILE_NONE, NULL))
	{
		md.position = utils_get_setting_integer(kf, PLUGIN_NAME, "position", POS_SIDEBAR);
		md.font_size = g_key_file_get_double(kf, PLUGIN_NAME, "font_size", NULL);
		if (md.font_size < 4 || md.font_size > 72)
			md.font_size = 11.0;
		bg = utils_get_setting_string(kf, PLUGIN_NAME, "background", "#ffffff");
		if (!gdk_rgba_parse(&md.background, bg))
			gdk_rgba_parse(&md.background, "#ffffff");
		g_free(bg);
	}
	g_key_file_free(kf);
	load_css();
}

static void save_config(void)
{
	GKeyFile *kf = g_key_file_new();
	gchar *dir = g_path_get_dirname(md.conf_file);
	gchar *bg = gdk_rgba_to_string(&md.background);
	gchar *data;

	g_key_file_load_from_file(kf, md.conf_file, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_key_file_set_integer(kf, PLUGIN_NAME, "position", md.position);
	g_key_file_set_double(kf, PLUGIN_NAME, "font_size", md.font_size);
	g_key_file_set_string(kf, PLUGIN_NAME, "background", bg);

	if (utils_mkdir(dir, TRUE) == 0)
	{
		data = g_key_file_to_data(kf, NULL, NULL);
		utils_write_file(md.conf_file, data);
		g_free(data);
	}
	g_free(bg);
	g_free(dir);
	g_key_file_free(kf);
}

static GtkNotebook *target_notebook(void)
{
	GeanyMainWidgets *mw = md.plugin->geany_data->main_widgets;
	return GTK_NOTEBOOK(md.position == POS_MSGWIN ? mw->message_window_notebook : mw->sidebar_notebook);
}

static void place_tab(gboolean show)
{
	GtkNotebook *nb = target_notebook();
	GtkWidget *parent = gtk_widget_get_parent(md.scrolled);
	gint page;

	if (parent == GTK_WIDGET(nb))
		return;

	g_object_ref(md.scrolled);
	if (parent)
		gtk_container_remove(GTK_CONTAINER(parent), md.scrolled);
	page = gtk_notebook_append_page(nb, md.scrolled, gtk_label_new(PREVIEW_LABEL));
	gtk_notebook_set_tab_reorderable(nb, md.scrolled, TRUE);
	g_object_unref(md.scrolled);
	gtk_widget_show_all(md.scrolled);
	if (show)
		gtk_notebook_set_current_page(nb, page);
}

/* Re-read the stylesheet when it is saved from Geany. */
static void on_document_save(GObject *obj, GeanyDocument *doc, gpointer data)
{
	if (doc->real_path)
	{
		gchar *css_real = utils_get_real_path(md.css_file);
		if (css_real && strcmp(css_real, doc->real_path) == 0)
		{
			load_css();
			schedule_update(0);
		}
		g_free(css_real);
	}
}

static void on_edit_css_clicked(GtkButton *button, gpointer data)
{
	if (!g_file_test(md.css_file, G_FILE_TEST_EXISTS))
	{
		gchar *dir = g_path_get_dirname(md.css_file);
		utils_mkdir(dir, TRUE);
		utils_write_file(md.css_file, css_template);
		g_free(dir);
	}
	document_open_file(md.css_file, FALSE, NULL, NULL);
	gtk_dialog_response(GTK_DIALOG(gtk_widget_get_toplevel(GTK_WIDGET(button))), GTK_RESPONSE_CANCEL);
}

static void on_configure_response(GtkDialog *dialog, gint response, gpointer data)
{
	if (response != GTK_RESPONSE_OK && response != GTK_RESPONSE_APPLY)
		return;

	md.position = gtk_combo_box_get_active(GTK_COMBO_BOX(md.pref_pos));
	md.font_size = gtk_spin_button_get_value(GTK_SPIN_BUTTON(md.pref_size));
	gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(md.pref_bg), &md.background);
	save_config();

	place_tab(TRUE);
	schedule_update(0);
}

static GtkWidget *md_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer data)
{
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *label, *button;
	gchar *css_utf8, *hint;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 6);

	label = gtk_label_new(_("Show preview in:"));
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	md.pref_pos = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(md.pref_pos), _("Sidebar"));
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(md.pref_pos), _("Message window"));
	gtk_combo_box_set_active(GTK_COMBO_BOX(md.pref_pos), md.position);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), md.pref_pos, 1, 0, 1, 1);

	label = gtk_label_new(_("Base font size (pt):"));
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	md.pref_size = gtk_spin_button_new_with_range(4, 72, 0.5);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(md.pref_size), md.font_size);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), md.pref_size, 1, 1, 1, 1);

	label = gtk_label_new(_("Background colour:"));
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	md.pref_bg = gtk_color_button_new_with_rgba(&md.background);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), md.pref_bg, 1, 2, 1, 1);

	button = gtk_button_new_with_mnemonic(_("_Edit Stylesheet"));
	css_utf8 = utils_get_utf8_from_locale(md.css_file);
	hint = g_strdup_printf(_("Opens %s in Geany. Saving it updates the preview."), css_utf8);
	gtk_widget_set_tooltip_text(button, hint);
	g_free(hint);
	g_free(css_utf8);
	g_signal_connect(button, "clicked", G_CALLBACK(on_edit_css_clicked), NULL);
	gtk_grid_attach(GTK_GRID(grid), button, 0, 3, 2, 1);

	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), NULL);
	gtk_widget_show_all(grid);
	return grid;
}


/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

static gboolean md_init(GeanyPlugin *plugin, gpointer data)
{
	gchar *dir;
	GtkWidget *viewport;

	md.plugin = plugin;
	md.shown_doc_id = -1;
	md.quads = g_new(fz_quad, MAX_SEL_QUADS);
	md.scroll_pos = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

	md.ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!md.ctx)
	{
		g_warning("Markdown preview: cannot create MuPDF context");
		return FALSE;
	}
	fz_set_warning_callback(md.ctx, on_fz_message, NULL);
	fz_set_error_callback(md.ctx, on_fz_message, NULL);
	fz_register_document_handlers(md.ctx);

	dir = g_build_filename(plugin->geany_data->app->configdir, "plugins", PLUGIN_NAME, NULL);
	md.conf_file = g_build_filename(dir, PLUGIN_NAME ".conf", NULL);
	md.css_file = g_build_filename(dir, "preview.css", NULL);
	g_free(dir);
	load_config();

	md.darea = gtk_drawing_area_new();
	gtk_widget_set_can_focus(md.darea, TRUE);
	gtk_widget_add_events(md.darea, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK);
	g_signal_connect(md.darea, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(md.darea, "button-press-event", G_CALLBACK(on_button_press), NULL);
	g_signal_connect(md.darea, "button-release-event", G_CALLBACK(on_button_release), NULL);
	g_signal_connect(md.darea, "motion-notify-event", G_CALLBACK(on_motion), NULL);
	g_signal_connect(md.darea, "map", G_CALLBACK(on_darea_map), NULL);

	md.scrolled = gtk_scrolled_window_new(NULL, NULL);
	/* Geany may destroy the notebook (and us) before calling cleanup */
	g_object_add_weak_pointer(G_OBJECT(md.scrolled), (gpointer *) &md.scrolled);
	/* vertical bar always present, so it can't flip the width back and forth */
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(md.scrolled),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
	gtk_container_add(GTK_CONTAINER(md.scrolled), md.darea);
	viewport = gtk_bin_get_child(GTK_BIN(md.scrolled));
	/* Giving the drawing area keyboard focus (a click) makes the scrolled
	 * window scroll the focused widget into view, i.e. back to the top of
	 * the document. Point its focus adjustments at throw-away ones. */
	gtk_container_set_focus_vadjustment(GTK_CONTAINER(viewport),
		gtk_adjustment_new(0, 0, 1, 1, 1, 1));
	gtk_container_set_focus_hadjustment(GTK_CONTAINER(viewport),
		gtk_adjustment_new(0, 0, 1, 1, 1, 1));
	g_signal_connect(viewport, "size-allocate", G_CALLBACK(on_viewport_size_allocate), NULL);
	g_signal_connect(vadj(), "changed", G_CALLBACK(on_vadj_changed), NULL);

	place_tab(TRUE);

#define CONNECT(sig, cb) plugin_signal_connect(plugin, NULL, (sig), TRUE, G_CALLBACK(cb), NULL)
	CONNECT("editor-notify", on_editor_notify);
	CONNECT("document-activate", on_document_signal);
	CONNECT("document-new", on_document_signal);
	CONNECT("document-open", on_document_signal);
	CONNECT("document-reload", on_document_signal);
	CONNECT("document-filetype-set", on_document_filetype_set);
	CONNECT("document-save", on_document_save);
	CONNECT("document-close", on_document_close);
#undef CONNECT
	plugin_signal_connect(plugin, NULL, "key-press", FALSE, G_CALLBACK(on_key_press), NULL);
	plugin_signal_connect(plugin, G_OBJECT(gtk_widget_get_screen(md.darea)), "notify::resolution",
		FALSE, G_CALLBACK(on_resolution_changed), NULL);
	g_signal_connect(md.darea, "notify::scale-factor", G_CALLBACK(on_scale_factor_changed), NULL);

	schedule_update(0);
	return TRUE;
}

static void md_cleanup(GeanyPlugin *plugin, gpointer data)
{
	if (md.update_source)
		g_source_remove(md.update_source);
	if (md.relayout_source)
		g_source_remove(md.relayout_source);
	if (md.scroll_clear_source)
		g_source_remove(md.scroll_clear_source);

	if (md.scrolled)
	{
		g_object_remove_weak_pointer(G_OBJECT(md.scrolled), (gpointer *) &md.scrolled);
		gtk_widget_destroy(md.scrolled);
	}

	if (md.ctx)
	{
		drop_document();
		fz_drop_context(md.ctx);
	}
	if (md.scroll_pos)
		g_hash_table_destroy(md.scroll_pos);
	g_free(md.quads);
	g_free(md.base_dir);
	g_free(md.pending_anchor);
	g_free(md.conf_file);
	g_free(md.css_file);
	g_free(md.css);
	memset(&md, 0, sizeof md);
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	main_locale_init(LOCALEDIR, GETTEXT_PACKAGE);

	plugin->info->name = _("Markdown Preview (MuPDF)");
	plugin->info->description = _("Live Markdown preview in the sidebar, rendered with MuPDF");
	plugin->info->version = "0.4";
	plugin->info->author = "sjukfan@gmail.com";

	plugin->funcs->init = md_init;
	plugin->funcs->cleanup = md_cleanup;
	plugin->funcs->configure = md_configure;

	/* MuPDF's cmark-gfm registers an atexit() handler; the code must stay
	 * loaded even if the plugin is disabled. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 225);
}
