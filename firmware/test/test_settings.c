// Native test bench for the persistent settings and the user presets.
//
// The cases here are the ones that decide whether a module comes back up the
// way it was left: virgin flash, a save cut in half by a power loss, a record
// written by a different build, a single bit rotted in place. The flash is a
// RAM fake, so the serialisation being exercised is the real one.
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

static int failures;

#define CHECK(cond, ...)         \
    do {                         \
        if (!(cond)) {           \
            printf("  FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
            failures++;          \
        }                        \
    } while (0)

// --- the fake flash ---------------------------------------------------------
// One page per sector, erased to 0xFF exactly as real flash comes back.

#define FAKE_SECTOR_SIZE 4096

static uint8_t fake[SETTINGS_SECTORS][FAKE_SECTOR_SIZE];
static uint32_t writes[SETTINGS_SECTORS];
static bool write_fails;

static void fake_erase_all(void) {
    memset(fake, 0xFF, sizeof(fake));
    memset(writes, 0, sizeof(writes));
    write_fails = false;
}

static const void *fake_read(uint32_t sector) {
    return sector < SETTINGS_SECTORS ? fake[sector] : NULL;
}

static bool fake_write(uint32_t sector, const void *buf, uint32_t len) {
    if (write_fails || sector >= SETTINGS_SECTORS || len > FAKE_SECTOR_SIZE) {
        return false;
    }
    memset(fake[sector], 0xFF, FAKE_SECTOR_SIZE);  // erase
    memcpy(fake[sector], buf, len);                // program
    writes[sector]++;
    return true;
}

static const SettingsBackend BACKEND = {fake_read, fake_write};

// --- cases ------------------------------------------------------------------

static void test_defaults(void) {
    SettingsRecord r;
    settings_defaults(&r);

    CHECK(settings_valid(&r), "the factory record must validate");
    CHECK(r.midi_channel == 9, "default channel is 10 on a panel, got %u",
          r.midi_channel);
    CHECK(r.boot_preset == PRESET_NONE, "no preset at boot by default");
    CHECK(r.velocity_fixed == SETTINGS_VELOCITY_FOLLOW, "velocity follows by default");
    CHECK(r.dynamics == SETTINGS_DYNAMICS_MID, "dynamics is MID by default, got %u",
          r.dynamics);
    CHECK(r.master_gain == SETTINGS_GAIN_UNITY / 2, "default gain is -6dB, got %u",
          r.master_gain);

    // The note map must be the GM one the mixer boots with, or a fresh module
    // would answer to different notes before and after its first save.
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        CHECK(r.slot_note[s] == mixer_default_note(s),
              "slot %u note %u, expected %u", s, r.slot_note[s],
              mixer_default_note(s));
    }
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        CHECK(r.slot_pan[s] == 0, "slot %u must come up centred, got %d", s,
              r.slot_pan[s]);
        CHECK(r.slot_level[s] == 0, "slot %u must come up at 0dB, got %u", s,
              r.slot_level[s]);
    }
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        CHECK(r.preset[p].used == 0, "preset %u must come up empty", p);
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            CHECK(r.preset[p].lib_index[s] == -1,
                  "preset %u slot %u must come up unassigned", p, s);
        }
    }
}

static void test_virgin_flash(void) {
    fake_erase_all();
    bool loaded = settings_init(&BACKEND);

    CHECK(!loaded, "virgin flash must not be read as a record");
    CHECK(settings_is_default(), "a virgin module runs on the defaults");
    CHECK(settings_get()->midi_channel == 9, "and the defaults are the factory ones");
    // 0xFF everywhere is the exact trap sample_lib_valid() exists for: the
    // magic is what stops it being taken as data.
    CHECK(!settings_valid((const SettingsRecord *)fake[0]),
          "0xFF must fail validation");
}

static void test_save_and_reload(void) {
    fake_erase_all();
    settings_init(&BACKEND);

    SettingsRecord *r = settings_get();
    r->midi_channel = 3;
    r->master_gain = 3000;
    r->slot_note[0] = 60;
    r->boot_preset = 2;
    r->preset[2].used = 1;
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        r->preset[2].lib_index[s] = 100 + s;
    }
    CHECK(settings_save(), "the save must succeed");

    // Re-init is what a power cycle does.
    CHECK(settings_init(&BACKEND), "the record must come back out of flash");
    CHECK(!settings_is_default(), "and not be reported as the defaults");
    r = settings_get();
    CHECK(r->midi_channel == 3, "channel survived: %u", r->midi_channel);
    CHECK(r->master_gain == 3000, "gain survived: %u", r->master_gain);
    CHECK(r->slot_note[0] == 60, "note map survived: %u", r->slot_note[0]);
    CHECK(r->boot_preset == 2, "boot preset survived: %u", r->boot_preset);
    CHECK(r->preset[2].used == 1, "preset 2 is marked used");
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        CHECK(r->preset[2].lib_index[s] == 100 + s,
              "preset 2 slot %u came back as %d", s, r->preset[2].lib_index[s]);
    }
}

