#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

#include "alfred/synthesis.hpp"

namespace alfred::synthesizer {
    inline constexpr std::size_t MIN_VOICES = 2;
    inline constexpr std::size_t MAX_VOICES = 12;

    // The synthesizer is responsible for instruments/presets storage and for voices handling and mixing
    // It needs to be regularly updated
    class Synthesizer {
    public:
        Synthesizer();
        virtual ~Synthesizer() = default;

        Synthesizer(const Synthesizer&) = delete;
        Synthesizer& operator=(const Synthesizer&) = delete;
        Synthesizer(Synthesizer&&) = default;
        Synthesizer& operator=(Synthesizer&&) = default;

        // MIDI-like note events
        virtual void note_on(syn::NoteId note, syn::InstrumentId instrument, syn::Velocity velocity) = 0;
        virtual void note_off(syn::NoteId note, syn::InstrumentId instrument) = 0;

        // Regularly called in order to keep the synthesizer functional
        virtual double update() = 0;

        // Smoothly stop any current voice
        virtual void silence() = 0;

        // Abruptly stop any current voice
        virtual void silence_immediately() = 0;

        // Set the max voices; abruptly silence the synthesizer after calling this
        virtual void polyphony(std::size_t max_voices) = 0;

        // Add external instruments to the storage
        // Return true if inserted new
        virtual bool store_instrument(std::unique_ptr<syn::Instrument> instrument) = 0;

        // Instruments/presets interrogation
        // Just retrieving the instrument objects themselves is fine
        void for_each_instrument(const std::function<void(const syn::Instrument&)>& function) const;
        const syn::Instrument& get_instrument(syn::InstrumentId instrument) const;

        // Get/set instruments' volumes in decibels
        // These values are thread safe
        syn::volume::Volume mixer_volume(syn::InstrumentId instrument) const;
        void mixer_volume(syn::InstrumentId instrument, syn::volume::Volume volume);

        // Reset all volumes to the default value
        void mixer_reset();

        // Merge other synthesizer's instruments into this synthesizer
        // If both this and other have the same instrument ID, then other's instrument will not replace this
        void merge_instruments(const Synthesizer& other);

        // Get the current voice and max voice counts
        std::size_t current_voices() const { return m_voices.size(); }
        std::size_t polyphony() const { return m_max_voices; }
    protected:
        bool insert_instrument(std::unique_ptr<syn::Instrument> instrument);
        bool in_range(syn::NoteId note, syn::InstrumentId instrument) const;
        void note_on(double time, syn::NoteId note, syn::InstrumentId instrument, syn::Velocity velocity);
        void note_off(double time, syn::NoteId note, syn::InstrumentId instrument);
        static void note_off(double time, syn::voice::Voice& voice);
        void set_polyphony(std::size_t max_voices);
        void update_voices(double time);
        std::vector<syn::voice::Ptr>::iterator find_voice(syn::NoteId note, syn::InstrumentId instrument);
        void sample_update(double time) const noexcept;
        double sample_sound(double time) const noexcept;

        // Instruments storage
        std::unordered_map<syn::InstrumentId, syn::InstrumentPtr> m_instruments;

        // Instruments mixer
        // See https://www.usenix.org/legacy/event/hotpar11/tech/final_files/Boehm.pdf
        std::unordered_map<syn::InstrumentId, std::atomic<syn::volume::Volume>> m_volumes;

        // Current "active" voices that produce sounds
        std::vector<syn::voice::Ptr> m_voices;

        // Current polyphony setting
        std::size_t m_max_voices = 4;
    };

    // Synthesizer whose output is an internal buffer
    // It is not tied to some specific clock time
    // Every call of update produces one sample of output
    class VirtualSynthesizer : public Synthesizer {
    public:
        using Synthesizer::polyphony;

        void note_on(syn::NoteId note, syn::InstrumentId instrument, syn::Velocity velocity) override;
        void note_off(syn::NoteId note, syn::InstrumentId instrument) override;
        double update() override;
        void silence() override;
        void silence_immediately() override;
        void polyphony(std::size_t max_voices) override;
        bool store_instrument(std::unique_ptr<syn::Instrument> instrument) override;

        // Invalidate this synthesizer
        void reset();

        std::size_t get_buffer_size() const { return m_buffer.size(); }
        const double* get_buffer_data() const { return m_buffer.data(); }
        double* get_buffer_data() { return m_buffer.data(); }
    private:
        double m_time {};
        std::vector<double> m_buffer;
    };
}
