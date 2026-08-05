#include "hints.h"

#include <fstream>
#include <sstream>
#include <algorithm>

#include "../config/config.h"
#include "../config/paths.h"
#include "import_files.h"

namespace goose {

namespace {

// 判断 dir 是否为 root 的严格子目录：按路径分量比较而非字符串前缀，
// 否则 /home/u/proj2 会被误判为 /home/u/proj 的子目录。
bool is_subdirectory(const std::filesystem::path& dir, const std::filesystem::path& root) {
    if (dir == root) return false;
    auto rel = dir.lexically_relative(root);
    if (rel.empty()) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    return true;
}

std::filesystem::path find_git_root(const std::filesystem::path& start_dir) {
    std::filesystem::path check_dir = start_dir;
    while (true) {
        if (std::filesystem::exists(check_dir / ".git")) {
            return check_dir;
        }
        const auto parent = check_dir.parent_path();
        if (parent == check_dir) break;
        check_dir = parent;
    }
    return {};
}

std::vector<std::filesystem::path> get_local_directories(
    const std::filesystem::path& git_root, const std::filesystem::path& cwd) {
    std::vector<std::filesystem::path> directories;
    if (git_root.empty()) {
        directories.push_back(cwd);
        return directories;
    }
    std::vector<std::filesystem::path> reversed;
    std::filesystem::path current = cwd;
    while (true) {
        reversed.push_back(current);
        if (current == git_root) break;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    directories.assign(reversed.rbegin(), reversed.rend());
    return directories;
}

std::string load_hints_from_directory(
    const std::filesystem::path& directory,
    const std::filesystem::path& working_dir,
    const std::vector<std::string>& hint_filenames) {
    if (!std::filesystem::is_directory(directory) || !directory.is_absolute()) {
        return "";
    }
    if (!is_subdirectory(directory, working_dir)) {
        return "";
    }

    const auto git_root = find_git_root(working_dir);
    const auto import_boundary = git_root.empty() ? working_dir : git_root;
    const GitignoreMatcher gitignore = build_gitignore(working_dir);

    std::vector<std::filesystem::path> directories;
    std::filesystem::path dir = directory;
    while (true) {
        if (!is_subdirectory(dir, working_dir)) break;
        directories.push_back(dir);
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    std::reverse(directories.begin(), directories.end());

    std::vector<std::string> contents;
    for (const auto& d : directories) {
        for (const auto& filename : hint_filenames) {
            const auto hints_path = d / filename;
            if (std::filesystem::is_regular_file(hints_path)) {
                std::set<std::filesystem::path> visited;
                const std::string expanded = read_referenced_files(
                    hints_path, import_boundary, visited, 0, gitignore);
                if (!expanded.empty()) {
                    contents.push_back(expanded);
                }
            }
        }
    }

    if (contents.empty()) return "";
    std::string joined;
    for (size_t i = 0; i < contents.size(); ++i) {
        if (i > 0) joined += "\n";
        joined += contents[i];
    }
    return "### Subdirectory Hints (" + directory.string() + ")\n" + joined;
}

} // namespace

std::vector<std::string> get_context_filenames() {
    const auto param = Config::global().get_param("CONTEXT_FILE_NAMES");
    if (param) {
        std::vector<std::string> names;
        std::stringstream ss(*param);
        std::string name;
        while (std::getline(ss, name, ',')) {
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (!name.empty()) names.push_back(name);
        }
        if (!names.empty()) return names;
    }
    return {kHintsFilename, kAgentsMdFilename};
}

SubdirectoryHintTracker::SubdirectoryHintTracker() : hint_filenames_(get_context_filenames()) {}

void SubdirectoryHintTracker::record_tool_arguments(const nlohmann::json& arguments,
                                                    const std::filesystem::path& working_dir) {
    if (!arguments.is_object()) return;

    auto resolve_to_parent_dir = [&working_dir](const std::string& token) {
        std::filesystem::path path(token);
        const auto resolved = path.is_absolute() ? path : working_dir / path;
        return resolved.parent_path();
    };

    if (arguments.contains("path") && arguments["path"].is_string()) {
        const auto dir = resolve_to_parent_dir(arguments["path"].get<std::string>());
        if (!dir.empty()) pending_dirs_.push_back(dir);
    }

    if (arguments.contains("command") && arguments["command"].is_string()) {
        std::stringstream ss(arguments["command"].get<std::string>());
        std::string token;
        while (ss >> token) {
            if (!token.empty() && token[0] == '-') continue;
            if (token.find('/') != std::string::npos || token.find('.') != std::string::npos) {
                const auto dir = resolve_to_parent_dir(token);
                if (!dir.empty()) pending_dirs_.push_back(dir);
            }
        }
    }
}

std::vector<std::pair<std::string, std::string>> SubdirectoryHintTracker::load_new_hints(
    const std::filesystem::path& working_dir) {
    std::vector<std::pair<std::string, std::string>> results;
    if (pending_dirs_.empty()) return results;

    const std::vector<std::filesystem::path> pending = std::move(pending_dirs_);
    pending_dirs_.clear();

    for (const auto& dir : pending) {
        if (!is_subdirectory(dir, working_dir)) continue;
        if (loaded_dirs_.count(dir)) continue;
        const std::string content =
            load_hints_from_directory(dir, working_dir, hint_filenames_);
        if (!content.empty()) {
            results.emplace_back("subdir_hints:" + dir.string(), content);
        }
        loaded_dirs_.insert(dir);
    }
    return results;
}

HintsManager& HintsManager::global() {
    static HintsManager instance;
    return instance;
}

std::string HintsManager::load_hints(const std::filesystem::path& working_dir) {
    const auto hint_filenames = get_context_filenames();

    std::vector<std::string> global_hints_contents;
    std::vector<std::filesystem::path> global_hints_paths;
    for (const auto& name : hint_filenames) {
        global_hints_paths.push_back(paths::config_dir() / "hints" / name);
    }
    if (std::find(hint_filenames.begin(), hint_filenames.end(), kAgentsMdFilename) !=
        hint_filenames.end()) {
        global_hints_paths.push_back(paths::config_dir() / "agents" / kAgentsMdFilename);
    }

    for (const auto& global_path : global_hints_paths) {
        if (!std::filesystem::is_regular_file(global_path)) continue;
        const auto hints_dir = global_path.parent_path();
        const GitignoreMatcher global_ignore; // empty: never ignore
        std::set<std::filesystem::path> visited;
        const std::string expanded =
            read_referenced_files(global_path, hints_dir, visited, 0, global_ignore);
        if (!expanded.empty()) {
            global_hints_contents.push_back(expanded);
        }
    }

    const auto git_root = find_git_root(working_dir);
    const auto local_directories = get_local_directories(git_root, working_dir);
    const auto import_boundary = git_root.empty() ? working_dir : git_root;
    const GitignoreMatcher gitignore = build_gitignore(working_dir);

    std::vector<std::string> local_hints_contents;
    for (const auto& directory : local_directories) {
        for (const auto& filename : hint_filenames) {
            const auto hints_path = directory / filename;
            if (!std::filesystem::is_regular_file(hints_path)) continue;
            std::set<std::filesystem::path> visited;
            const std::string expanded = read_referenced_files(
                hints_path, import_boundary, visited, 0, gitignore);
            if (!expanded.empty()) {
                local_hints_contents.push_back(expanded);
            }
        }
    }

    std::string hints;
    if (!global_hints_contents.empty()) {
        hints += "\n### Global Hints\nThese are my global kacli hints.\n";
        for (size_t i = 0; i < global_hints_contents.size(); ++i) {
            if (i > 0) hints += "\n";
            hints += global_hints_contents[i];
        }
    }
    if (!local_hints_contents.empty()) {
        if (!hints.empty()) hints += "\n\n";
        hints += "### Project Hints\nThese are hints for working on the project in this directory.\n";
        for (size_t i = 0; i < local_hints_contents.size(); ++i) {
            if (i > 0) hints += "\n";
            hints += local_hints_contents[i];
        }
    }
    return hints;
}

} // namespace goose
