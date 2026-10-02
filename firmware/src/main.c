// Phase 3: playback engine, user interface (encoder and OLED) and MIDI in.
//
// Core split:
//   core1  audio rendering in the DMA ISR, nothing else
//   core0  encoder, display, USB console, MIDI parsing
//
// MIDI is a parser and nothing else: bytes off UART1 go into midi_feed(), and
// a Note On comes out as mixer_trigger_note(). The queue to core1 is lock-free,
// so calling it from here is safe. The USB console stays alive alongside all of
// it: it is the diagnostic tool that isolated every fault so far, and keeping
// it costs nothing.
#include <math.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "hardware/gpio.h"

#include "hardware/uart.h"

#include "audio_i2s.h"
#include "encoder.h"
#include "midi.h"
#include "mixer.h"
#include "oled_bus.h"
#include "pins.h"
#include "sample_lib.h"
#include "settings.h"
#include "settings_flash.h"
#include "ssd1306.h"
#include "ui.h"

#define SINE_TABLE_LEN 256
#define SINE_HZ 440

static int16_t sine_table[SINE_TABLE_LEN];
static uint32_t sine_phase;

// The test tone is no longer the default: it was there to validate the DAC
// when no sound came out, and now that the chain is proven, booting into a
// fixed 440Hz would only be annoying. It stays available on the 's' command.
static volatile bool sine_mode;
static volatile bool library_ok;
static bool oled_ok;

// The display is an accessory, and it is also slower to wake than the MCU.
// On Eurorack power the +12V -> K7805 -> 5V rail ramps while the RP2350 is
// already running from VSYS, so the module can still be in its own power-on
// reset when the probe goes out and it never answers; on USB, VBUS is settled
// the moment the connector seats, which is why the same firmware and the same
// panel come up on one supply and not on the other. A single attempt at boot
// therefore leaves the screen dark until the next power cycle, on both panels
// - the init sequence is not what differs between them here, the timing is.
// Retrying costs one 1-byte probe with its own timeout while it is absent, and
// nothing at all once it has answered.
#define OLED_RETRY_MS 250u

static Midi midi;
// Notes that no slot owns, counted separately: on a full GM kit most of the
// stream is for instruments this module does not have, and that is normal, not
// a fault. Keeping it apart from the parser's own counters is what lets the
// console tell "nothing arrives" from "it arrives and it is not ours".
static uint32_t midi_unmapped;
static uint32_t midi_hits;

static Encoder enc;

// Q16 phase step that yields SINE_HZ out of the table.
#define SINE_STEP \
    ((uint32_t)(((uint64_t)SINE_TABLE_LEN * SINE_HZ << 16) / AUDIO_I2S_RATE))

static void render_sine(int32_t *frames, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int32_t s = sine_table[(sine_phase >> 16) & (SINE_TABLE_LEN - 1)];
        sine_phase += SINE_STEP;
        frames[i] = (int32_t)(((uint32_t)s << 16) | ((uint32_t)s & 0xFFFFu));
    }
}

// The single rendering entry point, called from the DMA ISR on core1.
static void render(int32_t *frames, size_t n) {
    if (sine_mode) {
        render_sine(frames, n);
    } else {
        mixer_render(frames, n);
    }
}

// --- getting core1 out of the flash ----------------------------------------
// A sector erase makes the whole XIP window unreadable, and core1 is executing
// from it. Before a save, core1 is asked to sit in a spin loop that lives in
// RAM, and the audio ISR is muted so that it too stops touching flash. Core0
// erases only once core1 has acknowledged, and lets it go afterwards.
//
// The audio does not stop: the DMA chain keeps running and the ISR keeps
// refilling the buffers with zeros, so a save costs a short silence rather
// than a stall or a loop of the last buffer.
// Flash writes and the audio, on RP2350 with both cores live.
//
// The hand-rolled version of this parked core1 in a RAM spin loop and called
// flash_range_erase() raw. It wrote the record correctly and it hung the module:
// a cheap erase (a still-virgin sector, ~47ms) survived, a full one (~147ms)
// left core0 stuck inside the erase with the USB CDC no longer serviced. Core1
// spinning in a RAM function is not the lockout the RP2350 bootrom's flash
// routines expect, so the SDK's own flash_safe_execute() does that part now -
// see settings_flash.c - and core1 registers as its victim below.
//
// The cost is the audio: while core1 is locked out its DMA ISR cannot run, so
// the ping-pong is not re-armed and the pair goes idle. It cannot restart
// itself, each channel needing the other to fire first, so the chain is rebuilt
// explicitly on the way out rather than assumed to have survived.
//
// Idle is not silent, though, and that is what a save was audibly costing: with
// nothing feeding the PIO the I2S clocks stop dead for the whole erase, and the
// DAC's PLL - it runs off BCK, SCK is grounded on the board - unlocks and then
// re-locks on the way back, loudly, with no MIDI involved at all. So the pair
// is handed over to a keep-alive DMA that needs no interrupt (see
// audio_i2s_hold_silence()) rather than simply left to stop.
static void flash_guard_enter_impl(void) {
    audio_i2s_set_mute(true);
    // Two buffer periods, so what is already queued in the DMA drains and the
    // erase starts against silence instead of cutting a note in half.
    sleep_ms(1 + (2 * 1000 * AUDIO_BUF_FRAMES) / AUDIO_I2S_RATE);
    audio_i2s_hold_silence();
}

