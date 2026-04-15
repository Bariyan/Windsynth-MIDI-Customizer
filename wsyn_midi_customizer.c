#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "lv2/core/lv2.h"
#include "lv2/urid/urid.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/atom/forge.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/resize-port/resize-port.h"

#define PLUGIN_URI "urn:bariyan:wsyn-midi-customizer"

typedef enum {
    PORT_MIDI_IN         = 0,
    PORT_MIDI_OUT        = 1,
    PORT_CENTERIN        = 2,
    PORT_DEADZONE        = 3,
    PORT_DIST_U          = 4,
    PORT_DIST_L          = 5,
    PORT_CURVE_U         = 6,
    PORT_CURVE_L         = 7,
    PORT_V_FLOOR         = 8,
    PORT_TRANSPOSE       = 9,
    PORT_VEL0_AS_NOTEOFF = 10, 
    PORT_CHANNEL         = 11,
    PORT_OUTCAP          = 12
} PortIndex;

typedef enum { CURVE_LIN = 0, CURVE_EXP = 1, CURVE_LOG = 2 } CurveType;
typedef struct { CurveType type; double k; } CurveParams;

typedef struct {
    const LV2_Atom_Sequence* in_seq;
    LV2_Atom_Sequence* out_seq;

    const float* channel;
    const float* transpose;
    const float* center_in;
    const float* deadzone;
    const float* dist_u;
    const float* dist_l;
    const float* curve_u;
    const float* curve_l;
    const float* v_floor;
    const float* out_capacity;
    const float* vel0_as_noteoff; 

    /* Internal state */
    uint8_t last_breath;

    LV2_URID_Map* map;
    const LV2_Options_Option* options;
    const LV2_Resize_Port_Resize* resize;

    LV2_URID urid_midi_event;
    LV2_URID urid_bufsz_sequenceSize;
    LV2_URID urid_atom_int;
    LV2_URID urid_atom_long;

    uint32_t host_sequence_size;

    LV2_Atom_Forge forge;
    LV2_Atom_Forge_Frame seq_frame;
} WSCustomizer;

static int clamp_int(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}


// Write an Atom (header + body) into the current forge output.
// Returns 1 on success, 0 on failure (e.g. buffer full).
static int forge_write_atom_body(LV2_Atom_Forge* forge,
                                 uint32_t type,
                                 const void* body,
                                 uint32_t body_size)
{
    // lv2_atom_forge_atom() writes the LV2_Atom header (size/type).
    if (!lv2_atom_forge_atom(forge, body_size, type)) {
        return 0;
    }
    // Then write the payload.
    if (body_size > 0) {
        if (!lv2_atom_forge_write(forge, body, body_size)) {
            return 0;
        }
    }
    return 1;
}
static CurveParams decode_curve_params(int v) {
    CurveParams p;
    v = clamp_int(v, 0, 10);
    if (v == 0) { p.type = CURVE_LIN; p.k = 1.0; }
    else if (v <= 5) { p.type = CURVE_EXP; p.k = (double)v; }
    else { p.type = CURVE_LOG; p.k = (double)(v - 5); }
    return p;
}

static double apply_curve(double x, CurveType type, double k) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    switch (type) {
        case CURVE_EXP: return pow(x, k);
        case CURVE_LOG: return 1.0 - pow(1.0 - x, k);
        default:        return x;
    }
}

static int map_pitchbend(int vin, int cin, int dz, int du, int dl, CurveParams cu, CurveParams cl) {
    const int c0 = 8192;
    vin = clamp_int(vin, 0, 16383);
    const int diff = vin - cin;
    const int abs_diff = (diff < 0) ? -diff : diff;
    if (abs_diff <= dz) return c0;

    int vout;
    if (diff > dz) {
        if (du <= 0) vout = 16383;
        else {
            double x = (double)(diff - dz) / (double)du;
            vout = c0 + (int)(apply_curve(x, cu.type, cu.k) * 8191.0 + 0.5);
        }
    } else {
        if (dl <= 0) vout = 0;
        else {
            double x = (double)(cin - dz - vin) / (double)dl;
            vout = c0 - (int)(apply_curve(x, cl.type, cl.k) * 8192.0 + 0.5);
        }
    }
    return clamp_int(vout, 0, 16383);
}

static const LV2_Feature* find_feature(const LV2_Feature* const* f, const char* uri) {
    for (int i = 0; f && f[i]; ++i) if (!strcmp(f[i]->URI, uri)) return f[i];
    return NULL;
}

