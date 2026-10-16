#include "SC_PlugIn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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
//   24 .. 29 : waveform shape (pulse: duty cycle; filtered noise: low-pass 0, band-pass 0.5, high-pass 1)
//   30 .. 35 : per-operator harmonic limits
//   36 .. 71 : 6x6 phase-modulation matrix, row-major (destination, source)
//   72       : oversampling factor 1, 2 or 4 (optional; read once at construction; 1 when absent)
//   73 .. 114: optional operator filters, six [type, freq, octaves, q, formantRatio, formantLevel, gainDb] rows
constexpr int kCtlBase = 0;
constexpr int kWaveBase = kNumOps * kNumControls;
constexpr int kShapeBase = kWaveBase + kNumOps;
constexpr int kHarmBase = kShapeBase + kNumOps;
constexpr int kModBase = kHarmBase + kNumOps;
constexpr int kNumInputs = kModBase + kNumMatrix;
constexpr int kOversampleIndex = kNumInputs;
constexpr int kMaxWave = 7;

// Operator filters (optional): six [type, freq, octaves, q, formantRatio, formantLevel, gainDb] rows after the
// oversampling input. type: 0 off, 1 LP 2-pole, 2 LP 4-pole, 3 HP 2-pole, 4 HP 4-pole, 5 band-pass, 6 notch, 7 peak,
// 8 formant. The frequency is freq * 2^octaves, computed here in double precision.
constexpr int kFilterBase = kOversampleIndex + 1;
constexpr int kNumFilterFields = 7;
constexpr int kNumInputsWithFilters = kFilterBase + kNumOps * kNumFilterFields;
constexpr int kMaxFilterType = 8;
constexpr double kButterworthK = 1.4142135623730951;

inline int filterIndex(int op, int field) { return kFilterBase + op * kNumFilterFields + field; }

inline int ctlIndex(int op, int field) { return kCtlBase + op * kNumControls + field; }
inline int freqIndex(int op) { return ctlIndex(op, 0); }
inline int phaseIndex(int op) { return ctlIndex(op, 1); }
inline int ampIndex(int op) { return ctlIndex(op, 2); }
inline int waveIndex(int op) { return kWaveBase + op; }
inline int shapeIndex(int op) { return kShapeBase + op; }
inline int harmIndex(int op) { return kHarmBase + op; }
inline int modIndex(int dest, int src) { return kModBase + dest * kNumOps + src; }

// Halfband decimators: Kaiser-windowed, linear phase. The centre tap is 0.5, every even offset from it is zero, and
// these are the taps at odd offsets 1, 3, 5, ... from the centre. Frequencies below are in cycles per sample at the
// input of the stage.
//
// Final stage (2x -> 1x): 67 taps, beta 10.35. Passband to 0.2 (0.4 of the output rate: 19.2 kHz at 48 kHz),
// ripple 0.00007 dB; stopband from 0.3, 102 dB. Only components above 0.4 of the output rate can alias, and they
// land above 0.4 of it again.
constexpr double kFinalTaps[] = {
    0.31687588570509284, -0.10186909104321777, 0.056829847613847524, -0.03635784539753035,
    0.024368801002835734, -0.016501836538929292, 0.011072803407119419, -0.007268077708132389,
    0.0046189186309444205, -0.0028142241434505255, 0.0016261803924266208, -0.0008791917322733311,
    0.00043639646857262306, -0.0001930582052257388, 7.213584500219032E-05, -2.0101383481845616E-05,
    2.457086399902565E-06,
};

// First stage of 4x (4x -> 2x): 23 taps, beta 10.5. Passband to 0.1, ripple 0.0001 dB; stopband from 0.4, 98 dB.
// Its transition band only has to stay clear of what the final stage passes.
constexpr double kFirstTaps[] = {
    0.3054193276781219, -0.0726917533603646, 0.02144931133186937, -0.004713801131178444,
    0.0005433070647214894, -6.3915831696255476E-06,
};

