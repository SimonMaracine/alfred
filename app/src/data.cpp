#include "data.hpp"

#include <sstream>
#include <format>

#include <cereal/archives/binary.hpp>
#include <cereal/types/set.hpp>
#include <cereal/types/string.hpp>

namespace alfred::data {
    void export_data(const Data& data, utility::Buffer& buffer) {
        std::ostringstream stream {std::ios_base::binary};
        stream.exceptions(std::ios_base::failbit);

        try {
            cereal::BinaryOutputArchive archive {stream};
            archive(data);
        } catch (const cereal::Exception& e) {
            throw DataError(std::format("Could not write to stream: {}", e.what()));
        } catch (...) {
            throw DataError("Unexpected error writing to stream");
        }

        buffer.data = stream.str();
    }

    void import_data(Data& data, const utility::Buffer& buffer) {
        std::istringstream stream {buffer.data, std::ios_base::binary};
        stream.exceptions(std::ios_base::failbit);

        try {
            cereal::BinaryInputArchive archive {stream};
            archive(data);
        } catch (const cereal::Exception& e) {
            throw DataError(std::format("Could not read from stream: {}", e.what()));
        } catch (...) {
            throw DataError("Unexpected error reading from stream");
        }
    }
}
