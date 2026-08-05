#pragma once

#include <string>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>

namespace goose {

struct TemplateInfo {
    std::string name;
    std::string description;
    std::string default_content;
    std::string user_content;
    bool is_customized = false;
};

// Prompt template registry: renders the embedded goose-derived templates with
// a Jinja2-like engine (inja), honoring user overrides in ~/.kacli/prompts/.
class PromptTemplate {
public:
    static PromptTemplate& global();

    // Render a registered template by name (user override wins), with the given context.
    std::string render(const std::string& name, const nlohmann::json& context);

    // Render an arbitrary template string with the same engine and preprocessing.
    std::string render_string(const std::string& tmpl, const nlohmann::json& context);

    TemplateInfo get_template(const std::string& name);
    void save_template(const std::string& name, const std::string& content);
    void reset_template(const std::string& name);
    std::vector<TemplateInfo> list_templates();

private:
    PromptTemplate();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Strip invisible Unicode Tags block characters (U+E0000-U+E007F) that can be
// used for steganographic prompt injection.
std::string sanitize_unicode_tags(const std::string& input);

// Wrap code in a markdown fence longer than any backtick run it contains.
std::string code_fence(const std::string& code);

} // namespace goose
