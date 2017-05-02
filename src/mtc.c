/* mtc -- LV2 MIDI timecode generator
 *
 * Copyright (C) 2017 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#define MTC_URI "http://gareus.org/oss/lv2/mtc"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

#include "../libtimecode/timecode.h"

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Sequence;
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Float;
	LV2_URID atom_Int;
	LV2_URID atom_Long;
	LV2_URID time_Position;
	LV2_URID time_speed;
	LV2_URID time_frame;
} MTCURIs;

typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;
	LV2_Atom_Sequence* midiout;

	float* p_fps;
	float* p_sync;
	float* p_transport;
	float* p_rewind;
	float* p_zeropos;
	float* p_tc[4];

	/* Cached Ports */
	float c_fps;
	float c_transport;
	float c_rewind;

	/* atom-forge and URI mapping */
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Frame frame;
	MTCURIs uris;

	/* LV2 Output */
	LV2_Log_Log* log;
	LV2_Log_Logger logger;

	/* Host Time */
	bool     host_info;
	int64_t  host_frame;
	float    host_speed;

	/* Settings */
	double sample_rate;
	uint8_t mtc_tc;
	TimecodeRate framerate;

	/* State */
	bool     rolling;
	int64_t sample_at_cycle_start; // host-time

  TimecodeTime cur_tc;
	int next_qf_to_send;
	double next_qf_tme;

} MTC;

/* *****************************************************************************
 * helper functions
 */

/** map uris */
static void
map_uris (LV2_URID_Map* map, MTCURIs* uris)
{
	uris->atom_Blank          = map->map (map->handle, LV2_ATOM__Blank);
	uris->atom_Object         = map->map (map->handle, LV2_ATOM__Object);
	uris->midi_MidiEvent      = map->map (map->handle, LV2_MIDI__MidiEvent);
	uris->atom_Sequence       = map->map (map->handle, LV2_ATOM__Sequence);
	uris->time_Position       = map->map (map->handle, LV2_TIME__Position);
	uris->atom_Long           = map->map (map->handle, LV2_ATOM__Long);
	uris->atom_Int            = map->map (map->handle, LV2_ATOM__Int);
	uris->atom_Float          = map->map (map->handle, LV2_ATOM__Float);
	uris->time_speed          = map->map (map->handle, LV2_TIME__speed);
	uris->time_frame          = map->map (map->handle, LV2_TIME__frame);
}

/**
 * Update the current position based on a host message. This is called by
 * run() when a time:Position is received.
 */
static bool
update_position (MTC* self, const LV2_Atom_Object* obj)
{
	const MTCURIs* uris = &self->uris;

	LV2_Atom* speed = NULL;
	LV2_Atom* frame = NULL;

	lv2_atom_object_get (
			obj,
			uris->time_speed, &speed,
			uris->time_frame, &frame,
			NULL);

	if (   speed && speed->type == uris->atom_Float
			&& frame && frame->type == uris->atom_Long)
	{
		self->host_speed = ((LV2_Atom_Float*)speed)->body;
		self->host_frame = ((LV2_Atom_Long*)frame)->body;
		self->host_info  = true;
		if (self->host_frame < 0) {
			self->host_info  = false;
		}
		return true;
	}
	return false;
}

/**
 * add a midi message to the output port
 */
static void
forge_midimessage (MTC* self,
                   uint32_t tme,
                   const uint8_t* const buffer,
                   uint32_t size)
{
	LV2_Atom midiatom;
	midiatom.type = self->uris.midi_MidiEvent;
	midiatom.size = size;

	if (0 == lv2_atom_forge_frame_time (&self->forge, tme)) return;
	if (0 == lv2_atom_forge_raw (&self->forge, &midiatom, sizeof (LV2_Atom))) return;
	if (0 == lv2_atom_forge_raw (&self->forge, buffer, size)) return;
	lv2_atom_forge_pad (&self->forge, sizeof (LV2_Atom) + size);
}

/* *****************************************************************************
 * Timecode helper
 */