static void test_sectors_alternate(void) {
    fake_erase_all();
    settings_init(&BACKEND);

    settings_get()->master_gain = 1000;
    settings_save();
    settings_get()->master_gain = 1100;
    settings_save();
    settings_get()->master_gain = 1200;
    settings_save();

    // Three saves, alternating: the point is that a write never lands on the
    // sector currently holding the good copy.
    CHECK(writes[0] == 2 && writes[1] == 1,
          "expected the writes to alternate 2/1, got %u/%u", writes[0], writes[1]);
    CHECK(settings_init(&BACKEND) && settings_get()->master_gain == 1200,
          "the newest record wins");
}

static void test_power_cut_falls_back(void) {
    fake_erase_all();
    settings_init(&BACKEND);
    settings_get()->master_gain = 1000;
    settings_save();  // sector 0, seq 1
    settings_get()->master_gain = 2000;
    settings_save();  // sector 1, seq 2

    // A power cut during the third save: the erase went through, the program
    // did not. Sector 0 is back to virgin, sector 1 still holds seq 2.
    memset(fake[0], 0xFF, FAKE_SECTOR_SIZE);

    CHECK(settings_init(&BACKEND), "the intact sector must still be found");
    CHECK(settings_get()->master_gain == 2000,
          "the surviving record is the previous good one, got %u",
          settings_get()->master_gain);

    // And the next save goes back to the erased sector, not over the good one.
    settings_get()->master_gain = 3000;
    CHECK(settings_save(), "saving again must work");
    CHECK(settings_valid((const SettingsRecord *)fake[0]) &&
              settings_valid((const SettingsRecord *)fake[1]),
          "both sectors hold a valid record again");
}

static void test_crc_catches_rot(void) {
    fake_erase_all();
    settings_init(&BACKEND);
    settings_get()->slot_note[3] = 70;
    settings_save();

    // One bit, in the middle of the presets, far from magic and version.
    fake[0][offsetof(SettingsRecord, preset[4].lib_index[1])] ^= 0x01;

    CHECK(!settings_valid((const SettingsRecord *)fake[0]),
          "a single flipped bit must fail the CRC");
    CHECK(!settings_init(&BACKEND), "and the module falls back to the defaults");
    CHECK(settings_is_default(), "reported as running on the defaults");
}

static void test_foreign_record_refused(void) {
    SettingsRecord r;
    settings_defaults(&r);

    r.version = SETTINGS_VERSION + 1;
    settings_seal(&r);  // a valid CRC over a version this build does not know
    r.version = SETTINGS_VERSION + 1;
    CHECK(!settings_valid(&r), "a record from another version is refused");

    settings_defaults(&r);
    r.size = sizeof(r) - 4;
    r.crc = settings_crc32(&r, (uint32_t)offsetof(SettingsRecord, crc));
    CHECK(!settings_valid(&r), "a record of another size is refused");
}

