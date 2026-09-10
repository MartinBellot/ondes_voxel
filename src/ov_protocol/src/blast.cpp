#include "ov/protocol/blast.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

#include <cmath>

namespace ov::net {

bool Explosion::add_record(BlockPos cell) {
    // Offsets from the **floor** of the centre, as the captured packet has
    // them: a charge at y = -59.94 names the block it stands in as dy = 0,
    // not as dy = +1.
    const auto base_x = static_cast<i64>(std::floor(x));
    const auto base_y = static_cast<i64>(std::floor(y));
    const auto base_z = static_cast<i64>(std::floor(z));
    const i64  dx     = static_cast<i64>(cell.x) - base_x;
    const i64  dy     = static_cast<i64>(cell.y) - base_y;
    const i64  dz     = static_cast<i64>(cell.z) - base_z;
    const auto fits   = [](i64 value) { return value >= -128 && value <= 127; };
    if (!fits(dx) || !fits(dy) || !fits(dz)) {
        return false;
    }
    records.push_back(
        std::array<i8, 3>{static_cast<i8>(dx), static_cast<i8>(dy), static_cast<i8>(dz)});
    return true;
}

std::vector<u8> encode_explosion(const Explosion& explosion) {
    io::ByteWriter writer;
    writer.write_f64(explosion.x);
    writer.write_f64(explosion.y);
    writer.write_f64(explosion.z);
    writer.write_f32(explosion.radius);
    write_varint(writer, static_cast<i32>(explosion.records.size()));
    for (const auto& record : explosion.records) {
        writer.write_i8(record[0]);
        writer.write_i8(record[1]);
        writer.write_i8(record[2]);
    }
    writer.write_f32(explosion.knockback_x);
    writer.write_f32(explosion.knockback_y);
    writer.write_f32(explosion.knockback_z);
    return writer.take();
}

std::optional<Explosion> parse_explosion(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    Explosion      out;
    const auto     x      = reader.read_f64();
    const auto     y      = reader.read_f64();
    const auto     z      = reader.read_f64();
    const auto     radius = reader.read_f32();
    if (!x || !y || !z || !radius) {
        return std::nullopt;
    }
    out.x      = *x;
    out.y      = *y;
    out.z      = *z;
    out.radius = *radius;
    const auto count = read_varint(reader);
    // Three bytes a record, so a count the payload cannot hold is refused
    // before anything is reserved for it: a hostile count must not become an
    // allocation.
    if (!count || *count < 0 || static_cast<usize>(*count) * 3 + 12 > reader.remaining()) {
        return std::nullopt;
    }
    out.records.reserve(static_cast<usize>(*count));
    for (i32 i = 0; i < *count; ++i) {
        const auto dx = reader.read_i8();
        const auto dy = reader.read_i8();
        const auto dz = reader.read_i8();
        if (!dx || !dy || !dz) {
            return std::nullopt;
        }
        out.records.push_back(std::array<i8, 3>{*dx, *dy, *dz});
    }
    const auto kx = reader.read_f32();
    const auto ky = reader.read_f32();
    const auto kz = reader.read_f32();
    if (!kx || !ky || !kz || reader.remaining() != 0) {
        return std::nullopt;
    }
    out.knockback_x = *kx;
    out.knockback_y = *ky;
    out.knockback_z = *kz;
    return out;
}

}  // namespace ov::net
