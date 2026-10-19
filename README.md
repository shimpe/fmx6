# FMX6UGens

Experimental SuperCollider server plugin implementing a six-operator phase-modulation matrix with selectable operator waveforms.

## What it is

FMX6 follows the useful core model of `FM7`: six operators, six outputs, and a 6x6 matrix whose rows are modulation destinations and columns are modulation sources. Each sample's matrix is calculated from the previous sample's six operator outputs, so arbitrary cycles and self-feedback are possible with a one-sample delay.

Extra operator waveforms:

- 0 sine
- 1 triangle (band-limited-ish additive Fourier series)
- 2 saw (band-limited-ish additive Fourier series)
- 3 square (band-limited-ish additive Fourier series)
- 4 pulse (band-limited-ish additive Fourier series; `shape` is duty cycle)
- 5 sample-and-hold noise
- 6 white noise
- 7 filtered noise (white noise through a 2-pole state-variable filter, Q = 0.707; the operator frequency is the cutoff, `shape` selects low-pass 0, band-pass 0.5, high-pass 1, crossfaded in between)

For the periodic non-sine waveforms, harmonic count is dynamically capped using an estimate of instantaneous frequency (base frequency plus the sample-to-sample derivative of phase modulation). This is substantially safer than evaluating a naive discontinuous saw/pulse directly at an arbitrarily phase-modulated phase.

**On its own it is not mathematically alias-free.** Deep/high-rate PM can itself create sidebands above Nyquist even when the underlying waveform's Fourier series is band-limited. The `oversample` argument (2 or 4) runs the whole matrix, feedback included, at that multiple of the sample rate and decimates each operator output with linear-phase halfband FIR filters: 67 taps for the last factor of two (passband to 0.4 of the sample rate, ripple 0.0001 dB, stopband 102 dB) and 23 taps before it for 4x (98 dB). Aliasing products then stay around -110 dB or lower, except for those that fold into the top 20% of the band (above 19.2 kHz at 48 kHz). It costs about 2x or 4x the CPU and adds 16.5 or 19.25 samples of latency. The noise waveforms are scaled so that their level does not depend on the factor. With `oversample: 1`, the default, the output is identical to earlier versions, and SynthDefs built without the argument keep working.

## API

```supercollider
FMX6.ar(
    ctlMatrix,
    modMatrix,
    waves: 0,
    shapes: 0.5,
    harmonicLimits: 32,
    oversample: 1,
    filters: nil
)
```

`ctlMatrix` is 6 x 3:

```supercollider
[
    [freqHz, phaseRadians, amp], // OP1
    [freqHz, phaseRadians, amp], // OP2
    ...
]
```

`modMatrix` is 6 x 6, row = destination, column = source. Matrix values are phase modulation in radians per unit source amplitude.

## Operator filters

`filters` is optional: six rows `[type, freq, octaves, q, formantRatio, formantLevel, gainDb]`, one per operator. The filter
sits between the waveform and the amplitude: what the operator outputs and what it modulates other operators with is
the filtered signal. type: 0 off, 1 low-pass 2-pole, 2 low-pass 4-pole, 3 high-pass 2-pole, 4 high-pass 4-pole,
5 band-pass (unity gain at the centre), 6 notch, 7 peak (gainDb at the centre), 8 formant (band-pass peaks at freq and
freq * formantRatio, the second at formantLevel). The frequency is `freq * 2 ** octaves`; drive `octaves`
with an EnvGen for a filter sweep. q from 0.5 to 20, for a formant filter 0.5 to 50. Without this argument the UGen
behaves as before.

```supercollider
(
{
    var ctl = FMX6.controlMatrix([0, 110, 0, 0.2]);
    var sweep = 3 * EnvGen.kr(Env([0, 1, 0], [0.05, 0.6]));
    var filters = [[2, 220, sweep, 2, 1, 0, 0]] ++ ({ [0, 0, 0, 1, 1, 0, 0] } ! 5);
    FMX6.ar(ctl, nil, waves: [2, 0, 0, 0, 0, 0], filters: filters)[0] ! 2
}.play;
)
```

### Five formants

A row can instead have 17 fields for a formant filter with five formants, each with its own q:
`[type, freq, octaves, q, ratio2, level2, gainDb, q2, ratio3, level3, q3, ratio4, level4, q4, ratio5, level5, q5]`.
F1 is at the filter frequency with q; formant n is at F1 * ratioN with levelN (relative to F1) and qN, and a qN of 0 or
less means the same as q. The response is the sum of unity-gain band-passes, F1 + level2 * F2 + ... + level5 * F5; a
formant F3..F5 whose level is 0 is off and costs nothing. 7- and 17-field rows can be mixed (a 7-field row has F2 with
q and F3..F5 off).

With only 7-field rows the SynthDef has 115 inputs, exactly as before. With a 17-field row it has 175: the six 7-field
rows, then six rows `[q2, ratio3, level3, q3, ratio4, level4, q4, ratio5, level5, q5]`.

```supercollider
// A soprano "a": formants at 800, 1150, 2900, 3900 and 4950 Hz
(
{
    var ctl = FMX6.controlMatrix([0, 220, 0, 0.5]);
    var vowel = [8, 800, 0, 10, 1.4375, 0.5012, 0, 12.78, 3.625, 0.0251, 24.17, 4.875, 0.1, 30, 6.1875, 0.0032, 35.36];
    var filters = [vowel] ++ ({ [0, 0, 0, 1, 1, 0, 0] } ! 5);
    FMX6.ar(ctl, nil, waves: [2, 0, 0, 0, 0, 0], harmonicLimits: 64, filters: filters)[0] ! 2
}.play;
)
```

## Build

You need the SuperCollider **source tree** matching your installed SuperCollider major/minor version. The current official documentation recommends CMake for server plugins.

```bash
mkdir build
cd build
cmake -DSC_PATH=/path/to/supercollider/source ..
cmake --build . --config Release
```

The build produces `FMX6.scx` on Windows/macOS, or `FMX6.so` on Linux, and copies `Classes/FMX6.sc` next to it.

For installation, create a folder such as:

```text
<Platform.userExtensionDir>/FMX6UGens/
```

and place the compiled plugin plus the `Classes` folder there. Optionally also copy `HelpSource` and `examples`.

Then restart/recompile the SuperCollider class library and reboot the server.

## Smoke test

See `examples/test.scd`.

## Design notes

- Matrix feedback is one sample delayed, matching the important topology behavior of FM7 rather than using `LocalIn`/`LocalOut` block feedback.
- Control matrix and modulation matrix inputs are expected at scalar/control rate, like FM7-style usage. Audio-rate parameter modulation is not implemented in this first version.
- S&H noise is clocked by the operator's base phase wraps. Incoming phase modulation does not change its random update timing in v1.
- White noise ignores phase modulation as a waveform source, but can itself modulate other operators through the matrix.