static bool flash_guard_enter(void) {
    flash_guard_enter_impl();
    return true;
}

static void flash_guard_leave(void) {
    audio_i2s_restart_chain();
    audio_i2s_set_mute(false);
}

static void core1_main(void) {
    audio_i2s_init(render);
    // Lets core0 lock this core out for a flash write. Without it
    // flash_safe_execute() refuses rather than risking the write.
    flash_safe_execute_core_init();
    while (true) {
        tight_loop_contents();
    }
}

static void print_library(void) {
    const SampleLib *lib = SAMPLE_LIB;
    if (!library_ok) {
        printf("\nNO LIBRARY at 0x%08X.\n", SAMPLE_LIB_XIP_ADDR);
        printf("  Flash it with: picotool load -o 0x%08X sample_lib.bin\n",
               SAMPLE_LIB_XIP_ADDR);
        printf("  The test tone works regardless.\n");
        return;
    }
    printf("\nLibrary: %u samples at %u Hz, %u bytes (version %u)\n", lib->count,
           lib->sample_rate, lib->total_bytes, lib->version);
    if (mixer_kit_count()) {
        printf("Layout: %u kits of %d, entry k*%d+slot\n", mixer_kit_count(),
               KIT_SIZE, KIT_SIZE);
    } else {
        printf("Layout: flat, no kits\n");
    }
    for (unsigned i = 0; i < lib->count; i++) {
        if (mixer_kit_count() && i % KIT_SIZE == 0) {
            printf("  --- kit %u: %s\n", i / KIT_SIZE, mixer_kit_name(i / KIT_SIZE));
        }
        // Precision, not a bare %s: the name is a 24-byte field read straight
        // from flash, and a corrupt blob need not carry the NUL. Without the
        // bound printf walks flash until it finds a zero.
        printf("  [%u] %-22.*s %7u samples  %.3fs\n", i,
               (int)SAMPLE_LIB_NAME_LEN, lib->entries[i].name,
               lib->entries[i].length_samples,
               (double)lib->entries[i].length_samples / lib->sample_rate);
    }
}

// --- settings and presets ---------------------------------------------------

// Pushes the record onto the things it configures. Called at boot and after
// any change, so there is one path from "the record says X" to "the module
// does X" and no field that only takes effect on the next power cycle.
static void settings_apply(void) {
    const SettingsRecord *r = settings_get();
    midi_set_filter(&midi, r->midi_channel);
    mixer_set_master_gain(r->master_gain);
    mixer_set_dynamics(r->dynamics);
    mixer_set_hat_choke(r->hat_choke != 0);
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        mixer_set_slot_note(s, r->slot_note[s]);
        mixer_set_slot_pan(s, r->slot_pan[s]);
        mixer_set_slot_level(s, r->slot_level[s]);
    }
    ssd1306_set_contrast(r->oled_contrast);
}

// Writes the eight slots as they stand into preset `p`. The note map is not
// copied: it is configuration, not part of a preset.
static void preset_store(uint8_t p) {
    if (p >= NUM_PRESETS) {
        return;
    }
    SettingsRecord *r = settings_get();
    r->preset[p].used = 1;
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        r->preset[p].lib_index[sl] = mixer_slot_index(sl);
    }
}

static bool preset_load(uint8_t p) {
    const SettingsRecord *r = settings_get();
    if (p >= NUM_PRESETS || !r->preset[p].used) {
        return false;
    }
    // A preset saved against a different library can point past the end of
    // this one. An index out of range becomes an empty slot: a silent slot is
    // recoverable, a read off the end of the blob is not.
    int32_t count = (int32_t)mixer_library_count();
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        int32_t idx = r->preset[p].lib_index[sl];
        mixer_assign_slot(sl, (idx >= 0 && idx < count) ? idx : -1);
    }
    return true;
}

