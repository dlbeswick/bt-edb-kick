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
#include "libbuzztrax-gst/ui.h"
#include <gst/gstobject.h>
#include <math.h>

#define PINK_NOISE_OCTAVES 17
#define OVERTONES 10

#define GFX_WIDTH 64
#define GFX_HEIGHT 64

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
  gfloat noise_octaves;
  gfloat noise_time;
  gfloat noise_shape_a;
  gfloat noise_shape_b;
  gfloat noise_shape_exp;
  gfloat noise_vol;
  gfloat fundamental_vol;
  gfloat overtone_vol;
  gfloat overtone_freq_factor;
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
  guint retrigger;
  gfloat retrigger_period;

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
  gfloat c_retrigger_period;
  
  guint retrig_count;
  gfloat retrig_period_cur;
  guint lcg_state[PINK_NOISE_OCTAVES];
  gfloat lcg_noise[PINK_NOISE_OCTAVES];
  gfloat noise;
  gint16 pink_accum;
  gfloat accum[OVERTONES + 1];
  gfloat seconds;
  GstClockTime running_time;
  GstClockTime time_off;
  BtEdbPropertiesSimple* props;
  GstBtToneConversion* tones;

  BtUiCustomGfx gfx;
  guint32 gfx_data[GFX_WIDTH * GFX_HEIGHT];
};

G_DEFINE_TYPE(BtEdbKickV, btedb_kickv, GST_TYPE_OBJECT)

static guint signal_bt_gfx_invalidated;

/*static gfloat db_to_gain(gfloat db) {
  return powf(10.0f, db / 20.0f);
  }*/

static const guint lcg_multiplier = 1103515245;
static const guint lcg_increment = 12345;

static inline gfloat logscale(gfloat min, gfloat max, gfloat base, gfloat x) {
  gfloat logbase = logf(base);
  return logf(MAX(1,x-min)) / logbase / (logf(max) / logbase);
}

