// Native test bench for the UI state machine and for the drawing.
//
// ui.c and ssd1306.c do not depend on the SDK: here they run against a fake
// library and a bus stub, so the select/assign transitions and the framebuffer
// content are verified with no display, no encoder and no Pico.
#include <stdio.h>
#include <string.h>

#include "mixer.h"
#include "sample_lib.h"
#include "settings.h"
#include "ssd1306.h"
#include "ui.h"

extern size_t stub_bytes_written;
extern int    stub_writes;
extern size_t stub_max_write;
extern int    stub_fail_after;
void stub_reset(void);

#define N_TEST 8
#define TEST_LEN 200

// Mirrors the row layout private to ui.c (it has no reason to export it):
// the hint/message always lands on the last row, and config's label/value
// split rows differently on the two panels. See "geometry" in ui.c.
#if OLED_H == 64
#define TEST_ROW_FOOT       56
#define TEST_ROW_PREV       16
#define TEST_ROW_CUR        24
#define TEST_ROW_NEXT       32
#define TEST_ROW_CFG_LABEL  24
#define TEST_ROW_CFG_VALUE  32
#else
#define TEST_ROW_FOOT       24
#define TEST_ROW_CFG_LABEL  16
#define TEST_ROW_CFG_VALUE  24
#endif

static uint8_t libbuf[64 * 1024];
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

// A flat version-1 library: no kits, so the long press stays inert and the
// select/assign tests below exercise exactly what they did before kit mode.
static const SampleLib *make_lib(unsigned count) {
    memset(libbuf, 0, sizeof libbuf);
    memcpy(libbuf, "SMPL", 4);
    *(uint16_t *)(libbuf + 4) = 1;
    *(uint16_t *)(libbuf + 6) = (uint16_t)count;
    *(uint32_t *)(libbuf + 8) = 22050;

    uint32_t off = 16 + 32 * count;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = libbuf + 16 + 32 * i;
        snprintf((char *)e, 24, "SAMPLE_%02u", i);
        *(uint32_t *)(e + 24) = off;
        *(uint32_t *)(e + 28) = TEST_LEN;
        int16_t *d = (int16_t *)(libbuf + off);
        for (int k = 0; k < TEST_LEN; k++) {
            d[k] = 1000;
        }
        off += TEST_LEN * 2;
    }
    *(uint32_t *)(libbuf + 12) = off;
    return (const SampleLib *)libbuf;
}

// A kit-organised version-2 library: `kits` kits of KIT_SIZE, named the way
// the converter names them, MACHINE_KIT_ROLE.
static const SampleLib *make_kit_lib(unsigned kits) {
    static const char *const ROLE[KIT_SIZE] = {
        "KICK", "SNARE", "CH", "OH", "FLEX1", "FLEX2", "FLEX3", "FLEX4"};
    unsigned count = kits * KIT_SIZE;
    memset(libbuf, 0, sizeof libbuf);
    memcpy(libbuf, "SMPL", 4);
    *(uint16_t *)(libbuf + 4) = 2;
    *(uint16_t *)(libbuf + 6) = (uint16_t)count;
    *(uint32_t *)(libbuf + 8) = 22050;

    uint32_t off = 16 + 32 * count;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = libbuf + 16 + 32 * i;
        snprintf((char *)e, 24, "M%u_KIT%02u_%s", i / KIT_SIZE, i / KIT_SIZE,
                 ROLE[i % KIT_SIZE]);
        *(uint32_t *)(e + 24) = off;
        *(uint32_t *)(e + 28) = TEST_LEN;
        int16_t *d = (int16_t *)(libbuf + off);
        for (int k = 0; k < TEST_LEN; k++) {
            d[k] = 1000;
        }
        off += TEST_LEN * 2;
    }
    *(uint32_t *)(libbuf + 12) = off;
    return (const SampleLib *)libbuf;
}

// How many lit pixels there are in the horizontal band [y0, y0+h).
static int pixels_in_band(int y0, int h) {
    const uint8_t *fb = ssd1306_framebuffer();
    int n = 0;
    for (int y = y0; y < y0 + h; y++) {
        for (int x = 0; x < OLED_W; x++) {
            if (fb[(y / 8) * OLED_W + x] & (1u << (y & 7))) {
                n++;
            }
        }
    }
    return n;
}

// The UI's two hooks. `applies` and `saves` are what the banks below look at
// to tell "the setting changed" from "the setting was pushed onto the module",
// which are not the same thing and have gone wrong separately before.
static int applies;
static int saves;
static bool save_result = true;

// A RAM stand-in for the two settings sectors. The UI's save hook is the real
// settings_save(), because "was anything actually written" is half of what
// these banks are checking, and a counter alone cannot answer it.
#define FAKE_SECTOR 4096
static uint8_t fake_flash[SETTINGS_SECTORS][FAKE_SECTOR];

static const void *fake_read(uint32_t sector) {
    return sector < SETTINGS_SECTORS ? fake_flash[sector] : NULL;
}
static bool fake_write(uint32_t sector, const void *buf, uint32_t len) {
    if (sector >= SETTINGS_SECTORS || len > FAKE_SECTOR) {
        return false;
    }
    memset(fake_flash[sector], 0xFF, FAKE_SECTOR);
    memcpy(fake_flash[sector], buf, len);
    return true;
}
static const SettingsBackend BACKEND = {fake_read, fake_write};

static void fake_apply(void) { applies++; }
static bool fake_save(void) {
    saves++;
    return save_result && settings_save();
}
static const UiHooks HOOKS = {fake_apply, fake_save};

