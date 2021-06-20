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

#define PINK_NOISE_OCTAVES 4
#define OVERTONES 10


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
  gfloat noise_time;
  gfloat noise_shape_a;
  gfloat noise_shape_b;
  gfloat noise_shape_exp;
  gfloat noise_vol;
  gfloat overtone_vol;
  gfloat overtone0;
  gfloat overtone1;
  gfloat overtone2;
  gfloat overtone3;
  gfloat overtone4;
  gfloat overtone5;
  gfloat overtone6;
  gfloat overtone7;
  gfloat overtone8;
  gfloat overtone9;
  gfloat volume;

  gfloat c_tone_start;
  gfloat c_tone_time;
  gfloat c_tone_shape_a;
  gfloat c_tone_shape_b;
  gfloat c_tone_shape_exp;
  gfloat c_amp_time;
  gfloat c_amp_shape_a;
  gfloat c_amp_shape_b;
  gfloat c_amp_shape_exp;
  gfloat c_noise_time;
  gfloat c_noise_shape_a;
  gfloat c_noise_shape_b;
  gfloat c_noise_shape_exp;
  
  guint lcg_state[PINK_NOISE_OCTAVES];
  gfloat lcg_noise[PINK_NOISE_OCTAVES];
  guint lcg_accum;
  gfloat accum[OVERTONES + 1];
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

// https://www.pcg-random.org/pdf/hmc-cs-2014-0905.pdf
static const guint lcg_multiplier = 1103515245;
static const guint lcg_increment = 12345;

static inline gfloat lcg(guint* state) {
  *state = (*state + lcg_increment) * lcg_multiplier;
  
  // https://www.pcg-random.org/posts/bounded-rands.html
  // https://www.exploringbinary.com/hexadecimal-floating-point-constants/
  // pg 57-58: http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf
  return -1.0 + 0x1.0p-32 * *state * 2;
}

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
  //self->accum = 0;
}

static inline gfloat amp(const BtEdbKickV* const self, const gfloat seconds) {
  return decay(seconds, 1, 0, self->c_amp_shape_a, self->c_amp_shape_b, self->c_amp_time, self->c_amp_shape_exp);
}

static inline gfloat freq(const BtEdbKickV* const self, gfloat seconds, gfloat start, gfloat end) {
  return decay(seconds, start, end, self->c_tone_shape_a, self->c_tone_shape_b, self->c_tone_time,
               self->c_tone_shape_exp);
}

static inline gfloat osc(gfloat* accum, gfloat timedelta, gfloat freqval, gfloat harmonic) {
  gfloat result = sin(*accum);
  *accum += 2 * G_PI * timedelta * freqval * harmonic;
  return result;
}