// Return a random float between -1.0 and 1.0.
// https://www.pcg-random.org/pdf/hmc-cs-2014-0905.pdf
static inline gfloat lcg(guint* state) {
  *state = (*state + lcg_increment) * lcg_multiplier;

  // Hexadecimal floating point literals are a means to define constant real values that can be exactly
  // represented as a floating point value.
  //
  // https://www.pcg-random.org/posts/bounded-rands.html
  // https://www.exploringbinary.com/hexadecimal-floating-point-constants/
  // pg 57-58: http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf
  return -1.0 + powf(0x1.0p-32 * *state, 20) * 2;
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

static void btedb_kickv_note_on(BtEdbKickV* self, gfloat seconds, guint retrig_cnt) {
  self->seconds = seconds;
  self->retrig_count = retrig_cnt;
  self->retrig_period_cur = self->c_retrigger_period;
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
  *accum += 2 * G_PI * timedelta * freqval * (harmonic+1);
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

  const gfloat overtone_vols[OVERTONES] = {
    self->overtone0 * self->overtone_vol,
    self->overtone1 * self->overtone_vol,
    self->overtone2 * self->overtone_vol,
    self->overtone3 * self->overtone_vol,
    self->overtone4 * self->overtone_vol,
    self->overtone5 * self->overtone_vol,
    self->overtone6 * self->overtone_vol,
    self->overtone7 * self->overtone_vol,
    self->overtone8 * self->overtone_vol,
    self->overtone9 * self->overtone_vol
  };

  for (guint i = 0; i < requested_frames; ++i) {
    gfloat fundamental;
    gfloat freqval = freq(self, self->seconds, freq_start, freq_note);
    
    if (self->fundamental_vol != 0.0) {
      fundamental = osc(&self->accum[0], timedelta, freqval, 0) * self->fundamental_vol;
    } else {
      fundamental = 0.0f;
    }

    gfloat otones = 0;

    if (self->overtone_vol != 0.0) {
      for (guint j = 0; j < OVERTONES; ++j)
        // Note: self->overtone_vols already pre-multiplied above.
        if (overtone_vols[j] != 0.0)
          otones += osc(&self->accum[j+1], timedelta, freqval, (j+1)*self->overtone_freq_factor) * overtone_vols[j];
    }

    outbuf[i] = (fundamental + otones) * amp(self, self->seconds);
    
    // https://www.firstpr.com.au/dsp/pink-noise/#Voss-McCartney
    if (self->noise_vol != 0.0) {
      // Add base white noise on each sample. Otherwise, the highest frequency noise is only every other sample.
      self->noise -= self->lcg_noise[0];
      self->lcg_noise[0] = lcg(&self->lcg_state[0]);
      self->noise += self->lcg_noise[0];

      // Subtracting the old noise value from an accumulated noise value avoids having to sum x stored noise values
      // each sample. As a result, some inertia is maintained if sweeping the octave value, but not a big deal.
      //
      // Select the noise state to update by counting trailing zeroes in the noise accumulator.
      guint update_idx = __builtin_ctz(self->pink_accum)+1;
      self->noise -= self->lcg_noise[update_idx];
      if (update_idx <= self->noise_octaves) {
        gfloat gain = MIN(1.0f, self->noise_octaves - (gfloat)(update_idx+1));
        self->lcg_noise[update_idx] = lcg(&self->lcg_state[update_idx]) * gain;
        self->noise += self->lcg_noise[update_idx];
      } else {
        self->lcg_noise[update_idx] = 0;
      }

      ++self->pink_accum;

      outbuf[i] +=
        (self->noise / self->noise_octaves) *
        decay(self->seconds, 1, 0, self->c_noise_shape_a, self->c_noise_shape_b, self->c_noise_time,
              self->c_noise_shape_exp) *
        self->noise_vol;
    }

    if (self->retrig_count > 0) {
      self->retrig_period_cur -= timedelta;
      if (self->retrig_period_cur <= 0) {
        btedb_kickv_note_on(self, -self->retrig_period_cur, --self->retrig_count);
        self->retrig_period_cur = self->c_retrigger_period;
      }
    }
    
    outbuf[i] *= self->volume;
    
    self->seconds += timedelta;
  }

  for (guint i = 0; i < 11; ++i)
    self->accum[i] = fmod(self->accum[i], 2 * G_PI);
}

static const BtUiCustomGfx* on_gfx_request(BtEdbKickV* self) {
  guint32* const gfx = self->gfx.data;

  for (int i = 0; i < GFX_WIDTH*GFX_HEIGHT; ++i) {
    gfx[i] = 0x00000000;
  }

  // Show 0.5 seconds of the amplitude envelope.
  for (int i = 0; i < GFX_WIDTH; i++) {
    const gfloat data = MIN(MAX(amp(self, (gfloat)i/GFX_WIDTH * 0.5), -1), 1);
    const guint y0 = GFX_HEIGHT/2 - (GFX_HEIGHT/2 * data);
    const guint y1 = GFX_HEIGHT/2 + (GFX_HEIGHT/2 * data);
    for (int y = y0; y < y1; ++y) {
      g_assert(i + GFX_WIDTH * y < GFX_WIDTH*GFX_HEIGHT);
      gfx[i + GFX_WIDTH * y] = 0x80000000;
    }
  }

  // Show 0.5 seconds of the frequency envelope (log graph)
  gfloat data_ = MIN(MAX(freq(self, 0, 1, 0), -1), 1);
  for (int i = 0; i < GFX_WIDTH; i++) {
    const gfloat data = 0.2f +
      MIN(MAX(logscale(10, 22050, 2, 10+freq(self, (gfloat)i/GFX_WIDTH * 0.5, 1, 0)*22040), 0), 1) * 0.8f;
    
    const guint y0 = (GFX_HEIGHT-1) - (GFX_HEIGHT-1) * data_;
    const guint y1 = (GFX_HEIGHT-1) - (GFX_HEIGHT-1) * data;
    for (int y = MIN(y0,y1); y <= MAX(y0,y1); ++y) {
      g_assert(i + GFX_WIDTH * y < GFX_WIDTH*GFX_HEIGHT);
      gfx[i + GFX_WIDTH * y] = 0xFF00FFFF;
    }
    data_ = data;
  }

  return &self->gfx;
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
      btedb_kickv_note_on(self, 0, self->retrigger);
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
    self->c_retrigger_period = 0.001 * powf(10, self->retrigger_period * 3);
  
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
      g_param_spec_float("volume", "Volume", "Volume", 0, 5, 1, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_uint("retrigger", "Retrigger", "Retrigger Count", 0, 20, 0, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("retrigger-period", "Retrg Period", "Retrigger Period", 0, 1, 0, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-start", "Tone Start", "Tone Start", 0, 1, 0.55, flags));
      
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("tone-time", "Tone Time", "Tone Time", 0, 1, 0, flags));
      
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
      g_param_spec_float("amp-time", "Amp Time", "Amp Time", 0, 1, 0, flags));
      
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
      g_param_spec_float("noise-octaves", "Noise Oct.", "Noise Octaves", 1.99999,
                         PINK_NOISE_OCTAVES+0.99999, 4, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("noise-time", "Noise Time", "Noise Time", 0, 1, 0, flags));
    
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
      g_param_spec_float("fundamental-vol", "Fund. Vol", "Fundamental Volume", 0, 1, 1, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone-vol", "Otone. Vol", "Overtone Volume", 0, 1, 0, flags));

    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone-freq-factor", "Otone. FF", "Overtone Frequency Factor", 0, 10, 2, flags));
    
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone0", "Otone 0", "Overtone 0", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone1", "Otone 1", "Overtone 1", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone2", "Otone 2", "Overtone 2", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone3", "Otone 3", "Overtone 3", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone4", "Otone 4", "Overtone 4", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone5", "Otone 5", "Overtone 5", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone6", "Otone 6", "Overtone 6", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone7", "Otone 7", "Overtone 7", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone8", "Otone 8", "Overtone 8", -1, 1, 0, flags));
    g_object_class_install_property(
      aclass, idx++,
      g_param_spec_float("overtone9", "Otone 9", "Overtone 9", -1, 1, 0, flags));
  }

  signal_bt_gfx_invalidated =
    g_signal_new (
      "bt-gfx-invalidated",
      G_OBJECT_CLASS_TYPE(klass),
      G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
      0 /* offset */,
      NULL /* accumulator */,
      NULL /* accumulator data */,
      NULL /* C marshaller */,
      G_TYPE_NONE /* return_type */,
      0     /* n_params */);
  
  g_signal_new_class_handler (
    "bt-gfx-request",
    G_OBJECT_CLASS_TYPE(klass),
    G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
    G_CALLBACK (on_gfx_request),
    NULL /* accumulator */,
    NULL /* accumulator data */,
    NULL /* C marshaller */,
    G_TYPE_POINTER /* return_type */,
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
  btedb_properties_simple_add(self->props, "noise-octaves", &self->noise_octaves);
  btedb_properties_simple_add(self->props, "noise-time", &self->noise_time);
  btedb_properties_simple_add(self->props, "noise-shape-a", &self->noise_shape_a);
  btedb_properties_simple_add(self->props, "noise-shape-b", &self->noise_shape_b);
  btedb_properties_simple_add(self->props, "noise-shape-exp", &self->noise_shape_exp);
  btedb_properties_simple_add(self->props, "fundamental-vol", &self->fundamental_vol);
  btedb_properties_simple_add(self->props, "overtone-vol", &self->overtone_vol);
  btedb_properties_simple_add(self->props, "overtone-freq-factor", &self->overtone_freq_factor);
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
  btedb_properties_simple_add(self->props, "retrigger", &self->retrigger);
  btedb_properties_simple_add(self->props, "retrigger-period", &self->retrigger_period);

  self->tones = gstbt_tone_conversion_new(GSTBT_TONE_CONVERSION_EQUAL_TEMPERAMENT);
  self->seconds = 3600;

  for (guint i = 0; i < PINK_NOISE_OCTAVES; ++i)
    self->lcg_state[i] = i;

  self->gfx = (struct BtUiCustomGfx){0, GFX_WIDTH, GFX_HEIGHT, self->gfx_data};
  self->pink_accum = 1;
}