static void setup_common(void) {
    stub_reset();
    ssd1306_init();
    memset(fake_flash, 0xFF, sizeof fake_flash);
    settings_init(&BACKEND);
    // One baseline write, so the banks start from a module that has been
    // configured before. A module that never has is dirty by definition, and
    // that case belongs to the settings bench, not to this one.
    settings_save();
    applies = saves = 0;
    save_result = true;
    ui_init();
    ui_set_hooks(&HOOKS);
}

// Long press opens the menu, then `entry` detents and a press open it. Every
// mode added after kit lives behind this, so the banks say so out loud rather
// than counting encoder events by hand.
static void open_menu(uint32_t entry) {
    ui_event(ENC_LONG_PRESS);
    for (uint32_t i = 0; i < entry; i++) {
        ui_event(ENC_CW);
    }
    ui_event(ENC_PRESS);
}

static void setup(unsigned lib_count) {
    mixer_init(lib_count ? make_lib(lib_count) : NULL);
    setup_common();
}

#define N_KITS 5

static void setup_kits(void) {
    mixer_init(make_kit_lib(N_KITS));
    setup_common();
    mixer_load_default_kit();
}

static void test_select_navigation(void) {
    setup(N_TEST);

    CHECK(ui_mode() == UI_SELECT, "the UI must start in select");
    CHECK(ui_slot() == 0, "initial slot expected 0, found %u", ui_slot());

    for (int i = 0; i < 3; i++) {
        ui_event(ENC_CW);
    }
    CHECK(ui_slot() == 3, "3 cw detents -> slot %u instead of 3", ui_slot());

    // Forward wrap: the detents left to get back round to 0. Counted from
    // NUM_SLOTS rather than written out, so the bank follows the slot count
    // instead of having to be rewritten every time it changes.
    for (int i = 3; i < NUM_SLOTS; i++) {
        ui_event(ENC_CW);
    }
    CHECK(ui_slot() == 0, "forward wrap -> slot %u instead of 0", ui_slot());

    // Backward wrap.
    ui_event(ENC_CCW);
    CHECK(ui_slot() == NUM_SLOTS - 1, "backward wrap -> slot %u instead of %d",
          ui_slot(), NUM_SLOTS - 1);

    printf("  slot navigation           : forward, backward and wrap over %d slots\n",
           NUM_SLOTS);
}

static void test_assign_confirm(void) {
    setup(N_TEST);
    mixer_assign_slot(2, 5);

    ui_event(ENC_CW);
    ui_event(ENC_CW);  // slot 2
    ui_event(ENC_PRESS);

    CHECK(ui_mode() == UI_ASSIGN, "the press must enter assign");
    // It has to start from the sample already assigned, not from zero.
    CHECK(ui_browse_index() == 5, "browser started at %u instead of 5",
          (unsigned)ui_browse_index());

    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(ui_browse_index() == 7, "2 detents -> index %u instead of 7",
          (unsigned)ui_browse_index());
    // Live, like kit mode: the slot plays what is under the cursor already.
    CHECK(mixer_slot_index(2) == 7, "browsing did not assign live (slot 2 = %d)",
          mixer_slot_index(2));

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the confirming press must return to select");
    CHECK(mixer_slot_index(2) == 7, "slot 2 assigned to %d instead of 7",
          mixer_slot_index(2));
    CHECK(mixer_slot_index(0) == -1, "the assignment touched slot 0");

#if OLED_H == 64
    // 128x64 only: browse index 7 of 8 wraps its neighbours to 6 and 0, and
    // all three get their own row - the point of the extra height.
    ui_render(0);
    CHECK(pixels_in_band(TEST_ROW_PREV, 8) > 0, "the previous entry row is blank");
    CHECK(pixels_in_band(TEST_ROW_CUR, 8) > 0, "the current entry row is blank");
    CHECK(pixels_in_band(TEST_ROW_NEXT, 8) > 0, "the next entry row is blank");
#endif

    printf("  assign with confirm       : slot 2 from sample 5 to sample 7\n");
}

static void test_assign_cancel(void) {
    setup(N_TEST);
    mixer_assign_slot(0, 3);

    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(ui_browse_index() == 5, "browser at %u instead of 5",
          (unsigned)ui_browse_index());
    CHECK(mixer_slot_index(0) == 5, "browsing did not assign live (slot 0 = %d)",
          mixer_slot_index(0));

    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press must leave assign");
    CHECK(mixer_slot_index(0) == 3,
          "cancelled but slot 0 changed to %d", mixer_slot_index(0));

    // An empty slot: browsing fills it live, cancelling empties it again
    // rather than leaving the last sample browsed.
    ui_event(ENC_CW);  // slot 1, empty
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    CHECK(mixer_slot_index(1) == 1, "empty slot not filled live (%d)",
          mixer_slot_index(1));
    ui_event(ENC_LONG_PRESS);
    CHECK(mixer_slot_index(1) == -1,
          "cancelled but empty slot 1 kept %d", mixer_slot_index(1));

    printf("  assign cancelled          : live while browsing, long press restores\n");
}

static void test_browse_wrap(void) {
    setup(N_TEST);
    ui_event(ENC_PRESS);

    ui_event(ENC_CCW);
    CHECK(ui_browse_index() == N_TEST - 1,
          "backward wrap in the library -> %u instead of %d",
          (unsigned)ui_browse_index(), N_TEST - 1);

    ui_event(ENC_CW);
    CHECK(ui_browse_index() == 0, "forward wrap -> %u instead of 0",
          (unsigned)ui_browse_index());

    printf("  library wrap              : %d entries, full circle both ways\n",
           N_TEST);
}

// With no library in flash, entering assign would be a dead end: it would show
// an empty list and the press would confirm nothing.
static void test_no_library(void) {
    setup(0);
    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_SELECT, "with no library assign must not be entered");

    ui_render(0);
    CHECK(pixels_in_band(0, OLED_H) > 0, "nothing was drawn on an empty display");

    printf("  library missing           : assign blocked, screen still readable\n");
}

