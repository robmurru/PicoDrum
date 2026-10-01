#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "logo_bitmap.h"
#include "mixer.h"
#include "settings.h"
#include "ssd1306.h"
#include "version.h"

// --- geometry ----------------------------------------------------------------
// The font is 7 tall, so one pixel of separation is left between rows without
// having to add margins.
//
// 128x32 (1U): four rows, one entry shown at a time.
// 128x64 (6HP): eight rows. The extra four are not stretched whitespace: every
// mode that browses a list (assign, kit, menu, preset, save, config) becomes a
// real three-line list, the entry before and after the current one shown
// dimmed above and below it — something the four-row build cannot do at all,
// not a bigger version of what it already does. See draw_context_list().
#if OLED_H == 64
// select / assign / kit / menu / preset / save: strip, bars, a three-line
// list, then a status line above the hint. The gap sits between the list and
// the status line rather than under the bars: the bars are only 3px tall on
// an 8px row and already read as light, but the list's dimmed text runs
// straight into the status line's without it.
#define ROW_STRIP 0
#define ROW_BARS  8
#define ROW_PREV  16
#define ROW_CUR   24
#define ROW_NEXT  32
#define ROW_GAP2  40
#define ROW_INFO  48
#define ROW_FOOT  56

// config / config-edit: a title bar instead of the strip (the slots are not
// what is being edited here), and the value gets a row of its own instead of
// sharing the label's. Row 48 is left blank on purpose, the same breathing
// room the gap rows give the other modes; the hint still lands on the shared
// ROW_FOOT below.
#define ROW_CFG_TITLE 0
#define ROW_CFG_PREV  16
#define ROW_CFG_CUR   24
#define ROW_CFG_VALUE 32
#define ROW_CFG_NEXT  40
#else
#define ROW_STRIP 0
#define ROW_BARS  8
#define ROW_NAME  16
#define ROW_FOOT  24
#endif

// Eight cells of 15px with a 1px gap: 0 + 8*16 = 128, the full width of the
// panel. At six slots these were 20px wide starting at x=2; the narrower cell
// still holds a centred digit with a pixel to spare on each side.
#define CELL_X0   0
#define CELL_W    15
#define CELL_STEP 16
#define CELL_DIGIT_DX ((CELL_W - OLED_CHAR_W) / 2)

_Static_assert(CELL_X0 + NUM_SLOTS * CELL_STEP <= OLED_W,
               "the slot strip does not fit the width of the display");

// Activity bar: narrower than the cell, so it reads as an indicator and not as
// a second selection.
#define BAR_DX 2
#define BAR_W  (CELL_W - 2 * BAR_DX)
#define BAR_Y  (ROW_BARS + 1)
#define BAR_H  3

// 21 characters fit in a row; in assign the ">" cursor eats one of them.
#define NAME_MAX_SELECT OLED_COLS
#define NAME_MAX_ASSIGN (OLED_COLS - 1)

// Blink of the cell being edited: 2Hz, i.e. half a period every 250ms.
#define BLINK_HALF_MS 250

// Redrawing the whole framebuffer is cheap, the I2C flush costs ~11ms. At 30Hz
// the flush only fires when something really changed, so on a still screen the
// cost is just the 512-byte comparison.
#define UI_PERIOD_MS 33

// How long the boot splash stays up if nothing dismisses it early.
#define SPLASH_MS 1400

// Which settings list the cursor is in, see "the settings list" below.
enum {
    CFG_GROUP_TOP = 0,
    CFG_GROUP_NOTES,
    CFG_GROUP_PAN,
};

static UiMode   mode;
static uint8_t  slot;
static uint32_t browse;
static uint32_t kit;
// What the slots held when kit mode was entered. Auditioning a kit assigns the
// six slots for real — that is the point, you hear it while you turn — so
// cancelling has to put back what was there, and what was there is not
// necessarily a kit: it can be six slots picked one at a time.
static int32_t  kit_undo[NUM_SLOTS];
static uint32_t last_tick_ms;
static bool     tick_started;

static bool     splash_active;
static uint32_t splash_start_ms;

static const UiHooks *hooks;

static uint32_t menu;          // cursor in the menu
static uint32_t preset;        // preset being auditioned or written to
static uint32_t cfg;           // the settings entry under the cursor (CFG_*)
static uint32_t cfg_group;     // which list the cursor is in: CFG_GROUP_*
static int32_t  cfg_value;     // value being edited, live
static int32_t  cfg_undo;      // what it was when editing started
static int32_t  preset_undo[NUM_SLOTS];
// What the slot held when assign was entered. Browsing assigns for real, the
// same as kit mode, so the sample under the cursor is what the next MIDI hit
// plays; cancelling puts this back, which may be -1 (an empty slot).
static int32_t  assign_undo;

// A one-line answer to a press that would otherwise look like nothing
// happened: a save that failed, a preset that is empty. Counted in frames
// rather than milliseconds so ui_render() stays a pure function of its
// argument and the tests can call it without a clock.
static const char *msg;
static uint16_t    msg_frames;

#define MSG_FRAMES 40  // ~1.3s at UI_PERIOD_MS

static void set_msg(const char *m) {
    msg = m;
    msg_frames = MSG_FRAMES;
}

static uint32_t cfg_list_pos(void);

void ui_set_hooks(const UiHooks *h) { hooks = h; }

void ui_init(void) {
    mode = UI_SELECT;
    slot = 0;
    browse = 0;
    kit = 0;
    menu = 0;
    preset = 0;
    cfg = 0;
    cfg_group = CFG_GROUP_TOP;
    cfg_value = 0;
    cfg_undo = 0;
    assign_undo = -1;
    // Not started here: only ui_show_splash() turns it on, so a test bench
    // that never calls it sees the ordinary select screen from frame one.
    splash_active = false;
    for (int i = 0; i < NUM_SLOTS; i++) {
        kit_undo[i] = -1;
        preset_undo[i] = -1;
    }
    msg = NULL;
    msg_frames = 0;
    last_tick_ms = 0;
    tick_started = false;
}

void ui_show_splash(uint32_t now_ms) {
    splash_active = true;
    splash_start_ms = now_ms;
}

