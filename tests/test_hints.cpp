#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>

#include "hints/gitignore.h"
#include "hints/hints.h"
#include "hints/import_files.h"

namespace fs = std::filesystem;
using namespace goose;

namespace {

class TempDir {
public:
    TempDir() : dir_(fs::temp_directory_path() / ("kacli_hints_test_" + std::to_string(rand()))) {
        fs::create_directories(dir_);
    }
    ~TempDir() { fs::remove_all(dir_); }
    const fs::path& path() const { return dir_; }
    void write(const fs::path& rel, const std::string& content) const {
        auto p = dir_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p);
        ofs << content;
    }

private:
    fs::path dir_;
};

std::string read_content(const fs::path& p) {
    std::ifstream ifs(p);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(Gitignore, BasicPatterns) {
    TempDir dir;
    dir.write(".gitignore", "*.env\n");
    dir.write("allowed.md", "ok");
    dir.write("secret.env", "SECRET_KEY=abc123");

    auto gi = build_gitignore(dir.path());
    EXPECT_FALSE(gi.is_ignored(dir.path() / "allowed.md"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "secret.env"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "sub" / "deep" / "x.env"));
}

TEST(Gitignore, ExactFileAndNegation) {
    TempDir dir;
    dir.write(".gitignore", "secret.md\n!keep.md\n");
    dir.write("secret.md", "s");
    dir.write("keep.md", "k");

    auto gi = build_gitignore(dir.path());
    EXPECT_TRUE(gi.is_ignored(dir.path() / "secret.md"));
    EXPECT_FALSE(gi.is_ignored(dir.path() / "keep.md"));
}

TEST(Gitignore, MergesNestedGitignores) {
    TempDir dir;
    fs::create_directory(dir.path() / ".git");
    dir.write(".gitignore", "*.log\n");
    dir.write("sub/.gitignore", "*.tmp\n");
    dir.write("debug.log", "log");
    dir.write("sub/cache.tmp", "tmp");
    dir.write("sub/readme.md", "readme");

    auto gi = build_gitignore(dir.path() / "sub");
    EXPECT_TRUE(gi.is_ignored(dir.path() / "debug.log"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "sub" / "cache.tmp"));
    EXPECT_FALSE(gi.is_ignored(dir.path() / "sub" / "readme.md"));
}

TEST(Gitignore, AnchoredAndDirOnlyPatterns) {
    TempDir dir;
    dir.write(".gitignore", "/root_only.md\n/build/\n\n");
    dir.write("root_only.md", "r");
    dir.write("sub/root_only.md", "s");
    dir.write("build/out.txt", "o");
    dir.write("src/build/x.txt", "x");
    dir.write("src/build/deep/y.txt", "y");

    auto gi = build_gitignore(dir.path());
    EXPECT_TRUE(gi.is_ignored(dir.path() / "root_only.md"));
    EXPECT_FALSE(gi.is_ignored(dir.path() / "sub" / "root_only.md"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "build" / "out.txt"));
    EXPECT_FALSE(gi.is_ignored(dir.path() / "src" / "build" / "x.txt"));
    EXPECT_FALSE(gi.is_ignored(dir.path() / "src" / "build" / "deep" / "y.txt"));
}

TEST(Gitignore, UnanchoredDirPatternMatchesAnyLevel) {
    TempDir dir;
    dir.write(".gitignore", "build/\n");
    dir.write("build/out.txt", "o");
    dir.write("src/build/x.txt", "x");
    dir.write("src/build/deep/y.txt", "y");

    auto gi = build_gitignore(dir.path());
    EXPECT_TRUE(gi.is_ignored(dir.path() / "build" / "out.txt"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "src" / "build" / "x.txt"));
    EXPECT_TRUE(gi.is_ignored(dir.path() / "src" / "build" / "deep" / "y.txt"));
}

TEST(Gitignore, LoadsFromGitRootWhenInSubdirectory) {
    TempDir dir;
    dir.write(".gitignore", "*.env\n");
    dir.write("secret.env", "SECRET_KEY=abc123");
    fs::create_directory(dir.path() / ".git");
    dir.write("sub/.goosehints", "H\n@../secret.env\n");

    auto gi = build_gitignore(dir.path() / "sub");
    EXPECT_TRUE(gi.is_ignored(dir.path() / "secret.env"));
}

TEST(ImportFiles, DirectReference) {
    TempDir dir;
    dir.write("basic_included_file.md", "This is basic content");
    dir.write("main.md", "Main content\n@basic_included_file.md\nMore content");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_TRUE(expanded.find("Main content") != std::string::npos);
    EXPECT_TRUE(expanded.find("--- Content from basic_included_file.md ---") != std::string::npos);
    EXPECT_TRUE(expanded.find("This is basic content") != std::string::npos);
    EXPECT_TRUE(expanded.find("--- End of basic_included_file.md ---") != std::string::npos);
    EXPECT_TRUE(expanded.find("More content") != std::string::npos);
}

TEST(ImportFiles, NestedReference) {
    TempDir dir;
    dir.write("level1.md", "Level 1 content\n@level2.md");
    dir.write("level2.md", "Level 2 content");
    dir.write("main.md", "Main content\n@level1.md");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_TRUE(expanded.find("Level 1 content") != std::string::npos);
    EXPECT_TRUE(expanded.find("Level 2 content") != std::string::npos);
}

TEST(ImportFiles, CircularReference) {
    TempDir dir;
    dir.write("file1.md", "File 1\n@file2.md");
    dir.write("file2.md", "File 2\n@file1.md");
    dir.write("main.md", "Main\n@file1.md");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_EQ(std::count(expanded.begin(), expanded.end(), '1') > 0, true);
    // "File 1" must appear exactly once thanks to cycle detection.
    size_t count = 0, pos = 0;
    while ((pos = expanded.find("File 1", pos)) != std::string::npos) {
        ++count;
        pos += 6;
    }
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(expanded.find("File 2") != std::string::npos);
}

TEST(ImportFiles, MaxDepthLimit) {
    TempDir dir;
    for (int i = 1; i <= 5; ++i) {
        std::string content = i < 5 ? "Level " + std::to_string(i) + " content\n@level" +
                                          std::to_string(i + 1) + ".md"
                                    : "Level " + std::to_string(i) + " content";
        dir.write("level" + std::to_string(i) + ".md", content);
    }
    dir.write("main.md", "Main\n@level1.md");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_TRUE(expanded.find("Level 1 content") != std::string::npos);
    EXPECT_TRUE(expanded.find("Level 2 content") != std::string::npos);
    EXPECT_TRUE(expanded.find("Level 3 content") != std::string::npos);
    EXPECT_TRUE(expanded.find("Level 4 content") == std::string::npos);
    EXPECT_TRUE(expanded.find("Level 5 content") == std::string::npos);
}

TEST(ImportFiles, MissingFileStaysUnchanged) {
    TempDir dir;
    dir.write("main.md", "Main\n@missing.md\nMore content");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_TRUE(expanded.find("@missing.md") != std::string::npos);
    EXPECT_TRUE(expanded.find("--- Content from") == std::string::npos);
}

TEST(ImportFiles, RespectsGitignore) {
    TempDir dir;
    dir.write(".gitignore", "secret.md\n");
    dir.write("allowed.md", "Allowed content");
    dir.write("secret.md", "Secret content");
    dir.write("main.md", "Main\n@allowed.md\n@secret.md");

    auto gi = build_gitignore(dir.path());
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, gi);

    EXPECT_TRUE(expanded.find("Allowed content") != std::string::npos);
    EXPECT_TRUE(expanded.find("Secret content") == std::string::npos);
    EXPECT_TRUE(expanded.find("@secret.md") != std::string::npos);
}

