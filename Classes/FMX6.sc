FMX6 : MultiOutUGen {
    *numOperators { ^6 }
    *numControls { ^3 }
    *numFilterFields { ^7 }
    // Formants F2..F5 of a formant filter: [q2, ratio3, level3, q3, ratio4, level4, q4, ratio5, level5, q5].
    *numFormantFields { ^10 }
    // F2 with the filter's q, F3..F5 off.
    *defaultFormantFields { ^[0, 1, 0, 0, 1, 0, 0, 1, 0, 0] }
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
        var rows;
        var wide;

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
        // Rows of 7 fields give exactly the SynthDef of earlier versions. When a row has 17 fields, its first 7 go with
        // the other rows and its last 10 go into a block after all six rows; 7-field rows then get the defaults there.
        if(filters.notNil) {
            rows = filters.asArray;
            wide = (rows.size == this.numOperators)
                and: { rows.every { |r| r.isArray and: { [this.numFilterFields, this.numFilterFields + this.numFormantFields].includes(r.size) } } };
            wide = wide and: { rows.any { |r| r.size == (this.numFilterFields + this.numFormantFields) } };
            if(wide) {
                fs = rows.collect { |r| r.copyRange(0, this.numFilterFields - 1) }.flatten(1)
                    ++ rows.collect { |r|
                        if(r.size > this.numFilterFields) { r.copyToEnd(this.numFilterFields) } { this.defaultFormantFields }
                    }.flatten(1);
            } {
                fs = filters.flatten(1);
                if(fs.size != (this.numOperators * this.numFilterFields)) {
                    Error("FMX6: filters must be 6 rows of either [type, freq, octaves, q, formantRatio, formantLevel, gainDb] "
                        ++ "or [type, freq, octaves, q, ratio2, level2, gainDb, q2, ratio3, level3, q3, ratio4, level4, q4, "
                        ++ "ratio5, level5, q5].").throw;
                };
            };
            inputs = inputs ++ fs;
        };
        ^this.multiNewList(['audio'] ++ inputs)
    }

    init { |...theInputs|
        inputs = theInputs;
        ^this.initOutputs(this.class.numOperators, rate)
    }

    // 72 inputs: built before oversampling existed; 73: no filters; 115: with filters; 175: with five formants.
    checkInputs {
        var n = this.class.numRequiredInputs;
        var f = n + (this.class.numOperators * this.class.numFilterFields);
        ^if([n - 1, n, f, f + (this.class.numOperators * this.class.numFormantFields)].includes(inputs.size).not) {
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
