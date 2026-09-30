#pragma once

#include "nbt/tag.hpp"
#include "util/json.hpp"

namespace mc::nbt {

// JSON data (a data pack file) as NBT: whole numbers become ints (longs when they don't fit),
// others doubles, booleans bytes. Codecs read numbers leniently, so this is how a JsonOps value
// looks to them in NbtOps.
Tag fromJson(const json::Value& v);

} // namespace mc::nbt