static void test_note_names(void) {
    struct { uint8_t note; const char *want; } cases[] = {
        {36, "C1"}, {37, "C#1"}, {41, "F1"}, {48, "C2"}, {24, "C0"}, {60, "C3"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char out[5];
        ui_note_name(cases[i].note, out);
        CHECK(strcmp(out, cases[i].want) == 0, "note %u -> \"%s\" instead of \"%s\"",
              cases[i].note, out, cases[i].want);
    }
    // The mixer slots start at 36 = C1, like the Volca Drum.
    setup(N_TEST);
    char out[5];
    ui_note_name(mixer_slot_note(0), out);
    CHECK(strcmp(out, "C1") == 0, "slot 0 mapped to %s instead of C1", out);

    printf("  note names                : C1 at 36, consistent with the slot map\n");
}

// The two modes must be tellable apart on the display, not only in the
// internal state: the ">" cursor and the blinking are the only visual cue.
static void test_render_modes(void) {
    setup(N_TEST);

    ui_render(0);
    int select_px = pixels_in_band(0, OLED_H);
    CHECK(select_px > 0, "the select screen is empty");

    // The cell of the current slot is filled: the strip band must have many
    // more lit pixels than six bare digits would produce.
    int strip_px = pixels_in_band(0, 8);
    CHECK(strip_px > 100, "the selected cell does not look inverted (%d pixels)",
          strip_px);

    ui_event(ENC_PRESS);

    // The lit and unlit phases of the blink must produce different drawings.
    ui_render(0);
    int blink_on = pixels_in_band(0, 8);
    ui_render(250);
    int blink_off = pixels_in_band(0, 8);
    CHECK(blink_on != blink_off,
          "the cell does not blink in assign (%d pixels in both phases)",
          blink_on);

    printf("  drawing                   : select inverted, assign blinking at 2Hz\n");
}

// The flush must stay quiet when the content has not changed: at 400kHz a
// frame costs ~11ms and repeating it thirty times a second for nothing would
// waste core0.
static void test_flush_only_when_dirty(void) {
    setup(N_TEST);

    ui_render(0);
    ssd1306_flush();

    stub_writes = 0;
    stub_bytes_written = 0;
    for (int i = 0; i < 10; i++) {
        ui_render(0);
        ssd1306_flush();
    }
    CHECK(stub_writes == 0, "%d pointless flushes on identical content", stub_writes);

    ui_event(ENC_CW);
    ui_render(0);
    bool sent = ssd1306_flush();
    CHECK(sent, "after a slot change the flush did not transmit");

    printf("  flush                     : quiet on a still screen, transmits on change\n");
}

// The flush must only send the pages that changed. Changing slot touches the
// strip and the two bottom rows, not the activity-bar band: a whole frame
// would be wasted bandwidth and, above all, more milliseconds in which core0
// is not sampling the encoder.
static void test_flush_partial(void) {
    setup(N_TEST);
    ui_render(0);
    ssd1306_flush();

    stub_reset();
    ui_event(ENC_CW);
    ui_render(0);
    ssd1306_flush();

    CHECK(stub_bytes_written > 0, "a slot change transmitted nothing");
    CHECK(stub_bytes_written < OLED_FB_LEN,
          "%zu bytes sent for a slot change: that is a whole frame, "
          "the per-page flush is not working", stub_bytes_written);
    CHECK(stub_max_write <= 1 + OLED_FB_LEN,
          "%zu byte write, past the size of the buffer", stub_max_write);

    printf("  partial flush             : %zu bytes instead of %d for a slot change\n",
           stub_bytes_written, OLED_FB_LEN);
}

// The bug that drove the display crazy: if the write fails, the flush must not
// consider the frame sent. Otherwise the display is left with half a frame on
// it and nobody ever corrects it, or it retransmits forever.
static void test_flush_failure_recovers(void) {
    setup(N_TEST);
    ui_render(0);

    // The window goes through, the data does not: exactly the truncation
    // caused by a timeout.
    stub_fail_after = 1;
    bool ok = ssd1306_flush();
    CHECK(!ok, "failed flush reported as successful");

    // Once the bus is back, the pages left behind must go out again. Not the
    // whole frame: only the dirty ones, which is the point of the per-page
    // flush.
    stub_reset();
    ok = ssd1306_flush();
    CHECK(ok, "after an error the flush retransmitted nothing");
    size_t recovered = stub_bytes_written;
    CHECK(recovered > 0, "empty retransmission after the error");

    // The invariant that matters: the sent state now really matches the
    // framebuffer, so the next flush must stay quiet. Had the flush declared
    // the failed frame sent, it would stay quiet here with half a frame on the
    // display, and that is exactly the fault seen on the hardware.
    stub_reset();
    ok = ssd1306_flush();
    CHECK(!ok, "repeated flush on an identical frame retransmitted");
    CHECK(stub_bytes_written == 0, "%zu bytes sent on a still screen",
          stub_bytes_written);

    printf("  failed flush              : nothing taken as sent, %zu bytes resent on recovery\n",
           recovered);
}

// Kit mode: the long press in select loads whole kits as it is turned, and the
// press keeps the one you are hearing.
static void test_kit_confirm(void) {
    setup_kits();
    CHECK(mixer_kit_count() == N_KITS, "%u kits instead of %d",
          mixer_kit_count(), N_KITS);
    CHECK(mixer_current_kit() == 0, "boot did not load kit 0 (%d)",
          mixer_current_kit());

    open_menu(UI_MENU_KIT);
    CHECK(ui_mode() == UI_KIT, "the KIT menu entry must enter kit mode");
    CHECK(ui_kit_index() == 0, "kit browser started at %u instead of 0",
          (unsigned)ui_kit_index());

    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(ui_kit_index() == 2, "2 detents -> kit %u instead of 2",
          (unsigned)ui_kit_index());
    // Loaded while browsing: you pick a kit by ear, so all six slots must have
    // followed the knob already, before any confirmation.
    CHECK(mixer_current_kit() == 2, "browsing did not load kit 2 (%d)",
          mixer_current_kit());
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(mixer_slot_index((uint8_t)i) == 2 * KIT_SIZE + i,
              "slot %d on entry %d instead of %d", i + 1,
              mixer_slot_index((uint8_t)i), 2 * KIT_SIZE + i);
    }

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the press must return to select");
    CHECK(mixer_current_kit() == 2, "confirming lost the kit (%d)",
          mixer_current_kit());

    printf("  kit confirmed             : %d kits, %d slots follow the knob\n",
           N_KITS, NUM_SLOTS);
}