static void
set_up_framerate (MTC* self, int fps_port)
{
	TimecodeRate* fr = &self->framerate;
	fr->subframes = 0;
	switch (fps_port) {
		case 0:
			fr->num = 24; fr->den = 1; fr->drop = 0;
			self->mtc_tc = 0x00;
			break;
		case 1:
			fr->num = 25; fr->den = 1; fr->drop = 0;
			self->mtc_tc = 0x20;
			break;
		case 2:
			fr->num = 30000; fr->den = 1001; fr->drop = 1;
			self->mtc_tc = 0x40;
			break;
		default:
			fr->num = 30; fr->den = 1; fr->drop = 0;
			self->mtc_tc = 0x60;
			break;
	}
}

/* *****************************************************************************
 * MTC MIDI Msgs
 */

static void
queue_mtc_quarterframe (MTC* self, const uint32_t tme, const TimecodeTime* const t, const int qf)
{
	uint8_t mtc_msg = 0;
	const uint8_t mtc_tc = self->mtc_tc;
	switch (qf) {
		case 0: mtc_msg =  0x00 |  (t->frame & 0xf); break;
		case 1: mtc_msg =  0x10 | ((t->frame & 0xf0) >> 4); break;
		case 2: mtc_msg =  0x20 |  (t->second & 0xf); break;
		case 3: mtc_msg =  0x30 | ((t->second & 0xf0) >>4 ); break;
		case 4: mtc_msg =  0x40 |  (t->minute & 0xf); break;
		case 5: mtc_msg =  0x50 | ((t->minute & 0xf0) >> 4); break;
		case 6: mtc_msg =  0x60 |  ((mtc_tc | (t->hour % 24)) & 0xf); break;
		case 7: mtc_msg =  0x70 | (((mtc_tc | (t->hour % 24)) & 0xf0) >>4); break;
		default: assert (0); break;
	}

	uint8_t mmsg[2];
	mmsg[0] = 0xf1;
	mmsg[1] = mtc_msg;
	forge_midimessage (self, tme, mmsg, 2);
}

static void
queue_mtc_sysex (MTC* self, const uint32_t tme, const TimecodeTime* const t)
{
	uint8_t sysex[10];

  sysex[0]  = (unsigned char) 0xf0; // fixed
  sysex[1]  = (unsigned char) 0x7f; // fixed
  sysex[2]  = (unsigned char) 0x7f; // sysex channel
  sysex[3]  = (unsigned char) 0x01; // fixed
  sysex[4]  = (unsigned char) 0x01; // fixed
  sysex[5]  = (unsigned char) 0x00; // hour
  sysex[6]  = (unsigned char) 0x00; // minute
  sysex[7]  = (unsigned char) 0x00; // seconds
  sysex[8]  = (unsigned char) 0x00; // frame
  sysex[9]  = (unsigned char) 0xf7; // fixed

  sysex[5] |= (unsigned char) (self->mtc_tc & 0x60);
  sysex[5] |= (unsigned char) ((t->hour % 24) & 0x1f);
  sysex[6] |= (unsigned char) (t->minute & 0x7f);
  sysex[7] |= (unsigned char) (t->second & 0x7f);
  sysex[8] |= (unsigned char) (t->frame & 0x7f);

	forge_midimessage (self, tme, sysex, 10);
}


/* *****************************************************************************
 * LV2 Plugin
 */

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	MTC* self = (MTC*)calloc (1, sizeof (MTC));
	LV2_URID_Map* map = NULL;

	int i;
	for (i=0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			self->log = (LV2_Log_Log*)features[i]->data;
		}
	}

	lv2_log_logger_init (&self->logger, map, self->log);

	if (!map) {
		lv2_log_error (&self->logger, "MTC.lv2 error: Host does not support urid:map\n");
		free (self);
		return NULL;
	}

	lv2_atom_forge_init (&self->forge, map);
	map_uris (map, &self->uris);

	self->sample_rate = rate;
	self->rolling = false;

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	MTC* self = (MTC*)instance;

	switch (port) {
		case 0:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->midiout = (LV2_Atom_Sequence*)data;
			break;
		case 2:
			self->p_fps = (float*)data;
			break;
		case 3:
			self->p_sync = (float*)data;
			break;
		case 4:
			self->p_transport = (float*)data;
			break;
		case 5:
			self->p_rewind = (float*)data;
			break;
		case 6:
			self->p_zeropos = (float*)data;
			break;
		case 7:
			self->p_tc[0] = (float*)data;
			break;
		case 8:
			self->p_tc[1] = (float*)data;
			break;
		case 9:
			self->p_tc[2] = (float*)data;
			break;
		case 10:
			self->p_tc[3] = (float*)data;
			break;
		default:
			break;
	}
}


