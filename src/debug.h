#pragma once
#include <gst/gstinfo.h>

#define GST_PLUGIN_DESC "Kick"
#define GST_MACHINE_CATEGORY "Source/Audio"

// Note: GST_PLUGIN_NAME defined via Makefile.am
#define GST_CAT_DEFAULT GST_PLUGIN_NAME
GST_DEBUG_CATEGORY_EXTERN(GST_CAT_DEFAULT);
