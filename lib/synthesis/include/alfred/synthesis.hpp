#pragma once

#include <memory>
#include <array>
#include <vector>
#include <numeric>
#include <algorithm>
#include <limits>
#include <utility>
#include <cmath>
#include <cstdint>

#include "alfred/allocator.hpp"

namespace alfred::syn {
    enum NoteName : std::uint32_t {
        A,
        As,
        B,
        C,
        Cs,
        D,
        Ds,
        E,
        F,
        Fs,
        G,
        Gs
    };

    enum NoteOctave : std::uint32_t {
        Octave0,
        Octave1,
        Octave2,
        Octave3,
        Octave4,
        Octave5,
        Octave6,
        Octave7,
        Octave8
    };

    // MIDI-like note ID
    using NoteId = std::uint32_t;

    // MIDI-like value from 0 to 1
    using Velocity = double;

    // Instruments/presets are identified by IDs
    using InstrumentId = std::uint32_t;

    // Min and max pitch of the instrument, inclusive range
    using InstrumentRange = std::pair<NoteId, NoteId>;

    constexpr NoteId note(NoteName name, NoteOctave octave) {
        const std::uint32_t base = name;
        const std::uint32_t multiplier = octave;

        if (base < 3) {
            return base + 12 * multiplier;
        }

        return base + 12 * (multiplier - 1);
    }

    constexpr std::pair<NoteName, NoteOctave> note(NoteId id) {
        std::uint32_t octave = id / 12;
        const std::uint32_t name = id % 12;

        if (name >= 3) {
            octave += 1;
        }

        return std::make_pair(NoteName(name), NoteOctave(octave));
    }

    static_assert(note(A, Octave5) == 60);
    static_assert(note(B, Octave5) == 62);
    static_assert(note(C, Octave2) == 15);
    static_assert(note(2) == std::pair(B, Octave0));
    static_assert(note(16) == std::pair(Cs, Octave2));
    static_assert(note(85) == std::pair(As, Octave7));

    namespace keyboard {
        enum Octave : std::uint32_t {
            OctaveFirst,
            OctaveSecond,
            OctaveThird,
            OctaveFourth,
            OctaveFifth,
            OctaveSixth,
            OctaveSeventh
        };

        inline constexpr Octave OCTAVE_BEGIN = OctaveFirst;
        inline constexpr Octave OCTAVE_END = OctaveSeventh;

        inline constexpr int OCTAVES = 7;
        inline constexpr int EXTRA = 4;
        inline constexpr int NOTES = OCTAVES * 12 + EXTRA;

        inline constexpr NoteId ID_BEGIN = 0;
        inline constexpr NoteId ID_END = NOTES - 1;
        inline constexpr InstrumentRange ID_FULL_RANGE = std::make_pair(ID_BEGIN, ID_END);
    }

    namespace volume {
        // Volume type in decibels
        using Volume = std::int32_t;

        inline constexpr Volume MIN = -40;
        inline constexpr Volume DEFAULT = 0;
        inline constexpr Volume MAX = 12;

        // Decibels (power) to amplitude
        inline double amplitude(Volume volume) {
            return std::pow(10.0, double(volume) / 20.0);
        }
    }

    namespace envelope {
        using Storage = allocator::StaticAllocatorStorage<keyboard::NOTES * 10, 96, 8, true>;

        // Abstract class representing an envelope
        // Envelopes use a custom allocator; they are usually dynamically allocated
        struct Envelope {
            Envelope() = default;
            virtual ~Envelope() = default;

            Envelope(const Envelope&) = default;
            Envelope& operator=(const Envelope&) = default;
            Envelope(Envelope&&) = default;
            Envelope& operator=(Envelope&&) = default;

            // MIDI-like note events
            virtual void note_on(double time) = 0;
            virtual void note_off(double time) = 0;

            // Called for every sample of output
            virtual void update(double time) = 0;

            // Get the current value
            virtual double value() const = 0;

            // If the envelope has finished its lifetime
            virtual bool done() const = 0;
        };

        enum class Type {
            Linear,
            Exponential
        };

        struct DescriptionAdsr {
            double duration_attack = 0.1;
            double duration_decay = 0.02;
            double duration_release = 0.2;
            double value_sustain = 0.8;
        };

        struct DescriptionAdr {
            double duration_attack = 0.1;
            double duration_decay = 1.0;
            double duration_release = 0.2;
        };

        struct DescriptionNull {};

        class AdsrLinear : public Envelope, public allocator::StaticAllocated<AdsrLinear, Storage> {
        public:
            explicit AdsrLinear(const DescriptionAdsr& description = {})
                : m_description(description) {}