static void
run (LV2_Handle instance, uint32_t n_samples)
{
	MTC* self = (MTC*)instance;
	if (!self->midiout || !self->control) {
		return;
	}

	/* initialize output port */
	const uint32_t capacity = self->midiout->atom.size;
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->midiout, capacity);
	lv2_atom_forge_sequence_head (&self->forge, &self->frame, 0);

	/* process control events */
	LV2_Atom_Event* ev = lv2_atom_sequence_begin (&(self->control)->body);
	while (!lv2_atom_sequence_is_end (&(self->control)->body, (self->control)->atom.size, ev)) {
		if (ev->body.type == self->uris.atom_Blank || ev->body.type == self->uris.atom_Object) {
			const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == self->uris.time_Position) {
				update_position (self, obj);
			}
		}
		ev = lv2_atom_sequence_next (ev);
	}

	bool send_full_tc = false;
	bool rolling;
	int64_t sample_at_cycle_start;
	float speed;

	/* set position and speed */
	if (*self->p_sync > 0 && self->host_info) {
		rolling = self->host_speed != 0;
		speed = self->host_speed;
		if (self->sample_at_cycle_start != self->host_frame) {
#ifdef DEBUG
			printf ("HOST TIME MISMATCH %ld - %ld = %ld\n",
					self->sample_at_cycle_start, self->host_frame,
					self->sample_at_cycle_start - self->host_frame
					);
#endif
			/* handle "micro" jumps
			 * if speed != 1.0 and synced to host, the next cycle's start
			 * may not idendical to sample_at_cycle_end (rounding, interpolation)
			 * e.g. Ardour uses cubic interpolation
			 */
			if (rolling && llabs (self->sample_at_cycle_start - self->host_frame) <= 2) {
				// TODO only if it'd be skipped (depending on speed/direction)
				if (llabs ((int64_t)self->next_qf_tme - self->host_frame) <= 2) {
#ifdef DEBUG
					printf ("ADJUST %.1f -> %ld\n", self->next_qf_tme, self->host_frame);
#endif
					self->next_qf_tme = self->host_frame;
				}
			} else {
				/* locate */
				send_full_tc = true;
			}
		}
		sample_at_cycle_start = self->host_frame;
	} else {
		rolling = *self->p_transport > 0;
		speed = rolling ? 1.f: 0.f;
		sample_at_cycle_start = self->sample_at_cycle_start;
	}

	/* handle reset/rewind */
	if (*self->p_rewind > 0 && self->c_rewind <= 0) { // only rising edge.
		send_full_tc = true;
		if (*self->p_sync > 0 && self->host_info) {
			; // host sync
		} else {
			sample_at_cycle_start = *self->p_zeropos * self->sample_rate;
		}
	}
	self->c_rewind = *self->p_rewind;

	/* set fps */
	if (self->c_fps != *self->p_fps) {
		self->c_fps = *self->p_fps;
		send_full_tc = true;
		set_up_framerate (self, (int)rintf (self->c_fps));
	}

	/* start <> stop transition */
	if (self->rolling != rolling) {
		send_full_tc = true;
	}

	/* locate */
	if (!rolling && self->sample_at_cycle_start != sample_at_cycle_start) {
		send_full_tc = true;
	}

	if (speed > 0 && self->next_qf_tme < sample_at_cycle_start) {
		send_full_tc = true;
	}

	if (speed < 0 && self->next_qf_tme > sample_at_cycle_start) {
		send_full_tc = true;
	}

	if (send_full_tc) {
		TimecodeTime t;
		timecode_sample_to_time (&t, &self->framerate, self->sample_rate, sample_at_cycle_start);
		assert (t.subframe == 0);
		queue_mtc_sysex (self, 0, &t);

		int64_t tczero = timecode_to_sample (&t, &self->framerate, self->sample_rate);
		int64_t tcdiff = sample_at_cycle_start - tczero;
#ifdef DEBUG
		printf ("Now: %ld  == %02d:%02d:%02d:%02d + %ld, speed = %f\n",
				sample_at_cycle_start,
				t.hour, t.minute, t.second, t.frame,
				tcdiff, speed
				);
#endif
		assert (tcdiff >= 0);

		self->next_qf_to_send = 0;
		self->next_qf_tme = sample_at_cycle_start;

    /* 24, 30 drop and 30 non-drop, the frame number computed from quarter frames is always even.
		 * only for 25fps it is valid to send odd TC, depending on which frame-number the sequence starts.
     */
		if (self->mtc_tc != 0x20 && (t.frame % 2) == 1) {
			tcdiff = -1; // force update
		}

		/* calculate position of next "0" quarter-frame */
		if (rolling && tcdiff != 0) {
			do {
				if (speed > 0) {
					timecode_time_increment (&t, &self->framerate);
				} else {
					if (timecode_time_decrement (&t, &self->framerate)) {
						t.hour = t.minute = t.second = t.frame = 0;
					}
				}
			} while (self->mtc_tc != 0x20 && (t.frame % 2) == 1);

			tczero = timecode_to_sample (&t, &self->framerate, self->sample_rate);
			tcdiff = tczero - sample_at_cycle_start;
			assert ((speed > 0 && tcdiff >= 0) || (speed < 0 && tcdiff <= 0));
			self->next_qf_tme = sample_at_cycle_start + tcdiff;
		}
#ifdef DEBUG
		printf ("NEXT QF0: %.1f  == %02d:%02d:%02d:%02d\n",
				self->next_qf_tme,
				t.hour, t.minute, t.second, t.frame);
#endif
		memcpy (&self->cur_tc, &t, sizeof(TimecodeTime));
	}

	const int distance = (int) floor (speed * (float)n_samples);
	int64_t sample_at_cycle_end;
	if (-distance >= sample_at_cycle_start) {
		sample_at_cycle_end = 0;
	} else {
		sample_at_cycle_end = sample_at_cycle_start + distance;
		assert (speed != 0.f || sample_at_cycle_start == sample_at_cycle_end);
	}

	if (rolling) {
		assert (speed != 0.f);

		const double fptcf = timecode_frames_per_timecode_frame (&self->framerate, self->sample_rate);
		const double spqf = fptcf / 4.0;

		int qf = self->next_qf_to_send;
		double next_qf_tme = self->next_qf_tme;

		// §6.3.1.4 - float to integer discards fractional part (rounds towards zero)
		while (
				(speed > 0 && next_qf_tme >= sample_at_cycle_start && next_qf_tme < sample_at_cycle_end)
				||
				(speed < 0 && next_qf_tme <= sample_at_cycle_start && next_qf_tme > sample_at_cycle_end)
				)
		{

			if (qf == 0) {
				timecode_sample_to_time (&self->cur_tc, &self->framerate, self->sample_rate, next_qf_tme);
				assert (self->cur_tc.subframe == 0);
#ifdef DEBUG
				printf ("QF0: %02d:%02d:%02d:%02d\n", self->cur_tc.hour, self->cur_tc.minute, self->cur_tc.second, self->cur_tc.frame);
#endif
			}

			if (speed < 0) {
				if (--qf < 0) { qf = 7; }
			}

			uint32_t pos = (next_qf_tme - sample_at_cycle_start) / speed;
			assert (pos >= 0 && pos < n_samples);

			queue_mtc_quarterframe (self, pos, &self->cur_tc, qf);

			if (speed > 0) {
				if (++qf > 7) { qf = 0; }
				next_qf_tme += spqf;
			} else {
				next_qf_tme -= spqf;
			}
		}

		self->next_qf_to_send = qf;
		self->next_qf_tme = next_qf_tme;
	}

	*self->p_tc[0] = self->cur_tc.hour;
	*self->p_tc[1] = self->cur_tc.minute;
	*self->p_tc[2] = self->cur_tc.second;
	*self->p_tc[3] = self->cur_tc.frame;

	/* save state */
	self->rolling = rolling;

	self->sample_at_cycle_start = sample_at_cycle_end;
	/* keep track of host-time */
	if (-distance >= self->host_frame) {
		self->host_frame = 0;
	} else {
		self->host_frame += distance;
	}
}

static void
cleanup (LV2_Handle instance)
{
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
	MTC_URI,
	instantiate,
	connect_port,
	NULL,
	run,
	NULL,
	cleanup,
	extension_data
};

#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
#    define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
#    define LV2_SYMBOL_EXPORT  __attribute__ ((visibility ("default")))
#endif
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
	case 0:
		return &descriptor;
	default:
		return NULL;
	}
}
