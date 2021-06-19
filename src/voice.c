/*
  Kick machine for Buzztrax
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

#include "src/voice.h"
#include "src/debug.h"
#include "src/properties_simple.h"
#include "libbuzztrax-gst/musicenums.h"
#include "libbuzztrax-gst/toneconversion.h"
#include <gst/gstobject.h>
#include <math.h>

struct _BtEdbKickV
{
  GstObject parent;

  GstBtNote note;
  gfloat tone_start;
  gfloat tone_time;
  gfloat tone_shape_a;
  gfloat tone_shape_b;
  gfloat tone_shape_exp;
  gfloat amp_time;
  gfloat amp_shape_a;
  gfloat amp_shape_b;
  gfloat amp_shape_exp;
  gfloat tune;

  gfloat accum;
  gfloat seconds;
  GstClockTime running_time;
  GstClockTime time_off;
  BtEdbPropertiesSimple* props;
  GstBtToneConversion* tones;
};

G_DEFINE_TYPE(BtEdbKickV, btedb_kickv, GST_TYPE_OBJECT)

static guint signal_bt_gfx_present;
static guint signal_bt_gfx_invalidated;

/*static gfloat db_to_gain(gfloat db) {
  return powf(10.0f, db / 20.0f);
  }*/

static inline gfloat plerp(gfloat a, gfloat b, gfloat alpha, gfloat power) {
  return powf(a + (b-a) * MAX(MIN(alpha,1),0), power);
}

static inline gfloat decay(gfloat t, gfloat start, gfloat end, gfloat a, gfloat b, gfloat decay_time, gfloat power) {
  return start + (end - start) * (1 - expf(-t / plerp(a, b, t / decay_time, power)));
}

void btedb_kickv_note_off(BtEdbKickV* self, GstClockTime time) {
  self->time_off = time;
}

void btedb_kickv_note_on(BtEdbKickV* self, GstClockTime time, gfloat anticlick) {
  self->seconds = 0;
  self->accum = 0;
}

static inline gfloat amp(const BtEdbKickV* const self, const gfloat seconds) {
  return decay(seconds, 1, 0, self->amp_shape_a, self->amp_shape_b, self->amp_time, self->amp_shape_exp);
}

static void process(const BtEdbKickV* const self, gfloat* const outbuf, guint requested_frames,
                    guint rate, guint note, gfloat* accum, gfloat* seconds) {
  
  const gfloat freq_note = (gfloat)gstbt_tone_conversion_translate_from_number(self->tones, note);

  gdouble tune = powf(2, self->tune/12.0f);
  
  for (guint i = 0; i < requested_frames; ++i) {
    const gfloat freq =
      decay(*seconds, self->tone_start * tune, freq_note * tune, self->tone_shape_a, self->tone_shape_b,
            self->tone_time, self->tone_shape_exp);
      
    const gfloat ampval = amp(self, *seconds);

    outbuf[i] = sin(*accum) * ampval;
    *accum += 2 * G_PI * 1.0f/rate * freq;
    *seconds += 1.0f/rate;
  }
  
  *accum = fmod(*accum, 2 * G_PI);
}

void btedb_kickv_process(
  BtEdbKickV* const self, GstBuffer* const gstbuf, GstMapInfo* info, GstClockTime running_time, guint samples,
  guint rate) {
  // Necessary to update parameters from pattern.
  //
  // The parent machine is responsible for delgating process to any children it has; the pattern control group
  // won't have called it for each voice. Although maybe it should?
  gst_object_sync_values((GstObject*)self, GST_BUFFER_PTS(gstbuf));

  process(self, (gfloat*)(info->data), samples, rate, self->note, &self->accum, &self->seconds);
}

static void update_gfx(BtEdbKickV* self, void* callback) {
  const int width = 64;
  const int height = 64;
  u_int32_t gfx[width*height];

  for (int i = 0; i < width*height; ++i) {
    gfx[i] = 0x00000000;
  }

  for (int i = 0; i < width; i++) {
    gfloat data = MIN(MAX(amp(self, (gfloat)i/width), -1), 1);
    const guint y0 = height/2 - (height/2 * data);
    const guint y1 = height/2 + (height/2 * data);
    for (int y = y0; y < y1; ++y) {
      g_assert(i + width * y < width*height);
      gfx[i + width * y] = 0x80000000;
    }
  }

  GBytes* bytes = g_bytes_new(gfx, sizeof(gfx));
  g_signal_emit(self, signal_bt_gfx_present, 0, width, height, bytes);
  g_bytes_unref(bytes);
}