static void test_range_checks(void) {
    SettingsRecord r;

    // Each of these is a record with an honest CRC and a field this build
    // cannot act on. Sealing after the edit is what makes it a range test and
    // not a CRC test.
    settings_defaults(&r);
    r.midi_channel = 16;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "channel 16 does not exist");

    settings_defaults(&r);
    r.midi_channel = SETTINGS_MIDI_CHANNEL_ANY;
    settings_seal(&r);
    CHECK(settings_valid(&r), "'any channel' is a legal setting");

    settings_defaults(&r);
    r.boot_preset = NUM_PRESETS;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "a boot preset past the last one is refused");

    settings_defaults(&r);
    r.master_gain = SETTINGS_GAIN_MAX + 1;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "gain above unity would only clip");

    settings_defaults(&r);
    r.slot_note[5] = 128;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "note 128 is not a MIDI note");

    settings_defaults(&r);
    r.velocity_fixed = 128;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "velocity 128 is not a MIDI velocity");

    settings_defaults(&r);
    r.dynamics = SETTINGS_DYNAMICS_HIGH;
    settings_seal(&r);
    CHECK(settings_valid(&r), "HIGH is the top of the range and must be legal");

    settings_defaults(&r);
    r.dynamics = SETTINGS_DYNAMICS_HIGH + 1;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "one past HIGH is not a curve this build knows");

    settings_defaults(&r);
    r.slot_pan[3] = MIXER_PAN_RIGHT;
    settings_seal(&r);
    CHECK(settings_valid(&r), "hard right is the end of the range and must be legal");

    settings_defaults(&r);
    r.slot_pan[3] = MIXER_PAN_RIGHT + 1;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "a pan past hard right is refused");

    settings_defaults(&r);
    r.slot_pan[3] = MIXER_PAN_LEFT - 1;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "a pan past hard left is refused");

    settings_defaults(&r);
    CHECK(r.hat_choke == 0, "the hat choke must come up off");
    r.hat_choke = 1;
    settings_seal(&r);
    CHECK(settings_valid(&r), "hat choke on must be legal");
    r.hat_choke = 2;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "a hat choke of 2 is refused");

    settings_defaults(&r);
    r.slot_level[5] = MIXER_LEVEL_OFF;
    settings_seal(&r);
    CHECK(settings_valid(&r), "OFF is the end of the level range and must be legal");

    settings_defaults(&r);
    r.slot_level[5] = MIXER_LEVEL_OFF + 1;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "a level past OFF is refused");

    settings_defaults(&r);
    r.preset[0].lib_index[0] = -2;
    settings_seal(&r);
    CHECK(!settings_valid(&r), "-1 is the only legal negative index");
}

// The phase3 record, 320 bytes and version 1, byte for byte as a module in
// the field has it in flash. Built by hand here rather than borrowed from
// settings.c, so the test would catch settings.c getting its own copy wrong.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    uint8_t  midi_channel, boot_preset, velocity_fixed, oled_contrast;
    uint16_t master_gain;
    uint8_t  slot_note[NUM_SLOTS];
    uint8_t  dynamics, pad2;
    Preset   preset[NUM_PRESETS];
    uint32_t crc;
} OldRecord;

static void write_v1(uint32_t sector, uint32_t seq, uint8_t channel) {
    OldRecord o;
    memset(&o, 0, sizeof o);
    o.magic = SETTINGS_MAGIC;
    o.version = 1;
    o.size = 320;
    o.seq = seq;
    o.midi_channel = channel;
    o.boot_preset = 2;
    o.velocity_fixed = 100;
    o.oled_contrast = 0x40;
    o.master_gain = 3072;
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        o.slot_note[s] = (uint8_t)(60 + s);
    }
    o.dynamics = SETTINGS_DYNAMICS_HIGH;
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        o.preset[p].used = (p == 2);
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            o.preset[p].lib_index[s] = (p == 2) ? 16 + s : -1;
        }
    }
    o.crc = settings_crc32(&o, (uint32_t)offsetof(OldRecord, crc));
    memset(fake[sector], 0xFF, FAKE_SECTOR_SIZE);
    memcpy(fake[sector], &o, sizeof o);
}

// Upgrading to phase4 must not cost anyone their presets: a version-1 record
// is read, converted with every slot centred, and kept until the next save
// rewrites it as version 2.
static void test_upgrade_from_v1(void) {
    CHECK(sizeof(OldRecord) == 320, "the test's own v1 layout is %zu bytes",
          sizeof(OldRecord));

    fake_erase_all();
    write_v1(0, 5, 3);
    CHECK(settings_init(&BACKEND), "a version-1 record was not recognised");
    CHECK(!settings_is_default(), "reported as defaults after an upgrade");
    const SettingsRecord *r = settings_get();
    CHECK(r->midi_channel == 3 && r->boot_preset == 2 && r->velocity_fixed == 100 &&
              r->oled_contrast == 0x40 && r->master_gain == 3072 &&
              r->dynamics == SETTINGS_DYNAMICS_HIGH,
          "a configuration field was lost in the upgrade");
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        CHECK(r->slot_note[s] == 60 + s, "slot %u note %u after upgrade", s,
              r->slot_note[s]);
        CHECK(r->slot_pan[s] == 0, "slot %u came up panned %d", s, r->slot_pan[s]);
        CHECK(r->slot_level[s] == 0, "slot %u came up at level %u", s,
              r->slot_level[s]);
    }
    CHECK(r->preset[2].used && r->preset[2].lib_index[7] == 23,
          "preset 3 did not survive the upgrade");
    CHECK(!r->preset[0].used, "an empty preset came back used");

    // Nothing changed by the user, so nothing to write: the upgrade alone is
    // not worth an erase and its audio gap.
    CHECK(!settings_dirty(), "an untouched upgraded record reads as dirty");

    // The first real save writes the current version to the other sector.
    settings_get()->slot_pan[0] = -4;
    CHECK(settings_save(), "the first save after an upgrade failed");
    CHECK(writes[1] == 1 && writes[0] == 0, "the save did not go to the other sector");
    CHECK(settings_valid((const SettingsRecord *)fake[1]),
          "what was written is not a valid current-version record");
    settings_init(&BACKEND);
    CHECK(settings_get()->slot_pan[0] == -4 && settings_get()->preset[2].used,
          "the save after the upgrade did not come back after a reboot");

    // Between an old v1 and a newer v1, the newer wins, as between two v2s.
    fake_erase_all();
    write_v1(0, 7, 4);
    write_v1(1, 6, 5);
    settings_init(&BACKEND);
    CHECK(settings_get()->midi_channel == 4, "the older v1 record won on seq");

    // A v1 record with a rotten bit is as dead as a v2 one.
    fake_erase_all();
    write_v1(0, 1, 3);
    fake[0][20] ^= 0x01;
    CHECK(!settings_init(&BACKEND), "a corrupt v1 record was accepted");
}