// Cancelling has to put back what was there, and what was there is not
// necessarily a kit: a slot picked by hand must come back too.
static void test_kit_cancel(void) {
    setup_kits();
    mixer_assign_slot(1, 4 * KIT_SIZE + 1);  // kit 0 with somebody else's snare
    CHECK(mixer_current_kit() == -1, "a hand-mixed set reported as a kit");

    open_menu(UI_MENU_KIT);
    CHECK(ui_kit_index() == 0,
          "with no whole kit loaded the browser must start at 0, found %u",
          (unsigned)ui_kit_index());
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(mixer_current_kit() == 3, "browsing did not load kit 3 (%d)",
          mixer_current_kit());

    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press must leave kit mode");
    CHECK(mixer_slot_index(0) == 0, "slot 1 not restored (%d)",
          mixer_slot_index(0));
    CHECK(mixer_slot_index(1) == 4 * KIT_SIZE + 1,
          "the hand-picked snare was not restored (%d)", mixer_slot_index(1));
    CHECK(mixer_current_kit() == -1, "cancelling left a whole kit loaded");

    printf("  kit cancelled             : the mixed set restored slot by slot\n");
}

static void test_kit_wrap_and_no_kits(void) {
    setup_kits();
    open_menu(UI_MENU_KIT);
    ui_event(ENC_CCW);
    CHECK(ui_kit_index() == N_KITS - 1, "backward wrap -> kit %u instead of %d",
          (unsigned)ui_kit_index(), N_KITS - 1);
    ui_event(ENC_CW);
    CHECK(ui_kit_index() == 0, "forward wrap -> kit %u instead of 0",
          (unsigned)ui_kit_index());

    // On a flat library there is no kit to load, so the entry must refuse and
    // leave the menu up: opening an empty browser would strand you in a mode
    // whose only exit is another long press.
    setup(N_TEST);
    open_menu(UI_MENU_KIT);
    CHECK(ui_mode() == UI_MENU, "kit mode entered on a library with no kits");

    // Nor with no library at all.
    setup(0);
    open_menu(UI_MENU_KIT);
    CHECK(ui_mode() == UI_MENU, "kit mode entered with no library");

    printf("  kit mode guards           : full circle, inert without kits\n");
}

// In kit mode the whole strip blinks, not one cell: what is changing is every
// slot at once, and one blinking cell would read as "slot 3 is being edited".
static void test_render_kit(void) {
    setup_kits();
    open_menu(UI_MENU_KIT);

    ui_render(0);
    int on = pixels_in_band(0, 8);
    ui_render(250);
    int off = pixels_in_band(0, 8);
    CHECK(on != off, "the strip does not blink in kit mode (%d pixels both ways)",
          on);

    ui_event(ENC_PRESS);
    ui_render(0);
    int select_px = pixels_in_band(0, 8);
    // Every cell inverted lights up far more than one.
    CHECK(on > select_px,
          "kit mode lights %d pixels against %d in select: the whole strip is "
          "not being inverted", on, select_px);

    printf("  kit drawing               : all cells inverted, %d px against %d\n",
           on, select_px);
}


// --- menu, presets and the settings page -------------------------------------

// One encoder, three gestures, and select had already spent all three: the
// menu is what everything added since hangs off, so its own navigation is
// worth a bank of its own.
static void test_menu_navigation(void) {
    setup_kits();

    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_MENU, "the long press must open the menu");
    CHECK(ui_menu_index() == 0, "the menu must open on its first entry");

    ui_event(ENC_CCW);
    CHECK(ui_menu_index() == UI_MENU_COUNT - 1,
          "backward wrap -> entry %u instead of %d", (unsigned)ui_menu_index(),
          UI_MENU_COUNT - 1);
    ui_event(ENC_CW);
    CHECK(ui_menu_index() == 0, "forward wrap -> entry %u instead of 0",
          (unsigned)ui_menu_index());

    // A long press in the menu leaves it, and must not have touched a slot on
    // the way in or out.
    int32_t before = mixer_slot_index(0);
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press must leave the menu");
    CHECK(mixer_slot_index(0) == before, "opening the menu changed a slot");

    printf("  menu navigation           : %d entries, wraps, leaves clean\n",
           UI_MENU_COUNT);
}

