#!/usr/bin/env python3
"""
Synthesises the muRDrum starter library: three kits of eight slots, from
scratch.

Nothing here is sampled from anything. Every sound is built out of oscillators,
noise and filters, so the result is muR Lab's own recording and can be shipped
on hardware, which is exactly what a downloaded sample pack cannot be: "free to
use in your music" and "free to redistribute in a product" are different
permissions, and the packs that grant the first almost never grant the second.

Output: 24 mono 16-bit WAVs at 22050 Hz, named MACHINE_KIT_ROLE so the
firmware's mixer_kit_name() can show the kit on the display (it prints the name
up to the last underscore). Slot order is the firmware's own:

    KICK SNARE CH OH FLEX1 FLEX2 FLEX3 FLEX4

with FLEX2 the ride, the one slot allowed 1.5s because a cymbal tail is the
only thing that needs it.

Dependencies: none, stdlib only, so it runs wherever python3 does.
"""

import math
import os
import random
import struct
import sys
import wave

SR = 22050
TWO_PI = 2.0 * math.pi


# --------------------------------------------------------------------------
# Filters: RBJ cookbook biquads, direct form 1
# --------------------------------------------------------------------------
# Plain one-pole filters are not enough here. A hi-hat is noise with everything
# below ~5kHz taken away, and a one-pole rolls off at 6dB/octave, which leaves
# audible low rumble in what should be air. 12dB/octave is the difference
# between "noise" and "cymbal".


def _norm(b0, b1, b2, a0, a1, a2):
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def lowpass(f0, q=0.707):
    w0 = TWO_PI * f0 / SR
    c, s = math.cos(w0), math.sin(w0)
    alpha = s / (2.0 * q)
    return _norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha)


def highpass(f0, q=0.707):
    w0 = TWO_PI * f0 / SR
    c, s = math.cos(w0), math.sin(w0)
    alpha = s / (2.0 * q)
    return _norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha)


def bandpass(f0, q=1.0):
    """Constant 0 dB peak gain."""
    w0 = TWO_PI * f0 / SR
    c, s = math.cos(w0), math.sin(w0)
    alpha = s / (2.0 * q)
    return _norm(alpha, 0.0, -alpha, 1 + alpha, -2 * c, 1 - alpha)


def biquad(x, coeffs):
    b0, b1, b2, a1, a2 = coeffs
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, xv in enumerate(x):
        yv = b0 * xv + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out[i] = yv
        x2, x1 = x1, xv
        y2, y1 = y1, yv
    return out


# --------------------------------------------------------------------------
# Building blocks
# --------------------------------------------------------------------------


def noise(n, seed):
    rnd = random.Random(seed)
    return [rnd.uniform(-1.0, 1.0) for _ in range(n)]


def decay(n, tau, curve=1.0):
    """Exponential fall. curve > 1 bends it steeper at the start."""
    return [math.exp(-((i / SR) / tau) ** curve) for i in range(n)]


def sweep_sine(n, f_start, f_end, f_tau, phase=0.0):
    """Sine whose pitch falls exponentially from f_start to f_end.

    A drum membrane's pitch drop is what the ear reads as the body of a kick;
    a fixed-frequency sine reads as a test tone instead.
    """
    out = [0.0] * n
    ph = phase
    for i in range(n):
        f = f_end + (f_start - f_end) * math.exp(-(i / SR) / f_tau)
        ph += TWO_PI * f / SR
        out[i] = math.sin(ph)
    return out


def partials(n, base, ratios, tau, seed):
    """Inharmonic additive stack: the metal in a cymbal or a rim.

    The ratios are deliberately not integers. Integer ratios give a pitched,
    harmonic tone, which is a bell; a cymbal is what you get when no partial
    is a multiple of any other.
    """
    rnd = random.Random(seed)
    out = [0.0] * n
    for k, r in enumerate(ratios):
        f = base * r
        if f >= SR * 0.47:  # above Nyquist, it would alias into a whistle
            continue
        ph = rnd.uniform(0, TWO_PI)
        t = tau * (0.55 + 0.45 * math.exp(-0.28 * k))  # highs die first
        step = TWO_PI * f / SR
        amp = 1.0 / (1.0 + 0.55 * k)
        for i in range(n):
            out[i] += amp * math.sin(ph + step * i) * math.exp(-(i / SR) / t)
    return out