constexpr int kFinalLength = 4 * static_cast<int>(sizeof(kFinalTaps) / sizeof(double)) - 1;
constexpr int kFirstLength = 4 * static_cast<int>(sizeof(kFirstTaps) / sizeof(double)) - 1;

// Decimates by two. Keeps the last (length - 1) inputs between calls in the front of its buffer.
struct Halfband {
    const double* taps;
    int numTaps;  // taps at odd offsets
    int length;   // full filter length
    float* buffer; // (length - 1) history + room for one block of input

    void init(const double* t, int n, int len, float* memory) {
        taps = t;
        numTaps = n;
        length = len;
        buffer = memory;
        std::fill(buffer, buffer + (length - 1), 0.f);
    }

    // in may alias out: the input is copied into the buffer before anything is written.
    void process(const float* in, int numIn, float* out) {
        const int history = length - 1;
        std::memcpy(buffer + history, in, sizeof(float) * static_cast<size_t>(numIn));
        const int centre = history / 2;
        for (int m = 0; m < numIn / 2; ++m) {
            // Newest input of this output: buffer[history + 2m + 1]; the centre of the filter lies half the
            // history before it, and history = 2 * centre.
            const float* x = buffer + 2 * m + 1 + centre;
            double y = 0.5 * static_cast<double>(x[0]);
            for (int j = 0; j < numTaps; ++j) {
                const int k = 2 * j + 1;
                y += taps[j] * (static_cast<double>(x[-k]) + static_cast<double>(x[k]));
            }
            out[m] = static_cast<float>(y);
        }
        std::memmove(buffer, buffer + numIn, sizeof(float) * static_cast<size_t>(history));
    }
};

struct Svf {
    double ic1;
    double ic2;

    // The coefficients of the last frequency and damping, recomputed only when either changes: the same inputs give
    // the same coefficients, so this is bit-identical to computing them every sample. Zero frequency never matches.
    double cachedF;
    double cachedK;
    double a1;
    double a2;
    double a3;

    // Kept in the first filter of an operator: 2^octaves of the envelope offset and the gain of a peak.
    bool hasOctaves;
    double cachedOctaves;
    double octaveFactor;
    bool hasGain;
    double cachedGainDb;
    double gain;
};

struct FMX6Op {
    double phaseCycles;
    float freq;
    float phaseOffset;
    float amp;
    float lastPhaseMod;
    float mem;
    float sampleHold;
    std::uint32_t rng;
    double svfIc1;
    double svfIc2;
    float filterFreq;
    float filterOct;
    Svf filterA;
    Svf filterB;
};

