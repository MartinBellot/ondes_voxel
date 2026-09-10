#include "ov/worldgen/random_factory.hpp"

namespace ov::worldgen {

PositionalRandomFactory PositionalRandomFactory::for_world(i64 seed, bool legacy) noexcept {
    if (legacy) {
        math::LegacyRandomSource source{seed};
        return PositionalRandomFactory{
            math::LegacyPositionalFactory{static_cast<u64>(source.next_long())}};
    }
    math::XoroshiroRandomSource source{seed};
    return PositionalRandomFactory{source.fork_positional()};
}

NormalNoise PositionalRandomFactory::normal_noise(std::string_view name, i32 first_octave,
                                                  std::span<const f64> amplitudes) const {
    return std::visit(
        [&](const auto& factory) {
            auto source = factory.from_hash_of(name);
            return NormalNoise::create(source, first_octave, amplitudes);
        },
        factory_);
}

PositionalRandomFactory PositionalRandomFactory::fork_named(std::string_view name) const noexcept {
    if (const auto* legacy_one = legacy_factory()) {
        auto source = legacy_one->from_hash_of(name);
        return PositionalRandomFactory{
            math::LegacyPositionalFactory{static_cast<u64>(source.next_long())}};
    }
    auto source = std::get<math::XoroshiroPositionalFactory>(factory_).from_hash_of(name);
    return PositionalRandomFactory{source.fork_positional()};
}

f32 PositionalRandomFactory::next_float_at(i32 x, i32 y, i32 z) const noexcept {
    return std::visit(
        [&](const auto& factory) {
            auto source = factory.at(x, y, z);
            return source.next_float();
        },
        factory_);
}

f64 PositionalRandomFactory::next_double_at(i32 x, i32 y, i32 z) const noexcept {
    return std::visit(
        [&](const auto& factory) {
            auto source = factory.at(x, y, z);
            return source.next_double();
        },
        factory_);
}

}  // namespace ov::worldgen
