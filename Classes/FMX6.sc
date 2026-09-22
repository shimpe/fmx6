FMX6 : MultiOutUGen {
    *numOperators { ^6 }
    *numControls { ^3 }
    *controlArraySize { ^this.numOperators * this.numControls }
    *modArraySize { ^this.numOperators.squared }
    *numRequiredInputs { ^this.controlArraySize + (this.numOperators * 3) + this.modArraySize }

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
        |ctlMatrix, modMatrix, waves = 0, shapes = 0.5, harmonicLimits = 32|

        var ctls;
        var mods;
        var ws;
        var sh;
        var hs;
        var inputs;

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

        inputs = ctls ++ ws ++ sh ++ hs ++ mods;
        ^this.multiNewList(['audio'] ++ inputs)
    }

    init { |...theInputs|
        inputs = theInputs;
        ^this.initOutputs(this.class.numOperators, rate)
    }

    checkInputs {
        ^if(inputs.size != this.class.numRequiredInputs) {
            this.class.numRequiredInputs.asString ++ " inputs required (" ++ inputs.size ++ ")"
        }
    }

    *sine { ^0 }
    *triangle { ^1 }
    *saw { ^2 }
    *square { ^3 }
    *pulse { ^4 }
    *sampleHoldNoise { ^5 }
    *whiteNoise { ^6 }
}
