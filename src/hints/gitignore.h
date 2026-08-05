#pragma once

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace goose {

// Minimal .gitignore matcher with git-like semantics:
//   - comments (#) and blank lines are skipped
//   - `!` negates the previous rule; the last matching rule wins
//   - a pattern without a slash (other than a trailing one) matches at any level
//   - a leading or interior slash anchors the pattern to the gitignore's root
//   - a trailing `/` marks a directory-only rule
//   - `*`, `?`, `**` glob wildcards are supported
class GitignoreMatcher {
public:
    // Load the rules from <dir>/.gitignore if present; the rules are relative
    // to <dir>. Call once per gitignore directory (git root first, then nested).
    void load_from(const std::filesystem::path& dir);

    bool is_ignored(const std::filesystem::path& path) const;

private:
    struct Rule {
        bool negate = false;
        bool dir_only = false;
        bool anchored = false;
        std::filesystem::path root;
        std::string pattern;
    };

    void add_rule(const std::filesystem::path& root, const std::string& line);
    bool matches(const std::filesystem::path& rel, const Rule& rule) const;

    std::vector<Rule> rules_;
};

// Build a matcher that includes .gitignore files from the git root down to
// `cwd`, mirroring git's hierarchical ignore semantics. Without a git root,
// only cwd/.gitignore is loaded.
GitignoreMatcher build_gitignore(const std::filesystem::path& cwd);

} // namespace goose
