// Pin map — Eurorack 1U 20HP Sample Player (Pico 2 / RP2350)
//
// Constraints honoured here:
//  - I2S on PIO: DATA / BCK / LRCK contiguous, with BCK and LRCK adjacent
//    (the PIO program drives both clocks from a single base pin).
//  - Encoder A/B contiguous, so a PIO quadrature decoder can replace the
//    polling one without rewiring anything.
//  - GP0/GP1 left free for the UART0 debug console.
//  - GP26..GP28 (ADC) left free for future CV inputs.
#ifndef PINS_H
#define PINS_H

// --- I2S to the PCM5102A (hardware mode) ---
// Board config: SCK->GND (enables the internal PLL; without it the DAC waits
// for a master clock we never generate), FLT->GND, DEMP->GND, FMT->GND (I2S
// format), XSMT->3V3.
// Watch out for XSMT: it is an ACTIVE LOW soft-mute, tied to GND the DAC stays
// silent.
#define PIN_I2S_DATA        18  // DIN
#define PIN_I2S_CLOCK_BASE  19  // GP19 = BCK, GP20 = LRCK (WS)

// --- OLED SSD1306, I2C0. 128x32 (1U) or 128x64 (6HP), picked at compile
// time with OLED_PANEL_128X64 - same pins for both, see ssd1306.h. ---
#define PIN_OLED_SDA        16
#define PIN_OLED_SCL        17
#define OLED_I2C_ADDR       0x3C
#define OLED_I2C_BAUD       400000

// --- MIDI IN (5 pin DIN via 6N138), UART1 RX only at 31250 baud ---
#define PIN_MIDI_RX          5
#define MIDI_BAUD        31250

// --- Rotary encoder with pushbutton ---
#define PIN_ENC_A           10
#define PIN_ENC_B           11
#define PIN_ENC_SW          12  // active low, internal pull-up

// Order in which the two quadrature channels are handed to the decoder.
// The swap lives here and not in the pin numbers above: those must keep naming
// the physical wiring, otherwise pins.h stops matching the board and the next
// wiring check lies. Flipping the direction is swapping these two, and nothing
// else in the firmware knows about it.
//
// Fed straight through by default (2026-09-08). The breadboard wired CLK on
// GP10 and DT on GP11 and needed the channels swapped here; the muRDrum board
// wires them the other way round (ROT_CLK on GP11, ROT_DT on GP10), so the board's swap and the software one cancelled and the knob
// turned backwards on the real module. Undoing the swap here is what makes the
// board decode CW as CW.
//
// ENCODER_REVERSED (set with MURDRUM_ENC=REV, see build.sh) swaps them back.
// It exists because the EC11 part itself is not standardised: the A and C/B
// terminals of the outer pair are not marked on most of the parts in
// circulation, and a unit soldered the other way round - or simply sourced
// from another maker - decodes CW as CCW with no wiring fault to find. The
// firmware cannot tell the two apart, so the direction is a build option and
// both UF2s are shipped.
#if defined(ENCODER_REVERSED)
#define PIN_ENC_QUAD_A      PIN_ENC_B
#define PIN_ENC_QUAD_B      PIN_ENC_A
#else
#define PIN_ENC_QUAD_A      PIN_ENC_A
#define PIN_ENC_QUAD_B      PIN_ENC_B
#endif

#endif // PINS_H