def mix(n, *layers):
    out = [0.0] * n
    for layer in layers:
        for i in range(min(n, len(layer))):
            out[i] += layer[i]
    return out


def apply_env(x, env):
    return [v * e for v, e in zip(x, env)]


def saturate(x, drive):
    """Soft clip. Unity gain at low level, so it adds grit without volume."""
    if drive <= 0:
        return x
    k = math.tanh(drive)
    return [math.tanh(v * drive) / k for v in x]


def bitcrush(x, bits):
    steps = float(1 << (bits - 1))
    return [round(v * steps) / steps for v in x]


def fade_in(x, ms=0.4):
    """Kills the DC step at sample zero without softening the transient."""
    n = min(len(x), int(SR * ms / 1000.0))
    for i in range(n):
        x[i] *= i / n
    return x


def fade_out(x, ms=3.0):
    """The tail has to reach zero: the mixer only fades what it truncates."""
    n = min(len(x), int(SR * ms / 1000.0))
    base = len(x) - n
    for i in range(n):
        x[base + i] *= 1.0 - (i / n)
    return x


def to_peak(x, target):
    peak = max((abs(v) for v in x), default=0.0)
    if peak <= 0.0:
        return x
    g = target / peak
    return [v * g for v in x]


def seconds(s):
    return int(s * SR)


# --------------------------------------------------------------------------
# The instruments
# --------------------------------------------------------------------------
# Each takes a `v` dict of per-kit variations, so the three kits are the same
# synthesis with different numbers rather than three piles of copied code.


def make_kick(v, seed):
    n = seconds(v["kick_dur"])
    body = sweep_sine(n, v["kick_f0"], v["kick_f1"], v["kick_ftau"])
    body = apply_env(body, decay(n, v["kick_tau"], v["kick_curve"]))
    body = saturate(body, v["kick_drive"])

    # The click is what makes a kick cut through a mix on small speakers: the
    # sine alone disappears on anything without a woofer.
    click = biquad(noise(n, seed), highpass(1800, 0.8))
    click = apply_env(click, decay(n, v["kick_click_tau"]))
    click = [c * v["kick_click"] for c in click]

    out = mix(n, body, click)
    out = biquad(out, lowpass(v["lp"], 0.7))
    return fade_out(fade_in(to_peak(out, 0.95)))


def make_snare(v, seed):
    n = seconds(v["snare_dur"])
    # Two membranes, a fifth apart and detuned: one sine reads as a tom.
    t1 = apply_env(sweep_sine(n, v["sn_f1"] * 1.12, v["sn_f1"], 0.012),
                   decay(n, v["sn_tone_tau"]))
    t2 = apply_env(sweep_sine(n, v["sn_f2"] * 1.10, v["sn_f2"], 0.010),
                   decay(n, v["sn_tone_tau"] * 0.8))
    tone = [(a + 0.7 * b) * v["sn_tone"] for a, b in zip(t1, t2)]

    rattle = biquad(noise(n, seed), bandpass(v["sn_noise_f"], 0.55))
    rattle = biquad(rattle, highpass(420, 0.7))
    rattle = apply_env(rattle, decay(n, v["sn_noise_tau"], 1.3))
    rattle = [r * v["sn_noise"] for r in rattle]

    out = biquad(mix(n, tone, rattle), lowpass(v["lp"], 0.7))
    return fade_out(fade_in(to_peak(out, 0.86)))