            void note_on(double time) override;
            void note_off(double time) override;
            void update(double time) override;
            double value() const override;
            bool done() const override;
        private:
            enum class Segment {
                None,
                Attack,
                Decay,
                Sustain,
                Release
            } m_segment {};

            DescriptionAdsr m_description;

            double m_value_current {};

            double m_attack_increment {};
            double m_decay_increment {};
            double m_release_increment {};
        };

        class Adsr : public Envelope, public allocator::StaticAllocated<Adsr, Storage> {
        public:
            explicit Adsr(const DescriptionAdsr& description = {})
                : m_description(description) {}

            void note_on(double time) override;
            void note_off(double time) override;
            void update(double time) override;
            double value() const override;
            bool done() const override;
        private:
            enum class Segment {
                None,
                Attack,
                Decay,
                Sustain,
                Release
            } m_segment {};

            DescriptionAdsr m_description;

            double m_time_note_on {};
            double m_time_note_off {};
            double m_time_decay {};

            double m_value_current {};
            double m_value_note_on {};
            double m_value_note_off {};
        };

        class AdrLinear : public Envelope, public allocator::StaticAllocated<AdrLinear, Storage> {
        public:
            explicit AdrLinear(const DescriptionAdr& description = {})
                : m_description(description) {}

            void note_on(double time) override;
            void note_off(double time) override;
            void update(double time) override;
            double value() const override;
            bool done() const override;
        private:
            enum class Segment {
                None,
                Attack,
                Decay,
                Release
            } m_segment {};

            DescriptionAdr m_description;

            double m_value_current {};

            double m_attack_increment {};
            double m_decay_increment {};
            double m_release_increment {};
        };

        class Adr : public Envelope, public allocator::StaticAllocated<Adr, Storage> {
        public:
            explicit Adr(const DescriptionAdr& description = {})
                : m_description(description) {}

            void note_on(double time) override;
            void note_off(double time) override;
            void update(double time) override;
            double value() const override;
            bool done() const override;
        private:
            enum class Segment {
                None,
                Attack,
                Decay,
                Release
            } m_segment {};

            DescriptionAdr m_description;

            double m_time_note_on {};
            double m_time_note_off {};
            double m_time_decay {};

            double m_value_current {};
            double m_value_note_on {};
            double m_value_note_off {};
        };

        // Envelope that should not actually change the value
        class Null : public Envelope, public allocator::StaticAllocated<Null, Storage> {
        public:
            explicit Null(const DescriptionNull& = {}) {}

            void note_on(double) override {}
            void note_off(double) override {}
            void update(double) override {}
            double value() const override { return 1.0; }
            bool done() const override { return true; }
        };

        using Ptr = std::unique_ptr<Envelope>;
    }

    namespace voice {
#ifdef ALFRED_WINDOWS  // Windows in debug mode is just stupid
        using Storage = allocator::StaticAllocatorStorage<keyboard::NOTES, 88, 8, true>;
#else
        using Storage = allocator::StaticAllocatorStorage<keyboard::NOTES, 72, 8, true>;
#endif

        // A voice represents a particular sound made by some instrument at some point in time in the synthesizer
        // There can be multiple voices with the same instrument provided that their note (pitch) is different
        // A synthesizer then stores and processes multiple voices in order to produce a sample of sound output
        // A voice is abstract; it is subclassed in order to provide synthesis method specific data
        struct Voice {
            Voice() = default;
            virtual ~Voice() = default;

            Voice(const Voice&) = delete;
            Voice& operator=(const Voice&) = delete;
            Voice(Voice&&) = default;
            Voice& operator=(Voice&&) = default;

            // Generic methods used by subclasses to execute stuff with their own data
            virtual void note_on(double time) = 0;
            virtual void note_off(double time) = 0;
            virtual void update(double time) = 0;

            NoteId note {};
            InstrumentId instrument {};
            envelope::Ptr overall_envelope;
            double amplitude {};
            double time_on {};
            double time_off = -std::numeric_limits<double>::infinity();
        };

        struct VoiceAdd : Voice, allocator::StaticAllocated<VoiceAdd, Storage> {
            // The following vector itself allocates dynamically pointers of envelopes
            // Ensure we avoid those dynamic allocations; an upper bound is probably fine :D
            using EnvelopesStorage = allocator::StaticAllocatorStorage<1024, 8, 8, true>;
            using PartialEnvelopes = std::vector<envelope::Ptr, allocator::StaticAllocator<envelope::Ptr, EnvelopesStorage>>;

            // Vector of envelopes corresponding to the vector of partials
            PartialEnvelopes partial_envelopes;