struct FMX6 : public Unit {
    FMX6Op ops[kNumOps];
    float mod[kNumMatrix];
    int oversample;
    bool hasFilters;
    float* memory;
    float* scratch[kNumOps];
    Halfband finalStage[kNumOps];
    Halfband firstStage[kNumOps];
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

// Filtered noise: white noise through a TPT state-variable filter with Q = 1/sqrt(2). The cutoff is the operator
// frequency; shape 0 = low-pass, 0.5 = band-pass (unity gain at the centre), 1 = high-pass, crossfaded in between.
inline float filteredNoise(FMX6Op& op, float cutoffHz, float shape, float sampleRate, float noiseGain) {
    const double x = static_cast<double>(randBipolar(op.rng) * noiseGain);
    const double fc = std::max(10.0, std::min(static_cast<double>(std::fabs(cutoffHz)), 0.49 * static_cast<double>(sampleRate)));
    const double g = std::tan(kPi * fc / static_cast<double>(sampleRate));
    const double k = 1.4142135623730951; // 1/Q
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;

    const double v3 = x - op.svfIc2;
    const double v1 = a1 * op.svfIc1 + a2 * v3;
    const double v2 = op.svfIc2 + a2 * op.svfIc1 + a3 * v3;
    op.svfIc1 = 2.0 * v1 - op.svfIc1;
    op.svfIc2 = 2.0 * v2 - op.svfIc2;

    const double lp = v2;
    const double bp = k * v1;
    const double hp = x - k * v1 - v2;

    const double m = std::max(0.0, std::min(static_cast<double>(shape), 1.0));
    const double y = m < 0.5
        ? (1.0 - 2.0 * m) * lp + (2.0 * m) * bp
        : (2.0 - 2.0 * m) * bp + (2.0 * m - 1.0) * hp;
    return static_cast<float>(y);
}

// One TPT state-variable filter step, the same arithmetic as the filtered-noise waveform with k = 1/Q.
inline void svfTick(double x, double fc, double k, double sampleRate, Svf& s, double& v1, double& v2) {
    const double f = std::max(10.0, std::min(fc, 0.49 * sampleRate));
    if (f != s.cachedF || k != s.cachedK) {
        const double g = std::tan(kPi * f / sampleRate);
        s.a1 = 1.0 / (1.0 + g * (g + k));
        s.a2 = g * s.a1;
        s.a3 = g * s.a2;
        s.cachedF = f;
        s.cachedK = k;
    }
    const double v3 = x - s.ic2;
    v1 = s.a1 * s.ic1 + s.a2 * v3;
    v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;
}

// The operator filter. HearSay reproduces this function operation for operation (OperatorFilterKernel).
inline double operatorFilter(int type, double x, double baseHz, double octaves, double q, double formantRatio,
                             double formantLevel, double gainDb, double sampleRate, Svf& a, Svf& b) {
    if (!a.hasOctaves || octaves != a.cachedOctaves) {
        a.octaveFactor = std::pow(2.0, octaves);
        a.cachedOctaves = octaves;
        a.hasOctaves = true;
    }
    const double fc = baseHz * a.octaveFactor;
    const double k = 1.0 / std::max(0.5, std::min(q, 20.0));
    double v1 = 0.0, v2 = 0.0, w1 = 0.0, w2 = 0.0;
    switch (type) {
    case 1:
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        return v2;
    case 2:
        svfTick(x, fc, kButterworthK, sampleRate, a, v1, v2);
        svfTick(v2, fc, k, sampleRate, b, w1, w2);
        return w2;
    case 3:
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        return x - k * v1 - v2;
    case 4: {
        svfTick(x, fc, kButterworthK, sampleRate, a, v1, v2);
        const double hp1 = x - kButterworthK * v1 - v2;
        svfTick(hp1, fc, k, sampleRate, b, w1, w2);
        return hp1 - k * w1 - w2;
    }
    case 5:
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        return k * v1;
    case 6:
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        return x - k * v1;
    case 7: {
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        if (!a.hasGain || gainDb != a.cachedGainDb) {
            a.gain = std::pow(10.0, gainDb / 20.0);
            a.cachedGainDb = gainDb;
            a.hasGain = true;
        }
        return x + (a.gain - 1.0) * (k * v1);
    }
    case 8:
        svfTick(x, fc, k, sampleRate, a, v1, v2);
        svfTick(x, fc * formantRatio, k, sampleRate, b, w1, w2);
        return k * v1 + formantLevel * (k * w1);
    default:
        return x;
    }
}

inline float evalWaveform(FMX6Op& op, int wave, double phaseRad, float baseHz, float effectiveHz,
                          int harmonicLimit, float shape, float sampleRate, float noiseGain) {
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
        return randBipolar(op.rng) * noiseGain;

    case 7: // filtered noise; the cutoff follows the operator frequency, phase modulation has no effect
        return filteredNoise(op, baseHz, shape, sampleRate, noiseGain);

    default:
        return static_cast<float>(std::sin(std::remainder(phaseRad, kTwoPi)));
    }
}