// Saving writes the slots as they stand into a preset and asks for a flash
// write. What must NOT happen is the note map going with them: recalling a
// preset has to change the sounds, never the notes the module answers to.
static void test_preset_save(void) {
    setup_kits();
    mixer_assign_slot(1, 4 * KIT_SIZE + 1);  // kit 0 with somebody else's snare

    open_menu(UI_MENU_SAVE);
    CHECK(ui_mode() == UI_SAVE, "the SAVE entry must open the save page");
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(ui_preset_index() == 2, "save target %u instead of 3",
          (unsigned)ui_preset_index() + 1);

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_SELECT, "confirming a save must return to select");
    CHECK(saves == 1, "the save was not written to flash (%d calls)", saves);

    const SettingsRecord *r = settings_get();
    CHECK(r->preset[2].used, "preset 3 was not marked used");
    CHECK(r->preset[2].lib_index[0] == 0, "preset 3 slot 1 = %d instead of 0",
          r->preset[2].lib_index[0]);
    CHECK(r->preset[2].lib_index[1] == 4 * KIT_SIZE + 1,
          "the hand-picked snare was not stored (%d)", r->preset[2].lib_index[1]);
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        CHECK(r->slot_note[sl] == mixer_default_note(sl),
              "saving a preset moved the note of slot %u", sl + 1u);
    }

    printf("  preset saved              : slots stored, note map untouched\n");
}

// A failed write has to be reported as failed: the preset is in RAM either
// way, and a module that says "saved" after a refused erase loses the work at
// the next power cut without ever warning anybody.
static void test_preset_save_failure(void) {
    setup_kits();
    save_result = false;

    open_menu(UI_MENU_SAVE);
    ui_event(ENC_PRESS);
    CHECK(saves == 1, "the save was not attempted");
    CHECK(ui_mode() == UI_SELECT, "the page must close either way");

    ui_render(0);
    // The message goes where the hint goes: the bottom right corner is the only
    // part of the layout not already carrying information.
    CHECK(pixels_in_band(TEST_ROW_FOOT, 8) > 0, "nothing was drawn on the footer row");

    printf("  preset save refused       : reported, not swallowed\n");
}

static void test_preset_load_and_cancel(void) {
    setup_kits();

    // Two presets, deliberately different: 3 holds kit 3, 5 a hand-made mix.
    mixer_load_kit(3);
    open_menu(UI_MENU_SAVE);
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    ui_event(ENC_PRESS);  // preset 3

    mixer_load_kit(1);
    mixer_assign_slot(2, 4 * KIT_SIZE + 2);
    open_menu(UI_MENU_SAVE);
    for (int i = 0; i < 4; i++) {
        ui_event(ENC_CW);
    }
    ui_event(ENC_PRESS);  // preset 5

    // Now come back to kit 0 and browse the presets.
    mixer_load_kit(0);
    open_menu(UI_MENU_LOAD);
    CHECK(ui_mode() == UI_PRESET, "the LOAD entry must open the preset browser");
    CHECK(ui_preset_index() == 0, "the browser must start at preset 1");

    ui_event(ENC_CW);
    ui_event(ENC_CW);
    // Auditioned while browsing, exactly like a kit: you pick by ear.
    CHECK(mixer_current_kit() == 3, "browsing did not load preset 3 (kit %d)",
          mixer_current_kit());

    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(mixer_slot_index(2) == 4 * KIT_SIZE + 2,
          "preset 5's hand-picked slot did not load (%d)", mixer_slot_index(2));

    // Cancelling restores what was there before the browser was opened, which
    // was kit 0 and not a preset at all.
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press must leave the browser");
    CHECK(mixer_current_kit() == 0, "cancelling did not restore kit 0 (%d)",
          mixer_current_kit());

    printf("  preset load / cancel      : auditioned live, cancel restores\n");
}

// With nothing saved the entry must refuse, for the same reason the KIT entry
// refuses on a flat library: an empty browser is a mode you can only long-press
// your way out of.
static void test_preset_load_when_empty(void) {
    setup_kits();
    open_menu(UI_MENU_LOAD);
    CHECK(ui_mode() == UI_MENU, "the preset browser opened with no presets");

    printf("  preset browser guard      : inert with nothing saved\n");
}

static void test_config_edit(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);
    CHECK(ui_mode() == UI_CONFIG, "the CONFIG entry must open the settings");
    CHECK(ui_config_index() == 0, "the settings must open on the first entry");

    // Entry 0 is the MIDI channel. One press to edit, turns to move it.
    uint8_t before = settings_get()->midi_channel;
    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG_EDIT, "the press must start editing");
    ui_event(ENC_CW);
    CHECK(settings_get()->midi_channel == before + 1,
          "channel %u instead of %u", settings_get()->midi_channel, before + 1);
    // Applied while turning: a setting you judge by the result is useless if it
    // only reaches the module on confirmation.
    CHECK(applies > 0, "the change was not pushed onto the module");

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "confirming must go back to the list");
    CHECK(settings_get()->midi_channel == before + 1, "the confirmed value was lost");

    // Leaving writes: a settings page whose changes evaporate is worse than an
    // erase you did not explicitly ask for.
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press must leave the settings");
    CHECK(saves == 1, "leaving a changed settings page did not save (%d)", saves);

    printf("  config edit               : live, confirmed, saved on the way out\n");
}

static void test_config_cancel(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);

    uint8_t before = settings_get()->midi_channel;
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(settings_get()->midi_channel != before, "the edit did nothing");

    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "cancelling an edit must go back to the list");
    CHECK(settings_get()->midi_channel == before,
          "cancelled but the channel stayed at %u", settings_get()->midi_channel);

    // Nothing changed in the end, so leaving must not erase a sector for it.
    ui_event(ENC_LONG_PRESS);
    CHECK(saves == 0, "an unchanged settings page was written to flash");

    printf("  config cancel             : value restored, no pointless erase\n");
}

