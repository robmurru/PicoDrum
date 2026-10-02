// Native test bench for the mixer: checks the logic before the hardware.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mixer.h"
#include "sample_lib.h"

// Three test samples, all with the constant value 1000: that way gains, fades
// and sums can be read off the result by eye.
#define IDX_LONG 0
#define IDX_SHORT 1
#define IDX_MID 2

#define LEN_LONG 11025  // 0.5s at 22.05kHz, like a real snare
#define LEN_SHORT 100   // 4.5ms, to measure durations and fades
#define LEN_MID 2000

#define N_TEST 3
#define SAMPLE_VALUE 1000

static uint8_t libbuf[64 * 1024];
static int failures;

#define CHECK(cond, ...)    \
    do {                    \
        if (!(cond)) {      \
            printf("  FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n");   \
            failures++;     \
        }                   \
    } while (0)

// Builds a fake library with the same layout as the blob in flash.
static const SampleLib *make_lib(void) {
    memset(libbuf, 0, sizeof(libbuf));
    memcpy(libbuf, "SMPL", 4);
    *(uint16_t *)(libbuf + 4) = 1;
    *(uint16_t *)(libbuf + 6) = N_TEST;
    *(uint32_t *)(libbuf + 8) = 22050;

    uint32_t lens[N_TEST] = {LEN_LONG, LEN_SHORT, LEN_MID};
    uint32_t off = 16 + 32 * N_TEST;
    for (int i = 0; i < N_TEST; i++) {
        uint8_t *e = libbuf + 16 + 32 * i;
        snprintf((char *)e, 24, "TEST%d", i);
        *(uint32_t *)(e + 24) = off;
        *(uint32_t *)(e + 28) = lens[i];

        int16_t *d = (int16_t *)(libbuf + off);
        for (uint32_t k = 0; k < lens[i]; k++) {
            d[k] = SAMPLE_VALUE;
        }
        off += lens[i] * 2;
        off = (off + 3) & ~3u;
    }
    *(uint32_t *)(libbuf + 12) = off;
    return (const SampleLib *)libbuf;
}


// A library with real names and a chosen version: version 2 is kit-organised
// (entry k*6+s is slot s of kit k), version 1 is a flat list.
static uint8_t namedbuf[64 * 1024];

static const SampleLib *make_named_lib(const char *const *names, unsigned count,
                                       unsigned version) {
    memset(namedbuf, 0, sizeof(namedbuf));
    memcpy(namedbuf, "SMPL", 4);
    *(uint16_t *)(namedbuf + 4) = (uint16_t)version;
    *(uint16_t *)(namedbuf + 6) = (uint16_t)count;
    *(uint32_t *)(namedbuf + 8) = 22050;

    uint32_t off = 16 + 32 * count;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = namedbuf + 16 + 32 * i;
        snprintf((char *)e, 24, "%s", names[i]);
        *(uint32_t *)(e + 24) = off;
        *(uint32_t *)(e + 28) = LEN_SHORT;
        int16_t *d = (int16_t *)(namedbuf + off);
        for (int k = 0; k < LEN_SHORT; k++) {
            d[k] = SAMPLE_VALUE;
        }
        off += LEN_SHORT * 2;
        off = (off + 3) & ~3u;
    }
    *(uint32_t *)(namedbuf + 12) = off;
    return (const SampleLib *)namedbuf;
}

// The frame layout the I2S PIO program expects: right in the top half, left
// in the bottom one. See the comment at the end of mixer_render().
static int16_t left(int32_t frame) { return (int16_t)(frame & 0xFFFF); }
static int16_t right(int32_t frame) { return (int16_t)(frame >> 16); }

static void setup(const SampleLib *lib, uint8_t slot, int32_t idx) {
    mixer_init(lib);
    mixer_assign_slot(slot, idx);
    mixer_set_master_gain(4096);  // unity: keeps the arithmetic readable
}