void btedb_kickv_process(
  BtEdbKickV* const self, GstBuffer* const gstbuf, GstMapInfo* info, GstClockTime running_time, guint requested_frames,
  guint rate) {
  // Necessary to update parameters from pattern.
  //
  // The parent machine is responsible for delgating process to any children it has; the pattern control group
  // won't have called it for each voice. Although maybe it should?
  gst_object_sync_values((GstObject*)self, GST_BUFFER_PTS(gstbuf));

  const gdouble tune = powf(2, self->tune/12.0f);
  const gfloat freq_note = (gfloat)gstbt_tone_conversion_translate_from_number(self->tones, self->note) * tune;
  const gfloat freq_start = self->c_tone_start * tune;

  gfloat* outbuf = (gfloat*)(info->data);
  const gfloat timedelta = 1.0f/rate;

  gfloat overtone_vols[OVERTONES];
  overtone_vols[0] = self->overtone0 * self->overtone_vol;
  overtone_vols[1] = self->overtone1 * self->overtone_vol;
  overtone_vols[2] = self->overtone2 * self->overtone_vol;
  overtone_vols[3] = self->overtone3 * self->overtone_vol;
  overtone_vols[4] = self->overtone4 * self->overtone_vol;
  overtone_vols[5] = self->overtone5 * self->overtone_vol;
  overtone_vols[6] = self->overtone6 * self->overtone_vol;
  overtone_vols[7] = self->overtone7 * self->overtone_vol;
  overtone_vols[8] = self->overtone8 * self->overtone_vol;
  overtone_vols[9] = self->overtone9 * self->overtone_vol;
  
  for (guint i = 0; i < requested_frames; ++i) {
    {
      gfloat freqval = freq(self, self->seconds, freq_start, freq_note);
      gfloat val = osc(&self->accum[0], timedelta, freqval, 1);

      for (guint j = 0; j < OVERTONES; ++j)
        val += osc(&self->accum[j+1], timedelta, freqval, j+2) * overtone_vols[j];
      
      outbuf[i] = val * amp(self, self->seconds);
    }
    
    // https://www.firstpr.com.au/dsp/pink-noise/#Voss-McCartney
    {
      for (guint j = 0; j < self->lcg_accum % PINK_NOISE_OCTAVES; ++j)
        self->lcg_noise[j] = lcg(&self->lcg_state[j]) / PINK_NOISE_OCTAVES;

      gfloat noise = 0;
      for (guint j = 0; j < PINK_NOISE_OCTAVES; ++j)
        noise += self->lcg_noise[j];
      ++self->lcg_accum;

      outbuf[i] +=
        noise *
        decay(self->seconds, 1, 0, self->c_noise_shape_a, self->c_noise_shape_b, self->c_noise_time,
              self->c_noise_shape_exp) *
        self->noise_vol;
    }
    
    outbuf[i] *= self->volume;
    
    self->seconds += timedelta;
  }

  for (guint i = 0; i < 11; ++i)
    self->accum[i] = fmod(self->accum[i], 2 * G_PI);
}