TEST(ImportFiles, SecurityPathTraversalAndAbsoluteRejected) {
    TempDir dir;
    dir.write("legitimate_file.md", "This is safe content");
    dir.write("main.md", "Normal content here.\n@../etc/passwd\n@/etc/hostname\n@legitimate_file.md\n");

    GitignoreMatcher empty;
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "main.md", dir.path(), visited, 0, empty);

    EXPECT_TRUE(expanded.find("This is safe content") != std::string::npos);
    EXPECT_TRUE(expanded.find("root:") == std::string::npos);
    EXPECT_TRUE(expanded.find("@../etc/passwd") != std::string::npos);
    EXPECT_TRUE(expanded.find("@/etc/hostname") != std::string::npos);
}

TEST(ImportFiles, ImportBoundaryRespected) {
    TempDir dir;
    fs::create_directory(dir.path() / ".git");
    dir.write("root_file.md", "Root file content");
    dir.write("sub/local.md", "Local content");
    dir.write("sub/.goosehints", "Sub hints\n@local.md\n@../root_file.md\n");

    auto gitignore = build_gitignore(dir.path() / "sub");
    std::set<fs::path> visited;
    auto expanded = read_referenced_files(dir.path() / "sub" / ".goosehints",
                                          dir.path(), visited, 0, gitignore);

    EXPECT_TRUE(expanded.find("Local content") != std::string::npos);
    EXPECT_TRUE(expanded.find("--- Content from local.md ---") != std::string::npos);
    EXPECT_TRUE(expanded.find("Root file content") != std::string::npos);
    EXPECT_TRUE(expanded.find("--- Content from ../root_file.md ---") != std::string::npos);
}