UiMode   ui_mode(void)          { return mode; }
uint8_t  ui_slot(void)          { return slot; }
uint32_t ui_browse_index(void)  { return browse; }
uint32_t ui_kit_index(void)     { return kit; }
uint32_t ui_menu_index(void)    { return menu; }
uint32_t ui_preset_index(void)  { return preset; }
uint32_t ui_config_index(void)  { return cfg_list_pos(); }
uint32_t ui_config_depth(void)  { return cfg_group == CFG_GROUP_TOP ? 0u : 1u; }

void ui_note_name(uint8_t note, char *out) {
    static const char *const NAMES[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                          "F#", "G",  "G#", "A",  "A#", "B"};
    // Yamaha convention: note 36 is C1, the same one the General MIDI
    // percussion map the mixer assigns to the slots is read with.
    int octave = (int)(note / 12) - 2;
    snprintf(out, 5, "%s%d", NAMES[note % 12], octave);
}

// --- the settings list --------------------------------------------------------
// Every entry is an integer with a range and a step, which is what keeps the
// editing code to one function instead of one per setting.
//
// Two levels since phase4. The top list holds the six global settings,
// RESTORE, and two groups, NOTES and PAN, each a sublist of eight per-slot
// entries. Flat, the page would have been 23 entries long with sixteen of
// them the same eight roles twice over; grouped, the top list is nine and
// every sublist reads as one question asked of each slot. It costs no new
// gesture: a press on a group opens it, as a press on an entry edits it, and
// a long press inside a group goes back up one level instead of leaving.

enum {
    CFG_MIDI_CH = 0,
    CFG_GAIN,
    CFG_VELOCITY,
    CFG_DYNAMICS,
    CFG_CONTRAST,
    CFG_BOOT,
    CFG_NOTE0,
    CFG_PAN0 = CFG_NOTE0 + NUM_SLOTS,
    CFG_RESTORE = CFG_PAN0 + NUM_SLOTS,
    // Not settings: the two group rows of the top list. A press opens them.
    CFG_OPEN_NOTES,
    CFG_OPEN_PAN,
    CFG_COUNT,
};

static const uint8_t CFG_TOP[] = {
    CFG_MIDI_CH, CFG_GAIN, CFG_VELOCITY, CFG_DYNAMICS, CFG_CONTRAST, CFG_BOOT,
    CFG_OPEN_NOTES, CFG_OPEN_PAN, CFG_RESTORE,
};
#define CFG_TOP_LEN ((uint32_t)(sizeof CFG_TOP / sizeof CFG_TOP[0]))

static bool cfg_is_note(uint32_t e) { return e >= CFG_NOTE0 && e < CFG_NOTE0 + NUM_SLOTS; }
static bool cfg_is_pan(uint32_t e)  { return e >= CFG_PAN0 && e < CFG_PAN0 + NUM_SLOTS; }
static bool cfg_is_group(uint32_t e) { return e == CFG_OPEN_NOTES || e == CFG_OPEN_PAN; }

static uint32_t cfg_list_len(void) {
    return cfg_group == CFG_GROUP_TOP ? CFG_TOP_LEN : NUM_SLOTS;
}

static uint32_t cfg_list_entry(uint32_t i) {
    switch (cfg_group) {
        case CFG_GROUP_NOTES: return CFG_NOTE0 + i;
        case CFG_GROUP_PAN:   return CFG_PAN0 + i;
        default:              return CFG_TOP[i];
    }
}

// Where the entry under the cursor sits in the list it belongs to.
static uint32_t cfg_list_pos(void) {
    if (cfg_is_note(cfg)) {
        return cfg - CFG_NOTE0;
    }
    if (cfg_is_pan(cfg)) {
        return cfg - CFG_PAN0;
    }
    for (uint32_t i = 0; i < CFG_TOP_LEN; i++) {
        if (CFG_TOP[i] == cfg) {
            return i;
        }
    }
    return 0;
}

#define CFG_MIDI_CH_ANY 16  // one past channel 16, so "any" is just the last value

// SETTINGS_DYNAMICS_MID is stored as 0 so an old record reads as the ratio it
// already had (see settings.h), but on the knob OFF/LOW/MID/HIGH is the order
// that reads as a scale. These two tables are the only place that ordering is
// decided; everything else turns a position 0..3 into a stored value or back.
static const uint8_t DYNAMICS_POS_TO_STORED[4] = {
    SETTINGS_DYNAMICS_OFF, SETTINGS_DYNAMICS_LOW,
    SETTINGS_DYNAMICS_MID, SETTINGS_DYNAMICS_HIGH,
};
static const uint8_t DYNAMICS_STORED_TO_POS[4] = {
    2, 0, 1, 3,  // index by SETTINGS_DYNAMICS_{MID,OFF,LOW,HIGH}
};

static void cfg_range(uint32_t e, int32_t *lo, int32_t *hi, int32_t *step) {
    *step = 1;
    switch (e) {
        case CFG_MIDI_CH:  *lo = 0; *hi = CFG_MIDI_CH_ANY; break;
        case CFG_GAIN:
            *lo = SETTINGS_GAIN_MIN;
            *hi = SETTINGS_GAIN_MAX;
            *step = 128;  // 32 steps across the range: a knob turn, not a marathon
            break;
        case CFG_VELOCITY: *lo = 0; *hi = 127; *step = 8; break;
        case CFG_DYNAMICS: *lo = 0; *hi = 3; break;  // position, OFF/LOW/MID/HIGH on the knob
        case CFG_CONTRAST: *lo = 0; *hi = 255; *step = 16; break;
        case CFG_BOOT:     *lo = 0; *hi = NUM_PRESETS; break;
        case CFG_RESTORE:  *lo = 0; *hi = 1; break;
        default:
            if (cfg_is_pan(e)) {
                *lo = MIXER_PAN_LEFT;
                *hi = MIXER_PAN_RIGHT;
            } else {
                *lo = 0;
                *hi = 127;  // the note entries
            }
            break;
    }
}