def _hat(v, seed, dur, tau):
    n = seconds(dur)
    # Noise plus a thin metal stack: pure noise reads as a shaker, not a hat.
    air = biquad(noise(n, seed), highpass(v["hat_hp"], 0.7))
    air = biquad(air, bandpass(v["hat_bp"], 0.8))
    metal = partials(n, v["hat_base"], [1.0, 1.47, 1.93, 2.51, 3.13, 4.07],
                     tau * 0.7, seed + 1)
    out = mix(n, [a * 1.0 for a in air], [m * v["hat_metal"] for m in metal])
    out = apply_env(out, decay(n, tau, 1.25))
    out = biquad(out, highpass(v["hat_hp"] * 0.85, 0.7))
    return out


def make_ch(v, seed):
    out = _hat(v, seed, v["ch_dur"], v["ch_tau"])
    return fade_out(fade_in(to_peak(out, 0.46)), 2.0)


def make_oh(v, seed):
    out = _hat(v, seed, v["oh_dur"], v["oh_tau"])
    return fade_out(fade_in(to_peak(out, 0.54)))


def make_clap(v, seed):
    n = seconds(v["clap_dur"])
    out = [0.0] * n
    src = biquad(noise(n, seed), bandpass(v["clap_f"], 0.62))
    src = biquad(src, highpass(520, 0.7))
    # Three slapping hands, then the room. A clap is not one burst: the spacing
    # of the first bursts is the whole character of the sound.
    for k, off_ms in enumerate(v["clap_taps"]):
        off = int(SR * off_ms / 1000.0)
        amp = 1.0 - 0.16 * k
        for i in range(off, n):
            out[i] += src[i - off] * amp * math.exp(-((i - off) / SR) / 0.0042)
    tail_off = int(SR * v["clap_taps"][-1] / 1000.0)
    for i in range(tail_off, n):
        out[i] += src[i - tail_off] * 0.62 * math.exp(-((i - tail_off) / SR) / v["clap_tau"])
    out = biquad(out, lowpass(v["lp"], 0.7))
    return fade_out(fade_in(to_peak(out, 0.72)))


def make_ride(v, seed):
    """FLEX2, the one slot with 1.5s to spend."""
    n = seconds(1.5)
    metal = partials(n, v["ride_base"],
                     [1.0, 1.34, 1.71, 2.19, 2.78, 3.41, 4.19, 5.07, 6.23, 7.61],
                     v["ride_tau"], seed)
    wash = biquad(noise(n, seed + 7), highpass(v["ride_hp"], 0.7))
    wash = apply_env(wash, decay(n, v["ride_tau"] * 1.15, 0.85))
    ping = biquad(noise(n, seed + 3), bandpass(v["ride_hp"] * 1.6, 1.4))
    ping = apply_env(ping, decay(n, 0.03))

    out = mix(n, [m * 0.75 for m in metal], [w * v["ride_wash"] for w in wash],
              [p * 0.5 for p in ping])
    out = biquad(out, highpass(v["ride_hp"] * 0.8, 0.7))
    out = biquad(out, lowpass(v["lp"], 0.7))
    # 12ms out, longer than the rest: this one is cut by its own length, and a
    # step at the end of a cymbal is the click the mixer's fade exists to stop.
    return fade_out(fade_in(to_peak(out, 0.58)), 12.0)


def make_tom(v, seed):
    n = seconds(v["tom_dur"])
    body = sweep_sine(n, v["tom_f0"], v["tom_f1"], 0.030)
    body = apply_env(body, decay(n, v["tom_tau"], 1.1))
    skin = biquad(noise(n, seed), bandpass(v["tom_f0"] * 3.2, 0.9))
    skin = apply_env(skin, decay(n, 0.014))
    out = mix(n, body, [s * 0.30 for s in skin])
    out = saturate(out, v["kick_drive"] * 0.5)
    out = biquad(out, lowpass(v["lp"], 0.7))
    return fade_out(fade_in(to_peak(out, 0.80)))