TEST(Hints, ProjectHintsWhenPresent) {
    TempDir dir;
    dir.write(".goosehints", "Test hint content");

    auto hints = HintsManager::global().load_hints(dir.path());
    EXPECT_TRUE(hints.find("Test hint content") != std::string::npos);
    EXPECT_TRUE(hints.find("### Project Hints") != std::string::npos);
}

TEST(Hints, MissingWhenNoFiles) {
    TempDir dir;
    auto hints = HintsManager::global().load_hints(dir.path());
    EXPECT_TRUE(hints.empty());
}

TEST(Hints, NestedWithGitRoot) {
    TempDir dir;
    fs::create_directory(dir.path() / ".git");
    dir.write(".goosehints", "Root hints content");
    dir.write("subdir/.goosehints", "Subdir hints content");
    dir.write("subdir/current_dir/.goosehints", "current_dir hints content");

    auto hints = HintsManager::global().load_hints(dir.path() / "subdir" / "current_dir");
    EXPECT_TRUE(hints.find("Root hints content\nSubdir hints content\ncurrent_dir hints content") !=
                std::string::npos);
}

TEST(Hints, WithoutGitRootOnlyCurrentDir) {
    TempDir dir;
    dir.write(".goosehints", "Base hints content");
    dir.write("subdir/.goosehints", "Subdir hints content");
    dir.write("subdir/current_dir/.goosehints", "Current dir hints content");

    auto hints = HintsManager::global().load_hints(dir.path() / "subdir" / "current_dir");
    EXPECT_TRUE(hints.find("Current dir hints content") != std::string::npos);
    EXPECT_TRUE(hints.find("Base hints content") == std::string::npos);
    EXPECT_TRUE(hints.find("Subdir hints content") == std::string::npos);
}

TEST(Hints, GlobalHintsFromConfigDir) {
    const fs::path tmp = fs::temp_directory_path() / ("kacli_global_hints_" + std::to_string(rand()));
    fs::create_directories(tmp / "hints");
    std::ofstream(tmp / "hints" / ".goosehints") << "Global hint content";
    setenv("GOOSE_CONFIG_DIR", tmp.string().c_str(), 1);

    TempDir project;
    auto hints = HintsManager::global().load_hints(project.path());

    unsetenv("GOOSE_CONFIG_DIR");
    fs::remove_all(tmp);

    EXPECT_TRUE(hints.find("Global hint content") != std::string::npos);
    EXPECT_TRUE(hints.find("### Global Hints") != std::string::npos);
    EXPECT_TRUE(hints.find("### Project Hints") == std::string::npos);
}

