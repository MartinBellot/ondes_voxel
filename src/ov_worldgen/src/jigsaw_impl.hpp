// The jigsaw library's state, shared by its three source files. Private.
#pragma once

#include "ov/worldgen/jigsaw.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ov::worldgen {

struct JigsawLibrary::Impl {
    std::map<std::string, std::unique_ptr<TemplatePool>, std::less<>> pools;
    std::map<std::string, JigsawConfig, std::less<>>                  configs;
    std::map<std::string, const PoolElement*, std::less<>>            by_key;
    std::vector<std::string>                                          missing;

    /// The processors every pool element gets before its own: structure
    /// blocks dropped (a legacy element drops its air too), jigsaw blocks
    /// turned into their final state; and after its own, a terrain matching
    /// element's drop onto the surface.
    ProcessorRef ignore_structure_block;
    ProcessorRef ignore_structure_and_air;
    ProcessorRef jigsaw_replacement;
    ProcessorRef gravity;
};

}  // namespace ov::worldgen