static uint32_t read_sequence_size(const WSCustomizer* self) {
    if (!self->options) return 0;
    for (const LV2_Options_Option* o = self->options; o && o->key; ++o) {
        if (o->context == LV2_OPTIONS_INSTANCE && o->key == self->urid_bufsz_sequenceSize) {
            if (o->type == self->urid_atom_int) return (uint32_t)(*(const int32_t*)o->value);
            if (o->type == self->urid_atom_long) return (uint32_t)(*(const int64_t*)o->value);
        }
    }
    return 0;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double r, const char* p, const LV2_Feature* const* f) {
    (void)d;
    (void)r;
    (void)p;

    WSCustomizer* self = (WSCustomizer*)calloc(1, sizeof(WSCustomizer));
    if (!self) return NULL;
    const LV2_Feature* f_map = find_feature(f, LV2_URID__map);
    if (!f_map) { free(self); return NULL; }
    self->map = (LV2_URID_Map*)f_map->data;
    const LV2_Feature* f_opts = find_feature(f, LV2_OPTIONS__options);
    if (f_opts) self->options = (const LV2_Options_Option*)f_opts->data;
    const LV2_Feature* f_rsz = find_feature(f, LV2_RESIZE_PORT__resize);
    if (f_rsz) self->resize = (const LV2_Resize_Port_Resize*)f_rsz->data;

    self->urid_midi_event         = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    self->urid_bufsz_sequenceSize = self->map->map(self->map->handle, LV2_BUF_SIZE__sequenceSize);
    self->urid_atom_int           = self->map->map(self->map->handle, LV2_ATOM__Int);
    self->urid_atom_long          = self->map->map(self->map->handle, LV2_ATOM__Long);
    self->host_sequence_size      = read_sequence_size(self);
    self->last_breath             = 100; // Sensible default for Note On velocity

    lv2_atom_forge_init(&self->forge, self->map);
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    WSCustomizer* self = (WSCustomizer*)instance;
    switch ((PortIndex)port) {
        case PORT_MIDI_IN:          self->in_seq            = (const LV2_Atom_Sequence*)data; break;
        case PORT_MIDI_OUT:         self->out_seq           = (LV2_Atom_Sequence*)data; break;
        case PORT_CENTERIN:         self->center_in         = (const float*)data; break;
        case PORT_DEADZONE:         self->deadzone          = (const float*)data; break;
        case PORT_DIST_U:           self->dist_u            = (const float*)data; break;
        case PORT_DIST_L:           self->dist_l            = (const float*)data; break;
        case PORT_CURVE_U:          self->curve_u           = (const float*)data; break;
        case PORT_CURVE_L:          self->curve_l           = (const float*)data; break;
        case PORT_V_FLOOR:          self->v_floor           = (const float*)data; break;
        case PORT_VEL0_AS_NOTEOFF:  self->vel0_as_noteoff   = (const float*)data; break;
        case PORT_TRANSPOSE:        self->transpose         = (const float*)data; break;
        case PORT_CHANNEL:          self->channel           = (const float*)data; break;
        case PORT_OUTCAP:           self->out_capacity      = (const float*)data; break;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    (void)n_samples;

    WSCustomizer* self = (WSCustomizer*)instance;
    if (!self->in_seq || !self->out_seq) return;

    uint32_t actual_cap = (uint32_t)self->out_seq->atom.size + (uint32_t)sizeof(LV2_Atom);
    uint32_t target_cap = 8192;
    if (self->out_capacity && *self->out_capacity > 0) target_cap = clamp_u32((uint32_t)lround(*self->out_capacity) * 4096u, 256u, 65536u);
    else if (self->host_sequence_size >= 256) target_cap = clamp_u32(self->host_sequence_size, 256u, 65536u);

    if (self->resize && actual_cap < target_cap) {
        self->resize->resize(self->resize->data, PORT_MIDI_OUT, (size_t)target_cap);
        actual_cap = (uint32_t)self->out_seq->atom.size + (uint32_t)sizeof(LV2_Atom);
    }
    if (actual_cap < (uint32_t)sizeof(LV2_Atom_Sequence)) return;

    lv2_atom_forge_set_buffer(&self->forge, (uint8_t*)self->out_seq, (size_t)actual_cap);
    if (!lv2_atom_forge_sequence_head(&self->forge, &self->seq_frame, 0)) return;

    int cin = self->center_in ? (int)lround(*self->center_in) : 8192;
    int dz  = self->deadzone ? (int)lround(*self->deadzone) : 700;
    int du  = self->dist_u ? (int)lround(*self->dist_u) : 4000;
    int dl  = self->dist_l ? (int)lround(*self->dist_l) : 4000;
    CurveParams cu = decode_curve_params(self->curve_u ? (int)lround(*self->curve_u) : 0);
    CurveParams cl = decode_curve_params(self->curve_l ? (int)lround(*self->curve_l) : 0);
    int vf   = self->v_floor ? clamp_int((int)lround(*self->v_floor), 0, 127) : 0;
    int ch_f = self->channel ? clamp_int((int)lround(*self->channel), 0, 16) : 0;
    int semi = self->transpose ? (int)lround(*self->transpose) : 0;

    LV2_ATOM_SEQUENCE_FOREACH(self->in_seq, ev) {
        if (!lv2_atom_forge_frame_time(&self->forge, ev->time.frames)) break;
        if (ev->body.type != self->urid_midi_event) {
            if (!forge_write_atom_body(&self->forge, ev->body.type, LV2_ATOM_BODY(&ev->body), ev->body.size)) break;
            continue;
        }

        const uint8_t* msg = (const uint8_t*)LV2_ATOM_BODY(&ev->body);
        const uint32_t msz = ev->body.size;
        if (msz < 1) { if (!forge_write_atom_body(&self->forge, self->urid_midi_event, msg, msz)) break; continue; }

        const uint8_t status = msg[0] & 0xF0;
        const int midi_ch = (int)(msg[0] & 0x0F) + 1;
        if (ch_f != 0 && midi_ch != ch_f) { if (!forge_write_atom_body(&self->forge, self->urid_midi_event, msg, msz)) break; continue; }

        if (status == 0xB0 && msz >= 3) { // CC
            if (msg[1] == 2) self->last_breath = msg[2]; // Monitor Breath Control
            if (!forge_write_atom_body(&self->forge, self->urid_midi_event, msg, msz)) break;
        } else if (status == 0xE0 && msz >= 3) { // Pitch Bend
            const int vin  = (int)msg[1] | ((int)msg[2] << 7);
            const int vout = map_pitchbend(vin, cin, dz, du, dl, cu, cl);
            uint8_t out[3] = { msg[0], (uint8_t)(vout & 0x7F), (uint8_t)((vout >> 7) & 0x7F) };
            if (!forge_write_atom_body(&self->forge, self->urid_midi_event, out, 3)) break;
        //} else if (status == 0x90 && msz >= 3) { // Note On
        //    int note = clamp_int((int)msg[1] + semi, 0, 127);
        //    int velo = (vf > 0) ? (int)self->last_breath : (int)msg[2];
        //    if (vf > 0 && velo < vf) velo = vf;
        //    uint8_t out[3] = { msg[0], (uint8_t)note, (uint8_t)velo };
        //    if (!forge_write_atom_body(&self->forge, self->urid_midi_event, out, 3)) break;
        } else if ((status & 0xF0) == 0x90 && msz >= 3) { // Note On (any channel)
            const bool vel0_fix = (self->vel0_as_noteoff && *self->vel0_as_noteoff >= 0.5f);
            const uint8_t channel = status & 0x0F;
            const uint8_t velocity = msg[2];
            int note = clamp_int((int)msg[1] + semi, 0, 127);

            // If enabled and incoming Note On velocity is zero, rewrite as Note Off.
            if (vel0_fix && velocity == 0) {
                uint8_t out[3] = {
                    (uint8_t)(0x80 | channel), // Note Off status
                    (uint8_t)note,
                    0
                };
                if (!forge_write_atom_body(&self->forge, self->urid_midi_event, out, 3)) break;
            } else {
                // Normal Note On processing
                int velo = (vf > 0) ? (int)self->last_breath : (int)velocity;
                if (vf > 0 && velo < vf) velo = vf;
                
                uint8_t out[3] = { (uint8_t)(0x90 | channel), (uint8_t)note, (uint8_t)velo };
                if (!forge_write_atom_body(&self->forge, self->urid_midi_event, out, 3)) break;
            }
        } else if (status == 0x80 && msz >= 3) { // Note Off
            int note = clamp_int((int)msg[1] + semi, 0, 127);
            uint8_t out[3] = { msg[0], (uint8_t)note, msg[2] };
            if (!forge_write_atom_body(&self->forge, self->urid_midi_event, out, 3)) break;
        } else {
            if (!forge_write_atom_body(&self->forge, self->urid_midi_event, msg, msz)) break;
        }
    }
    lv2_atom_forge_pop(&self->forge, &self->seq_frame);
}

static void cleanup(LV2_Handle i) { free(i); }
static const LV2_Descriptor descriptor = { PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return (i == 0) ? &descriptor : NULL; }