// The firmware 1.0 record, 328 bytes and version 2: the v1 layout plus the
// pan. Built by hand for the same reason as the one above.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    uint8_t  midi_channel, boot_preset, velocity_fixed, oled_contrast;
    uint16_t master_gain;
    uint8_t  slot_note[NUM_SLOTS];
    uint8_t  dynamics, pad2;
    int8_t   slot_pan[NUM_SLOTS];
    Preset   preset[NUM_PRESETS];
    uint32_t crc;
} V2Record;

static void write_v2(uint32_t sector, uint32_t seq, uint8_t channel) {
    V2Record o;
    memset(&o, 0, sizeof o);
    o.magic = SETTINGS_MAGIC;
    o.version = 2;
    o.size = 328;
    o.seq = seq;
    o.midi_channel = channel;
    o.boot_preset = 1;
    o.velocity_fixed = 90;
    o.oled_contrast = 0x30;
    o.master_gain = 2048;
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        o.slot_note[s] = (uint8_t)(40 + s);
        o.slot_pan[s] = (int8_t)(s - 4);
    }
    o.dynamics = SETTINGS_DYNAMICS_LOW;
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        o.preset[p].used = (p == 5);
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            o.preset[p].lib_index[s] = (p == 5) ? 40 + s : -1;
        }
    }
    o.crc = settings_crc32(&o, (uint32_t)offsetof(V2Record, crc));
    memset(fake[sector], 0xFF, FAKE_SECTOR_SIZE);
    memcpy(fake[sector], &o, sizeof o);
}

// Upgrading from 1.0 to 1.1 must keep the pan as well as everything a v1
// record already had, and bring every slot up at 0dB: the module sounds the
// way it did until LEVEL is touched.
static void test_upgrade_from_v2(void) {
    CHECK(sizeof(V2Record) == 328, "the test's own v2 layout is %zu bytes",
          sizeof(V2Record));

    fake_erase_all();
    write_v2(1, 9, 9);
    CHECK(settings_init(&BACKEND), "a version-2 record was not recognised");
    CHECK(!settings_is_default(), "reported as defaults after an upgrade");
    const SettingsRecord *r = settings_get();
    CHECK(r->midi_channel == 9 && r->boot_preset == 1 && r->velocity_fixed == 90 &&
              r->oled_contrast == 0x30 && r->master_gain == 2048 &&
              r->dynamics == SETTINGS_DYNAMICS_LOW,
          "a configuration field was lost in the upgrade");
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        CHECK(r->slot_note[s] == 40 + s, "slot %u note %u after upgrade", s,
              r->slot_note[s]);
        CHECK(r->slot_pan[s] == s - 4, "slot %u pan %d after upgrade, expected %d",
              s, r->slot_pan[s], s - 4);
        CHECK(r->slot_level[s] == 0, "slot %u came up at level %u", s,
              r->slot_level[s]);
    }
    CHECK(r->hat_choke == 0, "a 1.0 record came up with the hat choke on");
    CHECK(r->preset[5].used && r->preset[5].lib_index[0] == 40,
          "preset 6 did not survive the upgrade");
    CHECK(!settings_dirty(), "an untouched upgraded record reads as dirty");

    // The first save goes to the other sector, here sector 0, as version 3.
    settings_get()->slot_level[2] = 4;
    CHECK(settings_save(), "the first save after an upgrade failed");
    CHECK(writes[0] == 1 && writes[1] == 0, "the save did not go to the other sector");
    CHECK(settings_valid((const SettingsRecord *)fake[0]) &&
              ((const SettingsRecord *)fake[0])->version == 3,
          "what was written is not a valid version-3 record");
    settings_init(&BACKEND);
    CHECK(settings_get()->slot_level[2] == 4 && settings_get()->slot_pan[7] == 3,
          "the version-3 save did not come back after a reboot");

    // Two versions side by side, as after a phase3 module was saved once on
    // 1.0: the newer seq wins whatever version each was written as.
    fake_erase_all();
    write_v1(0, 3, 4);
    write_v2(1, 4, 6);
    settings_init(&BACKEND);
    CHECK(settings_get()->midi_channel == 6, "the older v1 beat the newer v2");

    // A rotten v2 is refused, and is not mistaken for anything else.
    fake_erase_all();
    write_v2(0, 1, 3);
    fake[0][30] ^= 0x01;
    CHECK(!settings_init(&BACKEND), "a corrupt v2 record was accepted");
}

