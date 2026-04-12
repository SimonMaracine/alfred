#pragma once

#include <string>
#include <utility>
#include <variant>
#include <optional>
#include <vector>

#include "alfred/synthesis.hpp"
#include "alfred/hash.hpp"

// Runtime dynamic instruments/presets
// In this project, the term "instrument" represents an instrument in the generic sense, while "preset" specifically represents a runtime instrument (XML file)

namespace alfred::preset {
    using EnvelopeDescription = std::variant<syn::envelope::DescriptionAdsr, syn::envelope::DescriptionAdr, syn::envelope::DescriptionNull>;

    struct Envelope {
        EnvelopeDescription description;
        syn::envelope::Type type {};
    };

    // Base parameters that make up a runtime instrument
    struct BasePreset {
        std::string name;
        std::string description;
        syn::InstrumentRange range;
        Envelope envelope;  // Overall envelope
    };

    // Base template for any kind of runtime-defined instrument
    template<typename Preset>
    class BaseRuntimeInstrument : public syn::Instrument {
    public:
        explicit BaseRuntimeInstrument(Preset preset)
            : m_preset(std::move(preset)), m_id(hash::HashedStr32(m_preset.name)) {}

        // Retrieve a read only reference to the parameters that make up this instrument
        const Preset& preset() const { return m_preset; }

        const char* name() const override { return m_preset.name.c_str(); }
        const char* description() const override { return m_preset.description.c_str(); }
        syn::InstrumentId id() const override { return m_id; }
        syn::InstrumentRange range() const override { return m_preset.range; }
    protected:
        Preset m_preset;
        syn::InstrumentId m_id {};
    };

    namespace add {
        // One harmonic/inharmonic
        struct Partial {
            syn::oscillator::Type oscillator_type {};
            double frequency_multiplier = 1.0;
            double amplitude_divisor = 1.0;
            double phase {};
            std::optional<syn::LowFrequencyOscillator> lfo;
            Envelope envelope;
        };

        struct Preset : BasePreset {
            std::vector<Partial> partials;
        };

        class RuntimeInstrument : public BaseRuntimeInstrument<Preset> {
        public:
            explicit RuntimeInstrument(Preset preset);

            double sound(double time, const syn::voice::Voice& voice) const noexcept override;
            syn::voice::Ptr new_voice() const override;
            syn::envelope::Ptr new_overall_envelope() const override;
            double attack_duration() const override;
            double release_duration() const override;
            syn::InstrumentPtr clone() const override { return std::make_unique<RuntimeInstrument>(*this); }
        private:
            std::vector<double> m_amplitudes;
        };
    }

    namespace pad {
        enum class Profile {
            Gaussian,
            Even,
            SingleSine,
            DoubleSine,
            Arc
        };

        struct Preset : BasePreset {
            Profile profile {};
            double frequency = 261.63;
            double bandwidth = 20.0;
            std::vector<double> amplitude_harmonics;
        };

        class RuntimeInstrument : public BaseRuntimeInstrument<Preset> {
        public:
            explicit RuntimeInstrument(Preset preset);

            double sound(double time, const syn::voice::Voice& voice) const noexcept override;
            syn::voice::Ptr new_voice() const override;
            syn::envelope::Ptr new_overall_envelope() const override;
            double attack_duration() const override;
            double release_duration() const override;
            syn::InstrumentPtr clone() const override { return std::make_unique<RuntimeInstrument>(*this); }
        private:
            static syn::padsynth::Profile profile(const Preset& preset);

            static constexpr std::size_t SIZE = 262144;

            syn::padsynth::SampleCopyable m_sample;
        };
    }
}
