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

#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_DECLARE_FINAL_TYPE(BtEdbKickV, btedb_kickv, BTEDB, KICKV, GstObject);

void btedb_kickv_process(BtEdbKickV* self, GstBuffer* gstbuf, GstMapInfo* info, GstClockTime running_time,
  guint requested_frames, guint rate);
