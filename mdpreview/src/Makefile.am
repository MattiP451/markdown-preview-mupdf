include $(top_srcdir)/build/vars.build.mk
plugin = mdpreview

geanyplugins_LTLIBRARIES = mdpreview.la

mdpreview_la_SOURCES = mdpreview.c

mdpreview_la_CPPFLAGS = $(AM_CPPFLAGS) \
	-DG_LOG_DOMAIN=\"MDPreview\"
mdpreview_la_CFLAGS = $(AM_CFLAGS) $(MUPDF_CFLAGS)
mdpreview_la_LIBADD = $(COMMONLIBS) $(MUPDF_LIBS)
# keep MuPDF's thousands of symbols private to the plugin
mdpreview_la_LDFLAGS = $(AM_LDFLAGS) -Wl,--exclude-libs,ALL

include $(top_srcdir)/build/cppcheck.mk
