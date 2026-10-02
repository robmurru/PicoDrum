#include "mixer.h"

#include <string.h>

#include "audio_i2s.h"
#include "sample_lib.h"

// Samples are stored at 22050Hz, the I2S runs at 44100: each source sample
// lasts two output frames. A voice's position is counted in output frames, so
// the 2x duplication is implicit in `pos >> 1` and no phase accumulator is
// needed.
#define UPSAMPLE_SHIFT 1

// Anti-click: ~1ms in output frames at 44.1kHz.
//
// Fade-in and fade-out must have the SAME length. On a choke the two voices
// play together for the whole overlap, and two opposite linear ramps of equal
// duration sum to exactly one. With different durations the sum overshoots:
// with 44 in and 66 out, a retrigger produced a +33% peak.
#define FADE_FRAMES 44

#define GAIN_UNITY 4096  // Q12

typedef struct {
    const int16_t *base;
    uint32_t length;        // source samples
    uint32_t pos;           // in output frames
    uint16_t gain_l;        // Q12, velocity x level x pan, left side
    uint16_t gain_r;        // Q12, velocity x level x pan, right side
    uint16_t fade_out;      // fade-out frames left, 0 = not closing
    uint8_t  slot;
    bool     active;
} Voice;

static Voice voices[MAX_VOICES];
static SampleSlot slots[NUM_SLOTS];
static const SampleLib *library;
static uint16_t master_gain = GAIN_UNITY / 2;  // -6dB of headroom
static uint8_t dynamics_curve = 0;  // SETTINGS_DYNAMICS_MID, by value
static int8_t slot_pan[NUM_SLOTS];  // MIXER_PAN_LEFT..MIXER_PAN_RIGHT, 0 = centre
static uint8_t slot_level[NUM_SLOTS];  // attenuation steps, 0 = 0dB .. MIXER_LEVEL_OFF
static bool hat_choke;

// The two slots the hat choke ties together. Positional, like every role:
// the order of SLOT_ROLE below is the layout of a kit.
#define SLOT_CH 2
#define SLOT_OH 3

static volatile uint32_t st_triggers, st_chokes, st_steals, st_dropped;
static volatile uint32_t st_active;
static volatile uint32_t st_slot_mask;

// --- trigger queue core0 -> core1 -------------------------------------------
// SPSC: only core0 writes `head`, only core1 writes `tail`.

#define TRIG_QUEUE_LEN 32u  // power of two
#define TRIG_QUEUE_MASK (TRIG_QUEUE_LEN - 1u)

typedef struct {
    uint8_t slot;
    uint8_t velocity;
} TriggerEvent;

static TriggerEvent trig_q[TRIG_QUEUE_LEN];
static volatile uint32_t trig_head, trig_tail;

static void trig_push(uint8_t slot, uint8_t velocity) {
    uint32_t head = trig_head;
    uint32_t next = (head + 1u) & TRIG_QUEUE_MASK;
    if (next == (trig_tail & TRIG_QUEUE_MASK)) {
        st_dropped++;  // queue full: better to lose a hit than to block core0
        return;
    }
    trig_q[head & TRIG_QUEUE_MASK] = (TriggerEvent){slot, velocity};
    // The event must be visible to the other core before the index that
    // publishes it, otherwise core1 reads an entry that is not written yet.
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    trig_head = next;
}

static bool trig_pop(TriggerEvent *out) {
    uint32_t tail = trig_tail;
    if ((tail & TRIG_QUEUE_MASK) == (trig_head & TRIG_QUEUE_MASK)) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    *out = trig_q[tail & TRIG_QUEUE_MASK];
    trig_tail = (tail + 1u) & TRIG_QUEUE_MASK;
    return true;
}

// --- voice handling ---------------------------------------------------------

static uint32_t voice_remaining(const Voice *v) {
    uint32_t total = v->length << UPSAMPLE_SHIFT;
    return v->pos >= total ? 0 : total - v->pos;
}