// The note map is editable, which is exactly what makes a reset necessary: a
// slot moved to a note nothing sends is a silent module with no obvious way
// back. RESET SETTINGS puts the configuration back and keeps the presets.
static void test_config_reset_settings(void) {
    setup_kits();
    settings_get()->slot_note[0] = 99;
    settings_get()->midi_channel = 3;
    settings_get()->slot_level[2] = 4;
    settings_get()->slot_pan[3] = -5;
    settings_get()->preset[0].used = 1;
    settings_get()->preset[0].lib_index[0] = 17;

    open_menu(UI_MENU_CONFIG);
    ui_event(ENC_CCW);  // the list wraps: CLEAR PRESETS is the last entry
    ui_event(ENC_CCW);  // RESET SETTINGS just above it
    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG_EDIT, "the reset must ask before acting");

    // Left on NO it must do nothing at all.
    ui_event(ENC_PRESS);
    CHECK(settings_get()->slot_note[0] == 99, "the reset fired on NO");

    ui_event(ENC_PRESS);
    ui_event(ENC_CW);  // NO -> YES
    ui_event(ENC_PRESS);
    CHECK(settings_get()->slot_note[0] == mixer_default_note(0),
          "the note map was not reset (%u)", settings_get()->slot_note[0]);
    CHECK(settings_get()->midi_channel == 9, "the channel was not reset (%u)",
          settings_get()->midi_channel);
    CHECK(settings_get()->slot_level[2] == 0 && settings_get()->slot_pan[3] == 0,
          "level %u / pan %d survived the reset", settings_get()->slot_level[2],
          settings_get()->slot_pan[3]);
    CHECK(settings_get()->preset[0].used && settings_get()->preset[0].lib_index[0] == 17,
          "RESET SETTINGS threw a preset away");
    CHECK(applies > 0, "the reset configuration was not pushed onto the module");

    printf("  config reset settings     : two gestures, configuration only, presets kept\n");
}

// HAT CHOKE is a plain OFF/ON entry right after DYNAMICS, off by default,
// applied while turning like every other entry.
static void test_config_hat_choke(void) {
    setup_kits();
    CHECK(settings_get()->hat_choke == 0, "the hat choke must come up off");
    open_menu(UI_MENU_CONFIG);
    for (int i = 0; i < 4; i++) {
        ui_event(ENC_CW);
    }
    CHECK(ui_config_index() == 4, "expected HAT CHOKE at 4, got %u", ui_config_index());
    applies = 0;
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    CHECK(settings_get()->hat_choke == 1, "one detent did not turn it on");
    CHECK(applies > 0, "the hat choke was not pushed onto the module");
    ui_event(ENC_CW);
    CHECK(settings_get()->hat_choke == 1, "ON is the end, it clamps (%u)",
          settings_get()->hat_choke);
    ui_event(ENC_PRESS);
    int before = saves;
    ui_event(ENC_LONG_PRESS);
    CHECK(saves == before + 1, "leaving after HAT CHOKE did not save");

    printf("  config hat choke          : OFF/ON after DYNAMICS, off by default\n");
}

// CLEAR PRESETS empties the eight presets and touches nothing else, except a
// boot preset that would now point at nothing.
static void test_config_clear_presets(void) {
    setup_kits();
    settings_get()->slot_note[0] = 99;
    settings_get()->slot_level[2] = 4;
    settings_get()->boot_preset = 0;
    settings_get()->preset[0].used = 1;
    settings_get()->preset[5].used = 1;

    open_menu(UI_MENU_CONFIG);
    ui_event(ENC_CCW);  // CLEAR PRESETS
    ui_event(ENC_PRESS);
    ui_event(ENC_PRESS);  // NO
    CHECK(settings_get()->preset[0].used, "CLEAR PRESETS fired on NO");

    ui_event(ENC_PRESS);
    ui_event(ENC_CW);  // YES
    ui_event(ENC_PRESS);
    for (int p = 0; p < NUM_PRESETS; p++) {
        CHECK(!settings_get()->preset[p].used, "preset %d survived CLEAR PRESETS", p + 1);
    }
    CHECK(settings_get()->boot_preset == PRESET_NONE,
          "BOOT still points at cleared preset %u", settings_get()->boot_preset + 1u);
    CHECK(settings_get()->slot_note[0] == 99 && settings_get()->slot_level[2] == 4,
          "CLEAR PRESETS touched the configuration");

    // And leaving the page saves it, like any other change.
    int before = saves;
    ui_event(ENC_LONG_PRESS);
    CHECK(saves == before + 1, "leaving after CLEAR PRESETS did not save");

    printf("  config clear presets      : presets emptied, configuration kept\n");
}

// DYNAMICS sits right after VELOCITY, at index 3. Its stored value is not the
// order it turns in: SETTINGS_DYNAMICS_MID is 0 in the record (so an older
// save reads as the ratio it already had), but on the knob it is the third of
// four stops, OFF/LOW/MID/HIGH, which is the order that reads as a scale.
static void test_config_dynamics(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);
    ui_event(ENC_CW);  // GAIN
    ui_event(ENC_CW);  // VELOCITY
    ui_event(ENC_CW);  // DYNAMICS
    CHECK(ui_config_index() == 3, "expected DYNAMICS at index 3, got %u",
          ui_config_index());

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG_EDIT, "the press must start editing");
    CHECK(settings_get()->dynamics == SETTINGS_DYNAMICS_MID,
          "the module must come up on MID, got %u", settings_get()->dynamics);

    // Turning down twice from MID (position 2) reaches OFF (position 0), the
    // low end of the knob and SETTINGS_DYNAMICS_OFF in the record.
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);
    CHECK(settings_get()->dynamics == SETTINGS_DYNAMICS_OFF,
          "two steps down from MID landed on %u, not OFF", settings_get()->dynamics);
    CHECK(applies > 0, "the change was not pushed onto the module");

    // And three steps up from there reaches the other end, HIGH.
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(settings_get()->dynamics == SETTINGS_DYNAMICS_HIGH,
          "three steps up from OFF landed on %u, not HIGH", settings_get()->dynamics);

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "confirming must go back to the list");

    printf("  config dynamics            : OFF/LOW/MID/HIGH on the knob, MID by default\n");
}

