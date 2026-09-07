#include "ov/io/byte_reader.hpp"

namespace ov::io {

std::string_view to_string(ReadError error) noexcept {
    switch (error) {
        case ReadError::OutOfBounds:       return "out of bounds";
        case ReadError::InvalidLength:     return "invalid length";
        case ReadError::MalformedEncoding: return "malformed encoding";
    }
    return "unknown error";
}

}  // namespace ov::io