// Picks where to put a new voice. Prefers a free slot in the pool; if the pool
// is full it steals the one with the fewest samples left, i.e. the least
// audible.
static Voice *voice_alloc(void) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) {
            return &voices[i];
        }
    }
    Voice *victim = &voices[0];
    uint32_t least = voice_remaining(victim);
    for (int i = 1; i < MAX_VOICES; i++) {
        uint32_t rem = voice_remaining(&voices[i]);
        if (rem < least) {
            least = rem;
            victim = &voices[i];
        }
    }
    st_steals++;
    // Stealing is a hard cut: the voice is needed right now and there is no
    // third voice to fade it into. The fade-in of the new hit masks the step.
    return victim;
}

// Velocity 1..127 to gain, Q12, shaped by the dynamics curve. All integer:
// nothing in the render path is allowed to touch a float, and this runs on
// the same core as the render.
//
// MID is the plain ratio and is also what every curve agrees on at the ends:
// velocity 127 is always unity gain, whichever curve is selected, so the
// hardest hit sounds the same everywhere and only the hits under it move.
static uint16_t velocity_to_gain(uint8_t velocity) {
    switch (dynamics_curve) {
        case 1:  // SETTINGS_DYNAMICS_OFF
            return GAIN_UNITY;
        case 2: {  // SETTINGS_DYNAMICS_LOW: floor at 75%, only the top
                    // quarter of the range follows velocity
            uint32_t floor = (GAIN_UNITY * 3u) / 4u;
            return (uint16_t)(floor + ((GAIN_UNITY - floor) * velocity) / 127u);
        }
        case 3: {  // SETTINGS_DYNAMICS_HIGH: the square of the velocity
                   // ratio, so half velocity is a quarter of the gain
            uint32_t v = velocity;
            return (uint16_t)((v * v * GAIN_UNITY) / (127u * 127u));
        }
        default:  // SETTINGS_DYNAMICS_MID
            return (uint16_t)((velocity * GAIN_UNITY) / 127u);
    }
}

// Balance law, not constant power: at the centre both sides stay at unity,
// which is exactly how the module sounded before pan existed, and turning
// towards one side only attenuates the other one, linearly, to silence at the
// end stop. Constant power would put the centre at -3dB on each side and make
// a firmware upgrade quieter for everyone who never touches the pan.
static void pan_gains(int8_t pan, uint32_t *l, uint32_t *r) {
    *l = GAIN_UNITY;
    *r = GAIN_UNITY;
    if (pan > 0) {
        *l = (GAIN_UNITY * (uint32_t)(MIXER_PAN_RIGHT - pan)) / MIXER_PAN_RIGHT;
    } else if (pan < 0) {
        *r = (GAIN_UNITY * (uint32_t)(MIXER_PAN_RIGHT + pan)) / MIXER_PAN_RIGHT;
    }
}

// Slot level, Q12, indexed by attenuation step: 10^(-3k/20) x 4096, rounded,
// and the last entry silence. A table rather than a log, because nothing in
// the render core touches a float.
static const uint16_t LEVEL_Q12[MIXER_LEVEL_OFF + 1] = {
    4096, 2900, 2053, 1453, 1029, 728, 516, 365, 258, 0,
};