static void print_settings(void) {
    const SettingsRecord *r = settings_get();
    printf("\nSettings: %s%s\n", settings_is_default() ? "DEFAULTS (flash empty or invalid)"
                                                      : "loaded from flash",
           settings_dirty() ? ", UNSAVED CHANGES" : "");
    printf("  flash region 0x%08X, %u sectors of %u bytes, record %u bytes\n",
           (unsigned)SETTINGS_FLASH_XIP_ADDR, (unsigned)SETTINGS_SECTORS,
           (unsigned)SETTINGS_FLASH_SECTOR_SIZE, (unsigned)sizeof(SettingsRecord));
    if (r->midi_channel == SETTINGS_MIDI_CHANNEL_ANY) {
        printf("  MIDI channel : any\n");
    } else {
        printf("  MIDI channel : %u\n", r->midi_channel + 1u);
    }
    printf("  master gain  : %u/4096\n", r->master_gain);
    if (r->velocity_fixed == SETTINGS_VELOCITY_FOLLOW) {
        printf("  velocity     : follow\n");
    } else {
        printf("  velocity     : fixed at %u\n", r->velocity_fixed);
    }
    static const char *const DYNAMICS_NAME[4] = {"mid", "off", "low", "high"};
    printf("  dynamics     : %s\n",
           r->dynamics < 4 ? DYNAMICS_NAME[r->dynamics] : "?");
    printf("  hat choke    : %s\n", r->hat_choke ? "on" : "off");
    printf("  OLED contrast: %u\n", r->oled_contrast);
    if (r->boot_preset == PRESET_NONE) {
        printf("  boot preset  : none (kit 0)\n");
    } else {
        printf("  boot preset  : %u\n", r->boot_preset + 1u);
    }
    printf("  notes        :");
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        char n[5];
        ui_note_name(r->slot_note[sl], n);
        printf(" %s=%s", mixer_role_name(sl), n);
    }
    printf("\n");
    printf("  level        :");
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        uint8_t a = r->slot_level[sl];
        if (a >= MIXER_LEVEL_OFF) {
            printf(" %s=OFF", mixer_role_name(sl));
        } else {
            printf(" %s=%ddB", mixer_role_name(sl), -(int)(a * MIXER_LEVEL_DB_STEP));
        }
    }
    printf("\n");
    printf("  pan          :");
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
        int pan = r->slot_pan[sl];
        if (pan == 0) {
            printf(" %s=C", mixer_role_name(sl));
        } else {
            printf(" %s=%c%d", mixer_role_name(sl), pan < 0 ? 'L' : 'R',
                   pan < 0 ? -pan : pan);
        }
    }
    printf("\n");
    for (uint8_t p = 0; p < NUM_PRESETS; p++) {
        if (!r->preset[p].used) {
            printf("  preset %u     : empty\n", p + 1u);
            continue;
        }
        printf("  preset %u     :", p + 1u);
        for (uint8_t sl = 0; sl < NUM_SLOTS; sl++) {
            printf(" %d", r->preset[p].lib_index[sl]);
        }
        printf("\n");
    }
}

// Reads a slot number typed straight after the command letter. Returns 0xFF if
// nothing usable arrives: the console is driven both by hand and by
// tools/console.py, and neither should hang waiting for a digit.
static uint8_t read_preset_digit(void) {
    for (int tries = 0; tries < 2000; tries++) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) {
            continue;
        }
        if (c >= '1' && c <= '0' + NUM_PRESETS) {
            return (uint8_t)(c - '1');
        }
        return 0xFF;
    }
    return 0xFF;
}

// Every save goes through here. A flash erase holds core0 for ~147ms with its
// interrupts off, and MIDI keeps arriving through it: at 31250 baud that is
// ~460 bytes against a 32-byte UART FIFO. What survives in the FIFO afterwards
// is a fragment from before the gap, and the parser still holds the running
// status of a message that ended long ago - so those bytes get read against the
// wrong status and play as notes nobody sent, a burst of hits out of nothing.
// Dropping the fragment and making the parser wait for a fresh status byte is
// the same discipline it already applies to a stream joined mid-message at
// power-on. Bounded like midi_poll(), for the same reason.
static bool settings_save_resync(void) {
    bool ok = settings_save();
    unsigned dropped = 0;
    for (int i = 0; i < 64 && uart_is_readable(uart1); i++) {
        (void)uart_getc(uart1);
        dropped++;
    }
    midi_resync(&midi);
    if (dropped) {
        printf("MIDI: %u stale bytes dropped after the erase\n", dropped);
    }
    return ok;
}

static void save_settings_now(void) {
    if (!settings_dirty()) {
        printf("nothing to save\n");
        return;
    }
    printf("saving... (the audio goes quiet for the erase)\n");
    uint32_t t0 = time_us_32();
    bool ok = settings_save_resync();
    printf("%s in %u us\n", ok ? "saved" : "SAVE FAILED", time_us_32() - t0);
}

static void print_help(void) {
    printf(
        "\nCommands:\n"
        "  1-8  trigger a slot\n"
        "  s    440Hz test tone on/off\n"
        "  a    burst on every slot (voice stress test)\n"
        "  +/-  master gain\n"
        "  i    state and statistics\n"
        "  l    library listing\n"
        "  t    I2S pin test\n"
        "  c    I2C bus scan (display)\n"
        "  u    UI state\n"
        "  k    kit listing\n"
        "  m    MIDI state and counters\n"
        "  M    inject a MIDI test stream (no cable needed)\n"
        "  p    probe the MIDI pin as a raw GPIO for 2s\n"
        "  e    encoder test (turn the knob for 3s)\n"
        "  S    settings and presets\n"
        "  L<n> load preset n (1-8)\n"
        "  W<n> store the current slots into preset n and save\n"
        "  R    restore the factory settings (not saved until 'w')\n"
        "  w    write settings and presets to flash\n"
        "  ?    this help\n");
}

// Tells whether the PIO is really moving the pins, with no instruments needed.
static void print_pin_test(void) {
    audio_i2s_pin_probe_t probe[AUDIO_I2S_NUM_PINS];
    audio_i2s_probe_pins(probe, 20);

    printf("\nI2S pin test (20ms window):\n");
    bool all_moving = true;
    bool all_muxed = true;
    for (int i = 0; i < AUDIO_I2S_NUM_PINS; i++) {
        const audio_i2s_pin_probe_t *p = &probe[i];
        printf("  GP%-2u %-4s  function %u (expected %u)  level %u  edges %u\n",
               p->pin, p->name, p->func, p->func_expected, p->level ? 1 : 0,
               p->edges);
        if (p->edges == 0) {
            all_moving = false;
        }
        if (p->func != p->func_expected) {
            all_muxed = false;
        }
    }

    if (!all_muxed) {
        printf("\n  The pins are not assigned to the PIO: the pinmux is at fault.\n");
    } else if (!all_moving) {
        printf("\n  Pins assigned to the PIO but idle: the state machine is not driving them.\n");
    } else {
        printf(
            "\n  The pins are toggling: the I2S signal really leaves the RP2350.\n"
            "  If you hear nothing, the fault is downstream (wiring, DAC, monitoring).\n");
    }
}

