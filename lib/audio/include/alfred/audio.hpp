#pragma once

#include <stdexcept>
#include <span>
#include <utility>
#include <vector>

struct SDL_AudioStream;

namespace alfred::audio {
    // Represents an audio stream and device, i.e. speakers
    class Audio {
    public:
        Audio();
        virtual ~Audio();

        Audio(const Audio&) = delete;
        Audio& operator=(const Audio&) = delete;
        Audio(Audio&& other) noexcept;
        Audio& operator=(Audio&& other) noexcept;

        using Device = std::pair<unsigned int, const char*>;
        using Devices = std::span<const Device>;

        // Retrieve the name of the audio driver
        static const char* driver();

        // Retrieve the current audio device
        Device device() const;

        // Retrieve the current available audio devices
        Devices devices() const;

        // Interrogate the system for the currently available audio devices (update the list)
        void query_devices();

        // Open the default audio device
        void open();

        // Open a specific audio device
        void open(unsigned int device);

        // Close the audio device
        void close() const;

        // Start the audio device
        void resume() const;

        // Stop the audio device
        void pause() const;

        // Lock/unlock the internal mutex
        void lock() const;
        void unlock() const;

        // Get/set the volume
        void volume(double volume) const;
        double volume() const;

        // Is the audio device valid
        operator bool() const { return m_stream; }
    protected:
        // Called for every audio sample
        virtual void callback_update() noexcept = 0;
        virtual double callback_sound() const noexcept = 0;

        // Current output sample
        double sample() const { return m_sample; }

        // Device time in seconds, dependent on sample frequency
        double m_time {};
    private:
        static void stream_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) noexcept;

        SDL_AudioStream* m_stream {};
        std::vector<Device> m_devices;
        double m_sample {};
        bool m_initialized {};  // To support moving these audio objects
    };

    // Helper class for simpler and exception-safe locking/unlocking
    class AudioLockGuard {
    public:
        explicit AudioLockGuard(const Audio* audio);
        ~AudioLockGuard() noexcept;

        AudioLockGuard(const AudioLockGuard&) = delete;
        AudioLockGuard& operator=(const AudioLockGuard&) = delete;
        AudioLockGuard(AudioLockGuard&&) = delete;
        AudioLockGuard& operator=(AudioLockGuard&&) = delete;
    private:
        const Audio* m_audio {};
    };

    struct AudioError : std::runtime_error {
        using runtime_error::runtime_error;
    };
}