static void set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  BtEdbKickV* self = (BtEdbKickV*)object;

  switch (prop_id) {
  case 1: {
    GstBtNote note = g_value_get_enum(value);
    if (note == GSTBT_NOTE_OFF) {
      btedb_kickv_note_off(self, self->running_time);
    } else if (note != GSTBT_NOTE_NONE) {
      self->note = note;
      btedb_kickv_note_on(self, self->running_time, 0.01f);
    }
    break;
  }
  default:
    g_assert(self->props);
    btedb_properties_simple_set(self->props, pspec, value);
    g_signal_emit(self, signal_bt_gfx_invalidated, 0);
  }
}

static void get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  BtEdbKickV* self = (BtEdbKickV*)object;
  btedb_properties_simple_get(self->props, pspec, value);
}

static void dispose(GObject* object) {
  BtEdbKickV* self = (BtEdbKickV*)object;
  btedb_properties_simple_free(self->props);
  self->props = 0;
}

static void btedb_kickv_class_init(BtEdbKickVClass* const klass) {
  {
    GObjectClass* const aclass = (GObjectClass*)klass;
    aclass->set_property = set_property;
    aclass->get_property = get_property;
    aclass->dispose = dispose;

    // Note: variables will not be set to default values unless G_PARAM_CONSTRUCT is given.
    const GParamFlags flags =
      (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

    guint idx = 1;
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_enum("note", "Note", "Note", GSTBT_TYPE_NOTE, GSTBT_NOTE_NONE,
                        G_PARAM_WRITABLE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-start", "Tone Start", "Tone Start", 1, 22050, 302, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-time", "Tone Time", "Tone Time", FLT_MIN, 1, 0.078125, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-a", "Tone A", "Tone Shape A", 0, 0.2, 0.028571, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-b", "Tone B", "Tone Shape B", 0, 0.2, 0.029799, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-exp", "Tone Exp", "Tone Shape Exponent", 0, 3, 1.047619, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-time", "Amp Time", "Amp Time", FLT_MIN, 1, 0.485714, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-a", "Amp A", "Amp Shape A", 0, 0.2, 0.057617, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-b", "Amp B", "Amp Shape B", 0, 0.2, 0.104762, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-exp", "Amp Exp", "Amp Shape Exponent", 0, 3, 0.826922, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tune", "Tune", "Tune", -24, 24, 0, flags));
  }

  signal_bt_gfx_present = 
    g_signal_new (
      "bt-gfx-present",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      0 /* offset */,
      NULL /* accumulator */,
      NULL /* accumulator data */,
      NULL /* C marshaller */,
      G_TYPE_NONE /* return_type */,
      3     /* n_params */,
      G_TYPE_UINT /* param width */,
      G_TYPE_UINT /* param height */,
      G_TYPE_BYTES /* param data */
      );

  signal_bt_gfx_invalidated =
    g_signal_new (
      "bt-gfx-invalidated",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      0 /* offset */,
      NULL /* accumulator */,
      NULL /* accumulator data */,
      NULL /* C marshaller */,
      G_TYPE_NONE /* return_type */,
      0     /* n_params */);
  
  g_signal_new (
    "bt-gfx-request",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
    0 /* offset */,
    NULL /* accumulator */,
    NULL /* accumulator data */,
    NULL /* C marshaller */,
    G_TYPE_NONE /* return_type */,
    0     /* n_params */);
}

static void btedb_kickv_init(BtEdbKickV* const self) {
  self->props = btedb_properties_simple_new((GObject*)self);
  btedb_properties_simple_add(self->props, "tone-start", &self->tone_start);
  btedb_properties_simple_add(self->props, "tone-time", &self->tone_time);
  btedb_properties_simple_add(self->props, "tone-shape-a", &self->tone_shape_a);
  btedb_properties_simple_add(self->props, "tone-shape-b", &self->tone_shape_b);
  btedb_properties_simple_add(self->props, "tone-shape-exp", &self->tone_shape_exp);
  btedb_properties_simple_add(self->props, "amp-time", &self->amp_time);
  btedb_properties_simple_add(self->props, "amp-shape-a", &self->amp_shape_a);
  btedb_properties_simple_add(self->props, "amp-shape-b", &self->amp_shape_b);
  btedb_properties_simple_add(self->props, "amp-shape-exp", &self->amp_shape_exp);
  btedb_properties_simple_add(self->props, "tune", &self->tune);

  self->tones = gstbt_tone_conversion_new(GSTBT_TONE_CONVERSION_EQUAL_TEMPERAMENT);
  self->seconds = 3600;

  g_signal_connect (self, "bt-gfx-request", G_CALLBACK (update_gfx), 0);
}
