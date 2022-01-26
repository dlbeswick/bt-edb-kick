/*
  Kick generator for Buzztrax
  Copyright (C) 2021 David Beswick

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "src/debug.h"
#include "src/properties_simple.h"
#include "src/voice.h"

#include "libbuzztrax-gst/audiosynth.h"
#include "libbuzztrax-gst/childbin.h"
#include "libbuzztrax-gst/propertymeta.h"
#include "libbuzztrax-gst/ui.h"

#include <math.h>

GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

GType btedb_kick_get_type(void);

#define MAX_VOICES 16

typedef struct {
  GstBtAudioSynth parent;

  guint children;
  BtEdbKickV* voices[MAX_VOICES];
  BtEdbPropertiesSimple* props;
} BtEdbKick;

typedef struct {
  GstBtAudioSynthClass parent_class;
} BtEdbKickClass;

static GObject* child_proxy_get_child_by_index (GstChildProxy *child_proxy, guint index) {
  BtEdbKick* self = (BtEdbKick*)child_proxy;

  g_return_val_if_fail(index < MAX_VOICES, NULL);

  return gst_object_ref(self->voices[index]);
}

static guint child_proxy_get_children_count (GstChildProxy *child_proxy) {
  BtEdbKick* self = (BtEdbKick*)child_proxy;
  return self->children;
}

static void child_proxy_interface_init (gpointer g_iface, gpointer iface_data) {
  GstChildProxyInterface* iface = (GstChildProxyInterface*)g_iface;

  iface->get_child_by_index = child_proxy_get_child_by_index;
  iface->get_children_count = child_proxy_get_children_count;
}

static void gstbt_ui_custom_gfx_interface_init(GstBtUiCustomGfxInterface *iface);

G_DEFINE_TYPE_WITH_CODE (
  BtEdbKick,
  btedb_kick,
  GSTBT_TYPE_AUDIO_SYNTH,
  G_IMPLEMENT_INTERFACE(GST_TYPE_CHILD_PROXY, child_proxy_interface_init)
  G_IMPLEMENT_INTERFACE(GSTBT_TYPE_CHILD_BIN, NULL)
  G_IMPLEMENT_INTERFACE(GSTBT_UI_TYPE_CUSTOM_GFX, gstbt_ui_custom_gfx_interface_init))

static gboolean plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(
    GST_CAT_DEFAULT,
    G_STRINGIFY(GST_CAT_DEFAULT),
    GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLACK,
    GST_PLUGIN_DESC);

  return gst_element_register(
    plugin,
    G_STRINGIFY(GST_PLUGIN_NAME),
    GST_RANK_NONE,
    btedb_kick_get_type());
}

GST_PLUGIN_DEFINE(
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  GST_PLUGIN_NAME,
  GST_PLUGIN_DESC,
  plugin_init, VERSION, "GPL", PACKAGE_NAME, PACKAGE_BUGREPORT)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (F32) ", "
        "layout = (string) interleaved, "
        "rate = (int) [1, MAX], "
        "channels = (int) 1")
    );

static const GstBtUiCustomGfxResponse* on_gfx_request(GstBtUiCustomGfx* self) {
  return gstbt_ui_custom_gfx_request(GSTBT_UI_CUSTOM_GFX(((BtEdbKick*)self)->voices[0]));
}

static void on_voice_gfx_invalidated(void* voice, BtEdbKick* self) {
  g_signal_emit_by_name(self, "gstbt-ui-custom-gfx-invalidated", 0);
}

static void set_property (GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  BtEdbKick* self = (BtEdbKick*)object;
  g_assert(self->props);
  btedb_properties_simple_set(self->props, pspec, value);
}

static void get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec) {
  BtEdbKick* self = (BtEdbKick*)object;
  btedb_properties_simple_get(self->props, pspec, value);
}

static gboolean process(GstBtAudioSynth* synth, GstBuffer* gstbuf, GstMapInfo* info) {
  BtEdbKick* self = (BtEdbKick*)synth;
  for (int i = 0; i < self->children; ++i) {
    btedb_kickv_process(
      self->voices[i], gstbuf, info, self->parent.running_time, self->parent.generate_samples_per_buffer,
      self->parent.info.rate);
  }

  return TRUE;
}

static void dispose(GObject* object) {
  BtEdbKick* self = (BtEdbKick*)object;
  btedb_properties_simple_free(self->props);
  self->props = 0;
  g_signal_handlers_disconnect_by_func(self, on_voice_gfx_invalidated, self);
}

static void btedb_kick_class_init(BtEdbKickClass* const klass) {
  {
    GObjectClass* const aclass = (GObjectClass*)klass;
    aclass->set_property = set_property;
    aclass->get_property = get_property;
    aclass->dispose = dispose;

    // Note: variables will not be set to default values unless G_PARAM_CONSTRUCT is given.
/*    const GParamFlags flags =
      (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_uint("oversample", "Oversample", "Oversample", 1, 64, 2, flags ^ GST_PARAM_CONTROLLABLE));*/

    // GstBtChildBin interface properties
    guint idx = 1;
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_ulong("children", "Children", "", 0, MAX_VOICES, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  }

  {
    GstElementClass* const aclass = (GstElementClass*)klass;
    gst_element_class_set_static_metadata(
      aclass,
      G_STRINGIFY(GST_PLUGIN_NAME),
      GST_MACHINE_CATEGORY,
      GST_PLUGIN_DESC,
      PACKAGE_BUGREPORT);
    
    gst_element_class_add_metadata (
      aclass,
      GST_ELEMENT_METADATA_DOC_URI,
      "file://" DATADIR "" G_DIR_SEPARATOR_S "Gear" G_DIR_SEPARATOR_S "" PACKAGE ".html");

    gst_element_class_add_static_pad_template(aclass, &src_template);
  }

  {
    GstBtAudioSynthClass* const aclass = (GstBtAudioSynthClass*)klass;
    aclass->process = process;
  }
}

static void btedb_kick_init(BtEdbKick* const self) {
  self->props = btedb_properties_simple_new((GObject*)self);
  btedb_properties_simple_add(self->props, "children", &self->children);

  for (int i = 0; i < MAX_VOICES; i++) {
    BtEdbKickV* voice = g_object_new(btedb_kickv_get_type(), 0);

    char name[7];
    g_snprintf(name, sizeof(name), "voice%1d",i);
        
    gst_object_set_name((GstObject*)voice, name);
    gst_object_set_parent((GstObject*)voice, (GstObject *)self);

    self->voices[i] = voice;
  }
    
  g_signal_connect (self->voices[0], "gstbt-ui-custom-gfx-invalidated", G_CALLBACK (on_voice_gfx_invalidated), self);
}

static void gstbt_ui_custom_gfx_interface_init(GstBtUiCustomGfxInterface *iface)
{
  iface->request = on_gfx_request;
}