            void note_on(double time) override;
            void note_off(double time) override;
            void update(double time) override;
        };

        struct VoicePad : Voice, allocator::StaticAllocated<VoicePad, Storage> {
            void note_on(double) override {}
            void note_off(double) override {}
            void update(double) override {}
        };

        using Ptr = std::unique_ptr<Voice>;
    }

    // An instrument (or also called preset) describes how some voice should sound
    // It comprises all the parameters that make up a particular sound
    // Instruments must have working copy and move constructors
    // Instruments are completely immutable, and they contain only read-only data
    struct Instrument {
        Instrument() = default;
        virtual ~Instrument() = default;

        Instrument(const Instrument&) = default;
        Instrument& operator=(const Instrument&) = default;
        Instrument(Instrument&&) = default;
        Instrument& operator=(Instrument&&) = default;

        // Metadata and identification
        virtual const char* name() const = 0;
        virtual const char* description() const = 0;
        virtual InstrumentId id() const = 0;

        // Raw sound produced at this particular time (without taking into account envelope and other values)
        // This must be at full amplitude, between -1 and 1
        // Down cast the voice to the known subclass type, if needed
        virtual double sound(double time, const voice::Voice& voice) const noexcept = 0;

        // Note range (inclusive)
        virtual InstrumentRange range() const { return keyboard::ID_FULL_RANGE; }

        // Create a new voice specifically for this instrument
        virtual voice::Ptr new_voice() const = 0;

        // Create a new overall envelope specifically for this instrument
        virtual envelope::Ptr new_overall_envelope() const = 0;

        // Get the attack and release durations
        virtual double attack_duration() const = 0;
        virtual double release_duration() const = 0;

        // Deep copy this instrument as a dynamic allocation
        virtual std::unique_ptr<const Instrument> clone() const = 0;
    };

    using InstrumentPtr = std::unique_ptr<const Instrument>;

    struct LowFrequencyOscillator {
        double frequency {};
        double deviation {};
    };

    namespace oscillator {
        enum class Type {
            Sine,
            Square,
            Triangle,
            Sawtooth,
            Noise  // Not really an oscillator, but it's okay
        };

        double sine(double time, double frequency, double phase);
        double sine(double time, double frequency, double phase, LowFrequencyOscillator lfo);
        double square(double time, double frequency, double phase);
        double square(double time, double frequency, double phase, LowFrequencyOscillator lfo);
        double triangle(double time, double frequency, double phase);
        double triangle(double time, double frequency, double phase, LowFrequencyOscillator lfo);
        double sawtooth(double time, double frequency, double phase);
        double sawtooth(double time, double frequency, double phase, LowFrequencyOscillator lfo);
    }

    double frequency_modulation(double time, double frequency, LowFrequencyOscillator lfo);
    double noise();
    double random();
    double frequency(NoteId note);

    inline constexpr double FREQUENCY_MIN = 27.5;  // A0
    inline constexpr double FREQUENCY_MAX = 4186.0;  // C8

    namespace util {
        template<std::size_t N>
        constexpr std::array<double, N> amplitudes(std::array<double, N> divisors) {
            for (std::size_t i {}; i < divisors.size(); i++) {
                divisors[i] = 1.0 / divisors[i];
            }

            const double sum = std::accumulate(divisors.begin(), divisors.end(), 0.0);

            for (double& divisor : divisors) {
                divisor /= sum;
            }

            return divisors;
        }

        std::vector<double> amplitudes(std::vector<double> divisors);
        double sound(double time, NoteId note, const double* sample, std::size_t size, double frequency);
    }

    namespace padsynth {
        using Sample = std::unique_ptr<double[]>;
        using Profile = double(*)(double, double);

        // Paul's PadSynth synthesis algorithm
        Sample padsynth(
            std::size_t size,
            int sample_rate,
            double frequency,
            double bandwidth,
            const double* amplitude_harmonics,
            std::size_t number_harmonics,
            Profile profile = nullptr
        );

        // Utility wrapper for sample
        class SampleCopyable {
        public:
            SampleCopyable() = default;
            SampleCopyable(Sample sample, std::size_t size)
                : m_sample(std::move(sample)), m_size(size) {}

            ~SampleCopyable() = default;
            SampleCopyable(const SampleCopyable& other);
            SampleCopyable& operator=(const SampleCopyable& other);
            SampleCopyable(SampleCopyable&&) = default;
            SampleCopyable& operator=(SampleCopyable&&) = default;

            const Sample& get() const { return m_sample; }
            Sample& get() { return m_sample; }
        private:
            static Sample copy(const Sample& ptr, std::size_t size);

            Sample m_sample;
            std::size_t m_size {};
        };
    }
}