static int32_t cfg_get(uint32_t e) {
    const SettingsRecord *r = settings_get();
    switch (e) {
        case CFG_MIDI_CH:
            return r->midi_channel == SETTINGS_MIDI_CHANNEL_ANY
                       ? CFG_MIDI_CH_ANY
                       : r->midi_channel;
        case CFG_GAIN:     return r->master_gain;
        case CFG_VELOCITY: return r->velocity_fixed;
        case CFG_DYNAMICS:
            return r->dynamics < 4 ? DYNAMICS_STORED_TO_POS[r->dynamics]
                                   : DYNAMICS_STORED_TO_POS[SETTINGS_DYNAMICS_MID];
        case CFG_CONTRAST: return r->oled_contrast;
        case CFG_BOOT:
            return r->boot_preset == PRESET_NONE ? 0 : r->boot_preset + 1;
        case CFG_RESTORE:  return 0;
        default:
            if (cfg_is_pan(e)) {
                return r->slot_pan[e - CFG_PAN0];
            }
            if (cfg_is_note(e)) {
                return r->slot_note[e - CFG_NOTE0];
            }
            return 0;  // a group row has no value
    }
}

static void cfg_set(uint32_t e, int32_t v) {
    SettingsRecord *r = settings_get();
    switch (e) {
        case CFG_MIDI_CH:
            r->midi_channel = (v == CFG_MIDI_CH_ANY)
                                  ? SETTINGS_MIDI_CHANNEL_ANY
                                  : (uint8_t)v;
            break;
        case CFG_GAIN:     r->master_gain = (uint16_t)v; break;
        case CFG_VELOCITY: r->velocity_fixed = (uint8_t)v; break;
        case CFG_DYNAMICS: r->dynamics = DYNAMICS_POS_TO_STORED[v]; break;
        case CFG_CONTRAST: r->oled_contrast = (uint8_t)v; break;
        case CFG_BOOT:
            r->boot_preset = (v == 0) ? PRESET_NONE : (uint8_t)(v - 1);
            break;
        case CFG_RESTORE:  break;  // acted on when confirmed, not while turning
        default:
            if (cfg_is_pan(e)) {
                r->slot_pan[e - CFG_PAN0] = (int8_t)v;
            } else if (cfg_is_note(e)) {
                r->slot_note[e - CFG_NOTE0] = (uint8_t)v;
            }
            break;
    }
}

static const char *cfg_label(uint32_t e) {
    static char buf[16];
    switch (e) {
        case CFG_MIDI_CH:  return "MIDI CH";
        case CFG_GAIN:     return "GAIN";
        case CFG_VELOCITY: return "VELOCITY";
        case CFG_DYNAMICS: return "DYNAMICS";
        case CFG_CONTRAST: return "CONTRAST";
        case CFG_BOOT:     return "BOOT";
        case CFG_RESTORE:  return "RESTORE DEFAULTS";
        case CFG_OPEN_NOTES: return "NOTES >";
        case CFG_OPEN_PAN:   return "PAN >";
        default:
            // Inside a group the title bar already says NOTES or PAN, so the
            // role alone is the whole label.
            snprintf(buf, sizeof buf, "%s",
                     mixer_role_name((uint8_t)(cfg_is_pan(e) ? e - CFG_PAN0
                                                             : e - CFG_NOTE0)));
            return buf;
    }
}

static const char *cfg_text(uint32_t e, int32_t v) {
    static char buf[20];
    switch (e) {
        case CFG_MIDI_CH:
            if (v == CFG_MIDI_CH_ANY) {
                return "ANY";
            }
            snprintf(buf, sizeof buf, "%d", (int)v + 1);
            return buf;
        case CFG_GAIN:
            // Percent of full scale, not dB: a log would pull in libm for one
            // string, and what the setting buys is headroom, which reads
            // perfectly well as a fraction.
            snprintf(buf, sizeof buf, "%d%%",
                     (int)((v * 100) / SETTINGS_GAIN_UNITY));
            return buf;
        case CFG_VELOCITY:
            if (v == SETTINGS_VELOCITY_FOLLOW) {
                return "FOLLOW";
            }
            snprintf(buf, sizeof buf, "FIXED %d", (int)v);
            return buf;
        case CFG_DYNAMICS: {
            static const char *const NAMES[4] = {"OFF", "LOW", "MID", "HIGH"};
            return (v >= 0 && v < 4) ? NAMES[v] : "?";
        }
        case CFG_CONTRAST:
            snprintf(buf, sizeof buf, "%d", (int)v);
            return buf;
        case CFG_BOOT:
            if (v == 0) {
                return "OFF (KIT 1)";
            }
            snprintf(buf, sizeof buf, "PRESET %d", (int)v);
            return buf;
        case CFG_RESTORE:
            return v ? "YES" : "NO";
        case CFG_OPEN_NOTES:
        case CFG_OPEN_PAN:
            return "";
        default: {
            if (cfg_is_pan(e)) {
                // C in the middle, L1..L8 and R1..R8 either side: a position
                // on a knob, not a percentage of anything.
                if (v == 0) {
                    return "C";
                }
                snprintf(buf, sizeof buf, "%c%d", v < 0 ? 'L' : 'R',
                         (int)(v < 0 ? -v : v));
                return buf;
            }
            char note[5];
            ui_note_name((uint8_t)v, note);
            snprintf(buf, sizeof buf, "%s (%d)", note, (int)v);
            return buf;
        }
    }
}

// --- presets ------------------------------------------------------------------

// What a stored preset is called on the display: the kit it holds when its
// eight indices happen to be a whole kit, "MIX" when they do not. Nothing is
// stored for it — the name is derived, so it cannot go stale against the
// library the way a saved string would.
static const char *preset_label(uint32_t p) {
    const SettingsRecord *r = settings_get();
    if (p >= NUM_PRESETS || !r->preset[p].used) {
        return "<empty>";
    }
    int32_t k = mixer_kit_of(r->preset[p].lib_index);
    return k >= 0 ? mixer_kit_name((uint32_t)k) : "MIX";
}

static bool preset_apply(uint32_t p) {
    const SettingsRecord *r = settings_get();
    if (p >= NUM_PRESETS || !r->preset[p].used) {
        return false;
    }
    int32_t count = (int32_t)mixer_library_count();
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        int32_t idx = r->preset[p].lib_index[sl];
        mixer_assign_slot(sl, (idx >= 0 && idx < count) ? idx : -1);
    }
    return true;
}

static void preset_capture(uint32_t p) {
    SettingsRecord *r = settings_get();
    r->preset[p].used = 1;
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        r->preset[p].lib_index[sl] = mixer_slot_index(sl);
    }
}