def make_rim(v, seed):
    n = seconds(0.075)
    metal = partials(n, v["rim_base"], [1.0, 1.58, 2.24, 3.11, 4.36], 0.0085, seed)
    tick = biquad(noise(n, seed + 2), bandpass(v["rim_base"] * 1.9, 1.1))
    tick = apply_env(tick, decay(n, 0.0032))
    out = mix(n, metal, [t * 0.85 for t in tick])
    out = biquad(out, highpass(700, 0.7))
    out = biquad(out, lowpass(v["lp"], 0.7))
    return fade_out(fade_in(to_peak(out, 0.62)), 2.0)


# The role suffix is not decoration: tools/verify_blob.py checks that entry
# i % 8 ends with ROLES[i % 8], because the firmware goes by position and would
# happily play a hi-hat on the kick slot without noticing. Naming the FLEX
# slots after the sound in them (CLAP, RIDE, ...) reads better on the display
# and switches that check off, so the canonical names win and what each FLEX
# actually holds is documented in README.md instead.
#
# FLEX3 is the rim and FLEX4 the tom, matching the loader's picker and the GM
# notes they answer to (37 and 45). The first build had them the other way
# round. The third field is the seed index each sound was first made with, so
# swapping the slots left every sample byte-identical to the approved ones.
ROLES = [
    ("KICK", make_kick, 0),
    ("SNARE", make_snare, 1),
    ("CH", make_ch, 2),
    ("OH", make_oh, 3),
    ("FLEX1", make_clap, 4),  # a clap
    ("FLEX2", make_ride, 5),  # a ride, and the only slot allowed 1.5s
    ("FLEX3", make_rim, 7),   # a rimshot
    ("FLEX4", make_tom, 6),   # a tom
]


# --------------------------------------------------------------------------
# The three kits
# --------------------------------------------------------------------------
# SYN_ because they are synthesised and not sampled off a machine: the name on
# the display should not claim to be a TR-808 when it is not one.

BASE = {
    "lp": 9500.0,
    "kick_dur": 0.62, "kick_f0": 58.0, "kick_f1": 42.0, "kick_ftau": 0.022,
    "kick_tau": 0.40, "kick_curve": 1.0, "kick_drive": 0.0,
    "kick_click": 0.11, "kick_click_tau": 0.0026,
    "snare_dur": 0.30, "sn_f1": 186.0, "sn_f2": 331.0, "sn_tone": 0.85,
    "sn_tone_tau": 0.075, "sn_noise": 0.80, "sn_noise_f": 1650.0,
    "sn_noise_tau": 0.115,
    "hat_hp": 5600.0, "hat_bp": 7800.0, "hat_base": 2410.0, "hat_metal": 0.34,
    "ch_dur": 0.10, "ch_tau": 0.0145, "oh_dur": 0.46, "oh_tau": 0.105,
    "clap_dur": 0.34, "clap_f": 1220.0, "clap_tau": 0.085,
    "clap_taps": (0.0, 9.5, 19.0, 28.0),
    "ride_base": 512.0, "ride_tau": 0.62, "ride_hp": 3100.0, "ride_wash": 0.30,
    "tom_dur": 0.36, "tom_f0": 214.0, "tom_f1": 148.0, "tom_tau": 0.165,
    "rim_base": 1720.0,
}


def kit(name, **over):
    v = dict(BASE)
    v.update(over)
    return name, v


