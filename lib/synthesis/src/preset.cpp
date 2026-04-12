#include "alfred/preset.hpp"

#include <ranges>
#include <utility>
#include <algorithm>
#include <cmath>

#include "alfred/definitions.hpp"

namespace alfred::preset {
    namespace generic {
        template<typename TypeLinear, typename TypeExponential, std::size_t TypeIndex>
        static syn::envelope::Ptr new_envelope(const Envelope& envelope) {
            switch (envelope.type) {
                case syn::envelope::Type::Linear:
                    return std::make_unique<TypeLinear>(std::get<TypeIndex>(envelope.description));
                case syn::envelope::Type::Exponential:
                    return std::make_unique<TypeExponential>(std::get<TypeIndex>(envelope.description));
            }

            std::unreachable();
        }

        template<typename Preset>
        static syn::envelope::Ptr new_overall_envelope(const Preset& preset) {
            switch (preset.envelope.description.index()) {
                case 0:
                    return new_envelope<syn::envelope::AdsrLinear, syn::envelope::Adsr, 0>(preset.envelope);
                case 1:
                    return new_envelope<syn::envelope::AdrLinear, syn::envelope::Adr, 1>(preset.envelope);
                case 2:
                    return new_envelope<syn::envelope::Null, syn::envelope::Null, 2>(preset.envelope);
            }

            std::unreachable();
        }

        template<typename Preset>
        static syn::envelope::Ptr new_partial_envelope(const Preset& preset, std::size_t index) {
            switch (preset.partials.at(index).envelope.description.index()) {
                case 0:
                    return new_envelope<syn::envelope::AdsrLinear, syn::envelope::Adsr, 0>(preset.partials.at(index).envelope);
                case 1:
                    return new_envelope<syn::envelope::AdrLinear, syn::envelope::Adr, 1>(preset.partials.at(index).envelope);
                case 2:
                    return new_envelope<syn::envelope::Null, syn::envelope::Null, 2>(preset.partials.at(index).envelope);
            }

            std::unreachable();
        }

        template<typename Preset>
        static double attack_duration(const Preset& preset) {
            switch (preset.envelope.description.index()) {
                case 0:
                    return std::get<0>(preset.envelope.description).duration_attack;
                case 1:
                    return std::get<1>(preset.envelope.description).duration_attack;
                case 2:
                    return 0.0;
            }

            std::unreachable();
        }

        template<typename Preset>
        static double release_duration(const Preset& preset) {
            switch (preset.envelope.description.index()) {
                case 0:
                    return std::get<0>(preset.envelope.description).duration_release;
                case 1:
                    return std::get<1>(preset.envelope.description).duration_release;
                case 2:
                    return 0.0;
            }

            std::unreachable();
        }
    }

    namespace add {
        RuntimeInstrument::RuntimeInstrument(Preset preset)
            : BaseRuntimeInstrument(std::move(preset)) {
            for (const Partial& partial : m_preset.partials) {
                m_amplitudes.push_back(partial.amplitude_divisor);
            }

            m_amplitudes = syn::util::amplitudes(std::move(m_amplitudes));
        }

#define ALFRED_OSCILLATOR(OSCILLATOR_FUNCTION) \
    [this, time, &voice, i, &partial] { \
        if (partial.lfo) { \
            return m_amplitudes[std::size_t(i)] * OSCILLATOR_FUNCTION(time, partial.frequency_multiplier * syn::frequency(voice.note), partial.phase, *partial.lfo); \
        } \
        return m_amplitudes[std::size_t(i)] * OSCILLATOR_FUNCTION(time, partial.frequency_multiplier * syn::frequency(voice.note), partial.phase); \
    }()

