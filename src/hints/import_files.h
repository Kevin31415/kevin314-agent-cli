#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "gitignore.h"

namespace goose {

// Expand `@reference` file references inside a hints file, replacing each
// reference with the content of the referenced file wrapped in markers:
//
//   --- Content from <ref> ---
//   <content>
//   --- End of <ref> ---
//
// Rules (mirroring goose):
//   - absolute paths are rejected
//   - only files inside `import_boundary` may be imported
//   - nested references expand recursively up to MAX_DEPTH (3)
//   - circular references are detected via `visited`
//   - files matched by `ignore_patterns` are not imported
//   - the file content is capped at 128KB for reference parsing
std::string read_referenced_files(
    const std::filesystem::path& file_path,
    const std::filesystem::path& import_boundary,
    std::set<std::filesystem::path>& visited,
    int depth,
    const GitignoreMatcher& ignore_patterns);

} // namespace goose