static void do_trigger(uint8_t slot, uint8_t velocity) {
    if (slot >= NUM_SLOTS || library == NULL) {
        return;
    }
    int32_t idx = slots[slot].lib_index;
    if (idx < 0 || (uint32_t)idx >= library->count) {
        return;
    }

    // Choke: the voice already playing on this slot fades out instead of
    // vanishing at once. This is the Volca Drum behaviour. With the hat choke
    // on, CH and OH count as one slot for this, so either cuts the other.
    bool hat = hat_choke && (slot == SLOT_CH || slot == SLOT_OH);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        bool mine = v->slot == slot ||
                    (hat && (v->slot == SLOT_CH || v->slot == SLOT_OH));
        if (v->active && mine && v->fade_out == 0) {
            v->fade_out = FADE_FRAMES;
            st_chokes++;
        }
    }

    // Level and pan are read once, at the trigger, and folded into the
    // voice's two gains: turning the knob moves the next hit, never a tail
    // already sounding, so a change mid-note cannot step the level of a
    // voice. Both voices of a same-slot choke share a level and a pan, so
    // their fades still sum to one on each side; a hat choke crosses two
    // different sounds, where there is no sum to keep and the fade is only
    // there to avoid a click.
    //
    // A muted slot still takes a voice and still chokes: OFF means silent,
    // not ignored. A hit on it cuts its own tail like any other hit, and with
    // the hat choke on a muted CH still closes the open hat.
    uint32_t gain = (velocity_to_gain(velocity) * LEVEL_Q12[slot_level[slot]]) >> 12;
    uint32_t pl, pr;
    pan_gains(slot_pan[slot], &pl, &pr);

    Voice *v = voice_alloc();
    v->base = sample_lib_data(library, (unsigned)idx);
    v->length = library->entries[idx].length_samples;
    v->pos = 0;
    v->gain_l = (uint16_t)((gain * pl) >> 12);
    v->gain_r = (uint16_t)((gain * pr) >> 12);
    v->fade_out = 0;
    v->slot = slot;
    v->active = true;
    st_triggers++;
}

// --- slot map ---------------------------------------------------------------
// Slot -> MIDI note, following the General MIDI percussion map (the one every
// sequencer and drum pattern on channel 10 assumes). The notes are NOT
// contiguous: GM assigns a fixed note per instrument, and a kit that respects
// it plays right from any external source without remapping anything.
//
// The slots are ordered as a drum kit is read, not as the notes are numbered.
// The last four are free slots, so for them the note is a convention rather
// than a description: each carries the GM note of the sound the kits put there
// most often, so a pattern written for a full GM kit lands roughly right.
static const uint8_t SLOT_NOTES[NUM_SLOTS] = {
    36,  // C1  Bass Drum 1
    38,  // D1  Acoustic Snare
    42,  // F#1 Closed Hi-Hat
    46,  // A#1 Open Hi-Hat
    39,  // D#1 Hand Clap      (FLEX1)
    51,  // D#2 Ride Cymbal 1  (FLEX2)
    37,  // C#1 Side Stick     (FLEX3)
    45,  // A1  Low Tom        (FLEX4)
};

// The label of each slot, in the same order. Only for display: nothing in the
// engine branches on it. The library says what is on a slot, the slot does not.
static const char *const SLOT_ROLE[NUM_SLOTS] = {
    "KICK", "SNARE", "CH", "OH", "FLEX1", "FLEX2", "FLEX3", "FLEX4",
};

// The blob is indexed as kit * KIT_SIZE + slot, so the two have to agree.
_Static_assert(SAMPLE_LIB_KIT_SIZE == NUM_SLOTS,
               "the library's kit size does not match the number of slots");

// --- API --------------------------------------------------------------------

void mixer_init(const void *lib) {
    library = (const SampleLib *)lib;
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < NUM_SLOTS; i++) {
        slots[i].lib_index = -1;
        slots[i].midi_note = SLOT_NOTES[i];
        slot_pan[i] = 0;
        slot_level[i] = 0;
    }
    hat_choke = false;
    trig_head = trig_tail = 0;
    st_triggers = st_chokes = st_steals = st_dropped = st_active = 0;
}

void mixer_assign_slot(uint8_t slot, int32_t lib_index) {
    if (slot < NUM_SLOTS) {
        slots[slot].lib_index = lib_index;
    }
}

uint32_t mixer_kit_count(void) {
    return library != NULL ? sample_lib_kit_count(library) : 0;
}

void mixer_load_kit(uint32_t kit) {
    if (kit >= mixer_kit_count()) {
        return;
    }
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        slots[s].lib_index = (int32_t)(kit * KIT_SIZE + s);
    }
}

