#include "utility.hpp"

#include <fstream>
#include <memory>
#include <format>
#include <cassert>

#include <SDL3/SDL.h>

namespace alfred::utility {
    void read_file(const std::filesystem::path& path, Buffer& buffer) {
        std::ifstream stream {path, std::ios_base::binary};

        if (!stream.is_open()) {
            throw FileError("Could not open file");
        }

        stream.seekg(0, stream.end);
        const auto size = stream.tellg();
        stream.seekg(0, stream.beg);

        buffer.data.resize(std::size_t(size));
        stream.read(buffer.data.data(), size);

        if (stream.fail()) {
            throw FileError("Could not read from file");
        }
    }

    void write_file(const std::filesystem::path& path, const Buffer& buffer) {
        std::ofstream stream {path, std::ios_base::binary};

        if (!stream.is_open()) {
            throw FileError("Could not open file");
        }

        stream.write(buffer.data.data(), std::streamsize(buffer.data.size()));

        if (stream.fail()) {
            throw FileError("Could not write to file");
        }
    }

    using DataFilePath = std::unique_ptr<char, decltype(&SDL_free)>;
    static DataFilePath g_data_file_path {nullptr, SDL_free};

    void initialize_file_paths(const char* organization, const char* application) {
        g_data_file_path = DataFilePath(SDL_GetPrefPath(organization, application), SDL_free);
    }

    std::filesystem::path data_file_path() {
#ifdef ALFRED_DISTRIBUTION
        assert(g_data_file_path);
        return g_data_file_path.get();
#else
        return "./";  // Relative directory
#endif
    }

    std::vector<std::filesystem::path> glob_directory(const std::filesystem::path& path, const char* pattern) {
        int count {};

        const auto paths = std::unique_ptr<char*[], decltype(&SDL_free)>(
            SDL_GlobDirectory(path.string().c_str(), pattern, 0, &count),
            SDL_free
        );

        if (!paths) {
            throw FileError(std::format("SDL_GlobDirectory: {}", SDL_GetError()));
        }

        std::vector<std::filesystem::path> result;

        for (int i {}; i < count; i++) {
            result.emplace_back(paths[std::size_t(i)]);
        }

        return result;
    }

    void create_directory(const std::filesystem::path& path) {
        if (!SDL_CreateDirectory(path.string().c_str())) {
            throw FileError(std::format("SDL_CreateDirectory: {}", SDL_GetError()));
        }
    }

    const char* get_property(const char* property) {
        const char* value = SDL_GetAppMetadataProperty(property);

        if (!value) {
            return "";
        }

        return value;
    }

    void set_property(const char* property, const char* value) {
        (void) SDL_SetAppMetadataProperty(property, value);
    }

    void show_error_message_box(const char* title, const char* message) {
        (void) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
    }
}