static void save_to_flash(void) {
    if (hooks == NULL || hooks->save == NULL) {
        set_msg("NO FLASH");
        return;
    }
    set_msg(hooks->save() ? "SAVED" : "SAVE FAILED");
}

static void apply_settings(void) {
    if (hooks != NULL && hooks->apply != NULL) {
        hooks->apply();
    }
}

// --- events ------------------------------------------------------------------

static void step_slot(int delta) {
    // Full circle: wrapping saves half a knob turn compared to stopping at the
    // ends, and there is no ambiguity about where you are.
    slot = (uint8_t)(((int)slot + delta + NUM_SLOTS) % NUM_SLOTS);
}

static void step_browse(int delta) {
    uint32_t count = mixer_library_count();
    if (count == 0) {
        return;
    }
    browse = (uint32_t)(((int64_t)browse + delta + count) % count);
    // Assigned as it is browsed, like a kit: you pick a sample by ear.
    mixer_assign_slot(slot, (int32_t)browse);
}

static void step_kit(int delta) {
    uint32_t count = mixer_kit_count();
    if (count == 0) {
        return;
    }
    kit = (uint32_t)(((int64_t)kit + delta + count) % count);
    // Loaded as it is browsed: a kit is a character, and you pick it by ear.
    mixer_load_kit(kit);
}

static void step_preset(int delta) {
    preset = (uint32_t)(((int)preset + delta + NUM_PRESETS) % NUM_PRESETS);
    // Auditioned like a kit: you pick a preset by ear, not by its number. An
    // empty one leaves the slots alone rather than emptying them, so scrolling
    // past a gap is not a silence you have to undo.
    preset_apply(preset);
}

static void step_menu(int delta) {
    menu = (uint32_t)(((int)menu + delta + UI_MENU_COUNT) % UI_MENU_COUNT);
}

static void step_cfg(int delta) {
    uint32_t len = cfg_list_len();
    uint32_t pos = (uint32_t)(((int)cfg_list_pos() + delta + (int)len) % (int)len);
    cfg = cfg_list_entry(pos);
}

static void step_cfg_value(int delta) {
    int32_t lo, hi, step;
    cfg_range(cfg, &lo, &hi, &step);
    cfg_value += delta * step;
    // Clamped, not wrapped: on a value you are aiming at, running off the end
    // and reappearing at the other one is a way to overshoot twice.
    if (cfg_value < lo) {
        cfg_value = lo;
    }
    if (cfg_value > hi) {
        cfg_value = hi;
    }
    if (cfg == CFG_RESTORE) {
        return;  // NO/YES: an answer, not a setting. Acted on when confirmed.
    }
    cfg_set(cfg, cfg_value);
    // Applied while turning: gain, contrast and the note map are all things
    // you judge by the result, not by the number.
    apply_settings();
}

// Opening a menu entry. Returns the mode to land in: an entry that has
// nothing to show refuses and leaves the menu up, rather than opening an empty
// browser whose only exit is a long press.
static UiMode enter_menu_entry(void) {
    switch (menu) {
        case UI_MENU_KIT: {
            if (mixer_kit_count() == 0) {
                set_msg("NO KITS");
                return UI_MENU;
            }
            for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                kit_undo[i] = mixer_slot_index(i);
            }
            // Start from the kit already loaded, when the slots still form
            // one: browsing a kit list should begin where you are.
            int32_t cur = mixer_current_kit();
            kit = cur >= 0 ? (uint32_t)cur : 0;
            mixer_load_kit(kit);
            return UI_KIT;
        }
        case UI_MENU_LOAD: {
            const SettingsRecord *r = settings_get();
            bool any = false;
            for (uint8_t i = 0; i < NUM_PRESETS; i++) {
                any = any || r->preset[i].used;
            }
            if (!any) {
                set_msg("NO PRESETS");
                return UI_MENU;
            }
            for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                preset_undo[i] = mixer_slot_index(i);
            }
            preset = 0;
            preset_apply(preset);
            return UI_PRESET;
        }
        case UI_MENU_SAVE:
            preset = 0;
            return UI_SAVE;
        case UI_MENU_CONFIG:
            cfg_group = CFG_GROUP_TOP;
            cfg = CFG_TOP[0];
            return UI_CONFIG;
        case UI_MENU_ABOUT:
            return UI_ABOUT;
        default:
            return UI_MENU;
    }
}

