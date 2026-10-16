FMX6 : MultiOutUGen {
    *numOperators { ^6 }
    *numControls { ^3 }
    *numFilterFields { ^7 }
    *controlArraySize { ^this.numOperators * this.numControls }
    *modArraySize { ^this.numOperators.squared }
    *numRequiredInputs { ^this.controlArraySize + (this.numOperators * 3) + this.modArraySize + 1 }

    // ctlMatrix rows: [frequencyHz, phaseOffsetRadians, amplitude]
    *controlMatrix { |...args|
        var matrix = Array.fill2D(this.numOperators, this.numControls, 0.0);
        args.do { |x|
            var op = x[0].asInteger.clip(0, this.numOperators - 1);
            var row = x.copyToEnd(1);
            this.numControls.do { |i| matrix[op][i] = row.at(i) ? 0.0 };
        };
        ^matrix
    }

    // modMatrix entries: [destinationOperator, sourceOperator, radians]
    *modMatrix { |...args|
        var matrix = Array.fill2D(this.numOperators, this.numOperators, 0.0);
        args.do { |x|
            matrix[x[0].asInteger.clip(0, 5)][x[1].asInteger.clip(0, 5)] = x[2];
        };
        ^matrix
    }

    *ar {
        |ctlMatrix, modMatrix, waves = 0, shapes = 0.5, harmonicLimits = 32, oversample = 1, filters|

        var ctls;
        var mods;
        var ws;
        var sh;
        var hs;
        var inputs;
        var fs;

        ctls = (ctlMatrix ?? { this.controlMatrix }).flatten(1);
        mods = (modMatrix ?? { this.modMatrix }).flatten(1);

        if(ctls.size != this.controlArraySize) {
            Error("FMX6: ctlMatrix must flatten to 18 values (6 x [freq, phase, amp]).").throw;
        };
        if(mods.size != this.modArraySize) {
            Error("FMX6: modMatrix must flatten to 36 values (6 x 6).").throw;
        };

        ws = Array.fill(6, { |i| waves.asArray.wrapAt(i) });
        sh = Array.fill(6, { |i| shapes.asArray.wrapAt(i) });
        hs = Array.fill(6, { |i| harmonicLimits.asArray.wrapAt(i) });

        if([1, 2, 4].includes(oversample).not) {
            Error("FMX6: oversample must be 1, 2 or 4.").throw;
        };

        inputs = ctls ++ ws ++ sh ++ hs ++ mods ++ [oversample];

        // Without filters the SynthDef is exactly what it was before filters existed.
        if(filters.notNil) {
            fs = filters.flatten(1);
            if(fs.size != (this.numOperators * this.numFilterFields)) {
                Error("FMX6: filters must be 6 rows of [type, freq, octaves, q, formantRatio, formantLevel, gainDb].").throw;
            };
            inputs = inputs ++ fs;
        };
        ^this.multiNewList(['audio'] ++ inputs)
    }

    init { |...theInputs|
        inputs = theInputs;
        ^this.initOutputs(this.class.numOperators, rate)
    }

    // 72 inputs: built before oversampling existed; 73: no filters; 115: with filters.
    checkInputs {
        var n = this.class.numRequiredInputs;
        ^if([n - 1, n, n + (this.class.numOperators * this.class.numFilterFields)].includes(inputs.size).not) {
            n.asString ++ " inputs required (" ++ inputs.size ++ ")"
        }
    }

    *sine { ^0 }
    *triangle { ^1 }
    *saw { ^2 }
    *square { ^3 }
    *pulse { ^4 }
    *sampleHoldNoise { ^5 }
    *whiteNoise { ^6 }
    *filteredNoise { ^7 }

    *filterOff { ^0 }
    *lowPass2 { ^1 }
    *lowPass4 { ^2 }
    *highPass2 { ^3 }
    *highPass4 { ^4 }
    *bandPass { ^5 }
    *notch { ^6 }
    *peak { ^7 }
    *formant { ^8 }
}
