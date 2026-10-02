// Persistent settings and user presets.
//
// Everything the module must remember across a power cycle lives in one
// record: the configuration (MIDI channel, note map, master gain) and the
// eight user presets. One record, one write, one validation — the settings
// page and the presets share a block because they share a discipline, not
// because they are the same thing.
//
// No SDK dependency. The flash half is injected as a SettingsBackend, so this
// file compiles on the Mac and the tests exercise the real serialisation
// against a RAM fake. src/settings_flash.c is the only piece that knows about
// XIP and flash_range_erase.
//
// Two sectors, alternated: a save always writes the one that is NOT live, so a
// power cut in the middle of a write destroys the copy nobody is using. On
// boot both are read and the valid one with the higher `seq` wins. Virgin
// flash reads back 0xFF, which fails the magic — same trap, same answer, as
// sample_lib_valid().
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "mixer.h"  // NUM_SLOTS

#define SETTINGS_MAGIC   0x54455343u  // 'CSET' little-endian
// Version 2 (phase4, firmware 1.0) added slot_pan, version 3 (firmware 1.1)
// slot_level. Records of versions 1 and 2 are still read, and converted on
// the way in: see settings_init().
#define SETTINGS_VERSION 3

// Eight user presets, one per slot of the strip: the count is the number the
// UI can address without a second level of browsing, not a flash limit.
#define NUM_PRESETS 8

// boot_preset / a preset reference meaning "none".
#define PRESET_NONE 0xFFu

// midi_channel meaning "listen on every channel". Matches MIDI_CHANNEL_ANY in
// midi.h, which settings.c cannot include without pulling the parser in.
#define SETTINGS_MIDI_CHANNEL_ANY 0xFFu

// Master gain bounds, Q12. Unity is 4096; the default keeps 6dB of headroom
// because hits sum on the beat. Above unity the sum clips, so the ceiling is
// there to stop the config page offering a setting that only distorts.
#define SETTINGS_GAIN_MIN   256u   // -24 dB
#define SETTINGS_GAIN_MAX   4096u  //   0 dB
#define SETTINGS_GAIN_UNITY 4096u

// velocity_fixed: 0 = follow the incoming velocity. Anything else is the
// velocity every hit is played at, for sequencers that send a flat 127 or for
// deliberately uniform playing.
#define SETTINGS_VELOCITY_FOLLOW 0u

// dynamics: how the (possibly fixed) velocity maps to a voice's gain. MID is
// 0 on purpose: it is the ratio that already existed before this setting did,
// so a record saved by an older build reads as MID and nothing about how the
// module already sounds changes underneath it.
#define SETTINGS_DYNAMICS_MID  0u  // straight ratio, gain = velocity/127
#define SETTINGS_DYNAMICS_OFF  1u  // velocity ignored, every hit at full gain
#define SETTINGS_DYNAMICS_LOW  2u  // a soft hit still plays loud, subtle range
#define SETTINGS_DYNAMICS_HIGH 3u  // a soft hit falls off fast, wide range

// SSD1306 contrast. The driver's own default is what the display comes up with
// today, so it is also what the factory record stores.
#define SETTINGS_CONTRAST_DEFAULT 0x8Fu

// One user preset: which library sample sits in each slot. The note map is
// NOT part of a preset — it is configuration. Recalling a preset changes the
// sounds, never the notes the module answers to, otherwise a pattern that
// played a moment ago would stop playing.
typedef struct {
    uint8_t used;  // 0 = empty preset, never written by the user
    uint8_t pad[3];
    int32_t lib_index[NUM_SLOTS];  // -1 = empty slot, as in SampleSlot
} Preset;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;  // sizeof(SettingsRecord): a record from another build is refused
    uint32_t seq;   // between the two sectors, the higher valid one wins

    // --- configuration ---
    uint8_t  midi_channel;    // 0..15, or SETTINGS_MIDI_CHANNEL_ANY
    uint8_t  boot_preset;     // 0..NUM_PRESETS-1, or PRESET_NONE for kit 0
    uint8_t  velocity_fixed;  // 0 = follow, else the fixed velocity 1..127
    uint8_t  oled_contrast;   // SSD1306 contrast register, 0..255
    uint16_t master_gain;  // Q12
    uint8_t  slot_note[NUM_SLOTS];
    uint8_t  dynamics;   // SETTINGS_DYNAMICS_*, was pad2[0]
    uint8_t  hat_choke;   // 0 = off, 1 = CH and OH choke each other. Was pad2,
                          // zero in every older record, so they read as off
    int8_t   slot_pan[NUM_SLOTS];  // MIXER_PAN_LEFT..MIXER_PAN_RIGHT, 0 = centre
    // Attenuation in 3dB steps, 0 = 0dB .. MIXER_LEVEL_OFF. 0 is unity so a
    // record converted from an older version, zero-filled, sounds unchanged.
    uint8_t  slot_level[NUM_SLOTS];

    // --- user presets ---
    Preset preset[NUM_PRESETS];

    uint32_t crc;  // over every byte before it
} SettingsRecord;

_Static_assert(sizeof(SettingsRecord) == 336, "settings record layout changed");

// The flash half. `read` returns a pointer to the sector's contents (on the
// target an XIP pointer, in the tests a pointer into RAM) or NULL; `write`
// erases the sector and programs `len` bytes at its start.
typedef struct {
    const void *(*read)(uint32_t sector);
    bool (*write)(uint32_t sector, const void *buf, uint32_t len);
} SettingsBackend;

#define SETTINGS_SECTORS 2

// The factory record: GM notes from SLOT_NOTES, channel 10, -6dB, no preset
// at boot. What the module comes up with when flash holds nothing valid.
void settings_defaults(SettingsRecord *out);

// The two halves of a factory reset, offered separately on the config page
// since 1.1: one puts the configuration back (MIDI channel, gain, velocity,
// dynamics, contrast, boot preset, notes, levels, pans) and keeps the eight
// presets, the other empties the presets and keeps the configuration. Both
// edit `r` in place and leave seq alone; nothing reaches flash until a save.
void settings_reset_config(SettingsRecord *r);
void settings_clear_presets(SettingsRecord *r);

// Magic, version, size, CRC and range checks. False for virgin flash.
bool settings_valid(const SettingsRecord *r);

// Fills magic/version/size and recomputes the CRC. Call before writing.
void settings_seal(SettingsRecord *r);

uint32_t settings_crc32(const void *data, uint32_t len);

// Reads both sectors and adopts the newest valid record, or the defaults.
// Returns true when a record really came out of flash. An older record is
// converted in RAM, every slot centred (version 1, before pan existed) and at
// 0dB (versions 1 and 2, before level existed), so an upgrade keeps the
// presets and the note map; flash keeps the old record until the next save
// rewrites it in the current version.
bool settings_init(const SettingsBackend *backend);

// The live copy, edited in place by the UI and the console. Changes only
// reach flash on settings_save().
SettingsRecord *settings_get(void);

// True when the live copy differs from what is in flash.
bool settings_dirty(void);

// Bumps seq, seals and writes the sector that is not live. Audio stalls for
// the duration of the erase: the caller silences the output first.
bool settings_save(void);

// True when boot came up on the defaults, i.e. flash held nothing valid.
bool settings_is_default(void);

#endif // SETTINGS_H