int main(void) {
    const SampleLib *lib = make_lib();
    CHECK(sample_lib_valid(lib), "the fake library does not pass sample_lib_valid");

    static int32_t out[4096];
    mixer_stats_t st;
    int n;

    // --- silence at rest ---
    setup(lib, 0, IDX_LONG);
    mixer_render(out, 64);
    n = 0;
    for (int i = 0; i < 64; i++) {
        if (out[i] != 0) n++;
    }
    CHECK(n == 0, "output not silent without a trigger (%d frames)", n);

    // --- identical channels and fade-in from the attack ---
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);
    n = 0;
    for (int i = 0; i < 256; i++) {
        if (left(out[i]) != right(out[i])) n++;
    }
    CHECK(n == 0, "L and R differ on %d frames", n);
    CHECK(left(out[0]) == 0, "the first frame is %d instead of 0", left(out[0]));
    int monotone = 1;
    for (int i = 1; i < 44; i++) {
        if (left(out[i]) < left(out[i - 1])) monotone = 0;
    }
    CHECK(monotone, "the fade-in is not monotonic");
    CHECK(left(out[50]) == SAMPLE_VALUE, "after the fade-in expected %d, found %d",
          SAMPLE_VALUE, left(out[50]));

    // --- 2x upsampling: N source samples last 2N output frames ---
    setup(lib, 0, IDX_SHORT);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 512);
    int last_audible = -1;
    for (int i = 0; i < 512; i++) {
        if (left(out[i]) != 0) last_audible = i;
    }
    CHECK(last_audible == 2 * LEN_SHORT - 1,
          "the sample ends at frame %d, expected %d", last_audible,
          2 * LEN_SHORT - 1);

    // --- linear velocity ---
    setup(lib, 0, IDX_SHORT);
    mixer_trigger_slot(0, 64);
    mixer_render(out, 128);
    int expect = SAMPLE_VALUE * ((64 * 4096) / 127) / 4096;
    CHECK(abs(left(out[60]) - expect) <= 2, "velocity 64 -> %d, expected ~%d",
          left(out[60]), expect);

    // --- dynamics curves: 0 MID, 1 OFF, 2 LOW, 3 HIGH (see settings.h) ---
    // OFF ignores velocity: even the softest hit plays at full gain.
    setup(lib, 0, IDX_SHORT);
    mixer_set_dynamics(1);
    mixer_trigger_slot(0, 1);
    mixer_render(out, 128);
    CHECK(abs(left(out[60]) - SAMPLE_VALUE) <= 2,
          "OFF: velocity 1 gave %d, expected full scale %d", left(out[60]), SAMPLE_VALUE);

    // LOW floors at 75%: the softest hit still plays loud.
    setup(lib, 0, IDX_SHORT);
    mixer_set_dynamics(2);
    mixer_trigger_slot(0, 1);
    mixer_render(out, 128);
    int low_floor = SAMPLE_VALUE * 3 / 4;
    CHECK(left(out[60]) >= low_floor - 2,
          "LOW: velocity 1 gave %d, expected at least the %d floor", left(out[60]), low_floor);

    // HIGH is the square of the ratio: half velocity is a quarter of the
    // gain, well under what MID gives the same velocity.
    setup(lib, 0, IDX_SHORT);
    mixer_set_dynamics(3);
    mixer_trigger_slot(0, 64);
    mixer_render(out, 128);
    int high_expect = SAMPLE_VALUE * ((64 * 64 * 4096) / (127 * 127)) / 4096;
    CHECK(abs(left(out[60]) - high_expect) <= 2, "HIGH: velocity 64 -> %d, expected ~%d",
          left(out[60]), high_expect);
    CHECK(left(out[60]) < expect,
          "HIGH at half velocity must be quieter than MID's %d, got %d", expect,
          left(out[60]));

    // Every curve agrees at velocity 127: the hardest hit sounds the same
    // regardless of curve, only what happens under it moves.
    for (uint8_t curve = 0; curve <= 3; curve++) {
        setup(lib, 0, IDX_SHORT);
        mixer_set_dynamics(curve);
        mixer_trigger_slot(0, 127);
        mixer_render(out, 128);
        CHECK(abs(left(out[60]) - SAMPLE_VALUE) <= 2,
              "curve %u at velocity 127 gave %d, expected full scale", curve,
              left(out[60]));
    }
    mixer_set_dynamics(0);  // back to MID for every test after this one
    printf("  dynamics curves            : OFF/LOW/MID/HIGH agree at velocity 127\n");

    // --- choke: a retrigger replaces, it does not add ---
    setup(lib, 0, IDX_LONG);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);  // first voice at full level
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);  // second voice + fade-out of the first
    int peak = 0, dip = SAMPLE_VALUE;
    for (int i = 0; i < 256; i++) {
        if (left(out[i]) > peak) peak = left(out[i]);
        if (left(out[i]) < dip) dip = left(out[i]);
    }
    mixer_get_stats(&st);
    CHECK(st.chokes == 1, "chokes counted: %u, expected 1", st.chokes);
    // The two ramps have equal duration, so the sum must stay flat: neither a
    // bump (voices adding up) nor a dip (missing overlap).
    CHECK(peak <= SAMPLE_VALUE + 2, "+%d%% bump on the choke (peak %d)",
          (peak - SAMPLE_VALUE) * 100 / SAMPLE_VALUE, peak);
    CHECK(dip >= SAMPLE_VALUE - 50, "dip on the choke crossfade (minimum %d)", dip);
    printf("  choke                      : peak %d, minimum %d (nominal %d)\n",
           peak, dip, SAMPLE_VALUE);

    // --- musical use: every slot with a 0.5s sample, retriggered every 40ms ---
    // This is the worst case the module really meets. The pool must not run
    // out here, otherwise the NUM_SLOTS x 2 derivation does not hold.
    mixer_init(lib);
    for (int s = 0; s < NUM_SLOTS; s++) {
        mixer_assign_slot((uint8_t)s, IDX_LONG);
    }
    uint32_t peak_voices = 0;
    for (int round = 0; round < 100; round++) {
        for (int s = 0; s < NUM_SLOTS; s++) {
            mixer_trigger_slot((uint8_t)s, 127);
        }
        // 40ms at 44.1kHz = 1764 frames, rendered in blocks like the DMA does.
        for (int chunk = 0; chunk < 1764 / 256; chunk++) {
            mixer_render(out, 256);
            mixer_get_stats(&st);
            if (st.active_voices > peak_voices) peak_voices = st.active_voices;
        }
    }
    mixer_get_stats(&st);
    CHECK(st.steals == 0, "%u steals in musical use: the pool is not freeing up",
          st.steals);
    CHECK(st.dropped == 0, "%u triggers lost", st.dropped);
    CHECK(peak_voices > 0, "no active voice: the test is measuring nothing");
    printf("  %d slots at 40ms, 0.5s smpl: peak %u/%d voices, chokes %u, steals %u\n",
           NUM_SLOTS,
           peak_voices, MAX_VOICES, st.chokes, st.steals);

    // --- pathological case: retrigger faster than the fade-out ---
    // 32 frames between hits is ~1.4kHz per slot, unreachable from musical
    // MIDI but not from a malformed stream. Stealing has to step in and keep
    // everything bounded, without crashing.
    mixer_init(lib);
    for (int s = 0; s < NUM_SLOTS; s++) {
        mixer_assign_slot((uint8_t)s, IDX_LONG);
    }
    peak_voices = 0;
    for (int round = 0; round < 200; round++) {
        for (int s = 0; s < NUM_SLOTS; s++) {
            mixer_trigger_slot((uint8_t)s, 127);
        }
        mixer_render(out, 32);
        mixer_get_stats(&st);
        if (st.active_voices > peak_voices) peak_voices = st.active_voices;
    }
    CHECK(peak_voices <= MAX_VOICES, "%u active voices, over the maximum", peak_voices);
    CHECK(st.steals > 0, "expected stealing to kick in, it never did");
    printf("  pathological 1.4kHz/slot   : peak %u/%d voices, steals %u (the net)\n",
           peak_voices, MAX_VOICES, st.steals);

    // --- no overflow with every voice at full scale ---
    mixer_init(lib);
    for (int s = 0; s < NUM_SLOTS; s++) {
        mixer_assign_slot((uint8_t)s, IDX_LONG);
        mixer_trigger_slot((uint8_t)s, 127);
    }
    mixer_set_master_gain(8192);  // absurd gain: it must clamp, not wrap
    mixer_render(out, 256);
    n = 0;
    for (int i = 100; i < 256; i++) {
        if (left(out[i]) < 0) n++;  // the source is positive everywhere
    }
    CHECK(n == 0, "%d frames with a flipped sign: the sum overflows", n);

    // --- trigger queue: overfilling corrupts nothing ---
    setup(lib, 0, IDX_LONG);
    for (int i = 0; i < 1000; i++) {
        mixer_trigger_slot(0, 127);
    }
    mixer_render(out, 64);
    mixer_get_stats(&st);
    CHECK(st.dropped > 0, "1000 triggers with no render did not fill the queue");
    printf("  saturated queue            : %u accepted, %u dropped\n", st.triggers,
           st.dropped);

    // --- empty slot or out-of-range index: no audio, no crash ---
    mixer_init(lib);
    mixer_trigger_slot(0, 127);   // slot never assigned
    mixer_trigger_slot(99, 127);  // slot that does not exist
    mixer_assign_slot(2, 12345);  // index past the end of the library
    mixer_trigger_slot(2, 127);
    mixer_render(out, 64);
    n = 0;
    for (int i = 0; i < 64; i++) {
        if (out[i] != 0) n++;
    }
    CHECK(n == 0, "an invalid slot produced audio");


    // --- General MIDI note map on the slots ---
    // The notes are not contiguous: an off-by-one that made them 36..41 again
    // would silently send every kick pattern to the wrong slot.
    mixer_init(lib);
    static const uint8_t GM[NUM_SLOTS] = {36, 38, 42, 46, 39, 51, 37, 45};
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(mixer_slot_note((uint8_t)i) == GM[i], "slot %d on note %u instead of %u",
              i + 1, mixer_slot_note((uint8_t)i), GM[i]);
    }
    // A note has to reach its own slot and nobody else's.
    mixer_assign_slot(1, IDX_SHORT);
    CHECK(mixer_trigger_note(38, 127), "note 38 is owned by the snare slot");
    mixer_render(out, 64);
    mixer_get_stats(&st);
    CHECK(st.triggers == 1, "note 38 (snare) produced %u triggers instead of 1",
          st.triggers);
    CHECK(!mixer_trigger_note(50, 127),  // high tom: no slot owns it
          "note 50 is owned by no slot and must be reported as unmapped");
    mixer_render(out, 64);
    mixer_get_stats(&st);
    CHECK(st.triggers == 1, "an unmapped note triggered something (%u)", st.triggers);
    printf("  GM note map                : 36/38/42/46/39/51/37/45, unmapped ignored\n");

    // --- kits: the layout decides the slot, not the names ---
    // Three kits of six, exactly as the converter writes them. The firmware
    // must never look at these names to work out where a sample goes: the
    // whole point of the version-2 layout is that the index says it.
    static const char *const KITS[] = {
        "SYN_909_KICK",  "SYN_909_SNARE", "SYN_909_CH",   "SYN_909_OH",
        "SYN_909_FLEX1", "SYN_909_FLEX2", "SYN_909_FLEX3", "SYN_909_FLEX4",
        "SYN_808_KICK",  "SYN_808_SNARE", "SYN_808_CH",
        "SYN_808_OH",    "SYN_808_FLEX1", "SYN_808_FLEX2",
        "SYN_808_FLEX3", "SYN_808_FLEX4",
        "SYN_DUST_KICK",  "SYN_DUST_SNARE", "SYN_DUST_CH",    "SYN_DUST_OH",
        "SYN_DUST_FLEX1", "SYN_DUST_FLEX2", "SYN_DUST_FLEX3", "SYN_DUST_FLEX4",
    };
    mixer_init(make_named_lib(KITS, 24, 2));
    CHECK(mixer_kit_count() == 3, "%u kits out of 24 entries instead of 3",
          mixer_kit_count());

    // At boot every slot is kit 0, in order.
    mixer_load_default_kit();
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(mixer_slot_index((uint8_t)i) == i, "slot %d on entry %d instead of %d",
              i + 1, mixer_slot_index((uint8_t)i), i);
    }
    CHECK(mixer_current_kit() == 0, "loaded kit reported as %d instead of 0",
          mixer_current_kit());
    printf("  default kit                : kit 0 on all the slots, in order\n");

    // Loading a kit moves every slot at once, and every slot gets the sample
    // whose name carries its own role: an off-by-one here would be a hi-hat on
    // the kick, which is the failure the whole layout exists to prevent.
    mixer_load_kit(1);
    static const char *const WANT_K1[NUM_SLOTS] = {
        "SYN_808_KICK",  "SYN_808_SNARE", "SYN_808_CH",
        "SYN_808_OH",    "SYN_808_FLEX1", "SYN_808_FLEX2",
        "SYN_808_FLEX3", "SYN_808_FLEX4",
    };
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(strcmp(mixer_slot_name((uint8_t)i), WANT_K1[i]) == 0,
              "kit 1: slot %d holds %s instead of %s", i + 1,
              mixer_slot_name((uint8_t)i), WANT_K1[i]);
    }
    CHECK(mixer_current_kit() == 1, "after loading kit 1, current kit is %d",
          mixer_current_kit());
    CHECK(strcmp(mixer_kit_name(1), "SYN_808") == 0,
          "kit 1 named \"%s\" instead of \"SYN_808\"", mixer_kit_name(1));
    CHECK(strcmp(mixer_kit_name(2), "SYN_DUST") == 0,
          "kit 2 named \"%s\" instead of \"SYN_DUST\"", mixer_kit_name(2));
    printf("  load kit                   : %d slots at once, name \"%s\"\n",
           NUM_SLOTS, mixer_kit_name(1));

    // Three names alive at the same time. The 128x64 context list asks for the
    // previous, current and next kit inside one call to draw_context_list(),
    // and a single static buffer in mixer_kit_name() makes all three arguments
    // alias it: the list then shows one name three times. The checks above
    // cannot see that - they compare one name, then the next, each after the
    // one before it has already been used.
    {
        const char *k0 = mixer_kit_name(0);
        const char *k1 = mixer_kit_name(1);
        const char *k2 = mixer_kit_name(2);
        CHECK(strcmp(k0, "SYN_909") == 0 && strcmp(k1, "SYN_808") == 0 &&
              strcmp(k2, "SYN_DUST") == 0,
              "three kit names held at once alias each other: \"%s\" \"%s\" \"%s\"",
              k0, k1, k2);
        printf("  three kit names at once    : %s / %s / %s, distinct\n", k0, k1, k2);
    }

    // One slot changed by hand and the kit is no longer whole. The UI needs to
    // know: it is what tells "kit 2 is loaded" from "kit 2 with another snare".
    mixer_assign_slot(1, 0);
    CHECK(mixer_current_kit() == -1,
          "with a slot reassigned the current kit is still %d", mixer_current_kit());
    mixer_load_kit(2);
    CHECK(mixer_current_kit() == 2, "reloading kit 2 gives %d", mixer_current_kit());
    printf("  mixed slots                : reported as no kit, reloading restores one\n");

    // Out of range must not touch the slots, and must not read past the TOC.
    mixer_load_kit(3);
    CHECK(mixer_current_kit() == 2, "load_kit(3) on 3 kits changed the slots");
    CHECK(strcmp(mixer_kit_name(99), "---") == 0, "kit 99 named %s",
          mixer_kit_name(99));
    printf("  kit out of range           : ignored, no read past the TOC\n");

    // A flat version-1 library: no kits, and the slots fall back to the first
    // samples by position. It has to stay playable — that blob is what phase1
    // and every library built before the kits looks like.
    static const char *const FLAT[] = {"A", "B", "C", "D", "E",
                                       "F", "G", "H", "I"};
    mixer_init(make_named_lib(FLAT, 9, 1));
    CHECK(mixer_kit_count() == 0, "a version-1 blob reports %u kits",
          mixer_kit_count());
    mixer_load_default_kit();
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(mixer_slot_index((uint8_t)i) == i, "flat library: slot %d on entry %d",
              i + 1, mixer_slot_index((uint8_t)i));
    }
    CHECK(mixer_current_kit() == -1, "a flat library reports kit %d",
          mixer_current_kit());
    mixer_load_kit(0);  // must do nothing rather than pretend
    CHECK(mixer_slot_index(0) == 0, "load_kit on a flat library moved a slot");
    printf("  flat version-1 library     : no kits, first samples by position\n");

    // Fewer samples than slots: the extra slots stay empty instead of wrapping
    // around onto the same sample.
    static const char *const TINY[] = {"A", "B"};
    mixer_init(make_named_lib(TINY, 2, 1));
    mixer_load_default_kit();
    CHECK(mixer_slot_index(1) == 1, "slot 2 on entry %d", mixer_slot_index(1));
    CHECK(mixer_slot_index(2) == -1, "slot 3 filled from a 2-sample library");
    printf("  library smaller than a kit : the slots left over stay empty\n");

    // A version-2 count that is not a whole number of kits cannot come out of
    // the converter, which refuses it — but flash is flash, and indexing
    // kit*KIT_SIZE+s on a short last kit would read past the TOC.
    static const char *const SHORT[] = {"A", "B", "C", "D", "E", "F", "G"};
    mixer_init(make_named_lib(SHORT, 7, 2));
    CHECK(mixer_kit_count() == 0,
          "7 entries taken as %u kits: the last one is short", mixer_kit_count());
    printf("  incomplete last kit        : refused, no partial kit exposed\n");

    // The role labels are what the display and the kit list agree on.
    static const char *const ROLES[NUM_SLOTS] = {
        "KICK", "SNARE", "CH", "OH", "FLEX1", "FLEX2", "FLEX3", "FLEX4"};
    for (int i = 0; i < NUM_SLOTS; i++) {
        CHECK(strcmp(mixer_role_name((uint8_t)i), ROLES[i]) == 0,
              "slot %d labelled %s instead of %s", i + 1,
              mixer_role_name((uint8_t)i), ROLES[i]);
    }
    printf("  slot roles                 : KICK SNARE CH OH FLEX1-4\n");

    // --- pan, balance law ---
    // Every case reads frame 100, past the 44-frame fade-in, with a constant
    // sample of 1000 at velocity 127 and unity master gain: whatever the pan
    // does to it shows up as a plain number on each side.
    struct { int8_t pan; int l, r; } PAN_CASES[] = {
        {0, 1000, 1000},                 // centre: unity both sides, as before pan
        {MIXER_PAN_RIGHT, 0, 1000},      // hard right: left silent, right untouched
        {MIXER_PAN_LEFT, 1000, 0},       // hard left, the mirror
        {MIXER_PAN_RIGHT / 2, 500, 1000},  // halfway: only the far side moves
        {MIXER_PAN_LEFT / 2, 1000, 500},
    };
    for (unsigned c = 0; c < sizeof PAN_CASES / sizeof PAN_CASES[0]; c++) {
        setup(lib, 0, IDX_LONG);
        mixer_set_slot_pan(0, PAN_CASES[c].pan);
        mixer_trigger_slot(0, 127);
        mixer_render(out, 128);
        CHECK(left(out[100]) == PAN_CASES[c].l && right(out[100]) == PAN_CASES[c].r,
              "pan %d gave L=%d R=%d, expected L=%d R=%d", PAN_CASES[c].pan,
              left(out[100]), right(out[100]), PAN_CASES[c].l, PAN_CASES[c].r);
    }
    printf("  pan                        : balance law, centre is unity both sides\n");

    // Velocity and pan multiply: half velocity hard right is half on the right
    // and still nothing on the left.
    setup(lib, 0, IDX_LONG);
    mixer_set_slot_pan(0, MIXER_PAN_RIGHT);
    mixer_trigger_slot(0, 64);
    mixer_render(out, 128);
    CHECK(left(out[100]) == 0, "hard right leaked %d onto the left", left(out[100]));
    CHECK(right(out[100]) > 480 && right(out[100]) < 520,
          "velocity 64 hard right gave %d on the right, expected ~500",
          right(out[100]));
    printf("  pan x velocity             : the two gains multiply\n");

    // Out-of-range values clamp to the end stop rather than wrapping round to
    // the other side.
    mixer_init(lib);
    mixer_set_slot_pan(0, 100);
    mixer_set_slot_pan(1, -100);
    CHECK(mixer_slot_pan(0) == MIXER_PAN_RIGHT, "pan 100 stored as %d",
          mixer_slot_pan(0));
    CHECK(mixer_slot_pan(1) == MIXER_PAN_LEFT, "pan -100 stored as %d",
          mixer_slot_pan(1));
    CHECK(mixer_slot_pan(2) == 0, "a fresh slot must start centred");
    printf("  pan range                  : clamped, centred at init\n");

    // The pan is read at the trigger: moving it while a voice sounds moves the
    // next hit, not the tail already playing.
    setup(lib, 0, IDX_LONG);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 128);
    mixer_set_slot_pan(0, MIXER_PAN_RIGHT);
    mixer_render(out, 64);
    CHECK(left(out[10]) == 1000, "a sounding voice changed pan under it (L=%d)",
          left(out[10]));
    printf("  pan change mid-note        : applies to the next hit only\n");

    // A choke across a pan change: the fade-out keeps the old pan, the new hit
    // takes the new one, and on each side the two still never overshoot.
    setup(lib, 0, IDX_LONG);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);
    mixer_set_slot_pan(0, MIXER_PAN_LEFT / 2);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);
    int over = 0;
    for (int i = 0; i < 256; i++) {
        if (left(out[i]) > 1000 || right(out[i]) > 1000) over++;
    }
    CHECK(over == 0, "the choke overshot unity on %d frames with a pan change", over);
    CHECK(left(out[200]) == 1000 && right(out[200]) == 500,
          "after the choke expected L=1000 R=500, got L=%d R=%d",
          left(out[200]), right(out[200]));
    printf("  pan across a choke         : no overshoot, new hit takes the new pan\n");

    // --- slot level, 3dB steps ---
    // Same reading as the pan: frame 100, a constant 1000, velocity 127, unity
    // master. Each step is 10^(-3k/20) of it, rounded down by the Q12 maths.
    struct { uint8_t atten; int lo, hi; } LEVEL_CASES[] = {
        {0, 1000, 1000},                // 0dB: unity, as before level existed
        {1, 707, 708},                  // -3dB
        {2, 500, 501},                  // -6dB
        {8, 62, 63},                    // -24dB, the last audible step
        {MIXER_LEVEL_OFF, 0, 0},        // OFF
    };
    for (unsigned c = 0; c < sizeof LEVEL_CASES / sizeof LEVEL_CASES[0]; c++) {
        setup(lib, 0, IDX_LONG);
        mixer_set_slot_level(0, LEVEL_CASES[c].atten);
        mixer_trigger_slot(0, 127);
        mixer_render(out, 128);
        int l = left(out[100]), r = right(out[100]);
        CHECK(l == r && l >= LEVEL_CASES[c].lo && l <= LEVEL_CASES[c].hi,
              "level step %u gave L=%d R=%d, expected %d..%d", LEVEL_CASES[c].atten,
              l, r, LEVEL_CASES[c].lo, LEVEL_CASES[c].hi);
    }
    printf("  level                      : 0dB is unity, 3dB a step, OFF is silence\n");

    // Level, pan and velocity all multiply: -6dB, hard right, half velocity
    // is a quarter on the right and nothing on the left.
    setup(lib, 0, IDX_LONG);
    mixer_set_slot_level(0, 2);
    mixer_set_slot_pan(0, MIXER_PAN_RIGHT);
    mixer_trigger_slot(0, 64);
    mixer_render(out, 128);
    CHECK(left(out[100]) == 0, "-6dB hard right leaked %d onto the left", left(out[100]));
    CHECK(right(out[100]) > 240 && right(out[100]) < 265,
          "-6dB at velocity 64 hard right gave %d, expected ~250", right(out[100]));
    printf("  level x pan x velocity     : the three gains multiply\n");

    mixer_init(lib);
    mixer_set_slot_level(0, 200);
    CHECK(mixer_slot_level(0) == MIXER_LEVEL_OFF, "level 200 stored as %u",
          mixer_slot_level(0));
    CHECK(mixer_slot_level(1) == 0, "a fresh slot must start at 0dB");
    printf("  level range                : clamped to OFF, 0dB at init\n");

    // Read at the trigger, like the pan: muting a slot leaves the hit already
    // sounding alone, and silences the next one.
    setup(lib, 0, IDX_LONG);
    mixer_trigger_slot(0, 127);
    mixer_render(out, 128);
    mixer_set_slot_level(0, MIXER_LEVEL_OFF);
    mixer_render(out, 64);
    CHECK(left(out[10]) == 1000, "a sounding voice changed level under it (%d)",
          left(out[10]));
    printf("  level change mid-note      : applies to the next hit only\n");

    // A muted slot still chokes: OFF is silent, not ignored, so the next hit
    // on it fades out the voice before rather than letting it ring on.
    mixer_trigger_slot(0, 127);
    mixer_render(out, 256);
    mixer_get_stats(&st);
    CHECK(st.chokes == 1, "a hit on a muted slot choked %u voices, expected 1",
          st.chokes);
    CHECK(left(out[200]) == 0 && right(out[200]) == 0,
          "after a muted hit the slot still sounds: L=%d R=%d", left(out[200]),
          right(out[200]));
    printf("  muted slot                 : silent, and still chokes\n");

    // --- hat choke ---
    // OH rings, then CH is hit. Off (the 1.0 behaviour), both sound together
    // and nothing is choked; on, the OH fades out and only the CH is left.
    for (int on = 0; on <= 1; on++) {
        setup(lib, 3, IDX_LONG);  // OH
        mixer_assign_slot(2, IDX_LONG);  // CH
        mixer_set_hat_choke(on);
        mixer_trigger_slot(3, 127);
        mixer_render(out, 256);
        mixer_trigger_slot(2, 127);
        mixer_render(out, 256);
        mixer_get_stats(&st);
        CHECK(st.chokes == (uint32_t)on, "hat choke %s: %u chokes", on ? "on" : "off",
              st.chokes);
        int expect = on ? 1000 : 2000;
        CHECK(left(out[200]) == expect, "hat choke %s: %d after the CH, expected %d",
              on ? "on" : "off", left(out[200]), expect);
    }
    printf("  hat choke                  : CH closes the OH when on, nothing when off\n");

    // Both ways round, and nothing outside the pair: a kick under the hats
    // keeps ringing when the OH cuts the CH.
    setup(lib, 0, IDX_LONG);  // KICK
    mixer_assign_slot(2, IDX_LONG);
    mixer_assign_slot(3, IDX_LONG);
    mixer_set_hat_choke(true);
    mixer_trigger_slot(0, 127);
    mixer_trigger_slot(2, 127);
    mixer_render(out, 256);
    mixer_trigger_slot(3, 127);
    mixer_render(out, 256);
    mixer_get_stats(&st);
    CHECK(st.chokes == 1, "OH over CH and a kick choked %u voices, expected 1",
          st.chokes);
    CHECK(left(out[200]) == 2000, "expected the kick and the OH (2000), got %d",
          left(out[200]));
    printf("  hat choke scope            : both ways, only CH and OH\n");

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
