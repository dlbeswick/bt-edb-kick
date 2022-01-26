# Note: this must match the generated .so file name.
GST_PLUGIN_NAME = bt_edb_kick

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -DDATADIR=\"$(datadir)\" -DGST_PLUGIN_NAME=$(GST_PLUGIN_NAME)

SRC = src/machine.c src/voice.c src/properties_simple.c

plugin_LTLIBRARIES = libbt_edb_kick.la

libbt_edb_kick_la_SOURCES = $(SRC)
libbt_edb_kick_la_CFLAGS = $(PKGCONFIG_DEPS_CFLAGS) $(OPTIMIZE_CFLAGS) \
	-std=gnu99 -Werror -Wno-error=unused-variable -Wall -Wshadow -Wpointer-arith -Wstrict-prototypes \
	-fvisibility=hidden
libbt_edb_kick_la_LDFLAGS = $(PKGCONFIG_DEPS_LIBS) -module -avoid-version

presetdir = $(datadir)/gstreamer-$(GST_MAJORMINOR)/presets
preset_DATA = presets/BtEdbKick.prs

EXTRA_DIST = $(preset_DATA)

# Remove 'la' file as the generated lib isn't intended to be linked with others.
install-data-hook:
	$(RM) $(DESTDIR)$(plugindir)/lib$(GST_PLUGIN_NAME).la

userdoc_DATA = $(GST_PLUGIN_NAME).html

$(GST_PLUGIN_NAME).html: $(top_srcdir)/README.md
	kramdown < $< > $@
