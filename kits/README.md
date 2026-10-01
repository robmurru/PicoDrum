# muRDrum starter kits

Three kits of eight slots, **synthesised from scratch**, ready to load.

| file | what it is |
|---|---|
| `sample_lib.uf2` | **drag this onto the module.** 1873 blocks, family `absolute` (`0xe48bff57`), targeting `0x10080000` |
| `sample_lib.bin` | the same library as a raw blob, for `picotool load -o 0x10080000 sample_lib.bin` |
| `wav/` | the 24 source WAVs, 22050 Hz mono 16-bit, plus `kits.txt` |
| `make_starter_kits.py` | the synthesiser that produced them. No dependencies, stdlib only |

## Why these exist

**These three kits are what the module ships with** (decided 2026-09-23). They
had to be made rather than found: a sample pack that is free to
download is almost never free to *redistribute*: "use it in your music" and
"ship it on hardware you sell" are different permissions, and the second is
the one a preloaded module needs. MusicRadar's SampleRadar packs, for one, say
it outright: *"all we ask is that you don't re-distribute them."*

So nothing here is sampled from anything. Every sound is built out of
oscillators, noise and filters by `make_starter_kits.py`, which makes the
result muR Lab's own recording, free to ship, and reproducible: run the script
and you get these files back, byte for byte.

## The kits

Same synthesis in all three, different numbers. Measured from the finished blob:

| | kick | snare | closed hat | character |
|---|---|---|---|---|
| **SYN_808** | 43 Hz, 0.62s | 183 Hz | 7.4 kHz | long and round, the sine tail is the sound |
| **SYN_909** | 54 Hz, 0.42s | 205 Hz | 7.8 kHz | short and hard, driven kick, noisier snare |
| **SYN_DUST** | 43 Hz, 0.72s | 172 Hz | 4.0 kHz | dark and slow, low-passed and crushed to 10 bits |

The display shows the kit name, `SYN_808` and so on, because the firmware's
`mixer_kit_name()` prints the entry name up to its last underscore. **SYN_**
because they are synthesised: none of them is a TR-808 and the name should not
imply otherwise.

## What is on the FLEX slots

Slot order is fixed and **position is the role**, so the names have to be the
canonical ones: `tools/verify_blob.py` checks that entry `i % 8` ends with the
right suffix, which is the only thing standing between a typo and a library
that plays a hi-hat where the kick should be. What each FLEX actually holds:

| slot | name | sound |
|---|---|---|
| 1 | `KICK` | kick |
| 2 | `SNARE` | snare |
| 3 | `CH` | closed hat |
| 4 | `OH` | open hat |
| 5 | `FLEX1` | clap, four bursts and a room tail |
| 6 | `FLEX2` | ride, **1.5s**, the only slot allowed it |
| 7 | `FLEX3` | rimshot (GM 37, Side Stick) |
| 8 | `FLEX4` | tom (GM 45, Low Tom) |

## Loading it

Hold **BOOTSEL**, plug the USB cable in, and drag `sample_lib.uf2` onto the
drive that appears. The module reboots into the new library. Presets and
firmware are untouched.

The `wav/` folder is the other way in: drop those files into the
sample loader at [murlab.it/loader](https://murlab.it/loader) and arrange them yourself.

## Rebuilding

```sh
python3 make_starter_kits.py wav                    # 24 WAVs + wav/kits.txt
python3 ../firmware/tools/convert_wav.py \
        --list wav/kits.txt --kit-size 8 \
        -o sample_lib.bin --header ''
picotool uf2 convert sample_lib.bin -t bin \
        -o 0x10080000 --family absolute sample_lib.uf2
python3 ../firmware/tools/verify_blob.py sample_lib.bin
```

`convert_wav.py` and `verify_blob.py` live in `firmware/tools/`. **Use them rather than
writing the blob by hand**: the converter is what settles the TOC layout, the
4-byte alignment and the version-2 kit grouping, and the verifier is what
catches a slot landing in the wrong place.

## Numbers

- **468.2 KB**, 13.1% of the 3584 KB the library has to fit in
- 10.85s of audio across 24 samples, of which 4.5s is the three ride tails
- global peak **-0.45 dBFS**, nothing clipped
- blob version **2**, 3 kits of 8, `verify_blob.py` says `OK: blob consistent`

**TODO: nobody has listened to these yet.** They load on the module, but every
number on this page is arithmetic on the samples, not a speaker. Judge them by
ear before this kit goes anywhere public. The parameters are three dictionaries
at the top of `make_starter_kits.py`, one per kit, so a change is a number and
a rebuild.

Two things measured rather than assumed: the UF2's payload reconstructs
`sample_lib.bin` byte for byte with 88 bytes of zero padding in the last block,
and its blocks are contiguous from `0x10080000` in 256-byte steps.