void ui_event(EncoderEvent ev) {
    // Any input skips the splash rather than fighting it: the module is
    // already fully playable underneath (mode is still UI_SELECT), so the
    // first turn or press just ends the splash instead of also changing a
    // slot from under it.
    if (splash_active) {
        splash_active = false;
        return;
    }

    switch (ev) {
        case ENC_CW:
        case ENC_CCW: {
            int delta = (ev == ENC_CW) ? +1 : -1;
            switch (mode) {
                case UI_SELECT:      step_slot(delta); break;
                case UI_ASSIGN:      step_browse(delta); break;
                case UI_KIT:         step_kit(delta); break;
                case UI_MENU:        step_menu(delta); break;
                case UI_PRESET:      step_preset(delta); break;
                case UI_SAVE:
                    preset = (uint32_t)(((int)preset + delta + NUM_PRESETS) %
                                        NUM_PRESETS);
                    break;
                case UI_CONFIG:      step_cfg(delta); break;
                case UI_CONFIG_EDIT: step_cfg_value(delta); break;
                case UI_ABOUT:       break;  // nothing to browse
            }
            break;
        }

        case ENC_PRESS:
            switch (mode) {
                case UI_SELECT: {
                    // With no library there is nothing to browse: entering
                    // assign would show an empty list and a press that confirms
                    // nothing, i.e. a dead end.
                    if (mixer_library_count() == 0) {
                        break;
                    }
                    // Browsing starts from the sample already assigned, not
                    // from zero: in the vast majority of cases you want a small
                    // change.
                    int32_t cur = mixer_slot_index(slot);
                    assign_undo = cur;
                    browse = cur >= 0 ? (uint32_t)cur : 0;
                    mode = UI_ASSIGN;
                    break;
                }
                case UI_ASSIGN:
                    // Browsing has already assigned it: confirming is just
                    // leaving. Assigned again anyway, so a press with no turn
                    // on an empty slot still takes the entry on screen.
                    mixer_assign_slot(slot, (int32_t)browse);
                    mode = UI_SELECT;
                    break;
                case UI_KIT:
                    // Kit mode has already loaded it: confirming is just
                    // leaving.
                    mode = UI_SELECT;
                    break;
                case UI_MENU:
                    mode = enter_menu_entry();
                    break;
                case UI_PRESET:
                    if (!settings_get()->preset[preset].used) {
                        set_msg("EMPTY");
                        break;
                    }
                    mode = UI_SELECT;  // already loaded while browsing
                    break;
                case UI_SAVE:
                    preset_capture(preset);
                    save_to_flash();
                    mode = UI_SELECT;
                    break;
                case UI_CONFIG:
                    if (cfg_is_group(cfg)) {
                        // Opening a group, not editing: the cursor lands on
                        // its first slot and the mode stays UI_CONFIG.
                        cfg_group = (cfg == CFG_OPEN_NOTES) ? CFG_GROUP_NOTES
                                                            : CFG_GROUP_PAN;
                        cfg = cfg_list_entry(0);
                        break;
                    }
                    cfg_undo = cfg_value = cfg_get(cfg);
                    mode = UI_CONFIG_EDIT;
                    break;
                case UI_CONFIG_EDIT:
                    // Restore is the one entry whose value is an answer rather
                    // than a setting: it takes two deliberate gestures, turn to
                    // YES and press, because it throws away every preset.
                    if (cfg == CFG_RESTORE && cfg_value == 1) {
                        settings_defaults(settings_get());
                        apply_settings();
                        set_msg("RESTORED");
                    }
                    mode = UI_CONFIG;
                    break;
                case UI_ABOUT:
                    mode = UI_SELECT;  // nothing to confirm, just leave
                    break;
            }
            break;

        case ENC_LONG_PRESS:
            switch (mode) {
                case UI_SELECT:
                    menu = 0;
                    mode = UI_MENU;
                    break;
                case UI_MENU:
                    mode = UI_SELECT;
                    break;
                case UI_ASSIGN:
                    mixer_assign_slot(slot, assign_undo);  // cancel: old sample back
                    mode = UI_SELECT;
                    break;
                case UI_KIT:
                    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                        mixer_assign_slot(i, kit_undo[i]);
                    }
                    mode = UI_SELECT;
                    break;
                case UI_PRESET:
                    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                        mixer_assign_slot(i, preset_undo[i]);
                    }
                    mode = UI_SELECT;
                    break;
                case UI_SAVE:
                    mode = UI_SELECT;  // nothing was written
                    break;
                case UI_CONFIG:
                    if (cfg_group != CFG_GROUP_TOP) {
                        // Out of a group, one level up, the cursor back on the
                        // group's own row. Nothing is saved yet: that happens
                        // on leaving the page, once, however many groups were
                        // visited on the way.
                        cfg = (cfg_group == CFG_GROUP_NOTES) ? CFG_OPEN_NOTES
                                                             : CFG_OPEN_PAN;
                        cfg_group = CFG_GROUP_TOP;
                        break;
                    }
                    // Leaving the settings page writes them. A config page
                    // whose changes evaporate on the way out is worse than an
                    // erase you did not ask for, and the erase only happens
                    // when something actually changed.
                    if (settings_dirty()) {
                        save_to_flash();
                    }
                    mode = UI_SELECT;
                    break;
                case UI_CONFIG_EDIT:
                    if (cfg != CFG_RESTORE) {
                        cfg_set(cfg, cfg_undo);
                        apply_settings();
                    }
                    mode = UI_CONFIG;
                    break;
                case UI_ABOUT:
                    mode = UI_SELECT;
                    break;
            }
            break;

        case ENC_NONE:
        default:
            break;
    }
}

// --- drawing -----------------------------------------------------------------

static int cell_x(int i) { return CELL_X0 + i * CELL_STEP; }

// `invert` is a bitmask of the cells to draw in reverse video: one cell in
// select, one blinking cell in assign, all of them in kit — where it is the
// whole kit changing, not a slot.
static void draw_strip(uint32_t invert) {
    uint32_t active = mixer_active_slot_mask();

    for (int i = 0; i < NUM_SLOTS; i++) {
        int x = cell_x(i);
        char digit[2] = {(char)('1' + i), '\0'};
        bool sel = (invert & (1u << i)) != 0;

        if (sel) {
            ssd1306_fill(x, ROW_STRIP, CELL_W, 8, true);
        }
        // Inside a filled cell the text has to be drawn in reverse video,
        // otherwise it disappears into the white.
        ssd1306_text(x + CELL_DIGIT_DX, ROW_STRIP, digit, !sel);

        if (active & (1u << i)) {
            ssd1306_fill(x + BAR_DX, BAR_Y, BAR_W, BAR_H, true);
        }
    }
}

static void draw_right(int y, const char *s) {
    ssd1306_text(OLED_W - 1 - ssd1306_text_w(s), y, s, true);
}

// The right-hand hint, or the pending message when there is one: a press that
// refused to do anything has to say so somewhere, and that is the only corner
// of the layout not already carrying information.
static void draw_hint(const char *hint) {
    draw_right(ROW_FOOT, msg_frames > 0 && msg != NULL ? msg : hint);
}

static bool blinking(uint32_t now_ms) {
    return ((now_ms / BLINK_HALF_MS) & 1u) == 0;
}

#if OLED_H == 64
// The three-line list every browsing mode below is built from: the entry
// before and after the current one, plain, above and below a full-width
// inverted bar for the current entry — the SSD1306 is 1-bit, there is no
// grayscale to dim the neighbours with, so the current entry is made to pop
// instead, the same device draw_strip() already uses for the selected slot.
// What actually uses the eight rows instead of stretching the four-row
// layout into them.
static void draw_context_list(const char *prev, const char *cur, const char *next) {
    ssd1306_text_trunc(1, ROW_PREV, prev, NAME_MAX_SELECT, true);
    ssd1306_fill(0, ROW_CUR, OLED_W, 8, true);
    ssd1306_text_trunc(1, ROW_CUR, cur, NAME_MAX_SELECT, false);
    ssd1306_text_trunc(1, ROW_NEXT, next, NAME_MAX_SELECT, true);
}
#endif