KITS = [
    # Long, round, tuneful: the sine tail is the sound, so nothing saturates it
    # and the hats stay restrained.
    kit("SYN_808"),

    # Short and hard. The kick drops further and faster and is driven, the
    # snare trades membrane for noise, the hats are brighter and tighter.
    kit("SYN_909",
        kick_dur=0.42, kick_f0=79.0, kick_f1=49.0, kick_ftau=0.011,
        kick_tau=0.21, kick_curve=1.25, kick_drive=2.6,
        kick_click=0.26, kick_click_tau=0.0019,
        snare_dur=0.26, sn_f1=203.0, sn_f2=356.0, sn_tone=0.52,
        sn_tone_tau=0.045, sn_noise=1.00, sn_noise_f=2150.0,
        sn_noise_tau=0.095,
        hat_hp=6400.0, hat_bp=8600.0, hat_base=2810.0, hat_metal=0.30,
        ch_dur=0.075, ch_tau=0.0098, oh_dur=0.34, oh_tau=0.072,
        clap_f=1480.0, clap_tau=0.062, clap_taps=(0.0, 7.0, 14.5, 21.0),
        ride_base=596.0, ride_tau=0.50, ride_hp=3700.0, ride_wash=0.36,
        tom_dur=0.28, tom_f0=248.0, tom_f1=176.0, tom_tau=0.120),

    # Dusty. Everything darker and slower, softly saturated, and quantised to
    # 10 bits at the end so the tails grain up the way old samplers did.
    kit("SYN_DUST",
        lp=4200.0,
        kick_dur=0.72, kick_f0=52.0, kick_f1=38.0, kick_ftau=0.030,
        kick_tau=0.50, kick_drive=1.5,
        kick_click=0.06, kick_click_tau=0.0040,
        snare_dur=0.34, sn_f1=172.0, sn_f2=298.0, sn_tone=1.00,
        sn_tone_tau=0.095, sn_noise=0.58, sn_noise_f=1180.0,
        sn_noise_tau=0.140,
        hat_hp=3400.0, hat_bp=4300.0, hat_base=1780.0, hat_metal=0.44,
        ch_dur=0.12, ch_tau=0.0190, oh_dur=0.52, oh_tau=0.135,
        clap_f=980.0, clap_tau=0.105, clap_taps=(0.0, 11.5, 23.0, 33.0),
        ride_base=430.0, ride_tau=0.78, ride_hp=2200.0, ride_wash=0.26,
        tom_dur=0.42, tom_f0=186.0, tom_f1=128.0, tom_tau=0.200,
        rim_base=1310.0),
]

DUSTY = {"SYN_DUST"}


def write_wav(path, samples):
    frames = bytearray()
    clipped = 0
    for v in samples:
        s = int(round(v * 32767.0))
        if s > 32767:
            s, clipped = 32767, clipped + 1
        elif s < -32768:
            s, clipped = -32768, clipped + 1
        frames += struct.pack("<h", s)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))
    return clipped


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "wav"
    os.makedirs(out_dir, exist_ok=True)

    order = []
    print("Synthesising %d kits of %d, %d Hz mono" % (len(KITS), len(ROLES), SR))
    print("=" * 62)
    total = 0.0
    for k, (kit_name, v) in enumerate(KITS):
        for role, fn, seed_index in ROLES:
            seed = 1000 * (k + 1) + seed_index
            audio = fn(v, seed)
            if kit_name in DUSTY:
                audio = bitcrush(audio, 10)
            name = "%s_%s" % (kit_name, role)
            assert len(name) <= 23, "%s is too long for the 24-byte name field" % name
            path = os.path.join(out_dir, name + ".wav")
            clipped = write_wav(path, audio)
            order.append(name + ".wav")
            dur = len(audio) / SR
            total += dur
            print("  %-20s %5.3fs  %6d samples%s"
                  % (name, dur, len(audio), "  CLIPPED %d" % clipped if clipped else ""))
        print("  " + "-" * 58)

    with open(os.path.join(out_dir, "kits.txt"), "w") as f:
        f.write("# Slot order is the firmware's: KICK SNARE CH OH FLEX1..FLEX4.\n")
        f.write("# The order of these lines IS the order of the library, so do not sort.\n")
        for i, fn in enumerate(order):
            if i % len(ROLES) == 0:
                f.write("\n# --- kit %d ---\n" % (i // len(ROLES)))
            f.write(fn + "\n")

    print("=" * 62)
    print("  %d samples, %.2fs of audio, %.0f KiB of PCM"
          % (len(order), total, total * SR * 2 / 1024.0))
    print("  list written to %s/kits.txt" % out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
