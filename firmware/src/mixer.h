// Playback engine: shared voice pool, monophonic choke per slot.
//
// Triggers come from core0 (MIDI/UI), rendering runs in the audio ISR on
// core1. The handoff goes through a lock-free queue: neither core ever touches
// the other's voices directly.
#ifndef MIXER_H
#define MIXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUM_SLOTS 8

// A kit is one sample per slot, in slot order. In a kit-organised library
// (blob version 2) the kits sit consecutively, so kit k's slot s is entry
// k * KIT_SIZE + s — which is the whole reason the library is laid out that
// way: loading a kit is an index computation, not a search through the names.
#define KIT_SIZE NUM_SLOTS

// 8 slots x 2 voices (one playing + one fading out from a choke): the choke
// makes every slot monophonic, so the voice count is bounded by construction
// rather than by what is played.
#define MAX_VOICES 16

typedef struct {
    int32_t  lib_index;  // index into the flash library, -1 = empty slot
    uint8_t  midi_note;  // General MIDI percussion note, see SLOT_NOTES
} SampleSlot;

typedef struct {
    uint32_t active_voices;
    uint32_t triggers;
    uint32_t chokes;
    uint32_t steals;       // if this grows in normal use it is a bug, not a cap
    uint32_t dropped;      // triggers lost to a full queue
} mixer_stats_t;

// `lib` is the XIP pointer to the library, already validated by the caller.
void mixer_init(const void *lib);

// Assigns a library sample to a slot. From core0.
void mixer_assign_slot(uint8_t slot, int32_t lib_index);

// Fills the slots at boot: kit 0 of a kit-organised library, or the first
// NUM_SLOTS samples by position for a flat one. Harmless with no library.
// From core0, before the audio starts.
void mixer_load_default_kit(void);

// --- kits -------------------------------------------------------------------
// Only a kit-organised library has these; on a flat one the count is 0 and the
// UI leaves kit mode out.

// How many kits the library holds (0 = not kit-organised).
uint32_t mixer_kit_count(void);

// Assigns every slot from one kit. From core0.
void mixer_load_kit(uint32_t kit);

// Name of a kit, i.e. the name of its samples without the role suffix:
// "SYN_808_KICK" -> "SYN_808". Returns a pointer to a static buffer
// valid until the next call, so it is for printing, not for keeping.
const char *mixer_kit_name(uint32_t kit);

// The kit a set of NUM_SLOTS library indices forms, or -1 when they do not
// form one. A stored preset is exactly such a set, and this is how the UI
// tells "kit 7" from "a mix built by hand" without keeping a name for it.
int32_t mixer_kit_of(const int32_t *lib_index);

// The kit the slots currently form, or -1 if they do not form one — which is
// the normal state after a single slot has been reassigned by hand.
int32_t mixer_current_kit(void);

// Role of a slot as a short label: "KICK", "SNARE", "CH", "OH", "FLEX1"..
// "FLEX4". The four FLEX slots take whatever the kit puts there: a clap, a
// rimshot, a cymbal, a tom.
const char *mixer_role_name(uint8_t slot);
// The General MIDI note a slot is born with. The config page can move a slot
// off it, and the settings record needs to know what it is moving away from —
// SLOT_NOTES stays private, this is the one way out of it.
uint8_t mixer_default_note(uint8_t slot);

void mixer_set_slot_note(uint8_t slot, uint8_t midi_note);

// Pan of a slot, MIXER_PAN_LEFT (hard left) .. 0 (centre) .. MIXER_PAN_RIGHT
// (hard right), balance law: the centre is unity on both sides, as the module
// sounded before pan existed. Out-of-range values are clamped. Takes effect
// on the next hit of the slot, not on a voice already sounding. From core0.
#define MIXER_PAN_LEFT  (-8)
#define MIXER_PAN_RIGHT 8
void mixer_set_slot_pan(uint8_t slot, int8_t pan);
int8_t mixer_slot_pan(uint8_t slot);

// Level of a slot, as an attenuation in steps of 3dB: 0 is 0dB (unity, how
// the module sounded before level existed), 8 is -24dB, MIXER_LEVEL_OFF mutes
// the slot. Attenuation only, like the master gain: above unity the sum clips,
// and a sample that is too quiet is raised in the loader, not here. Clamped,
// read at the trigger like the pan. From core0.
#define MIXER_LEVEL_DB_STEP 3
#define MIXER_LEVEL_OFF     9
void mixer_set_slot_level(uint8_t slot, uint8_t atten);
uint8_t mixer_slot_level(uint8_t slot);

// Hat choke: when on, CH and OH behave as one instrument, so a hit on either
// fades out whatever the other is playing, as a closing hi-hat cuts its own
// open ring. Off (the default, and how 1.0 behaved), each slot only chokes
// itself. From core0.
void mixer_set_hat_choke(bool on);
const char *mixer_slot_name(uint8_t slot);

// Slot state, for the UI. -1 = slot with no sample assigned.
int32_t mixer_slot_index(uint8_t slot);
uint8_t mixer_slot_note(uint8_t slot);

// Bit i = slot i has at least one voice playing. Updated by the render on
// core1 and read from core0: it is a single word, so it reads back coherently
// without a lock.
uint32_t mixer_active_slot_mask(void);

// How many samples the library holds (0 if absent). The UI needs it to bound
// the browsing in assign mode.
uint32_t mixer_library_count(void);

// Name of the i-th library sample, for the browser in assign mode.
const char *mixer_library_name(uint32_t index);

// Trigger. From core0, never blocks: it queues and returns.
void mixer_trigger_slot(uint8_t slot, uint8_t velocity);

// Returns false when no slot owns the note. That is the normal case on a full
// General MIDI stream, not an error — but the caller needs to tell it apart
// from "nothing is arriving at all", which is the same silence with a very
// different cause.
bool mixer_trigger_note(uint8_t midi_note, uint8_t velocity);

// Master gain in Q12 (4096 = 1.0). Headroom is needed because hits sum on the
// beat: kick+snare+hat lined up is the rule, not the exception.
void mixer_set_master_gain(uint16_t gain_q12);
uint16_t mixer_get_master_gain(void);

// How a voice's gain follows the velocity it was triggered with: 0 = MID (the
// plain ratio), 1 = OFF (velocity ignored, always full gain), 2 = LOW (a soft
// hit still plays loud), 3 = HIGH (a soft hit falls off fast). Matches
// SETTINGS_DYNAMICS_* in settings.h by value; the mixer does not include that
// header, it just agrees with it on what the four numbers mean.
void mixer_set_dynamics(uint8_t curve);

// Fills n stereo frames. Called from the audio ISR on core1.
void mixer_render(int32_t *frames, size_t n);

void mixer_get_stats(mixer_stats_t *out);

#endif // MIXER_H