// NOTES, LEVEL and PAN are groups in the top list: a press opens the eight per-slot
// entries, a long press comes back up to the group's own row, and only
// leaving the page saves - once, however many groups were visited.
static void test_config_groups(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);
    CHECK(ui_config_depth() == 0, "the settings must open on the top list");

    // Top list: ... BOOT, NOTES >, LEVEL >, PAN >, RESET SETTINGS, CLEAR
    // PRESETS. Backwards from the first entry wraps onto CLEAR PRESETS, two
    // more land on PAN >.
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);
    CHECK(ui_config_index() == 9, "expected PAN > at 9, got %u", ui_config_index());

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "opening a group must not start an edit");
    CHECK(ui_config_depth() == 1, "the press did not open the PAN group");
    CHECK(ui_config_index() == 0, "a group must open on its first slot");

    // Slot 2 (SNARE), two steps right, applied live.
    ui_event(ENC_CW);
    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG_EDIT, "the press on a slot must edit it");
    ui_event(ENC_CW);
    ui_event(ENC_CW);
    CHECK(settings_get()->slot_pan[1] == 2, "SNARE pan %d, expected 2",
          settings_get()->slot_pan[1]);
    CHECK(applies > 0, "the pan was not pushed onto the module");

    // Running off the end clamps at hard right, it does not wrap to the left.
    for (int i = 0; i < 20; i++) {
        ui_event(ENC_CW);
    }
    CHECK(settings_get()->slot_pan[1] == MIXER_PAN_RIGHT,
          "the pan ran past hard right to %d", settings_get()->slot_pan[1]);
    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "confirming must go back to the group list");

    // The sublist wraps over its own eight entries, not the whole page.
    for (int i = 0; i < NUM_SLOTS; i++) {
        ui_event(ENC_CW);
    }
    CHECK(ui_config_index() == 1 && ui_config_depth() == 1,
          "eight detents in a group left the cursor at %u/depth %u",
          ui_config_index(), ui_config_depth());

    // Long press: one level up, back on PAN >, nothing saved yet.
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_CONFIG, "the long press left the page from a group");
    CHECK(ui_config_depth() == 0 && ui_config_index() == 9,
          "back from PAN the cursor is at %u/depth %u", ui_config_index(),
          ui_config_depth());
    CHECK(saves == 0, "coming out of a group saved (%d)", saves);

    // LEVEL > is the entry just above. It turns like a fader: it opens at
    // 0dB, clockwise is already the top, counter-clockwise goes down 3dB a
    // detent and ends on OFF, stored as attenuation steps.
    ui_event(ENC_CCW);
    ui_event(ENC_PRESS);
    CHECK(ui_config_depth() == 1, "the press did not open the LEVEL group");
    CHECK(settings_get()->slot_level[0] == 0, "KICK must start at 0dB, got %u",
          settings_get()->slot_level[0]);
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    CHECK(settings_get()->slot_level[0] == 0, "the level went above 0dB to %u",
          settings_get()->slot_level[0]);
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);
    CHECK(settings_get()->slot_level[0] == 2, "two detents down stored %u, expected 2 (-6dB)",
          settings_get()->slot_level[0]);
    for (int i = 0; i < 20; i++) {
        ui_event(ENC_CCW);
    }
    CHECK(settings_get()->slot_level[0] == MIXER_LEVEL_OFF,
          "the bottom of the travel stored %u, expected OFF",
          settings_get()->slot_level[0]);
    ui_event(ENC_PRESS);
    // A cancelled edit puts the old value back: SNARE, down one, long press.
    ui_event(ENC_CW);
    ui_event(ENC_PRESS);
    ui_event(ENC_CCW);
    CHECK(settings_get()->slot_level[1] == 1, "SNARE level %u while editing",
          settings_get()->slot_level[1]);
    ui_event(ENC_LONG_PRESS);
    CHECK(settings_get()->slot_level[1] == 0, "cancel left SNARE at %u",
          settings_get()->slot_level[1]);
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_config_depth() == 0 && ui_config_index() == 8,
          "back from LEVEL the cursor is at %u/depth %u", ui_config_index(),
          ui_config_depth());

    // NOTES > is the entry above that, and edits the note map.
    ui_event(ENC_CCW);
    ui_event(ENC_PRESS);
    CHECK(ui_config_depth() == 1, "the press did not open the NOTES group");
    uint8_t before = settings_get()->slot_note[0];
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    ui_event(ENC_PRESS);
    CHECK(settings_get()->slot_note[0] == before + 1, "KICK note %u, expected %u",
          settings_get()->slot_note[0], before + 1);
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_config_index() == 7, "back from NOTES the cursor is at %u",
          ui_config_index());

    // Leaving the page saves both groups' changes in one write.
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "the long press at the top must leave");
    CHECK(saves == 1, "leaving after three groups saved %d times", saves);

    // And RESET SETTINGS puts every slot back in the centre and at 0dB.
    open_menu(UI_MENU_CONFIG);
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);  // RESET SETTINGS
    ui_event(ENC_PRESS);
    ui_event(ENC_CW);
    ui_event(ENC_PRESS);
    CHECK(settings_get()->slot_pan[1] == 0, "restore left SNARE panned %d",
          settings_get()->slot_pan[1]);
    CHECK(settings_get()->slot_level[0] == 0, "restore left KICK at level %u",
          settings_get()->slot_level[0]);

    printf("  config groups             : NOTES, LEVEL and PAN open, come back, save once\n");
}

