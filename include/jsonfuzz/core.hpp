#pragma once

/// JSONFuzz public interface, v0.1.
///
/// Phase A: the core library carries only the artifact-identity `version()`. The
/// generator, mutators, oracles and the parser-agnostic SystemUnderTest interface are
/// Phase B (see docs/BRIEF-v0.1.md). Nothing here depends on any JSON parser.

#include <jsonfuzz/version.hpp>

#include <string>

namespace jsonfuzz {

/// The artifact-identity string this library reports: the PROJECT_VERSION as CMake
/// defines it, read back from the generated version header.
std::string version();

} // namespace jsonfuzz