const char *mixer_kit_name(uint32_t kit) {
    // A ring of buffers, not one: the 128x64 context list asks for the
    // previous, current and next kit in a single expression, and with one
    // static buffer all three arguments alias it and the list shows the same
    // name three times. Four gives the three simultaneous callers a margin.
    static char ring[4][SAMPLE_LIB_NAME_LEN];
    static unsigned next_buf;
    char *buf = ring[next_buf];
    next_buf = (next_buf + 1u) % (unsigned)(sizeof ring / sizeof ring[0]);
    if (kit >= mixer_kit_count()) {
        return "---";
    }
    // The names are MACHINE_KIT_ROLE. Dropping the last field leaves what
    // identifies the kit, which is what belongs on the display: six entries
    // reading "SYN_808" say more than one reading "SYN_808_KICK".
    const char *full = library->entries[kit * KIT_SIZE].name;
    size_t n = 0, cut = 0;
    while (n < SAMPLE_LIB_NAME_LEN - 1 && full[n] != '\0') {
        if (full[n] == '_') {
            cut = n;
        }
        n++;
    }
    if (cut == 0) {
        cut = n;  // no underscore: there is no suffix to drop
    }
    memcpy(buf, full, cut);
    buf[cut] = '\0';
    return buf;
}

int32_t mixer_kit_of(const int32_t *lib_index) {
    if (mixer_kit_count() == 0 || lib_index[0] < 0 ||
        lib_index[0] % (int32_t)KIT_SIZE != 0) {
        return -1;
    }
    uint32_t kit = (uint32_t)lib_index[0] / KIT_SIZE;
    for (uint8_t s = 1; s < NUM_SLOTS; s++) {
        if (lib_index[s] != (int32_t)(kit * KIT_SIZE + s)) {
            return -1;
        }
    }
    return (int32_t)kit;
}

int32_t mixer_current_kit(void) {
    int32_t idx[NUM_SLOTS];
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        idx[s] = slots[s].lib_index;
    }
    return mixer_kit_of(idx);
}

const char *mixer_role_name(uint8_t slot) {
    return slot < NUM_SLOTS ? SLOT_ROLE[slot] : "?";
}

void mixer_load_default_kit(void) {
    if (library == NULL) {
        return;
    }
    if (mixer_kit_count() > 0) {
        mixer_load_kit(0);
        return;
    }
    // A flat library has no kit to load and nothing in it says what a sample
    // is, so the slots take the first six in order. This is the version-1
    // fallback: it comes up playable, just not in kit order.
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        slots[s].lib_index = s < library->count ? (int32_t)s : -1;
    }
}

uint8_t mixer_default_note(uint8_t slot) {
    return slot < NUM_SLOTS ? SLOT_NOTES[slot] : 0;
}

void mixer_set_slot_note(uint8_t slot, uint8_t midi_note) {
    if (slot < NUM_SLOTS) {
        slots[slot].midi_note = midi_note;
    }
}

void mixer_set_slot_pan(uint8_t slot, int8_t pan) {
    if (slot >= NUM_SLOTS) {
        return;
    }
    if (pan < MIXER_PAN_LEFT) {
        pan = MIXER_PAN_LEFT;
    } else if (pan > MIXER_PAN_RIGHT) {
        pan = MIXER_PAN_RIGHT;
    }
    slot_pan[slot] = pan;
}

int8_t mixer_slot_pan(uint8_t slot) {
    return slot < NUM_SLOTS ? slot_pan[slot] : 0;
}

void mixer_set_slot_level(uint8_t slot, uint8_t atten) {
    if (slot < NUM_SLOTS) {
        slot_level[slot] = atten > MIXER_LEVEL_OFF ? MIXER_LEVEL_OFF : atten;
    }
}

uint8_t mixer_slot_level(uint8_t slot) {
    return slot < NUM_SLOTS ? slot_level[slot] : 0;
}

void mixer_set_hat_choke(bool on) {
    hat_choke = on;
}

int32_t mixer_slot_index(uint8_t slot) {
    return slot < NUM_SLOTS ? slots[slot].lib_index : -1;
}

uint8_t mixer_slot_note(uint8_t slot) {
    return slot < NUM_SLOTS ? slots[slot].midi_note : 0;
}

uint32_t mixer_active_slot_mask(void) { return st_slot_mask; }

