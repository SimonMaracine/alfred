#include "preset.hpp"

#include <sstream>
#include <format>

#include <cereal/archives/xml.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/utility.hpp>

namespace alfred::preset {
    namespace generic {
        template<typename Preset>
        static void export_preset(const Preset& preset, utility::Buffer& buffer) {
            std::ostringstream stream;
            stream.exceptions(std::ios_base::failbit);

            try {
                cereal::XMLOutputArchive archive {stream};
                archive(cereal::make_nvp("preset", preset));
            } catch (const cereal::Exception& e) {
                throw PresetError(std::format("Could not write to stream: {}", e.what()));
            } catch (...) {
                throw PresetError("Unexpected error writing to stream");
            }

            buffer.data = stream.str();
        }

        template<typename Preset>
        static void import_preset(Preset& preset, const utility::Buffer& buffer) {
            std::istringstream stream {buffer.data};
            stream.exceptions(std::ios_base::failbit);

            try {
                cereal::XMLInputArchive archive {stream};
                archive(cereal::make_nvp("preset", preset));
            } catch (const cereal::Exception& e) {
                throw PresetError(std::format("Could not read from stream: {}", e.what()));
            } catch (...) {
                throw PresetError("Unexpected error reading from stream");
            }
        }
    }

    void export_preset(const add::Preset& preset, utility::Buffer& buffer) {
        generic::export_preset(preset, buffer);
    }

    void import_preset(add::Preset& preset, const utility::Buffer& buffer) {
        generic::import_preset(preset, buffer);
    }

    void export_preset(const pad::Preset& preset, utility::Buffer& buffer) {
        generic::export_preset(preset, buffer);
    }

    void import_preset(pad::Preset& preset, const utility::Buffer& buffer) {
        generic::import_preset(preset, buffer);
    }
}
