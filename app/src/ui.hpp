#pragma once

#include <utility>
#include <vector>
#include <cstdint>

#include <imgui.h>
#include <alfred/synthesis.hpp>

#include "error.hpp"

// Functions and definitions used by the UI of the application

namespace alfred::ui {
    inline constexpr float FONT_SIZE = 13.0f;

    enum ColorScheme : unsigned int {
        ColorSchemeDark,
        ColorSchemeLight,
        ColorSchemeClassic
    };

    enum Scale : unsigned int {
        Scale100,
        Scale125,
        Scale150,
        Scale175,
        Scale200
    };

    enum Tool : int {
        ToolMeasure,
        ToolNote
    };

    enum Loudness : int {
        LoudnessPianississimo,
        LoudnessPianissimo,
        LoudnessPiano,
        LoudnessMezzoPiano,
        LoudnessMezzoForte,
        LoudnessForte,
        LoudnessFortissimo,
        LoudnessFortississimo
    };

    struct Dynamics {
        bool varying {};
        Loudness loudness1 = LoudnessMezzoForte;
        Loudness loudness2 = LoudnessMezzoForte;
    };

    using Tempo = unsigned int;

    struct Agogic {
        bool varying {};
        Tempo tempo1 = 90;
        Tempo tempo2 = 90;
    };

    struct TimeSignature {
        enum Beats : unsigned int {
            Beats1,
            Beats2,
            Beats3,
            Beats4,
            Beats5,
            Beats6,
            Beats7,
            Beats8,
            Beats9,
            Beats10,
            Beats11,
            Beats12,
            Beats13,
            Beats14,
            Beats15,
            Beats16
        };

        enum Value : unsigned int {
            Value1,
            Value2,
            Value4,
            Value8,
            Value16
        };

        Beats beats = Beats4;
        Value value = Value4;
    };

    enum Value : int {
        ValueWhole,
        ValueHalf,
        ValueQuarter,
        ValueEighth,
        ValueSixteenth
    };

    enum Tuplet : int {
        TupletNone,
        TupletTriplet
    };

    enum Octave : int {
        OctaveFirst,
        OctaveSecond,
        OctaveThird,
        OctaveFourth,
        OctaveFifth,
        OctaveSixth,
        OctaveSeventh
    };

    // https://www.colorhexa.com/color-names

    using ColorIndex = std::uint32_t;  // These are serialized

    inline constexpr std::pair<const char*, ImU32> COLORS[] {
        { "Ash grey", IM_COL32(178, 190, 181, 255) },
        { "Debian red", IM_COL32(215, 10, 83, 255) },
        { "Amethyst", IM_COL32(153, 102, 204, 255) },
        { "Bright pink", IM_COL32(255, 0, 127, 255) },
        { "India green", IM_COL32(19, 136, 8, 255) },
        { "Azure", IM_COL32(0, 127, 255, 255) },
        { "Lemon", IM_COL32(255, 247, 0, 255) },
        { "Antique fuchsia", IM_COL32(145, 92, 131, 255) },
        { "Pastel blue", IM_COL32(174, 198, 207, 255) },
        { "Bittersweet", IM_COL32(254, 111, 94, 255) },
        { "Pale lavender", IM_COL32(220, 208, 255, 255) },
        { "Blue violet", IM_COL32(138, 43, 226, 255) },
        { "Medium Blue", IM_COL32(0, 0, 205, 255) },
        { "Android green", IM_COL32(164, 198, 67, 255) },
        { "Bright green", IM_COL32(102, 255, 0, 255) },
        { "Bronze", IM_COL32(205, 127, 50, 255) },
        { "Army green", IM_COL32(75, 83, 32, 255) },
        { "Amber", IM_COL32(255, 191, 0, 255) }
    };

    struct Composition {
        char title[64] {};
        char author[64] {};
        short year {};
    };

    namespace preset {
        struct EnvelopeDescription {
            double duration_attack = 0.1;
            double duration_decay = 0.1;
            double duration_release = 0.1;
            double value_sustain = 0.9;
        };

        enum EnvelopeType : int {
            EnvelopeTypeAdsrLinear,
            EnvelopeTypeAdsrExponential,
            EnvelopeTypeAdrLinear,
            EnvelopeTypeAdrExponential,
            EnvelopeTypeNull
        };

        struct Envelope {
            EnvelopeDescription description;
            EnvelopeType type {};
        };

        struct BasePreset {
            char name[64] {};
            char description[64] {};
            unsigned int range[2] { syn::keyboard::ID_BEGIN, syn::keyboard::ID_END };
            Envelope envelope;
        };

        enum OscillatorType : int {
            OscillatorTypeSine,
            OscillatorTypeSquare,
            OscillatorTypeTriangle,
            OscillatorTypeSawtooth,
            OscillatorTypeNoise
        };

        struct LowFrequencyOscillator {
            bool enabled {};
            double frequency = 1.0;
            double deviation = 0.1;
        };

        struct Partial {
            OscillatorType oscillator_type {};
            double frequency_multiplier = 1.0;
            double amplitude_divisor = 1.0;
            double phase {};
            LowFrequencyOscillator lfo;
            Envelope envelope;
        };

        struct PresetAdd : BasePreset {
            std::vector<Partial> partials;
        };

        enum Profile : int {
            ProfileGaussian,
            ProfileEven,
            ProfileSingleSine,
            ProfileDoubleSine,
            ProfileArc
        };

        struct PresetPad : BasePreset {
            static constexpr double DEFAULT_AMPLITUDE = 1.0;

            Profile profile {};
            double frequency = 261.63;
            double bandwidth = 20.0;
            std::vector<double> amplitude_harmonics;
        };
    }

    enum class CreateInstrumentTab {
        Additive,
        PadSynth
    };

    float rem(float size);
    ImVec2 rem(ImVec2 size);
    void set_style();
    void set_scale(float scale);
    void set_color_scheme(ColorScheme color_scheme);

    constexpr float scale(Scale scale) {
        switch (scale) {
            case Scale100:
                return 1.0f;
            case Scale125:
                return 1.25f;
            case Scale150:
                return 1.5f;
            case Scale175:
                return 1.75f;
            case Scale200:
                return 2.0f;
        }

        std::unreachable();
    }

    struct UiError : error::Error {
        using Error::Error;

        ALFRED_ERROR_NAME(UiError)
    };
}
