#include "settings.h"

#include <stddef.h>
#include <string.h>

// The live copy, plus a shadow of what flash actually holds. The shadow costs
// 336 bytes of RAM and buys an exact "dirty" answer: the UI can offer to save
// only when there is something to save, which matters when a save is an audio
// dropout and a flash erase.
static SettingsRecord live;
static SettingsRecord shadow;

static const SettingsBackend *be;
static uint32_t live_sector;  // the sector `live` came from; a save writes the other
static bool from_flash;

uint32_t settings_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

// The record as phase3 wrote it: version 1, 320 bytes, no pan. Kept only to
// read an existing module's flash after an upgrade. Its layout is frozen, so
// the size is asserted against the number, not derived.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    uint8_t  midi_channel;
    uint8_t  boot_preset;
    uint8_t  velocity_fixed;
    uint8_t  oled_contrast;
    uint16_t master_gain;
    uint8_t  slot_note[NUM_SLOTS];
    uint8_t  dynamics;
    uint8_t  pad2;
    Preset   preset[NUM_PRESETS];
    uint32_t crc;
} SettingsRecordV1;

_Static_assert(sizeof(SettingsRecordV1) == 320, "the version-1 layout is frozen");

// The record as firmware 1.0 (phase4) wrote it: version 2, 328 bytes, pan but
// no level. Frozen like the one above.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    uint8_t  midi_channel;
    uint8_t  boot_preset;
    uint8_t  velocity_fixed;
    uint8_t  oled_contrast;
    uint16_t master_gain;
    uint8_t  slot_note[NUM_SLOTS];
    uint8_t  dynamics;
    uint8_t  pad2;
    int8_t   slot_pan[NUM_SLOTS];
    Preset   preset[NUM_PRESETS];
    uint32_t crc;
} SettingsRecordV2;

_Static_assert(sizeof(SettingsRecordV2) == 328, "the version-2 layout is frozen");

// Everything up to, but not including, the crc field.
#define CRC_LEN ((uint32_t)offsetof(SettingsRecord, crc))

void settings_defaults(SettingsRecord *out) {
    memset(out, 0, sizeof(*out));
    out->magic = SETTINGS_MAGIC;
    out->version = SETTINGS_VERSION;
    out->size = (uint16_t)sizeof(SettingsRecord);
    out->seq = 0;

    out->midi_channel = 9;  // channel 10 on a front panel, GM percussion
    out->boot_preset = PRESET_NONE;
    out->velocity_fixed = SETTINGS_VELOCITY_FOLLOW;
    out->dynamics = SETTINGS_DYNAMICS_MID;
    out->oled_contrast = SETTINGS_CONTRAST_DEFAULT;
    out->master_gain = SETTINGS_GAIN_UNITY / 2;  // -6dB, as mixer.c comes up
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        out->slot_note[s] = mixer_default_note(s);
    }
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        out->preset[p].used = 0;
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            out->preset[p].lib_index[s] = -1;
        }
    }
    settings_seal(out);
}

void settings_reset_config(SettingsRecord *r) {
    uint32_t seq = r->seq;
    Preset keep[NUM_PRESETS];
    memcpy(keep, r->preset, sizeof(keep));
    settings_defaults(r);
    memcpy(r->preset, keep, sizeof(keep));
    r->seq = seq;
    settings_seal(r);
}

void settings_clear_presets(SettingsRecord *r) {
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        r->preset[p].used = 0;
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            r->preset[p].lib_index[s] = -1;
        }
    }
    // A boot preset is configuration, but one pointing at a preset that no
    // longer exists means nothing: boot would quietly fall back to kit 1
    // while BOOT still said PRESET 3. Put it where the module actually goes.
    r->boot_preset = PRESET_NONE;
    settings_seal(r);
}

void settings_seal(SettingsRecord *r) {
    r->magic = SETTINGS_MAGIC;
    r->version = SETTINGS_VERSION;
    r->size = (uint16_t)sizeof(SettingsRecord);
    r->crc = settings_crc32(r, CRC_LEN);
}

// Range checks: a record can be intact and still come from a build that
// meant something different by these fields.
static bool fields_valid(const SettingsRecord *r) {
    if (r->midi_channel > 15 && r->midi_channel != SETTINGS_MIDI_CHANNEL_ANY) {
        return false;
    }
    if (r->boot_preset >= NUM_PRESETS && r->boot_preset != PRESET_NONE) {
        return false;
    }
    if (r->velocity_fixed > 127) {
        return false;
    }
    if (r->dynamics > SETTINGS_DYNAMICS_HIGH) {
        return false;
    }
    if (r->hat_choke > 1) {
        return false;
    }
    if (r->master_gain < SETTINGS_GAIN_MIN || r->master_gain > SETTINGS_GAIN_MAX) {
        return false;
    }
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        if (r->slot_note[s] > 127) {
            return false;
        }
        if (r->slot_pan[s] < MIXER_PAN_LEFT || r->slot_pan[s] > MIXER_PAN_RIGHT) {
            return false;
        }
        if (r->slot_level[s] > MIXER_LEVEL_OFF) {
            return false;
        }
    }
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            if (r->preset[p].lib_index[s] < -1) {
                return false;
            }
        }
    }
    return true;
}

