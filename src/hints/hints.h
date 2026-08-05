#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "gitignore.h"

namespace goose {

inline constexpr const char* kHintsFilename = ".goosehints";
inline constexpr const char* kAgentsMdFilename = "AGENTS.md";

// Context filenames, honoring the CONTEXT_FILE_NAMES config param
// (comma-separated); defaults to [".goosehints", "AGENTS.md"].
std::vector<std::string> get_context_filenames();

// Tracks tool arguments that touch files or directories, so hints from newly
// discovered subdirectories can be injected into the conversation later.
class SubdirectoryHintTracker {
public:
    SubdirectoryHintTracker();

    void record_tool_arguments(const nlohmann::json& arguments,
                               const std::filesystem::path& working_dir);

    // Returns (key, content) pairs for newly seen subdirectories under
    // working_dir that contain hint files.
    std::vector<std::pair<std::string, std::string>> load_new_hints(
        const std::filesystem::path& working_dir);

private:
    std::set<std::filesystem::path> loaded_dirs_;
    std::vector<std::filesystem::path> pending_dirs_;
    std::vector<std::string> hint_filenames_;
};

// Loads global (~/.kacli/hints/ + ~/.kacli/agents/) and project (git root to
// cwd) hints.
class HintsManager {
public:
    static HintsManager& global();

    // Returns the combined hints text with "### Global Hints" and
    // "### Project Hints" sections; empty when nothing is found.
    std::string load_hints(const std::filesystem::path& working_dir);

    SubdirectoryHintTracker& tracker() { return tracker_; }

private:
    HintsManager() = default;
    SubdirectoryHintTracker tracker_;
};

} // namespace goose
