#include "composition.hpp"

#include <sstream>
#include <format>

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/set.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>

namespace alfred::composition {
    void export_composition(const Composition& composition, utility::Buffer& buffer) {
        std::ostringstream stream {std::ios_base::binary};
        stream.exceptions(std::ios_base::failbit);

        try {
            cereal::BinaryOutputArchive archive {stream};
            archive(composition);
        } catch (const cereal::Exception& e) {
            throw CompositionError(std::format("Could not write to stream: {}", e.what()));
        } catch (...) {
            throw CompositionError("Unexpected error writing to stream");
        }

        buffer.data = stream.str();
    }

    void import_composition(Composition& composition, const utility::Buffer& buffer) {
        std::istringstream stream {buffer.data, std::ios_base::binary};
        stream.exceptions(std::ios_base::failbit);

        try {
            cereal::BinaryInputArchive archive {stream};
            archive(composition);
        } catch (const cereal::Exception& e) {
            throw CompositionError(std::format("Could not read from stream: {}", e.what()));
        } catch (...) {
            throw CompositionError("Unexpected error reading from stream");
        }
    }
}
