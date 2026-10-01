# Flashing muRDrum

## Which file

The same files run on every module, including the launch edition with the
original PicoDrum panel. Releases before the rename called them `picodrum_…`.

| File | Panel | Encoder |
|---|---|---|
| `murdrum_1U_enc-std.uf2` | 1U, 20HP, 0.91" 128x32 display | standard |
| `murdrum_1U_enc-rev.uf2` | 1U, 20HP, 0.91" 128x32 display | reversed |
| `murdrum_3U_enc-std.uf2` | 3U, 6HP, 0.96" 128x64 display | standard |
| `murdrum_3U_enc-rev.uf2` | 3U, 6HP, 0.96" 128x64 display | reversed |

**The panel.** Match it to the display fitted on your module. A 1U build on the
3U display draws only in the top half, and a 3U build on the 1U display loses
half its rows.

**The encoder.** Flash the one that matches the **S** / **R** mark on the back
of the panel, PCB side. If there is no mark, flash `enc-std` first. If the
knob moves the selection backwards (you turn right and it goes left), flash
`enc-rev` instead. Nothing is faulty either way, and there is nothing to
configure.

Why there are two: the two quadrature pins of an EC11 encoder are unmarked on
most parts, and makers do not agree on which is which, so the same footprint
can take a part that reports its direction the other way round. The firmware
cannot tell the two cases apart. The two binaries differ only in which pin is
read as which.

## How

1. Hold the **BOOTSEL** button on the Pico 2 while you plug in the USB cable.
2. A drive called `RPI-RP2` appears. Drag the `.uf2` onto it.
3. The module reboots into the new firmware.

With [picotool](https://github.com/raspberrypi/picotool) you can do the same
with one command:

```sh
picotool load -x murdrum_1U_enc-std.uf2
```

If the module is already running, add `-f` and you do not need to hold
BOOTSEL.

## What a firmware update keeps

- **Your samples.** The sample library sits in its own flash area, and a
  firmware update does not touch it.
- **Your settings and the eight presets.** A firmware that adds new settings
  keeps the old ones and uses defaults for the new ones.

If the module shows `NO LIBRARY`, it has no samples yet. Load a library with
the sample loader at [murlab.it/loader](https://murlab.it/loader), or flash the bundled kits by dragging
`murdrum_starter_kits.uf2` (attached to every release, and the same file as
`kits/sample_lib.uf2`) onto the `RPI-RP2` drive, the same way as the firmware.

Loading a library replaces the samples only. Presets store positions in the
library, not the sounds themselves, so a preset saved with one library plays
whatever sits in the same positions of the next one.