static void render_select(void) {
    draw_strip(1u << slot);

#if OLED_H == 64
    // What the next slot over holds is new information here: on the 128x32
    // build you have to turn the knob to find out.
    uint8_t prev_slot = (uint8_t)((slot + NUM_SLOTS - 1) % NUM_SLOTS);
    uint8_t next_slot = (uint8_t)((slot + 1) % NUM_SLOTS);
    draw_context_list(mixer_slot_name(prev_slot), mixer_slot_name(slot),
                       mixer_slot_name(next_slot));

    char info[16], note[5];
    ui_note_name(mixer_slot_note(slot), note);
    snprintf(info, sizeof info, "%s %s", note, mixer_role_name(slot));
    ssd1306_text(1, ROW_INFO, info, true);
#else
    ssd1306_text_trunc(1, ROW_NAME, mixer_slot_name(slot), NAME_MAX_SELECT, true);

    // Note and role together: on the two free slots the GM note alone says
    // nothing about what is loaded, and "FLEX1" is the label the kit list and
    // this document both use.
    char foot[16], note[5];
    ui_note_name(mixer_slot_note(slot), note);
    snprintf(foot, sizeof foot, "%s %s", note, mixer_role_name(slot));
    ssd1306_text(1, ROW_FOOT, foot, true);
#endif

    draw_hint(mixer_library_count() ? "assign>" : "no lib");
}

static void render_assign(uint32_t now_ms) {
    // The cell blinks only in assign: that is the cue telling the two modes
    // apart at a glance, without having to read the bottom row.
    bool blink_on = ((now_ms / BLINK_HALF_MS) & 1u) == 0;
    draw_strip(blink_on ? (1u << slot) : 0u);

    uint32_t count = mixer_library_count();
    char pos[12];
    snprintf(pos, sizeof pos, "%03u/%03u", (unsigned)(browse + 1), (unsigned)count);

#if OLED_H == 64
    uint32_t prev_i = (browse + count - 1) % count;
    uint32_t next_i = (browse + 1) % count;
    draw_context_list(mixer_library_name(prev_i), mixer_library_name(browse),
                       mixer_library_name(next_i));
    ssd1306_text(1, ROW_INFO, pos, true);
#else
    ssd1306_text(1, ROW_NAME, ">", true);
    ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_NAME, mixer_library_name(browse),
                       NAME_MAX_ASSIGN, true);
    ssd1306_text(1, ROW_FOOT, pos, true);
#endif

    draw_right(ROW_FOOT, "press=ok");
}

// The whole strip blinks: what changes here is every slot at once, and one
// blinking cell would read as "slot 3 is being edited".
static void render_kit(uint32_t now_ms) {
    bool blink_on = ((now_ms / BLINK_HALF_MS) & 1u) == 0;
    draw_strip(blink_on ? ((1u << NUM_SLOTS) - 1u) : 0u);

    uint32_t count = mixer_kit_count();
    char pos[16];
    snprintf(pos, sizeof pos, "KIT %02u/%02u", (unsigned)(kit + 1), (unsigned)count);

#if OLED_H == 64
    uint32_t prev_i = (kit + count - 1) % count;
    uint32_t next_i = (kit + 1) % count;
    draw_context_list(mixer_kit_name(prev_i), mixer_kit_name(kit),
                       mixer_kit_name(next_i));
    ssd1306_text(1, ROW_INFO, pos, true);
#else
    ssd1306_text(1, ROW_NAME, ">", true);
    ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_NAME, mixer_kit_name(kit),
                       NAME_MAX_ASSIGN, true);
    ssd1306_text(1, ROW_FOOT, pos, true);
#endif

    draw_right(ROW_FOOT, "press=ok");
}

// The strip stays on top so the slot being worked on never leaves the screen.
static void render_menu(void) {
    static const char *const ENTRIES[UI_MENU_COUNT] = {
        "KIT", "LOAD PRESET", "SAVE PRESET", "CONFIG", "ABOUT",
    };
    draw_strip(1u << slot);

    char pos[16];
    snprintf(pos, sizeof pos, "MENU %u/%u", (unsigned)(menu + 1),
             (unsigned)UI_MENU_COUNT);

#if OLED_H == 64
    uint32_t prev_i = (menu + UI_MENU_COUNT - 1) % UI_MENU_COUNT;
    uint32_t next_i = (menu + 1) % UI_MENU_COUNT;
    draw_context_list(ENTRIES[prev_i], ENTRIES[menu], ENTRIES[next_i]);
    ssd1306_text(1, ROW_INFO, pos, true);
#else
    ssd1306_text(1, ROW_NAME, ">", true);
    ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_NAME, ENTRIES[menu],
                       NAME_MAX_ASSIGN, true);
    ssd1306_text(1, ROW_FOOT, pos, true);
#endif

    draw_hint("press=ok");
}

// Like kit mode, and for the same reason: every slot is about to change, so
// the whole strip blinks rather than one cell.
static void render_preset(uint32_t now_ms) {
    draw_strip(blinking(now_ms) ? ((1u << NUM_SLOTS) - 1u) : 0u);

    char pos[16];
    snprintf(pos, sizeof pos, "PRESET %u/%u", (unsigned)(preset + 1),
             (unsigned)NUM_PRESETS);

#if OLED_H == 64
    uint32_t prev_i = (preset + NUM_PRESETS - 1) % NUM_PRESETS;
    uint32_t next_i = (preset + 1) % NUM_PRESETS;
    draw_context_list(preset_label(prev_i), preset_label(preset),
                       preset_label(next_i));
    ssd1306_text(1, ROW_INFO, pos, true);
#else
    ssd1306_text(1, ROW_NAME, ">", true);
    ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_NAME, preset_label(preset),
                       NAME_MAX_ASSIGN, true);
    ssd1306_text(1, ROW_FOOT, pos, true);
#endif

    draw_hint("press=ok");
}

