#include "import_files.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <spdlog/spdlog.h>

namespace goose {

namespace {
constexpr int kMaxDepth = 3;
constexpr size_t kMaxContentLength = 131072; // 128KB

// 按路径分量判断 dir 是否为 root 的严格子目录，避免 /home/u/proj2
// 被字符串前缀误判为 /home/u/proj 的子目录。
bool is_subdirectory(const std::filesystem::path& dir, const std::filesystem::path& root) {
    if (dir == root) return false;
    auto rel = dir.lexically_relative(root);
    if (rel.empty()) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    return true;
}

// Mirrors goose's file reference pattern:
//   - paths with a file extension (@docs/guide.md, @file.test.js)
//   - single-word capitalised files (@Makefile, @LICENSE)
//   - any path containing a '/' or '.'
// Not matched: emails (@example.com), social handles (@username), URLs.
const std::regex kFileReferenceRegex(
    R"((?:^|\s)@([a-zA-Z0-9_\-./]+(?:\.[a-zA-Z0-9]+)+|[A-Z][a-zA-Z0-9_\-]*|[a-zA-Z0-9_\-./]*[./][a-zA-Z0-9_\-./]*))");

std::vector<std::filesystem::path> parse_file_references(const std::string& content) {
    std::vector<std::filesystem::path> references;
    if (content.size() > kMaxContentLength) {
        spdlog::warn("Content too large for file reference parsing: {} bytes (limit: {} bytes)",
                     content.size(), kMaxContentLength);
        return references;
    }
    const auto end = std::sregex_iterator();
    for (std::sregex_iterator it(content.begin(), content.end(), kFileReferenceRegex);
         it != end; ++it) {
        references.emplace_back((*it)[1].str());
    }
    return references;
}

// Returns an empty optional when the reference is unsafe (absolute, outside the
// import boundary, gitignored, or not a file).
std::optional<std::filesystem::path> sanitize_reference_path(
    const std::filesystem::path& reference,
    const std::filesystem::path& including_file_path,
    const std::filesystem::path& import_boundary,
    const GitignoreMatcher& ignore_patterns) {
    if (reference.is_absolute()) {
        spdlog::warn("Skipping absolute file reference: {}", reference.string());
        return std::nullopt;
    }

    std::error_code ec;
    const auto boundary_canonical = std::filesystem::canonical(import_boundary, ec);
    if (ec) {
        spdlog::warn("Import boundary directory not found: {}", import_boundary.string());
        return std::nullopt;
    }

    const auto resolved = including_file_path / reference;
    std::filesystem::path safe_path = resolved;
    const auto canonical = std::filesystem::canonical(resolved, ec);
    if (!ec) {
        if (!is_subdirectory(canonical, boundary_canonical)) {
            spdlog::warn("Include '{}' is outside the import boundary '{}'",
                         resolved.string(), import_boundary.string());
            return std::nullopt;
        }
        safe_path = canonical;
    }

    if (ignore_patterns.is_ignored(safe_path)) {
        spdlog::debug("Skipping ignored file reference: {}", safe_path.string());
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(safe_path)) {
        return std::nullopt;
    }
    return safe_path;
}

} // namespace

std::string read_referenced_files(
    const std::filesystem::path& file_path,
    const std::filesystem::path& import_boundary,
    std::set<std::filesystem::path>& visited,
    int depth,
    const GitignoreMatcher& ignore_patterns) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        spdlog::warn("Could not read file {}", file_path.string());
        return "";
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    const std::filesystem::path including_file_path = file_path.parent_path();

    std::string result = content;
    for (const auto& reference : parse_file_references(content)) {
        if (visited.count(reference)) continue;

        const auto safe_path = sanitize_reference_path(reference, including_file_path,
                                                       import_boundary, ignore_patterns);
        if (!safe_path) continue;

        if (depth >= kMaxDepth) {
            spdlog::warn("Maximum reference depth {} exceeded", kMaxDepth);
            continue;
        }

        visited.insert(reference);
        const std::string expanded = read_referenced_files(
            *safe_path, import_boundary, visited, depth + 1, ignore_patterns);
        visited.erase(reference);

        const std::string pattern = "@" + reference.string();
        const std::string replacement =
            "--- Content from " + reference.string() + " ---\n" + expanded +
            "\n--- End of " + reference.string() + " ---";

        size_t pos = 0;
        while ((pos = result.find(pattern, pos)) != std::string::npos) {
            result.replace(pos, pattern.size(), replacement);
            pos += replacement.size();
        }
    }
    return result;
}

} // namespace goose
