#pragma once

#include <string>
#include <filesystem>
#include <vector>

#include "error.hpp"

namespace alfred::utility {
    // Managed generic buffer of memory
    // Note that it always has an extra zero byte at the end not included in the size
    struct Buffer {
        std::string data;
    };

    // Read and write file into buffers
    void read_file(const std::filesystem::path& path, Buffer& buffer);
    void write_file(const std::filesystem::path& path, const Buffer& buffer);

    // Call this in main or before trying to access the data file path
    void initialize_file_paths(const char* organization, const char* application);

    // Get the path to where the application should store its user data files
    // See https://wiki.libsdl.org/SDL3/SDL_GetPrefPath
    std::filesystem::path data_file_path();

    // Recursively search and return all matching files paths from a directory
    std::vector<std::filesystem::path> glob_directory(const std::filesystem::path& path, const char* pattern = nullptr);

    // Create a directory and any parent directories on disk
    void create_directory(const std::filesystem::path& path);

    // Get/set SDL application properties
    const char* get_property(const char* property);
    void set_property(const char* property, const char* value);

    // Display a native error message box
    void show_error_message_box(const char* title, const char* message);

    struct FileError : error::Error {
        using Error::Error;

        ALFRED_ERROR_NAME(FileError)
    };
}
