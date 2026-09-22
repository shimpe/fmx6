#include "SC_PlugIn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

static InterfaceTable* ft;

namespace {

constexpr int kNumOps = 6;
constexpr int kNumControls = 3; // frequency, phase offset, amplitude
constexpr int kNumMatrix = kNumOps * kNumOps;
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kInvTwoPi = 1.0 / kTwoPi;

// Input layout:
//   0 .. 17  : six [freq, phase, amp] triplets
//   18 .. 23 : waveform selectors
//   24 .. 29 : waveform shape (currently pulse duty; reserved for future waves)
//   30 .. 35 : per-operator harmonic limits
//   36 .. 71 : 6x6 phase-modulation matrix, row-major (destination, source)
constexpr int kCtlBase = 0;
constexpr int kWaveBase = kNumOps * kNumControls;
constexpr int kShapeBase = kWaveBase + kNumOps;
constexpr int kHarmBase = kShapeBase + kNumOps;
constexpr int kModBase = kHarmBase + kNumOps;
constexpr int kNumInputs = kModBase + kNumMatrix;

inline int ctlIndex(int op, int field) { return kCtlBase + op * kNumControls + field; }
inline int freqIndex(int op) { return ctlIndex(op, 0); }
inline int phaseIndex(int op) { return ctlIndex(op, 1); }
inline int ampIndex(int op) { return ctlIndex(op, 2); }
inline int waveIndex(int op) { return kWaveBase + op; }
inline int shapeIndex(int op) { return kShapeBase + op; }
inline int harmIndex(int op) { return kHarmBase + op; }
inline int modIndex(int dest, int src) { return kModBase + dest * kNumOps + src; }

struct FMX6Op {
    double phaseCycles;
    float freq;
    float phaseOffset;
    float amp;
    float lastPhaseMod;
    float mem;
    float sampleHold;
    std::uint32_t rng;
};

struct FMX6 : public Unit {
    FMX6Op ops[kNumOps];
    float mod[kNumMatrix];
};

inline std::uint32_t xorshift32(std::uint32_t& state) {
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

inline float randBipolar(std::uint32_t& state) {
    // 24 useful high bits mapped approximately to [-1, 1).
    const std::uint32_t x = xorshift32(state) >> 8;
    return static_cast<float>(x) * (2.0f / 16777216.0f) - 1.0f;
}

inline int clampHarmonics(float effectiveHz, int requested, float sampleRate) {
    requested = std::max(1, std::min(requested, 128));
    const float f = std::fabs(effectiveHz);
    if (f < 1.0e-6f) return requested;

    // Keep the highest represented harmonic below ~90% of Nyquist.
    const float safeNyquist = 0.45f * sampleRate;
    const int byFrequency = static_cast<int>(std::floor(safeNyquist / f));
    return std::max(1, std::min(requested, byFrequency));
}

inline float waveformHarmonic(int wave, double phaseRad, int harmonics, float shape) {
    // Reduce large phase arguments to retain numerical precision under deep PM.
    const double p = std::remainder(phaseRad, kTwoPi);
    const double s1 = std::sin(p);
    const double c1 = std::cos(p);

    if (wave == 0) return static_cast<float>(s1);

    double sinN = s1;
    double cosN = c1;
    double sum = 0.0;

    const float duty = std::max(0.01f, std::min(shape, 0.99f));

    for (int n = 1; n <= harmonics; ++n) {
        switch (wave) {
        case 1: // triangle: odd harmonics, alternating sign, 1/n^2
            if (n & 1) {
                const int oddIndex = (n - 1) / 2;
                const double sign = (oddIndex & 1) ? -1.0 : 1.0;
                sum += sign * sinN / (static_cast<double>(n) * static_cast<double>(n));
            }
            break;

        case 2: // rising-ish saw: sine series
            sum += -sinN / static_cast<double>(n);
            break;

        case 3: // square: odd harmonics
            if (n & 1) sum += sinN / static_cast<double>(n);
            break;

        case 4: { // pulse, shape = duty cycle
            // Fourier series for +1 on [0, duty) and -1 elsewhere.
            const double nd = kTwoPi * static_cast<double>(n) * static_cast<double>(duty);
            const double a = 2.0 * std::sin(nd) / (kPi * static_cast<double>(n));
            const double b = 2.0 * (1.0 - std::cos(nd)) / (kPi * static_cast<double>(n));
            sum += a * cosN + b * sinN;
            break;
        }

        default:
            break;
        }

        // Recurrence for sin((n+1)p), cos((n+1)p): one sin/cos pair per operator/sample.
        const double nextSin = sinN * c1 + cosN * s1;
        const double nextCos = cosN * c1 - sinN * s1;
        sinN = nextSin;
        cosN = nextCos;
    }

    switch (wave) {
    case 1:
        return static_cast<float>((8.0 / (kPi * kPi)) * sum);
    case 2:
        return static_cast<float>((2.0 / kPi) * sum);
    case 3:
        return static_cast<float>((4.0 / kPi) * sum);
    case 4:
        return static_cast<float>((2.0 * duty - 1.0) + sum);
    default:
        return static_cast<float>(s1);
    }
}

inline float evalWaveform(FMX6Op& op, int wave, double phaseRad, float effectiveHz,
                          int harmonicLimit, float shape, float sampleRate) {
    switch (wave) {
    case 0: // sine
    case 1: // triangle
    case 2: // saw
    case 3: // square
    case 4: { // pulse
        const int harmonics = clampHarmonics(effectiveHz, harmonicLimit, sampleRate);
        return waveformHarmonic(wave, phaseRad, harmonics, shape);
    }

    case 5: // sample-and-hold noise; base oscillator cycles clock the random updates
        return op.sampleHold;

    case 6: // white noise; phase modulation intentionally has no effect
        return randBipolar(op.rng);

    default:
        return static_cast<float>(std::sin(std::remainder(phaseRad, kTwoPi)));
    }
}

void FMX6_next(FMX6* unit, int inNumSamples) {
    const float sampleRate = SAMPLERATE;
    const float sampleDur = SAMPLEDUR;

    float freq[kNumOps];
    float freqInc[kNumOps];
    float phaseOffset[kNumOps];
    float phaseOffsetInc[kNumOps];
    float amp[kNumOps];
    float ampInc[kNumOps];

    int wave[kNumOps];
    float shape[kNumOps];
    int harmonicLimit[kNumOps];

    float mod[kNumMatrix];
    float modInc[kNumMatrix];

    float* outs[kNumOps];

    for (int i = 0; i < kNumOps; ++i) {
        FMX6Op& op = unit->ops[i];

        freq[i] = op.freq;
        freqInc[i] = CALCSLOPE(IN0(freqIndex(i)), freq[i]);

        phaseOffset[i] = op.phaseOffset;
        phaseOffsetInc[i] = CALCSLOPE(IN0(phaseIndex(i)), phaseOffset[i]);

        amp[i] = op.amp;
        ampInc[i] = CALCSLOPE(IN0(ampIndex(i)), amp[i]);

        wave[i] = std::max(0, std::min(6, static_cast<int>(std::lround(IN0(waveIndex(i))))));
        shape[i] = IN0(shapeIndex(i));
        harmonicLimit[i] = std::max(1, std::min(128, static_cast<int>(std::lround(IN0(harmIndex(i))))));

        outs[i] = OUT(i);
    }

    for (int i = 0; i < kNumMatrix; ++i) {
        mod[i] = unit->mod[i];
        modInc[i] = CALCSLOPE(IN0(kModBase + i), mod[i]);
    }

    for (int s = 0; s < inNumSamples; ++s) {
        float matrixPhaseMod[kNumOps] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        float nextMem[kNumOps];
        double nextPhase[kNumOps];
        float nextPhaseModState[kNumOps];

        // All modulation for this sample is derived from the previous sample's operator outputs.
        // This exactly preserves the useful FM7 property that cycles/self-feedback cost one sample,
        // rather than one server block.
        for (int dest = 0; dest < kNumOps; ++dest) {
            float pm = 0.f;
            const int row = dest * kNumOps;
            for (int src = 0; src < kNumOps; ++src) {
                pm += mod[row + src] * unit->ops[src].mem;
            }
            matrixPhaseMod[dest] = pm;
        }

        for (int i = 0; i < kNumOps; ++i) {
            FMX6Op& op = unit->ops[i];

            const float totalPhaseMod = phaseOffset[i] + matrixPhaseMod[i];
            const float pmDelta = totalPhaseMod - op.lastPhaseMod;
            const float effectiveHz = freq[i] + pmDelta * sampleRate * static_cast<float>(kInvTwoPi);

            const double phaseRad = op.phaseCycles * kTwoPi + static_cast<double>(totalPhaseMod);

            double pNext = op.phaseCycles + static_cast<double>(freq[i]) * static_cast<double>(sampleDur);
            const bool wrapped = (pNext >= 1.0 || pNext < 0.0);
            pNext -= std::floor(pNext);

            const float osc = evalWaveform(
                op, wave[i], phaseRad, effectiveHz, harmonicLimit[i], shape[i], sampleRate);

            const float y = amp[i] * osc;
            outs[i][s] = y;

            // For S&H, change the held value at the boundary so the new value
            // begins on the next oscillator cycle (not one sample early).
            if (wave[i] == 5 && wrapped) op.sampleHold = randBipolar(op.rng);

            nextMem[i] = y;
            nextPhase[i] = pNext;
            nextPhaseModState[i] = totalPhaseMod;
        }

        // Commit operator states simultaneously after all six outputs were calculated.
        for (int i = 0; i < kNumOps; ++i) {
            FMX6Op& op = unit->ops[i];
            op.mem = nextMem[i];
            op.phaseCycles = nextPhase[i];
            op.lastPhaseMod = nextPhaseModState[i];

            freq[i] += freqInc[i];
            phaseOffset[i] += phaseOffsetInc[i];
            amp[i] += ampInc[i];
        }

        for (int i = 0; i < kNumMatrix; ++i) mod[i] += modInc[i];
    }

    for (int i = 0; i < kNumOps; ++i) {
        unit->ops[i].freq = freq[i];
        unit->ops[i].phaseOffset = phaseOffset[i];
        unit->ops[i].amp = amp[i];
    }
    for (int i = 0; i < kNumMatrix; ++i) unit->mod[i] = mod[i];
}

void FMX6_clear(FMX6* unit, int inNumSamples) {
    ClearUnitOutputs(unit, inNumSamples);
}

void FMX6_Ctor(FMX6* unit) {
    SETCALC(FMX6_next);

    if (unit->mNumInputs != kNumInputs || unit->mNumOutputs != kNumOps) {
        Print("FMX6: expected %d inputs and %d outputs, got %d inputs and %d outputs.\n",
              kNumInputs, kNumOps, unit->mNumInputs, unit->mNumOutputs);
        SETCALC(FMX6_clear);
        ClearUnitOutputs(unit, 1);
        return;
    }

    for (int i = 0; i < kNumOps; ++i) {
        FMX6Op& op = unit->ops[i];
        op.phaseCycles = 0.0;
        op.freq = IN0(freqIndex(i));
        op.phaseOffset = IN0(phaseIndex(i));
        op.amp = IN0(ampIndex(i));
        op.lastPhaseMod = op.phaseOffset;
        op.mem = 0.f;
        op.rng = 0x9E3779B9u ^ (0x85EBCA6Bu * static_cast<std::uint32_t>(i + 1));
        op.sampleHold = randBipolar(op.rng);
    }

    for (int i = 0; i < kNumMatrix; ++i) unit->mod[i] = IN0(kModBase + i);

    ClearUnitOutputs(unit, 1);
}

} // namespace

PluginLoad(FMX6) {
    ft = inTable;
    DefineSimpleUnit(FMX6);
}