// Tells whether anyone answers on the bus, with no instrument to hook up.
// The display address is not universal: plenty of modules sit at 0x3D.
static void print_i2c_scan(void) {
    uint8_t found[16];
    int n = oled_bus_scan(found, (int)(sizeof found));

    printf("\nI2C0 scan (SDA GP%d, SCL GP%d):\n", PIN_OLED_SDA, PIN_OLED_SCL);
    if (n == 0) {
        printf("  no device answers.\n");
        printf("  Check the supply, SDA/SCL and the pull-up resistors.\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        printf("  0x%02X%s\n", found[i],
               found[i] == OLED_I2C_ADDR ? "  <- display expected here" : "");
    }
}

// Encoder probe: counts the transitions of the two quadrature pins separately
// while the user turns the knob.
//
// It is there to tell apart three faults that look identical from the outside
// ("I turn it and nothing happens"): neither wire carries, only one does, or
// both toggle but not in quadrature. The single-wire case is the treacherous
// one, because the decoder adds +1 and -1 on the same transition and the net
// stays zero.
#define ENC_PROBE_MS 3000

static void print_encoder_test(void) {
    printf("\nEncoder test: turn the knob for %d seconds, now...\n",
           ENC_PROBE_MS / 1000);

    Encoder probe;
    encoder_init(&probe);

    bool pa = gpio_get(PIN_ENC_A);
    bool pb = gpio_get(PIN_ENC_B);
    bool ps = !gpio_get(PIN_ENC_SW);
    uint32_t edges_a = 0, edges_b = 0, edges_sw = 0;
    uint32_t cw = 0, ccw = 0, presses = 0;

    // The distinction that matters: in real quadrature the two bits change one
    // at a time (single transition), because the signals are 90 degrees apart.
    // If the two wires carry the same signal they change together, and a
    // two-bit jump has no direction: the table cancels it and no step ever
    // comes out. Per-pin counts alone do not separate the two cases, this does.
    uint32_t single_bit = 0, double_bit = 0;
    uint8_t prev_ab = (uint8_t)((pa ? 2 : 0) | (pb ? 1 : 0));

    // Trace of the first states seen, as digits 0..3. On healthy quadrature it
    // reads as the cyclic sequence 0-1-3-2 (or its reverse); on two shorted
    // wires all you see is a bounce between 0 and 3.
    char trace[41];
    unsigned n_trace = 0;

    absolute_time_t end = make_timeout_time_ms(ENC_PROBE_MS);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        bool a = gpio_get(PIN_ENC_A);
        bool b = gpio_get(PIN_ENC_B);
        bool sw = !gpio_get(PIN_ENC_SW);

        if (a != pa) { edges_a++; pa = a; }
        if (b != pb) { edges_b++; pb = b; }
        if (sw != ps) { edges_sw++; ps = sw; }

        uint8_t ab = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
        if (ab != prev_ab) {
            if (((prev_ab ^ ab) & 3) == 3) {
                double_bit++;
            } else {
                single_bit++;
            }
            if (n_trace < sizeof trace - 1) {
                trace[n_trace++] = (char)('0' + ab);
            }
            prev_ab = ab;
        }

        // The probe reads CLK/DT for the diagnostics above, but feeds the
        // decoder in the channel order fixed by PIN_ENC_QUAD_A/B, so the CW
        // and CCW counts printed here match what the UI actually sees.
        bool qa = (PIN_ENC_QUAD_A == PIN_ENC_A) ? a : b;
        bool qb = (PIN_ENC_QUAD_B == PIN_ENC_A) ? a : b;

        switch (encoder_update(&probe, qa, qb, sw,
                               to_ms_since_boot(get_absolute_time()))) {
            case ENC_CW:  cw++; break;
            case ENC_CCW: ccw++; break;
            case ENC_PRESS: presses++; break;
            default: break;
        }
    }

    printf("  A  GP%-2u level %u  edges %u\n", PIN_ENC_A, pa, edges_a);
    printf("  B  GP%-2u level %u  edges %u\n", PIN_ENC_B, pb, edges_b);
    printf("  SW GP%-2u level %u  edges %u\n", PIN_ENC_SW, ps, edges_sw);
    trace[n_trace] = '\0';
    printf("  decoded detents: %u cw, %u ccw\n", cw, ccw);
    printf("  transitions: %u single-bit, %u double-bit\n", single_bit, double_bit);
    printf("  state trace (A<<1|B): %s\n", trace);

    if (edges_a == 0 && edges_b == 0) {
        printf("\n  Neither pin moves.\n");
        printf("  If you really turned it: check GND and the wires on GP%u/GP%u.\n",
               PIN_ENC_A, PIN_ENC_B);
    } else if (edges_a == 0 || edges_b == 0) {
        printf("\n  Only one pin toggles (%s idle).\n", edges_a ? "B" : "A");
        printf("  It is the most common fault and it explains everything on its\n");
        printf("  own: without the second signal there is no quadrature, the\n");
        printf("  decoder adds +1 and -1 on the same transition and never emits\n");
        printf("  a step. Check the %s wire and its continuity.\n",
               edges_a ? "DT" : "CLK");
    } else if (double_bit > single_bit) {
        printf("\n  The two bits almost always change TOGETHER (%u times out of %u).\n",
               double_bit, double_bit + single_bit);
        printf("  That is not quadrature: it is the same signal read twice.\n");
        printf("  The two wires are shorted, or they end up on the same\n");
        printf("  breadboard rail. The trace alternates 0 and 3 instead of the\n");
        printf("  cyclic 0-1-3-2 sequence.\n");
        if (cw || ccw) {
            printf("  The %u detents read are phantoms: they come from the sampling\n",
                   cw + ccw);
            printf("  skew between the two GPIOs, not from a rotation.\n");
        }
    } else if (cw == 0 && ccw == 0) {
        printf("\n  The bits change one at a time (%u single transitions) but no\n",
               single_bit);
        printf("  step comes out: here the suspicion moves to the decoder, not\n");
        printf("  to the wiring. Look at the trace: healthy quadrature must read\n");
        printf("  as the 0-1-3-2 cycle or its reverse.\n");
    } else {
        printf("\n  The encoder works: %u detents read correctly.\n", cw + ccw);
        if (cw && ccw) {
            printf("  Detents both ways: normal if you changed direction.\n");
        }
    }
}