static void render_save(uint32_t now_ms) {
    draw_strip(blinking(now_ms) ? ((1u << NUM_SLOTS) - 1u) : 0u);

#if OLED_H == 64
    // The two presets either side of the write target, so an overwrite is
    // never a surprise about what was next to it either.
    uint32_t prev_i = (preset + NUM_PRESETS - 1) % NUM_PRESETS;
    uint32_t next_i = (preset + 1) % NUM_PRESETS;
    char prev_line[24], cur_line[24], next_line[24];
    snprintf(prev_line, sizeof prev_line, "%u %s", (unsigned)(prev_i + 1),
             preset_label(prev_i));
    snprintf(cur_line, sizeof cur_line, "%u %s", (unsigned)(preset + 1),
             preset_label(preset));
    snprintf(next_line, sizeof next_line, "%u %s", (unsigned)(next_i + 1),
             preset_label(next_i));
    draw_context_list(prev_line, cur_line, next_line);
    ssd1306_text(1, ROW_INFO, "SAVE TO", true);
#else
    // What is in the target today, so an overwrite is never blind.
    char line[24];
    snprintf(line, sizeof line, ">%u %s", (unsigned)(preset + 1),
             preset_label(preset));
    ssd1306_text_trunc(1, ROW_NAME, line, NAME_MAX_SELECT, true);

    ssd1306_text(1, ROW_FOOT, "SAVE TO", true);
#endif

    draw_hint("press=ok");
}

static const char *cfg_title(void) {
    switch (cfg_group) {
        case CFG_GROUP_NOTES: return "NOTES";
        case CFG_GROUP_PAN:   return "PAN";
        default:              return "CONFIG";
    }
}

static const char *cfg_hint(void) {
    if (mode == UI_CONFIG_EDIT) {
        return "press=ok";
    }
    return cfg_is_group(cfg) ? "press=open" : "press=edit";
}

static void render_config(uint32_t now_ms) {
    // A title bar instead of the slot strip: in the settings page the slots
    // are not what is being edited, and the position in the list is what the
    // top row has to carry. Inside a group the title names the group, so the
    // entries can be just the slot roles.
    uint32_t len = cfg_list_len();
    uint32_t pos_i = cfg_list_pos();
    char pos[12];
    snprintf(pos, sizeof pos, "%u/%u", (unsigned)(pos_i + 1), (unsigned)len);

#if OLED_H == 64
    ssd1306_fill(0, ROW_CFG_TITLE, OLED_W, 8, true);
    ssd1306_text(1, ROW_CFG_TITLE, cfg_title(), false);
    ssd1306_text(OLED_W - 1 - ssd1306_text_w(pos), ROW_CFG_TITLE, pos, false);

    // The entry before and after the one being edited, label only: showing
    // their values too would crowd a row that already carries the highlight.
    // The current entry gets the same inverted bar as the browsing modes'
    // draw_context_list(); which of UI_CONFIG/UI_CONFIG_EDIT it is stays
    // signalled by the value row blinking below, not by this row.
    uint32_t prev_e = cfg_list_entry((pos_i + len - 1) % len);
    uint32_t next_e = cfg_list_entry((pos_i + 1) % len);
    ssd1306_text_trunc(1, ROW_CFG_PREV, cfg_label(prev_e), NAME_MAX_SELECT, true);
    ssd1306_fill(0, ROW_CFG_CUR, OLED_W, 8, true);
    ssd1306_text_trunc(1, ROW_CFG_CUR, cfg_label(cfg), NAME_MAX_SELECT, false);
    ssd1306_text_trunc(1, ROW_CFG_NEXT, cfg_label(next_e), NAME_MAX_SELECT, true);

    // The value gets its own row, rather than sharing the label's: editing
    // blinks only this line, not the whole entry.
    bool show = mode == UI_CONFIG || blinking(now_ms);
    if (show) {
        int32_t v = (mode == UI_CONFIG_EDIT) ? cfg_value : cfg_get(cfg);
        ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_CFG_VALUE, cfg_text(cfg, v),
                           NAME_MAX_ASSIGN, true);
    }
#else
    ssd1306_fill(0, ROW_STRIP, OLED_W, 8, true);
    ssd1306_text(1, ROW_STRIP, cfg_title(), false);
    ssd1306_text(OLED_W - 1 - ssd1306_text_w(pos), ROW_STRIP, pos, false);

    ssd1306_text(1, ROW_NAME, mode == UI_CONFIG ? ">" : " ", true);
    ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_NAME, cfg_label(cfg),
                       NAME_MAX_ASSIGN, true);

    // Editing blinks the value, not the label: it is the value that is live
    // under the knob.
    bool show = mode == UI_CONFIG || blinking(now_ms);
    if (show) {
        int32_t v = (mode == UI_CONFIG_EDIT) ? cfg_value : cfg_get(cfg);
        ssd1306_text_trunc(1 + OLED_CHAR_ADV, ROW_FOOT, cfg_text(cfg, v),
                           NAME_MAX_ASSIGN, true);
    }
#endif
    draw_hint(cfg_hint());
}

#define SPLASH_TITLE     "muRDrum"
#define SPLASH_TITLE_LEN 7  // strlen(SPLASH_TITLE), kept honest by the assert below
#define SPLASH_GAP       6  // between the logo and the product name
#define SPLASH_ROW_GAP   3  // between the top row and the copyright line
#define SPLASH_GLYPH_H   7  // font5x7's glyph height, see ssd1306.c

#define SPLASH_TEXT_W(len) ((len) * OLED_CHAR_ADV - (OLED_CHAR_ADV - OLED_CHAR_W))
#define SPLASH_TITLE_W_1X SPLASH_TEXT_W(SPLASH_TITLE_LEN)

_Static_assert(sizeof(SPLASH_TITLE) - 1 == SPLASH_TITLE_LEN,
               "SPLASH_TITLE_LEN out of sync with SPLASH_TITLE");