bool settings_valid(const SettingsRecord *r) {
    if (r == NULL) {
        return false;
    }
    // Virgin flash is all 0xFF and dies on the magic, which is the whole point
    // of having one.
    if (r->magic != SETTINGS_MAGIC || r->version != SETTINGS_VERSION ||
        r->size != sizeof(SettingsRecord)) {
        return false;
    }
    if (r->crc != settings_crc32(r, CRC_LEN)) {
        return false;
    }
    return fields_valid(r);
}

// An older record converted to the current layout: whatever it did not have
// is left at zero, which is centre for the pan and 0dB for the level, i.e.
// exactly how that firmware sounded. Checked on its own CRC first, then on
// the same ranges as a native one. `out` is sealed, so it is a valid record
// of the current version with the old seq.
static bool upgrade_v2(const void *raw, SettingsRecord *out) {
    const SettingsRecordV2 *v2 = (const SettingsRecordV2 *)raw;
    if (v2 == NULL || v2->magic != SETTINGS_MAGIC || v2->version != 2 ||
        v2->size != sizeof(SettingsRecordV2) ||
        v2->crc != settings_crc32(v2, (uint32_t)offsetof(SettingsRecordV2, crc))) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->seq = v2->seq;
    out->midi_channel = v2->midi_channel;
    out->boot_preset = v2->boot_preset;
    out->velocity_fixed = v2->velocity_fixed;
    out->oled_contrast = v2->oled_contrast;
    out->master_gain = v2->master_gain;
    memcpy(out->slot_note, v2->slot_note, sizeof(out->slot_note));
    out->dynamics = v2->dynamics;
    memcpy(out->slot_pan, v2->slot_pan, sizeof(out->slot_pan));
    memcpy(out->preset, v2->preset, sizeof(out->preset));
    settings_seal(out);
    return fields_valid(out);
}

static bool upgrade_v1(const void *raw, SettingsRecord *out) {
    const SettingsRecordV1 *v1 = (const SettingsRecordV1 *)raw;
    if (v1 == NULL || v1->magic != SETTINGS_MAGIC || v1->version != 1 ||
        v1->size != sizeof(SettingsRecordV1) ||
        v1->crc != settings_crc32(v1, (uint32_t)offsetof(SettingsRecordV1, crc))) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->seq = v1->seq;
    out->midi_channel = v1->midi_channel;
    out->boot_preset = v1->boot_preset;
    out->velocity_fixed = v1->velocity_fixed;
    out->oled_contrast = v1->oled_contrast;
    out->master_gain = v1->master_gain;
    memcpy(out->slot_note, v1->slot_note, sizeof(out->slot_note));
    out->dynamics = v1->dynamics;
    memcpy(out->preset, v1->preset, sizeof(out->preset));
    settings_seal(out);
    return fields_valid(out);
}

bool settings_init(const SettingsBackend *backend) {
    be = backend;
    from_flash = false;
    // Nothing valid found: the first save must land on sector 0, so pretend
    // the live copy came from sector 1.
    live_sector = SETTINGS_SECTORS - 1;

    // Both sectors, each either a native record or an older one upgraded
    // into a scratch copy. The newest valid one wins, whichever version it
    // was written as: a module saved twice under an older firmware must come
    // up on the later of the two.
    static SettingsRecord scratch[SETTINGS_SECTORS];
    const SettingsRecord *best = NULL;
    if (be != NULL && be->read != NULL) {
        for (uint32_t i = 0; i < SETTINGS_SECTORS; i++) {
            const void *raw = be->read(i);
            const SettingsRecord *r = (const SettingsRecord *)raw;
            if (!settings_valid(r)) {
                if (!upgrade_v2(raw, &scratch[i]) && !upgrade_v1(raw, &scratch[i])) {
                    continue;
                }
                r = &scratch[i];
            }
            if (best == NULL || r->seq > best->seq) {
                best = r;
                live_sector = i;
            }
        }
    }

    if (best != NULL) {
        memcpy(&live, best, sizeof(live));
        memcpy(&shadow, best, sizeof(shadow));
        from_flash = true;
        return true;
    }

    settings_defaults(&live);
    memset(&shadow, 0, sizeof(shadow));  // no flash copy: everything reads dirty
    return false;
}

SettingsRecord *settings_get(void) { return &live; }

bool settings_is_default(void) { return !from_flash; }

bool settings_dirty(void) {
    if (!from_flash) {
        return true;
    }
    // seq and crc always differ from one write to the next; what matters is
    // whether the content the user can change has moved.
    SettingsRecord a = live, b = shadow;
    a.seq = b.seq = 0;
    a.crc = b.crc = 0;
    return memcmp(&a, &b, sizeof(a)) != 0;
}

bool settings_save(void) {
    if (be == NULL || be->write == NULL) {
        return false;
    }
    uint32_t target = (live_sector + 1u) % SETTINGS_SECTORS;
    live.seq = (from_flash ? shadow.seq : 0u) + 1u;
    settings_seal(&live);
    if (!be->write(target, &live, (uint32_t)sizeof(live))) {
        return false;
    }
    // Only now is the write the truth: adopt it as the shadow, and let the
    // next save go to the other sector.
    memcpy(&shadow, &live, sizeof(shadow));
    live_sector = target;
    from_flash = true;
    return true;
}