static const char *ui_mode_name(void) {
    switch (ui_mode()) {
        case UI_SELECT:      return "select";
        case UI_ASSIGN:      return "assign";
        case UI_KIT:         return "kit";
        case UI_MENU:        return "menu";
        case UI_PRESET:      return "preset";
        case UI_SAVE:        return "save preset";
        case UI_CONFIG:      return ui_config_depth() ? "config (group)" : "config";
        case UI_CONFIG_EDIT: return "config edit";
        case UI_ABOUT:       return "about";
        default:             return "?";
    }
}

// --- MIDI --------------------------------------------------------------------

// UART1 RX only. TX stays unclaimed: this is an input, and leaving the pin
// alone means a wiring mistake cannot make the module drive the MIDI line.
static void midi_hw_init(void) {
    uart_init(uart1, MIDI_BAUD);
    gpio_set_function(PIN_MIDI_RX, GPIO_FUNC_UART);
    // A MIDI line idles high, and the 6N138 output does too. Without the
    // pull-up the pin floats whenever nothing is plugged in, and the level
    // change at init alone is enough to clock in a spurious byte: the first
    // run on hardware showed exactly one, counted as an orphan.
    gpio_pull_up(PIN_MIDI_RX);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart1, true);
    midi_init(&midi, MIDI_CHANNEL_GM_DRUMS);
}

// One parsed byte. Kept apart from the UART read so the console can push bytes
// through exactly the same path, with no cable and no optocoupler.
static void midi_byte(uint8_t b) {
    MidiEvent ev;
    if (!midi_feed(&midi, b, &ev)) {
        return;
    }
    if (ev.type != MIDI_NOTE_ON) {
        return;  // one-shots have no note off
    }
    // A fixed velocity is a setting, not a mapping: the note still has to be
    // one this module owns, so the substitution happens here and not in the
    // parser, which stays a parser.
    uint8_t vel = settings_get()->velocity_fixed;
    if (vel == SETTINGS_VELOCITY_FOLLOW) {
        vel = ev.velocity;
    }
    if (mixer_trigger_note(ev.note, vel)) {
        midi_hits++;
    } else {
        midi_unmapped++;
    }
}

static void midi_poll(void) {
    // Bounded, not while(readable): a stream at full rate must not be able to
    // starve the encoder and the display of core0.
    for (int i = 0; i < 32 && uart_is_readable(uart1); i++) {
        midi_byte((uint8_t)uart_getc(uart1));
    }
}

static void print_midi(void) {
    printf("\ninput        : UART1 RX on GP%d, %d baud\n", PIN_MIDI_RX, MIDI_BAUD);
    if (midi.filter == MIDI_CHANNEL_ANY) {
        printf("channel      : any\n");
    } else {
        printf("channel      : %u (GM drums)\n", midi.filter + 1u);
    }
    printf("bytes        : %u\n", midi.bytes);
    printf("notes played : %u  unmapped: %u  filtered: %u\n",
           midi_hits, midi_unmapped, midi.filtered);
    printf("real-time    : %u  orphans: %u\n", midi.realtime, midi.orphans);
    printf("parser       : status=0x%02X n_data=%u%s\n", midi.status,
           midi.n_data, midi.in_sysex ? " (in sysex)" : "");
    printf("slot notes   :");
    for (int i = 0; i < NUM_SLOTS; i++) {
        printf(" %u", mixer_slot_note(i));
    }
    printf("\n");
}