TEST(Hints, HintsWithImports) {
    TempDir dir;
    fs::create_directory(dir.path() / ".git");
    dir.write("README.md", "# Project README");
    dir.write("config.md", "Configuration details");
    dir.write(".goosehints", "Project hints content\n@README.md\n@config.md\nAdditional instructions here.");

    auto hints = HintsManager::global().load_hints(dir.path());
    EXPECT_TRUE(hints.find("Project hints content") != std::string::npos);
    EXPECT_TRUE(hints.find("--- Content from README.md ---") != std::string::npos);
    EXPECT_TRUE(hints.find("# Project README") != std::string::npos);
    EXPECT_TRUE(hints.find("--- Content from config.md ---") != std::string::npos);
    EXPECT_TRUE(hints.find("Configuration details") != std::string::npos);
    EXPECT_TRUE(hints.find("Additional instructions here") != std::string::npos);
}

TEST(Hints, GitignoreFiltersReferencedFiles) {
    TempDir dir;
    fs::create_directory(dir.path() / ".git");
    dir.write(".gitignore", "*.env\n");
    dir.write("allowed.md", "Allowed content");
    dir.write("secret.env", "SECRET_KEY=abc123");
    dir.write(".goosehints", "Project hints\n@allowed.md\n@secret.env\nEnd of hints");

    auto hints = HintsManager::global().load_hints(dir.path());
    EXPECT_TRUE(hints.find("Allowed content") != std::string::npos);
    EXPECT_TRUE(hints.find("SECRET_KEY=abc123") == std::string::npos);
    EXPECT_TRUE(hints.find("@secret.env") != std::string::npos);
}

TEST(Tracker, RecordsPathArgument) {
    const fs::path wd = "/home/user/project";
    SubdirectoryHintTracker tracker;
    auto args = nlohmann::json{{"path", "src/main.rs"}};
    tracker.record_tool_arguments(args, wd);
    auto hints = tracker.load_new_hints(wd);
    EXPECT_TRUE(hints.empty());
}

TEST(Tracker, LoadsSubdirectoryHints) {
    TempDir dir;
    dir.write("nested/.goosehints", "nested subdirectory hints");

    SubdirectoryHintTracker tracker;
    auto args = nlohmann::json{{"path", "nested/foo.rs"}};
    tracker.record_tool_arguments(args, dir.path());
    auto hints = tracker.load_new_hints(dir.path());
    ASSERT_EQ(hints.size(), 1);
    EXPECT_TRUE(hints[0].first.find("nested") != std::string::npos);
    EXPECT_TRUE(hints[0].second.find("nested subdirectory hints") != std::string::npos);
}

TEST(Tracker, DeduplicatesDirectories) {
    TempDir dir;
    dir.write("nested/.goosehints", "nested hints");

    SubdirectoryHintTracker tracker;
    auto args = nlohmann::json{{"path", "nested/foo.rs"}};
    tracker.record_tool_arguments(args, dir.path());
    EXPECT_EQ(tracker.load_new_hints(dir.path()).size(), 1);
    tracker.record_tool_arguments(args, dir.path());
    EXPECT_TRUE(tracker.load_new_hints(dir.path()).empty());
}

TEST(Tracker, SkipsFlagsInCommand) {
    TempDir dir;
    dir.write("src/.goosehints", "src hints");

    SubdirectoryHintTracker tracker;
    auto args = nlohmann::json{{"command", "grep -rn pattern src/lib.rs"}};
    tracker.record_tool_arguments(args, dir.path());
    tracker.load_new_hints(dir.path());
    EXPECT_TRUE(fs::exists(dir.path() / "src" / ".goosehints"));
}