void FMX6_next(FMX6* unit, int inNumSamples) {
    // With oversampling everything runs at the higher rate, feedback included, and every operator output is
    // decimated afterwards. Without it the operators write straight to the outputs, exactly as before.
    const int os = unit->oversample;
    const int numInner = inNumSamples * os;
    const float sampleRate = SAMPLERATE * static_cast<float>(os);
    const float sampleDur = SAMPLEDUR / static_cast<float>(os);
    const float slopeScale = 1.f / static_cast<float>(os);

    // White noise spreads its power over the whole oversampled band and the decimator keeps 1/os of it: the source
    // is louder by sqrt(os) so that the noise waveforms keep their level. Exactly 1 without oversampling.
    const float noiseGain = std::sqrt(static_cast<float>(os));

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
    int ftype[kNumOps];
    float ffreq[kNumOps];
    float ffreqInc[kNumOps];
    float foct[kNumOps];
    float foctInc[kNumOps];
    double fq[kNumOps];
    double fratio[kNumOps];
    double flevel[kNumOps];
    double fgain[kNumOps];

    for (int i = 0; i < kNumOps; ++i) {
        FMX6Op& op = unit->ops[i];

        freq[i] = op.freq;
        freqInc[i] = CALCSLOPE(IN0(freqIndex(i)), freq[i]) * slopeScale;

        phaseOffset[i] = op.phaseOffset;
        phaseOffsetInc[i] = CALCSLOPE(IN0(phaseIndex(i)), phaseOffset[i]) * slopeScale;

        amp[i] = op.amp;
        ampInc[i] = CALCSLOPE(IN0(ampIndex(i)), amp[i]) * slopeScale;

        wave[i] = std::max(0, std::min(kMaxWave, static_cast<int>(std::lround(IN0(waveIndex(i))))));
        shape[i] = IN0(shapeIndex(i));
        harmonicLimit[i] = std::max(1, std::min(128, static_cast<int>(std::lround(IN0(harmIndex(i))))));
        if (unit->hasFilters) {
            ftype[i] = std::max(0, std::min(kMaxFilterType, static_cast<int>(std::lround(IN0(filterIndex(i, 0))))));
            ffreq[i] = op.filterFreq;
            ffreqInc[i] = CALCSLOPE(IN0(filterIndex(i, 1)), ffreq[i]) * slopeScale;
            foct[i] = op.filterOct;
            foctInc[i] = CALCSLOPE(IN0(filterIndex(i, 2)), foct[i]) * slopeScale;
            fq[i] = static_cast<double>(IN0(filterIndex(i, 3)));
            fratio[i] = static_cast<double>(IN0(filterIndex(i, 4)));
            flevel[i] = static_cast<double>(IN0(filterIndex(i, 5)));
            fgain[i] = static_cast<double>(IN0(filterIndex(i, 6)));
        } else {
            ftype[i] = 0;
            ffreq[i] = ffreqInc[i] = foct[i] = foctInc[i] = 0.f;
            fq[i] = fratio[i] = flevel[i] = fgain[i] = 0.0;
        }

        outs[i] = os == 1 ? OUT(i) : unit->scratch[i];
    }

    for (int i = 0; i < kNumMatrix; ++i) {
        mod[i] = unit->mod[i];
        modInc[i] = CALCSLOPE(IN0(kModBase + i), mod[i]) * slopeScale;
    }

    for (int s = 0; s < numInner; ++s) {
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
                op, wave[i], phaseRad, freq[i], effectiveHz, harmonicLimit[i], shape[i], sampleRate, noiseGain);

            const float shaped = ftype[i] == 0
                ? osc
                : static_cast<float>(operatorFilter(ftype[i], static_cast<double>(osc), static_cast<double>(ffreq[i]),
                                                    static_cast<double>(foct[i]), fq[i], fratio[i], flevel[i], fgain[i],
                                                    static_cast<double>(sampleRate), op.filterA, op.filterB));
            const float y = amp[i] * shaped;
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
            ffreq[i] += ffreqInc[i];
            foct[i] += foctInc[i];
        }

        for (int i = 0; i < kNumMatrix; ++i) mod[i] += modInc[i];
    }

    if (os > 1) {
        for (int i = 0; i < kNumOps; ++i) {
            float* x = unit->scratch[i];
            int n = numInner;
            if (os == 4) {
                unit->firstStage[i].process(x, n, x);
                n /= 2;
            }
            unit->finalStage[i].process(x, n, OUT(i));
        }
    }

    for (int i = 0; i < kNumOps; ++i) {
        unit->ops[i].freq = freq[i];
        unit->ops[i].phaseOffset = phaseOffset[i];
        unit->ops[i].amp = amp[i];
        unit->ops[i].filterFreq = ffreq[i];
        unit->ops[i].filterOct = foct[i];
    }
    for (int i = 0; i < kNumMatrix; ++i) unit->mod[i] = mod[i];
}