// Pushes through the parser the stream that broke every naive parser we know
// of: running status, a clock between the two data bytes, a note on a channel
// we do not listen to, and a velocity 0. If the six slots fire from this and
// nothing else does, the parser is right and only the cable is left to test.
static void print_midi_inject(void) {
    static const uint8_t stream[] = {
        0x99, 36, 100,          // kick, with its status byte
        0xF8, 38, 0xF8, 100,    // snare in running status, clock in the middle
        42, 90,                 // closed hat
        46, 90,                 // open hat
        0x90, 60, 100,          // channel 1: must be filtered out
        0x99, 39, 110,          // FLEX1
        51, 100,                // FLEX2, running status again
        36, 0,                  // velocity 0: a note off, not a silent hit
    };
    uint32_t before = midi_hits;
    printf("\ninjecting %u bytes...\n", (unsigned)sizeof(stream));
    for (size_t i = 0; i < sizeof(stream); i++) {
        midi_byte(stream[i]);
    }
    printf("notes played: %u (6 expected: one per slot)\n", midi_hits - before);
    print_midi();
}

// Looks at GP5 as a plain pin instead of a UART input. When the byte counter
// stays at zero the question is no longer "is the parser right" but "does
// anything reach the pin at all", and that is a different measurement:
//
//   idle high, no edges      nothing arriving. The line is at rest, or nothing
//                            is connected and the pull-up is holding it
//   idle low, no edges       the line is held down: inverted polarity, an
//                            optocoupler output with no pull-up, or a short
//   edges but bytes stay 0   the signal is there and the UART cannot frame it:
//                            wrong baud rate, or inverted data
static void print_midi_probe(void) {
    printf("\nprobing GP%d for 2s, send MIDI now...\n", PIN_MIDI_RX);

    gpio_set_function(PIN_MIDI_RX, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_MIDI_RX, GPIO_IN);
    gpio_pull_up(PIN_MIDI_RX);

    uint32_t samples = 0, high = 0, edges = 0;
    bool prev = gpio_get(PIN_MIDI_RX);
    absolute_time_t end = delayed_by_ms(get_absolute_time(), 2000);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        bool now = gpio_get(PIN_MIDI_RX);
        if (now != prev) {
            edges++;
            prev = now;
        }
        high += now ? 1u : 0u;
        samples++;
    }

    // Hand the pin back to the UART and throw away whatever the transition
    // shook loose, so the probe does not leave a phantom byte behind.
    gpio_set_function(PIN_MIDI_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_MIDI_RX);
    while (uart_is_readable(uart1)) {
        (void)uart_getc(uart1);
    }

    unsigned pct = (unsigned)((uint64_t)high * 100u / (samples ? samples : 1u));
    printf("samples %u, high %u%%, edges %u\n", samples, pct, edges);

    if (edges == 0) {
        printf("verdict: the pin never moved, it sits %s.\n",
               pct > 50 ? "high (line at rest, or nothing connected)"
                        : "LOW (held down: inverted polarity, an opto output "
                          "with no pull-up, or a short)");
    } else {
        printf("verdict: the signal is there (%u edges). If 'm' still counts 0 "
               "bytes the UART cannot frame it: check the baud rate and that "
               "the data is not inverted.\n", edges);
    }
    printf("note: at %d baud one bit lasts 32us, so a note is about 30 edges.\n",
           MIDI_BAUD);
}

static void print_ui_state(void) {
    int32_t cur_kit = mixer_current_kit();
    printf("\ndisplay      : %s\n", oled_ok ? "present" : "ABSENT");
    printf("mode         : %s\n", ui_mode_name());
    printf("current slot : %u %s (%.*s)\n", ui_slot() + 1,
           mixer_role_name(ui_slot()),
           (int)SAMPLE_LIB_NAME_LEN, mixer_slot_name(ui_slot()));
    printf("loaded kit   : ");
    if (cur_kit >= 0) {
        printf("%d/%u (%s)\n", cur_kit + 1, mixer_kit_count(),
               mixer_kit_name((uint32_t)cur_kit));
    } else {
        printf("mixed, no whole kit loaded\n");
    }
    if (ui_mode() == UI_ASSIGN) {
        printf("browser      : %u/%u (%.*s)\n", (unsigned)ui_browse_index() + 1,
               (unsigned)mixer_library_count(),
               (int)SAMPLE_LIB_NAME_LEN, mixer_library_name(ui_browse_index()));
    }
    if (ui_mode() == UI_KIT) {
        printf("kit browser  : %u/%u (%s)\n", (unsigned)ui_kit_index() + 1,
               mixer_kit_count(), mixer_kit_name(ui_kit_index()));
    }
}

// The kits in flash, one line each: the fastest way to tell from the console
// whether the library really is the one that was meant to be flashed.
static void print_kits(void) {
    uint32_t kits = mixer_kit_count();
    if (kits == 0) {
        printf("\nThe library is not kit-organised (no version-%d blob).\n",
               SAMPLE_LIB_VERSION);
        return;
    }
    printf("\n%u kits:\n", kits);
    for (uint32_t k = 0; k < kits; k++) {
        printf("  [%2u] %-16s", k, mixer_kit_name(k));
        for (uint32_t s = 0; s < KIT_SIZE; s++) {
            printf(" %.3fs", (double)SAMPLE_LIB->entries[k * KIT_SIZE + s]
                                     .length_samples / SAMPLE_LIB_RATE);
        }
        printf("\n");
    }
}