// The two halves of the old RESTORE DEFAULTS, each leaving the other half
// alone.
static void test_resets(void) {
    SettingsRecord r;
    settings_defaults(&r);
    r.seq = 41;
    r.midi_channel = 2;
    r.master_gain = 1024;
    r.oled_contrast = 0x10;
    r.dynamics = SETTINGS_DYNAMICS_HIGH;
    r.slot_note[1] = 70;
    r.slot_level[1] = 5;
    r.slot_pan[1] = 3;
    r.boot_preset = 4;
    r.preset[4].used = 1;
    r.preset[4].lib_index[2] = 33;

    SettingsRecord a = r;
    settings_reset_config(&a);
    SettingsRecord def;
    settings_defaults(&def);
    CHECK(a.midi_channel == def.midi_channel && a.master_gain == def.master_gain &&
              a.oled_contrast == def.oled_contrast && a.dynamics == def.dynamics &&
              a.boot_preset == PRESET_NONE,
          "reset_config left a global setting behind");
    CHECK(a.slot_note[1] == mixer_default_note(1) && a.slot_level[1] == 0 &&
              a.slot_pan[1] == 0,
          "reset_config left a per-slot setting behind");
    CHECK(a.preset[4].used && a.preset[4].lib_index[2] == 33,
          "reset_config lost a preset");
    CHECK(a.seq == 41, "reset_config moved seq to %u", a.seq);
    CHECK(settings_valid(&a), "reset_config left an invalid record");

    SettingsRecord b = r;
    settings_clear_presets(&b);
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        CHECK(!b.preset[p].used && b.preset[p].lib_index[2] == -1,
              "clear_presets left preset %u", p + 1u);
    }
    CHECK(b.boot_preset == PRESET_NONE, "clear_presets left BOOT on a cleared preset");
    CHECK(b.midi_channel == 2 && b.master_gain == 1024 && b.slot_note[1] == 70 &&
              b.slot_level[1] == 5 && b.slot_pan[1] == 3,
          "clear_presets touched the configuration");
    CHECK(settings_valid(&b), "clear_presets left an invalid record");
}

static void test_dirty(void) {
    fake_erase_all();
    settings_init(&BACKEND);
    CHECK(settings_dirty(), "a module on the defaults has never been saved");

    settings_save();
    CHECK(!settings_dirty(), "right after a save there is nothing to save");

    settings_get()->slot_note[1] = 41;
    CHECK(settings_dirty(), "an edit shows up as dirty");

    settings_get()->slot_note[1] = mixer_default_note(1);
    CHECK(!settings_dirty(), "an edit undone by hand is not a change");

    settings_save();
    CHECK(!settings_dirty(), "and a save settles it again");
}

static void test_write_failure_keeps_flash(void) {
    fake_erase_all();
    settings_init(&BACKEND);
    settings_get()->master_gain = 1500;
    settings_save();

    write_fails = true;
    settings_get()->master_gain = 2500;
    CHECK(!settings_save(), "a refused write must be reported");
    CHECK(settings_dirty(), "and the change must still count as unsaved");

    write_fails = false;
    CHECK(settings_init(&BACKEND) && settings_get()->master_gain == 1500,
          "flash still holds the last good record");
}

int main(void) {
    test_defaults();
    test_virgin_flash();
    test_save_and_reload();
    test_sectors_alternate();
    test_power_cut_falls_back();
    test_crc_catches_rot();
    test_foreign_record_refused();
    test_range_checks();
    test_upgrade_from_v1();
    test_upgrade_from_v2();
    test_resets();
    test_dirty();
    test_write_failure_keeps_flash();

    if (failures == 0) {
        printf("  settings: OK\n");
    } else {
        printf("  settings: %d FAILURES\n", failures);
    }
    return failures != 0;
}