// Inside a group the title bar names it, and the entries are the slot roles.
static void test_render_config_group(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);
    ui_render(0);
    int top_title = pixels_in_band(0, 8);

    ui_event(ENC_CCW);
    ui_event(ENC_CCW);
    ui_event(ENC_CCW);  // PAN >
    ui_event(ENC_PRESS);
    ui_render(0);
    int pan_title = pixels_in_band(0, 8);
    // "PAN 1/8" is fewer glyphs than "CONFIG 1/12", and the bar is inverted,
    // so it lights more pixels: the title changed, whichever way round.
    CHECK(pan_title != top_title, "the title bar did not change inside PAN");
    CHECK(pixels_in_band(TEST_ROW_CFG_VALUE, 8) > 0, "the pan value row is blank");

    printf("  config group drawing      : title names the group\n");
}

// The settings page replaces the slot strip with a title bar: in there the
// slots are not what is being edited, and the position in a fourteen-entry
// list is what the top row has to carry.
static void test_render_config(void) {
    setup_kits();
    open_menu(UI_MENU_CONFIG);

    ui_render(0);
    int title = pixels_in_band(0, 8);
    // A full-width inverted bar lights far more than eight small cells.
    CHECK(title > 400, "the settings title bar lit only %d pixels", title);

    // Editing blinks the value row, not the label row.
    ui_event(ENC_PRESS);
    ui_render(0);
    int value_on = pixels_in_band(TEST_ROW_CFG_VALUE, 8);
    int label_on = pixels_in_band(TEST_ROW_CFG_LABEL, 8);
    ui_render(250);
    int value_off = pixels_in_band(TEST_ROW_CFG_VALUE, 8);
    int label_off = pixels_in_band(TEST_ROW_CFG_LABEL, 8);
    CHECK(value_on != value_off, "the value does not blink while editing");
    CHECK(label_on == label_off, "the label blinks too, and it should not");

#if OLED_H == 64
    // Neighbouring entries are shown too, on their own rows above and below.
    CHECK(pixels_in_band(16, 8) > 0, "the previous config entry row is blank");
    CHECK(pixels_in_band(40, 8) > 0, "the next config entry row is blank");
#endif

    printf("  config drawing            : title bar %d px, only the value blinks\n",
           title);
}

// ABOUT has no state of its own to get wrong - the one thing worth checking
// is that it is reachable, draws something, and both gestures leave the same
// way everything else entered from the menu does.
static void test_about(void) {
    setup(N_TEST);
    open_menu(UI_MENU_ABOUT);
    CHECK(ui_mode() == UI_ABOUT, "the menu entry did not reach ABOUT");

    ui_render(0);
    CHECK(pixels_in_band(0, 8) > 0, "the ABOUT title bar is blank");
    CHECK(pixels_in_band(TEST_ROW_FOOT, 8) > 0, "the ABOUT hint is blank");

    ui_event(ENC_PRESS);
    CHECK(ui_mode() == UI_SELECT, "a press on ABOUT must leave to select");

    open_menu(UI_MENU_ABOUT);
    ui_event(ENC_LONG_PRESS);
    CHECK(ui_mode() == UI_SELECT, "a long press on ABOUT must leave to select");

    printf("  about                     : reachable from the menu, both gestures leave\n");
}

// The splash is off by construction (ui_init() never turns it on, only
// ui_show_splash() does) - every test above ran with no splash to skip past
// for exactly that reason. These two banks are what actually exercise it.
static void test_splash(void) {
    setup(N_TEST);
    ui_show_splash(1000);

    ui_render(1000);
    CHECK(pixels_in_band(0, OLED_H) > 0, "the splash drew nothing");
    CHECK(ui_mode() == UI_SELECT, "the mode underneath must stay select");
    uint8_t splash_fb[OLED_FB_LEN];
    memcpy(splash_fb, ssd1306_framebuffer(), OLED_FB_LEN);

    // Still up just before the deadline: the splash has nothing time-varying
    // in it, so an unchanged frame here means it is still the splash.
    ui_tick(1000 + 1399);
    CHECK(memcmp(splash_fb, ssd1306_framebuffer(), OLED_FB_LEN) == 0,
          "the splash changed before its own deadline");

    // Gone once the deadline passes - far enough past the previous tick that
    // UI_PERIOD_MS's own redraw throttle cannot be what swallows this one.
    ui_tick(1000 + 1440);
    CHECK(memcmp(splash_fb, ssd1306_framebuffer(), OLED_FB_LEN) != 0,
          "the screen did not change once the splash timed out");
    CHECK(ui_slot() == 0, "select is what is left showing underneath");

    // A second run, dismissed early by input instead of by the clock.
    setup(N_TEST);
    ui_show_splash(0);
    ui_event(ENC_CW);
    CHECK(ui_mode() == UI_SELECT, "an event during the splash must dismiss it");
    CHECK(ui_slot() == 0,
          "the dismissing event must not also act as a slot change");

    printf("  splash                    : times out at 1400ms, any input skips it early\n");
}

int main(void) {
    test_select_navigation();
    test_assign_confirm();
    test_assign_cancel();
    test_browse_wrap();
    test_no_library();
    test_note_names();
    test_render_modes();
    test_flush_only_when_dirty();
    test_flush_partial();
    test_flush_failure_recovers();
    test_kit_confirm();
    test_kit_cancel();
    test_kit_wrap_and_no_kits();
    test_render_kit();
    test_menu_navigation();
    test_preset_save();
    test_preset_save_failure();
    test_preset_load_and_cancel();
    test_preset_load_when_empty();
    test_config_edit();
    test_config_cancel();
    test_config_dynamics();
    test_config_hat_choke();
    test_config_reset_settings();
    test_config_clear_presets();
    test_render_config();
    test_config_groups();
    test_render_config_group();
    test_about();
    test_splash();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