static void print_stats(void) {
    audio_i2s_stats_t a;
    mixer_stats_t m;
    audio_i2s_get_stats(&a);
    mixer_get_stats(&m);

    printf("\nmode         : %s\n", sine_mode ? "test tone" : "samples");
    printf("master gain  : %u/4096\n", mixer_get_master_gain());
    printf("active voices: %u/%d\n", m.active_voices, MAX_VOICES);
    printf("triggers     : %u  chokes: %u  steals: %u  dropped: %u\n", m.triggers,
           m.chokes, m.steals, m.dropped);
    printf("render       : last %uus, peak %uus, budget %uus\n",
           a.render_us_last, a.render_us_max, a.render_us_budget);
    printf("buffers      : %u  late: %u\n", a.buffers_rendered,
           a.late_renders);
    // `buffers` stops counting while the ISR is muted, so a frozen count on its
    // own does not say whether the audio is muted or the DMA chain has died.
    // These three do.
    {
        bool mut = false, busy[2] = {false, false};
        uint32_t cnt[2] = {0, 0};
        audio_i2s_debug(&mut, busy, cnt);
        printf("audio chain  : muted=%d  dma0 busy=%d count=%u  dma1 busy=%d count=%u\n",
               mut, busy[0], cnt[0], busy[1], cnt[1]);
    }
    if (a.late_renders) {
        printf("  WARNING: render over budget, the audio is breaking up.\n");
    }
    printf("slots        :");
    for (int i = 0; i < NUM_SLOTS; i++) {
        printf(" %d=%.*s", i + 1, (int)SAMPLE_LIB_NAME_LEN,
               mixer_slot_name((uint8_t)i));
    }
    printf("\n");
}

