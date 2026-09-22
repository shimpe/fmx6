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

For the periodic non-sine waveforms, harmonic count is dynamically capped using an estimate of instantaneous frequency (base frequency plus the sample-to-sample derivative of phase modulation). This is substantially safer than evaluating a naive discontinuous saw/pulse directly at an arbitrarily phase-modulated phase.

**It is not mathematically alias-free.** Deep/high-rate PM can itself create sidebands above Nyquist even when the underlying waveform's Fourier series is band-limited. A future version could add internal oversampling and proper downsampling.

## API

```supercollider
FMX6.ar(
    ctlMatrix,
    modMatrix,
    waves: 0,
    shapes: 0.5,
    harmonicLimits: 32
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
