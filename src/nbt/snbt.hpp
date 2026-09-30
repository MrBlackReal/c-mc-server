#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "nbt/tag.hpp"

namespace mc::nbt {

// Tag.toString (StringTagVisitor): compact SNBT with sorted keys, e.g. {a:1b,b:"x",c:[I;1,2]}.
std::string toSnbt(const Tag& tag);

// TagParser: parses SNBT. Supports the usual syntax: compounds, lists, typed arrays, quoted and
// unquoted strings (with \n, \t, \x, \u, \U escapes), numbers with b/s/l/f/d suffixes, the
// u/s signedness prefixes (e.g. 200ub), hex/binary literals, underscores, and true/false.
// Not supported: the bool()/uuid() operations of the 1.21.5+ grammar.
std::optional<Tag> parseSnbt(std::string_view text, std::string* error = nullptr);
// A value at the start of `text` (TagParser.parseAsArgument); `consumed` gets its length.
std::optional<Tag> parseSnbtPrefix(std::string_view text, size_t& consumed, std::string* error = nullptr, size_t* errorPos = nullptr);

// Java's Float.toString / Double.toString (shortest repr: "1.0", "1.0E7", "1.0E-4").
std::string javaFloatString(float v);
std::string javaDoubleString(double v);

} // namespace mc::nbt
