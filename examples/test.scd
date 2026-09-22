/*
FMX6 smoke tests.
After compiling/installing the plugin and recompiling the class library:

s.boot;
*/

// ------------------------------------------------------------
// 1. One sine operator: OP1 only
// ------------------------------------------------------------
(
{
    var ctl = [
        [220, 0, 0.2],
        [0,   0, 0],
        [0,   0, 0],
        [0,   0, 0],
        [0,   0, 0],
        [0,   0, 0]
    ];
    var mod = Array.fill2D(6, 6, 0);
    FMX6.ar(ctl, mod, waves: 0)[0] ! 2;
}.play;
)

// ------------------------------------------------------------
// 2. Classic 2-op sine PM: OP2 -> OP1
// ------------------------------------------------------------
(
{
    var ctl = [
        [110, 0, 0.18],
        [220, 0, 1.0],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0]
    ];
    var mod = FMX6.modMatrix([0, 1, 4.0]);
    FMX6.ar(ctl, mod, waves: 0)[0] ! 2;
}.play;
)

// ------------------------------------------------------------
// 3. Saw modulator -> triangle carrier
// harmonicLimits caps additive complexity per operator.
// ------------------------------------------------------------
(
{
    var ctl = [
        [110, 0, 0.15],
        [165, 0, 0.9],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0]
    ];
    var mod = FMX6.modMatrix([0, 1, 2.5]);
    var ops = FMX6.ar(
        ctl,
        mod,
        waves: [FMX6.triangle, FMX6.saw, 0, 0, 0, 0],
        harmonicLimits: [32, 24, 16, 16, 16, 16]
    );
    ops[0] ! 2;
}.play;
)

// ------------------------------------------------------------
// 4. Matrix feedback and shared modulation
// OP3 -> OP2 -> OP1, OP2 self-feedback, saw/pulse waves.
// ------------------------------------------------------------
(
{
    var base = 82.41;
    var ctl = [
        [base,     0, 0.12],
        [base * 2, 0, 0.75],
        [base * 3, 0, 0.70],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0]
    ];
    var mod = FMX6.modMatrix(
        [0, 1, 3.0],
        [1, 2, 2.0],
        [1, 1, 0.55]
    );
    var ops = FMX6.ar(
        ctl,
        mod,
        waves: [FMX6.triangle, FMX6.saw, FMX6.pulse, 0, 0, 0],
        shapes: [0.5, 0.5, 0.31, 0.5, 0.5, 0.5],
        harmonicLimits: 24
    );
    LeakDC.ar(ops[0]).tanh ! 2;
}.play;
)

// ------------------------------------------------------------
// 5. Noise operator used as a PM source
// White noise ignores incoming phase modulation itself, but its
// output can modulate any periodic operator through the matrix.
// ------------------------------------------------------------
(
{
    var ctl = [
        [110, 0, 0.12],
        [440, 0, 0.35],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0],
        [0, 0, 0]
    ];
    var mod = FMX6.modMatrix([0, 1, 1.2]);
    FMX6.ar(
        ctl,
        mod,
        waves: [FMX6.sine, FMX6.whiteNoise, 0, 0, 0, 0]
    )[0] ! 2;
}.play;
)