int main(void) {
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < SINE_TABLE_LEN; i++) {
        sine_table[i] =
            (int16_t)(12000.0f * sinf(2.0f * (float)M_PI * i / SINE_TABLE_LEN));
    }

    const SampleLib *lib = SAMPLE_LIB;
    library_ok = sample_lib_valid(lib);

    mixer_init(lib);
    // The kit is built by name: kick on slot 1, snare on 2, hats on 3-4, and
    // so on, so that the General MIDI notes of the slots line up with the
    // sounds they trigger.
    mixer_load_default_kit();

    // Encoder: A/B with internal pull-ups (KY-040 breakouts already have their
    // own, but a bare EC11 does not, and having both does no harm), button
    // active low.
    gpio_init(PIN_ENC_A);
    gpio_init(PIN_ENC_B);
    gpio_init(PIN_ENC_SW);
    gpio_set_dir(PIN_ENC_A, GPIO_IN);
    gpio_set_dir(PIN_ENC_B, GPIO_IN);
    gpio_set_dir(PIN_ENC_SW, GPIO_IN);
    gpio_pull_up(PIN_ENC_A);
    gpio_pull_up(PIN_ENC_B);
    gpio_pull_up(PIN_ENC_SW);
    encoder_init(&enc);

    // The display is an accessory: if it does not answer the module must play
    // anyway. ssd1306_init() detects that and from then on every drawing call
    // is harmless.
    oled_ok = ssd1306_init();
    ui_init();
    ui_show_splash(to_ms_since_boot(get_absolute_time()));
    // The UI writes settings and asks for them to be saved; it does not know
    // what a flash sector is, and does not know the MIDI parser exists.
    static const UiHooks UI_HOOKS = {settings_apply, settings_save_resync};
    ui_set_hooks(&UI_HOOKS);
    midi_hw_init();

    multicore_launch_core1(core1_main);

    // Settings come after core1 is up, because a save needs it parked, and
    // after the mixer, the MIDI parser and the display exist, because
    // settings_apply() writes into all three.
    settings_flash_init(flash_guard_enter, flash_guard_leave);
    settings_init(settings_flash_backend());
    settings_apply();

    // The library and the settings sectors share one flash: a blob big enough
    // to reach them would be overwritten by the first save, silently. Cheap to
    // check, impossible to notice otherwise.
    if (library_ok) {
        uint32_t lib_end = SAMPLE_LIB_FLASH_OFFSET + 16u +
                           (uint32_t)lib->count * 32u + lib->total_bytes;
        if (!settings_flash_region_clear_of(lib_end)) {
            printf("\n*** the sample library runs into the settings sectors: "
                   "saving is disabled ***\n");
            settings_flash_init(NULL, NULL);
        }
    }

    {
        uint8_t boot = settings_get()->boot_preset;
        if (boot != PRESET_NONE && !preset_load(boot)) {
            // An empty preset selected at boot is not an error worth stopping
            // for: the default kit is already loaded, and that is a sane place
            // to come up.
        }
    }

    // The banner repeats until the first key arrives. Printed once at boot it
    // would be lost every time the terminal is opened after power-up, leaving
    // a silent console that looks like a fault.
    bool greeted = false;
    absolute_time_t next_greet = get_absolute_time();

    uint32_t last_oled_try_ms = 0;
    bool burst = false;
    absolute_time_t next_burst = get_absolute_time();
    uint8_t burst_slot = 0;

    while (true) {
        int ch = getchar_timeout_us(1000);

        if (ch != PICO_ERROR_TIMEOUT) {
            greeted = true;
        } else if (!greeted &&
                   absolute_time_diff_us(get_absolute_time(), next_greet) <= 0) {
            printf("\n=== Sample Player alpha ===\n");
            printf("sys_clk %u kHz, I2S %d Hz, buffer %d frames\n",
                   clock_get_hz(clk_sys) / 1000, AUDIO_I2S_RATE, AUDIO_BUF_FRAMES);
            print_library();
            print_help();
            printf("\nOLED display: %s\n",
                   oled_ok ? "found" : "ABSENT (use 'c' for the I2C scan)");
            printf("Encoder: turn to change slot, press to assign, "
                   "long press for the kit.\n");
            printf("MIDI: UART1 RX on GP%d, channel %u. "
                   "'M' injects a test stream with no cable.\n",
                   PIN_MIDI_RX, midi.filter + 1u);
            // The render state says right away whether PIO and DMA are running,
            // which is the first thing to know when nothing is heard.
            print_stats();
            next_greet = delayed_by_ms(get_absolute_time(), 3000);
        }

        switch (ch) {
            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8':
                sine_mode = false;
                mixer_trigger_slot((uint8_t)(ch - '1'), 100);
                break;
            case 's':
                sine_mode = !sine_mode;
                printf("mode: %s\n", sine_mode ? "test tone" : "samples");
                break;
            case 'a':
                burst = !burst;
                sine_mode = false;
                printf("burst: %s\n", burst ? "on" : "off");
                break;
            case '+': {
                uint16_t g = mixer_get_master_gain();
                if (g < 8192) {
                    mixer_set_master_gain(g + 256);
                }
                printf("master gain: %u/4096\n", mixer_get_master_gain());
                break;
            }
            case '-': {
                uint16_t g = mixer_get_master_gain();
                if (g > 256) {
                    mixer_set_master_gain(g - 256);
                }
                printf("master gain: %u/4096\n", mixer_get_master_gain());
                break;
            }
            case 'i':
                print_stats();
                break;
            case 'l':
                print_library();
                break;
            case 't':
                print_pin_test();
                break;
            case 'c':
                print_i2c_scan();
                break;
            case 'k':
                print_kits();
                break;
            case 'u':
                print_ui_state();
                break;
            case 'm':
                print_midi();
                break;
            case 'M':
                print_midi_inject();
                break;
            case 'p':
                print_midi_probe();
                break;
            case 'e':
                print_encoder_test();
                break;
            case 'S':
                print_settings();
                break;
            case 'L': {
                uint8_t p = read_preset_digit();
                if (p == 0xFF) {
                    printf("usage: L<n>, n = 1..%d\n", NUM_PRESETS);
                } else if (preset_load(p)) {
                    printf("preset %u loaded\n", p + 1u);
                } else {
                    printf("preset %u is empty\n", p + 1u);
                }
                break;
            }
            case 'W': {
                uint8_t p = read_preset_digit();
                if (p == 0xFF) {
                    printf("usage: W<n>, n = 1..%d\n", NUM_PRESETS);
                    break;
                }
                preset_store(p);
                printf("preset %u holds the current slots\n", p + 1u);
                save_settings_now();
                break;
            }
            case 'R':
                settings_defaults(settings_get());
                settings_apply();
                printf("factory settings restored in RAM. 'w' makes it stick.\n");
                break;
            case 'w':
                save_settings_now();
                break;
            case '?':
                print_help();
                break;
            default:
                break;
        }

        // Burst: one hit every 40ms, cycling through the slots. With samples of
        // ~0.3-0.6s it keeps the pool constantly loaded, which is the condition
        // the render peak has to be measured in.
        if (burst && absolute_time_diff_us(get_absolute_time(), next_burst) <= 0) {
            mixer_trigger_slot(burst_slot, 100);
            burst_slot = (uint8_t)((burst_slot + 1) % NUM_SLOTS);
            next_burst = delayed_by_ms(get_absolute_time(), 40);
        }

        midi_poll();

        // Encoder and display run on core0 alongside the console. Polling at
        // ~1ms (the getchar timeout) is plenty: a knob spun in a hurry does not
        // exceed a hundred transitions per second.
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        EncoderEvent ev = encoder_update(&enc, gpio_get(PIN_ENC_QUAD_A),
                                         gpio_get(PIN_ENC_QUAD_B),
                                         !gpio_get(PIN_ENC_SW), now_ms);
        if (ev != ENC_NONE) {
            // Turning the knob while the test tone plays would mean not hearing
            // the result: the first touch switches over to the samples.
            sine_mode = false;
            ui_event(ev);
        }
        // Retry the display until it answers: see OLED_RETRY_MS. Placed before
        // ui_tick() so a display that has just come up is drawn on this tick
        // rather than on the next one.
        if (!oled_ok && (uint32_t)(now_ms - last_oled_try_ms) >= OLED_RETRY_MS) {
            last_oled_try_ms = now_ms;
            oled_ok = ssd1306_init();
            if (oled_ok) {
                settings_apply();  // contrast, lost with the re-init
                printf("\nOLED display: found on retry at %u ms\n", now_ms);
            }
        }

        ui_tick(now_ms);

        gpio_put(PICO_DEFAULT_LED_PIN, (now_ms % 1000) < 50);
    }
}