static void update_gfx(BtEdbKickV* self, void* callback) {
  const int width = 64;
  const int height = 64;
  u_int32_t gfx[width*height];

  for (int i = 0; i < width*height; ++i) {
    gfx[i] = 0x00000000;
  }

  // Show 0.5 seconds of the amplitude envelope.
  for (int i = 0; i < width; i++) {
    const gfloat data = MIN(MAX(amp(self, (gfloat)i/width * 0.5), -1), 1);
    const guint y0 = height/2 - (height/2 * data);
    const guint y1 = height/2 + (height/2 * data);
    for (int y = y0; y < y1; ++y) {
      g_assert(i + width * y < width*height);
      gfx[i + width * y] = 0x80000000;
    }
  }

  // Show 0.5 seconds of the frequency envelope.
  gfloat data_ = MIN(MAX(freq(self, 0, 1, 0), -1), 1);
  for (int i = 0; i < width; i++) {
    const gfloat data = MIN(MAX(freq(self, (gfloat)i/width * 0.5, 1, 0), -1), 1);
    const guint y0 = height - height * data_;
    const guint y1 = height - height * data;
    for (int y = MIN(y0,y1); y < MAX(y0,y1); ++y) {
      g_assert(i + width * y < width*height);
      gfx[i + width * y] = 0xFF00FFFF;
    }
    data_ = data;
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

    self->c_tone_start = powf(2, self->tone_start * 14.5);
    self->c_tone_shape_a = 0.01 * powf(10, self->tone_shape_a * 3);
    self->c_tone_shape_b = 0.01 * powf(10, self->tone_shape_b * 3);
    self->c_tone_time = 0.001 * powf(10, self->tone_time * 4);
    self->c_tone_shape_exp = 0.01 * powf(10, self->tone_shape_exp * 3);
    self->c_amp_shape_a = 0.01 * powf(10, self->amp_shape_a * 3);
    self->c_amp_shape_b = 0.01 * powf(10, self->amp_shape_b * 3);
    self->c_amp_time = 0.001 * powf(10, self->amp_time * 4);
    self->c_amp_shape_exp = 0.01 * powf(10, self->amp_shape_exp * 3);
    self->c_noise_shape_a = 0.01 * powf(10, self->noise_shape_a * 3);
    self->c_noise_shape_b = 0.01 * powf(10, self->noise_shape_b * 3);
    self->c_noise_time = 0.001 * powf(10, self->noise_time * 4);
    self->c_noise_shape_exp = 0.01 * powf(10, self->noise_shape_exp * 3);
  
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
      g_param_spec_float("volume", "Volume", "Volume", 0, 5, 0, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-start", "Tone Start", "Tone Start", 0, 1, 0.55, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-time", "Tone Time", "Tone Time", 0, 1, 0.5, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-a", "Tone A", "Tone Shape A", 0, 1, 0.5, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-b", "Tone B", "Tone Shape B", 0, 1, 0.5, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-shape-exp", "Tone Exp", "Tone Shape Exponent", 0, 1, 0.672, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-time", "Amp Time", "Amp Time", 0, 1, 0.5, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-a", "Amp A", "Amp Shape A", 0, 1, 0.5, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-b", "Amp B", "Amp Shape B", 0, 1, 0.5, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("amp-shape-exp", "Amp Exp", "Amp Shape Exponent", 0, 1, 0.672, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tune", "Tune", "Tune", -24, 24, 0, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-vol", "Noise Vol", "Noise Volume", 0, 4, 0.5, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-time", "Noise Time", "Noise Time", 0, 1, 0.16, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-shape-a", "Noise A", "Noise Shape A", 0, 1, 0.0, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-shape-b", "Noise B", "Noise Shape B", 0, 1, 0.5, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-shape-exp", "Noise Exp", "Noise Shape Exponent", 0, 1, 0.672, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone-vol", "Overtone Vol", "Overtone Volume", 0, 1, 0, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone0", "Overtone 0", "Overtone 0", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone1", "Overtone 1", "Overtone 1", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone2", "Overtone 2", "Overtone 2", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone3", "Overtone 3", "Overtone 3", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone4", "Overtone 4", "Overtone 4", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone5", "Overtone 5", "Overtone 5", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone6", "Overtone 6", "Overtone 6", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone7", "Overtone 7", "Overtone 7", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone8", "Overtone 8", "Overtone 8", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone9", "Overtone 9", "Overtone 9", -1, 1, 0, flags));
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
  btedb_properties_simple_add(self->props, "noise-vol", &self->noise_vol);
  btedb_properties_simple_add(self->props, "noise-time", &self->noise_time);
  btedb_properties_simple_add(self->props, "noise-shape-a", &self->noise_shape_a);
  btedb_properties_simple_add(self->props, "noise-shape-b", &self->noise_shape_b);
  btedb_properties_simple_add(self->props, "noise-shape-exp", &self->noise_shape_exp);
  btedb_properties_simple_add(self->props, "overtone-vol", &self->overtone_vol);
  btedb_properties_simple_add(self->props, "overtone0", &self->overtone0);
  btedb_properties_simple_add(self->props, "overtone1", &self->overtone1);
  btedb_properties_simple_add(self->props, "overtone2", &self->overtone2);
  btedb_properties_simple_add(self->props, "overtone3", &self->overtone3);
  btedb_properties_simple_add(self->props, "overtone4", &self->overtone4);
  btedb_properties_simple_add(self->props, "overtone5", &self->overtone5);
  btedb_properties_simple_add(self->props, "overtone6", &self->overtone6);
  btedb_properties_simple_add(self->props, "overtone7", &self->overtone7);
  btedb_properties_simple_add(self->props, "overtone8", &self->overtone8);
  btedb_properties_simple_add(self->props, "overtone9", &self->overtone9);
  btedb_properties_simple_add(self->props, "volume", &self->volume);

  self->tones = gstbt_tone_conversion_new(GSTBT_TONE_CONVERSION_EQUAL_TEMPERAMENT);
  self->seconds = 3600;

  g_signal_connect (self, "bt-gfx-request", G_CALLBACK (update_gfx), 0);

  for (guint i = 0; i < PINK_NOISE_OCTAVES; ++i)
    self->lcg_state[i] = i;
}
