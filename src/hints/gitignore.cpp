#include "gitignore.h"

#include <fstream>
#include <sstream>

namespace goose {

namespace {

std::string glob_to_regex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                out += ".*";
                ++i;
            } else {
                out += "[^/]*";
            }
        } else if (c == '?') {
            out += "[^/]";
        } else if (c == '\\') {
            if (i + 1 < pattern.size()) {
                out += std::regex_replace(std::string(1, pattern[++i]),
                                          std::regex(R"([.+(){}^$|\[\]])"),
                                          "\\$&");
            }
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '{' ||
                   c == '}' || c == '^' || c == '$' || c == '|' ||
                   c == '[' || c == ']') {
            out += '\\';
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

bool regex_match_string(const std::string& value, const std::string& regex) {
    try {
        return std::regex_match(value, std::regex("^" + regex + "$"));
    } catch (const std::regex_error&) {
        return false;
    }
}

} // namespace

void GitignoreMatcher::add_rule(const std::filesystem::path& root, const std::string& line) {
    std::string pattern = line;
    if (pattern.empty()) return;

    Rule rule;
    rule.root = root;

    if (pattern[0] == '!') {
        rule.negate = true;
        pattern = pattern.substr(1);
        if (pattern.empty()) return;
    }
    if (pattern.size() > 1 && pattern.back() == '/') {
        rule.dir_only = true;
        pattern.pop_back();
        if (pattern.empty()) return;
    }
    if (!pattern.empty() && pattern[0] == '/') {
        pattern = pattern.substr(1);
        rule.anchored = true;
    }
    // A slash in the middle (or a non-trailing slash) anchors the pattern.
    rule.anchored = rule.anchored || pattern.find('/') != std::string::npos;

    rule.pattern = glob_to_regex(pattern);
    rules_.push_back(std::move(rule));
}

void GitignoreMatcher::load_from(const std::filesystem::path& dir) {
    std::ifstream ifs(dir / ".gitignore");
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Trailing spaces are ignored unless escaped by a backslash.
        if (line.size() > 1 && line.back() == ' ' && line[line.size() - 2] != '\\') {
            line.pop_back();
        }
        add_rule(dir, line);
    }
}

bool GitignoreMatcher::matches(const std::filesystem::path& rel, const Rule& rule) const {
    std::error_code ec;
    const std::string rel_str = rel.lexically_normal().generic_string();
    if (rel_str.empty() || rel_str == ".") return false;

    if (!rule.dir_only) {
        if (rule.anchored) {
            return regex_match_string(rel_str, rule.pattern);
        }
        return regex_match_string(rel.filename().string(), rule.pattern);
    }

    // Directory-only rule: matches the directory itself or anything under it.
    const std::string pat = rule.pattern;
    if (rule.anchored) {
        if (regex_match_string(rel_str, pat)) return true;
        const std::string prefix = rel_str + "/";
        size_t pos = prefix.find('/');
        while (pos != std::string::npos) {
            if (regex_match_string(prefix.substr(0, pos), pat)) return true;
            pos = prefix.find('/', pos + 1);
        }
        return false;
    }
    // Unanchored dir rule: any component may match.
    for (const auto& part : rel) {
        if (regex_match_string(part.string(), pat)) return true;
    }
    return false;
}

bool GitignoreMatcher::is_ignored(const std::filesystem::path& path) const {
    bool ignored = false;
    for (const auto& rule : rules_) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(path, rule.root, ec);
        if (ec || rel.empty()) continue;
        if (matches(rel, rule)) {
            ignored = !rule.negate;
        }
    }
    return ignored;
}

GitignoreMatcher build_gitignore(const std::filesystem::path& cwd) {
    // Collect directories from the git root down to cwd.
    std::vector<std::filesystem::path> directories;
    std::filesystem::path check = cwd;
    bool found_root = false;
    while (true) {
        if (std::filesystem::exists(check / ".git")) {
            found_root = true;
            break;
        }
        const auto parent = check.parent_path();
        if (parent == check) break;
        check = parent;
    }
    if (found_root) {
        std::vector<std::filesystem::path> reversed;
        std::filesystem::path dir = cwd;
        while (true) {
            reversed.push_back(dir);
            if (dir == check) break;
            const auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        directories.assign(reversed.rbegin(), reversed.rend());
    } else {
        directories.push_back(cwd);
    }

    GitignoreMatcher matcher;
    for (const auto& dir : directories) {
        matcher.load_from(dir);
    }
    return matcher;
}

} // namespace goose