uint32_t mixer_library_count(void) {
    return library != NULL ? library->count : 0;
}

const char *mixer_library_name(uint32_t index) {
    if (library == NULL || index >= library->count) {
        return "---";
    }
    return library->entries[index].name;
}

const char *mixer_slot_name(uint8_t slot) {
    if (slot >= NUM_SLOTS || library == NULL) {
        return "---";
    }
    int32_t idx = slots[slot].lib_index;
    if (idx < 0 || (uint32_t)idx >= library->count) {
        return "<empty>";
    }
    return library->entries[idx].name;
}

void mixer_trigger_slot(uint8_t slot, uint8_t velocity) {
    trig_push(slot, velocity);
}

bool mixer_trigger_note(uint8_t midi_note, uint8_t velocity) {
    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
        if (slots[i].midi_note == midi_note) {
            trig_push(i, velocity);
            return true;
        }
    }
    return false;
}

void mixer_set_master_gain(uint16_t gain_q12) { master_gain = gain_q12; }
uint16_t mixer_get_master_gain(void) { return master_gain; }

void mixer_set_dynamics(uint8_t curve) { dynamics_curve = curve; }

static int32_t clamp16(int32_t x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return x;
}

void mixer_render(int32_t *frames, size_t n) {
    TriggerEvent ev;
    while (trig_pop(&ev)) {
        do_trigger(ev.slot, ev.velocity);
    }

    for (size_t f = 0; f < n; f++) {
        int32_t acc_l = 0, acc_r = 0;

        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &voices[i];
            if (!v->active) {
                continue;
            }

            uint32_t src = v->pos >> UPSAMPLE_SHIFT;
            if (src >= v->length) {
                v->active = false;
                continue;
            }

            // The fades are shaped on the raw sample, once, before it splits
            // into two sides: int16 x FADE_FRAMES cannot overflow, and the
            // split then costs one multiply per side.
            int32_t s = v->base[src];

            // Fade-in on the attack, derived from the position: no separate
            // counter needed.
            if (v->pos < FADE_FRAMES) {
                s = (s * (int32_t)v->pos) / FADE_FRAMES;
            }

            if (v->fade_out) {
                // Closing because of a choke or a steal.
                s = (s * v->fade_out) / FADE_FRAMES;
                v->fade_out--;
                if (v->fade_out == 0) {
                    v->active = false;
                }
            } else {
                // Natural close at the end of the sample: one cut before it
                // has decayed would end on a step.
                uint32_t remaining = (v->length << UPSAMPLE_SHIFT) - v->pos;
                if (remaining < FADE_FRAMES) {
                    s = (s * (int32_t)remaining) / FADE_FRAMES;
                }
            }

            acc_l += (s * v->gain_l) >> 12;
            acc_r += (s * v->gain_r) >> 12;
            v->pos++;
        }

        acc_l = clamp16((acc_l * master_gain) >> 12);
        acc_r = clamp16((acc_r * master_gain) >> 12);

        // (R << 16) | L. The PIO program sends the top half while LRCK is
        // high, and in I2S LRCK high is the RIGHT channel (the PCM5102A reads
        // LRCK low as left) - the pico-extras layout, where int16 L,R
        // interleaved land as L in the low half of the little-endian word.
        // Up to phase3 the comment here said (L << 16) | R, and nobody could
        // hear it was wrong while both halves carried the same mono sum; the
        // first pan test on the module heard it at once.
        //
        // The casts to unsigned are needed because left-shifting a negative
        // int is UB.
        frames[f] = (int32_t)(((uint32_t)acc_r << 16) | ((uint32_t)acc_l & 0xFFFFu));
    }

    uint32_t n_active = 0;
    uint32_t mask = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) {
            n_active++;
            mask |= 1u << voices[i].slot;
        }
    }
    st_active = n_active;
    st_slot_mask = mask;
}

void mixer_get_stats(mixer_stats_t *out) {
    out->active_voices = st_active;
    out->triggers = st_triggers;
    out->chokes = st_chokes;
    out->steals = st_steals;
    out->dropped = st_dropped;
}
