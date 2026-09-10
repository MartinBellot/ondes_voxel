// The miniaudio feature switches, in one place.
//
// They change which declarations miniaudio.h contains, so the translation unit
// that compiles the implementation and the one that calls it must see the same
// set — two copies of the list drift, and the symptom is a link error at best
// and a struct laid out two ways at worst. Both include this first.
//
// Only the device layer is used. Decoding is stb_vorbis's job, and mixing,
// spatialisation and resampling are ours (sound_engine.cpp), because vanilla's
// distance model and category volumes are what has to be reproduced, not
// miniaudio's.
#pragma once

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