        double RuntimeInstrument::sound(double time, const syn::voice::Voice& voice) const noexcept {
            const auto& voice_add = static_cast<const syn::voice::VoiceAdd&>(voice);
            double output {};

            for (const auto& [i, partial] : m_preset.partials | std::views::enumerate) {
                const double amplitude = voice_add.partial_envelopes.at(std::size_t(i))->value();

                switch (partial.oscillator_type) {
                    case syn::oscillator::Type::Sine:
                        output += amplitude * ALFRED_OSCILLATOR(syn::oscillator::sine);
                        break;
                    case syn::oscillator::Type::Square:
                        output += amplitude * ALFRED_OSCILLATOR(syn::oscillator::square);
                        break;
                    case syn::oscillator::Type::Triangle:
                        output += amplitude * ALFRED_OSCILLATOR(syn::oscillator::triangle);
                        break;
                    case syn::oscillator::Type::Sawtooth:
                        output += amplitude * ALFRED_OSCILLATOR(syn::oscillator::sawtooth);
                        break;
                    case syn::oscillator::Type::Noise:
                        output += amplitude * m_amplitudes[std::size_t(i)] * syn::noise();
                        break;
                }
            }

            return output;
        }

        syn::voice::Ptr RuntimeInstrument::new_voice() const {
            std::vector<syn::envelope::Ptr> partial_envelopes;

            for (std::size_t i {}; i < m_preset.partials.size(); i++) {
                partial_envelopes.push_back(generic::new_partial_envelope(m_preset, i));
            }

            auto voice = std::make_unique<syn::voice::VoiceAdd>();
            voice->partial_envelopes = std::move(partial_envelopes);

            return voice;
        }

        syn::envelope::Ptr RuntimeInstrument::new_overall_envelope() const {
            return generic::new_overall_envelope(m_preset);
        }

        double RuntimeInstrument::attack_duration() const {
            return generic::attack_duration(m_preset);
        }

        double RuntimeInstrument::release_duration() const {
            return generic::release_duration(m_preset);
        }
    }

    namespace pad {
        // https://www.desmos.com/calculator/ejhicuxbr4

        static double profile_even(double frequency, double bandwidth) {
            return std::max((-std::pow(frequency / bandwidth, 100.0) + 1.0) / bandwidth, 0.0);
        }

        static double profile_single_sine(double frequency, double bandwidth) {
            const double x = frequency / bandwidth;
            return std::max((-100.0 * x * x + 1.0) / bandwidth, 0.0);
        }

        static double profile_two_sines(double frequency, double bandwidth) {
            const double x = frequency / bandwidth;
            return std::max((-std::abs(-4.0 * x * x + 4.0) + 1.0) / bandwidth, 0.0);
        }

        static double profile_arc(double frequency, double bandwidth) {
            const double x = frequency / (3.0 * bandwidth);
            return std::max((-x * x + 1.0) / bandwidth, 0.0);
        }

        RuntimeInstrument::RuntimeInstrument(Preset preset)
            : BaseRuntimeInstrument(std::move(preset)) {
            m_sample = syn::padsynth::SampleCopyable(
                syn::padsynth::padsynth(
                    SIZE,
                    def::SAMPLE_FREQUENCY,
                    m_preset.frequency,
                    m_preset.bandwidth,
                    m_preset.amplitude_harmonics.data(),
                    m_preset.amplitude_harmonics.size(),
                    profile(m_preset)
                ),
                SIZE
            );
        }

        double RuntimeInstrument::sound(double time, const syn::voice::Voice& voice) const noexcept {
            return syn::util::sound(time, voice.note, m_sample.get().get(), SIZE, m_preset.frequency);
        }

        syn::voice::Ptr RuntimeInstrument::new_voice() const {
            return std::make_unique<syn::voice::VoicePad>();
        }

        syn::envelope::Ptr RuntimeInstrument::new_overall_envelope() const {
            return generic::new_overall_envelope(m_preset);
        }

        double RuntimeInstrument::attack_duration() const {
            return generic::attack_duration(m_preset);
        }

        double RuntimeInstrument::release_duration() const {
            return generic::release_duration(m_preset);
        }

        syn::padsynth::Profile RuntimeInstrument::profile(const Preset& preset) {
            switch (preset.profile) {
                case Profile::Gaussian:
                    return nullptr;
                case Profile::Even:
                    return profile_even;
                case Profile::SingleSine:
                    return profile_single_sine;
                case Profile::DoubleSine:
                    return profile_two_sines;
                case Profile::Arc:
                    return profile_arc;
            }

            std::unreachable();
        }
    }
}