void FMX6_clear(FMX6* unit, int inNumSamples) {
    ClearUnitOutputs(unit, inNumSamples);
}

void FMX6_Ctor(FMX6* unit) {
    unit->memory = nullptr;
    unit->oversample = 1;
    unit->hasFilters = false;
    SETCALC(FMX6_next);

    // Without the oversampling input the UGen behaves as before; SynthDefs built before it existed keep working.
    if ((unit->mNumInputs != kNumInputs && unit->mNumInputs != kNumInputs + 1 && unit->mNumInputs != kNumInputsWithFilters)
        || unit->mNumOutputs != kNumOps) {
        Print("FMX6: expected %d, %d or %d inputs and %d outputs, got %d inputs and %d outputs.\n",
              kNumInputs, kNumInputs + 1, kNumInputsWithFilters, kNumOps, unit->mNumInputs, unit->mNumOutputs);
        SETCALC(FMX6_clear);
        ClearUnitOutputs(unit, 1);
        return;
    }

    if (unit->mNumInputs > kNumInputs) {
        const float requested = IN0(kOversampleIndex);
        unit->oversample = requested >= 3.f ? 4 : (requested >= 1.5f ? 2 : 1);
    }

    unit->hasFilters = unit->mNumInputs == kNumInputsWithFilters;

    if (unit->oversample > 1) {
        // One block for everything: per operator the oversampled samples, the final stage and, for 4x, the first.
        const int os = unit->oversample;
        const int block = BUFLENGTH;
        const int perScratch = block * os;
        const int perFinal = (kFinalLength - 1) + block * 2;
        const int perFirst = os == 4 ? (kFirstLength - 1) + block * 4 : 0;
        const size_t total = static_cast<size_t>(kNumOps) * static_cast<size_t>(perScratch + perFinal + perFirst);
        unit->memory = static_cast<float*>(RTAlloc(unit->mWorld, total * sizeof(float)));
        if (unit->memory == nullptr) {
            Print("FMX6: not enough real-time memory for %dx oversampling.\n", os);
            SETCALC(FMX6_clear);
            ClearUnitOutputs(unit, 1);
            return;
        }

        float* p = unit->memory;
        for (int i = 0; i < kNumOps; ++i) {
            unit->scratch[i] = p;
            p += perScratch;
            unit->finalStage[i].init(kFinalTaps, static_cast<int>(sizeof(kFinalTaps) / sizeof(double)), kFinalLength, p);
            p += perFinal;
            if (os == 4) {
                unit->firstStage[i].init(kFirstTaps, static_cast<int>(sizeof(kFirstTaps) / sizeof(double)), kFirstLength, p);
                p += perFirst;
            }
        }
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
        op.svfIc1 = 0.0;
        op.svfIc2 = 0.0;
        op.filterFreq = unit->hasFilters ? IN0(filterIndex(i, 1)) : 0.f;
        op.filterOct = unit->hasFilters ? IN0(filterIndex(i, 2)) : 0.f;
        op.filterA = Svf{};
        op.filterB = Svf{};
    }

    for (int i = 0; i < kNumMatrix; ++i) unit->mod[i] = IN0(kModBase + i);

    ClearUnitOutputs(unit, 1);
}

void FMX6_Dtor(FMX6* unit) {
    if (unit->memory != nullptr) RTFree(unit->mWorld, unit->memory);
}

} // namespace

PluginLoad(FMX6) {
    ft = inTable;
    DefineDtorUnit(FMX6);
}