// The copyright row is a pre-rendered bitmap on 128x32: it has to read
// distinctly smaller than font5x7's own 5x7, already the smallest text
// drawn anywhere else in the UI, and a second font table would exist only
// for this one fixed string - see tools/make_logo.py. 128x64 still draws it
// with font5x7, not asked to match yet.
//
// 128x32 also gets "muRDrum" at 2x: the logo is already close to the
// height this panel can spare it (see the row-height assert below), so the
// width left over next to it - a near-square logo cannot grow into it
// without also growing taller - is spent on the title's weight instead.
// 128x64 keeps the title at 1x, not asked to match.
#if OLED_H == 32
#define SPLASH_ROW_B_W COPYRIGHT_W
#define SPLASH_ROW_B_H COPYRIGHT_H
#define SPLASH_TITLE_W (2 * SPLASH_TITLE_W_1X)
#define SPLASH_TITLE_H (SPLASH_GLYPH_H * 2)
#else
#define SPLASH_COPYRIGHT     "(c) muR Lab 2026"
#define SPLASH_COPYRIGHT_LEN 16  // strlen(SPLASH_COPYRIGHT)
_Static_assert(sizeof(SPLASH_COPYRIGHT) - 1 == SPLASH_COPYRIGHT_LEN,
               "SPLASH_COPYRIGHT_LEN out of sync with SPLASH_COPYRIGHT");
#define SPLASH_ROW_B_W SPLASH_TEXT_W(SPLASH_COPYRIGHT_LEN)
#define SPLASH_ROW_B_H SPLASH_GLYPH_H
#define SPLASH_TITLE_W SPLASH_TITLE_W_1X
#define SPLASH_TITLE_H SPLASH_GLYPH_H
#endif

// The top row's own height is whichever of the logo or the title is taller.
#define SPLASH_ROW_A_H (LOGO_H > SPLASH_TITLE_H ? LOGO_H : SPLASH_TITLE_H)

// Caught the hard way more than once already on this exact splash (a fixed
// margin left over from a smaller bitmap, or content sized without checking
// it against the panel at all): every dimension the layout depends on gets
// one of these, not just the one that broke last time.
_Static_assert(SPLASH_ROW_A_H + SPLASH_ROW_GAP + SPLASH_ROW_B_H <= OLED_H,
               "splash rows taller than the panel");
_Static_assert(LOGO_W + SPLASH_GAP + SPLASH_TITLE_W <= OLED_W,
               "splash logo + title wider than the panel");
_Static_assert(SPLASH_ROW_B_W <= OLED_W,
               "splash copyright wider than the panel");

// Two rows: [logo][gap]["muRDrum"] on top, the copyright right-aligned
// below it. The logo (one bitmap, the chip with the crescent/dot/"uR" cut
// into its own body exactly as authored - see tools/make_logo.py) and the
// title are both vertically centred within the top row's own band, so
// either one being the taller of the two still looks balanced.
static void render_splash(void) {
    int block_h = SPLASH_ROW_A_H + SPLASH_ROW_GAP + SPLASH_ROW_B_H;
    int y0      = (OLED_H - block_h) / 2;
    int row_w   = LOGO_W + SPLASH_GAP + SPLASH_TITLE_W;
    int x0      = (OLED_W - row_w) / 2;

    ssd1306_bitmap(x0, y0 + (SPLASH_ROW_A_H - LOGO_H) / 2, LOGO_W, LOGO_H, LOGO_BITS);
    int title_x = x0 + LOGO_W + SPLASH_GAP;
    int title_y = y0 + (SPLASH_ROW_A_H - SPLASH_TITLE_H) / 2;
#if OLED_H == 32
    ssd1306_text_2x(title_x, title_y, SPLASH_TITLE, true);
#else
    ssd1306_text(title_x, title_y, SPLASH_TITLE, true);
#endif

    int row_b_y = y0 + SPLASH_ROW_A_H + SPLASH_ROW_GAP;
#if OLED_H == 32
    ssd1306_bitmap(OLED_W - 1 - COPYRIGHT_W, row_b_y, COPYRIGHT_W, COPYRIGHT_H,
                  COPYRIGHT_BITS);
#else
    ssd1306_text(OLED_W - 1 - SPLASH_ROW_B_W, row_b_y, SPLASH_COPYRIGHT, true);
#endif
}

// Version, build and copyright: nothing here is derived from module state,
// unlike every other screen, so there is no cursor and either gesture just
// leaves. See src/version.h for where FIRMWARE_VERSION/FIRMWARE_BUILD come
// from.
static void render_about(void) {
    char line[24];
#if OLED_H == 64
    ssd1306_fill(0, ROW_STRIP, OLED_W, 8, true);
    ssd1306_text(1, ROW_STRIP, "ABOUT", false);

    snprintf(line, sizeof line, "VERSION %s", FIRMWARE_VERSION);
    ssd1306_text_trunc(1, 16, line, NAME_MAX_SELECT, true);
    snprintf(line, sizeof line, "BUILD %s", FIRMWARE_BUILD);
    ssd1306_text_trunc(1, 24, line, NAME_MAX_SELECT, true);
    ssd1306_text_trunc(1, 40, "(C) 2026 muR Lab", NAME_MAX_SELECT, true);
#else
    ssd1306_fill(0, ROW_STRIP, OLED_W, 8, true);
    ssd1306_text(1, ROW_STRIP, "ABOUT", false);

    snprintf(line, sizeof line, "V%s %s", FIRMWARE_VERSION, FIRMWARE_BUILD);
    ssd1306_text_trunc(1, ROW_BARS, line, NAME_MAX_SELECT, true);
    ssd1306_text_trunc(1, ROW_NAME, "(C) 2026 muR Lab", NAME_MAX_SELECT, true);
#endif
    draw_hint("back");
}

void ui_render(uint32_t now_ms) {
    ssd1306_clear();
    if (splash_active) {
        render_splash();
        return;
    }
    switch (mode) {
        case UI_SELECT:      render_select(); break;
        case UI_ASSIGN:      render_assign(now_ms); break;
        case UI_KIT:         render_kit(now_ms); break;
        case UI_MENU:        render_menu(); break;
        case UI_PRESET:      render_preset(now_ms); break;
        case UI_SAVE:        render_save(now_ms); break;
        case UI_CONFIG:
        case UI_CONFIG_EDIT: render_config(now_ms); break;
        case UI_ABOUT:       render_about(); break;
    }
}

void ui_tick(uint32_t now_ms) {
    if (tick_started && (uint32_t)(now_ms - last_tick_ms) < UI_PERIOD_MS) {
        return;
    }
    tick_started = true;
    last_tick_ms = now_ms;

    if (msg_frames > 0) {
        msg_frames--;
    }
    if (splash_active && (uint32_t)(now_ms - splash_start_ms) >= SPLASH_MS) {
        splash_active = false;
    }

    ui_render(now_ms);
    ssd1306_flush();
}
