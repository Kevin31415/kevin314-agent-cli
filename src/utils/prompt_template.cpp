#include "prompt_template.h"
#include "prompt_templates_generated.h"

#include <inja.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>

#include "../config/paths.h"
#include "error.h"

namespace goose {

namespace {
constexpr const char* kEmbedded = "embedded";

std::filesystem::path user_prompts_dir() {
    return paths::config_dir() / "prompts";
}

// 拒绝含路径分隔符或 . / .. 分量的模板名，防止拼进目录后越界读写。
bool valid_template_name(const std::string& name) {
    if (name.empty()) return false;
    const std::filesystem::path p(name);
    if (p.has_parent_path()) return false;
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s == "." || s == "..") return false;
    }
    return true;
}

// inja (unlike minijinja) has no `is defined` test and no `{% with %}` block.
// Rewrite those constructs before rendering:
//   `{% with (a, b) = EXPR %}` -> `{% set a = at(EXPR, 0) %}{% set b = at(EXPR, 1) %}`
//   `{% endwith %}`            -> removed
//   `X is defined`             -> `true`/`false` (resolved against the context)
std::string normalize_template(const std::string& tmpl, const nlohmann::json& ctx) {
    std::string out = tmpl;

    static const std::regex with_re(R"(\{%\s*with\s*\(([^)]+)\)\s*=\s*([^%]+)%\})");
    static const std::regex endwith_re(R"(\{%\s*endwith\s*%\})");
    static const std::regex is_defined_re(R"((\w+)\s+is\s+defined)");

    out = std::regex_replace(out, endwith_re, "");

    auto expand_with = [](const std::smatch& m) {
        std::string names = m[1].str();
        std::string expr = m[2].str();
        std::string replaced;
        size_t start = 0;
        int idx = 0;
        while (true) {
            size_t comma = names.find(',', start);
            std::string name = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            name.erase(name.find_last_not_of(" \t") + 1);
            name.erase(0, name.find_first_not_of(" \t"));
            replaced += "{% set " + name + " = at(" + expr + ", " + std::to_string(idx) + ") %}";
            ++idx;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return replaced;
    };

    auto rewrite = [&out](const std::regex& re, const std::function<std::string(const std::smatch&)>& transform) {
        std::string result;
        const auto end = std::sregex_iterator();
        bool any = false;
        size_t last_end = 0;
        for (std::sregex_iterator it(out.cbegin(), out.cend(), re); it != end; ++it) {
            size_t match_start = static_cast<size_t>(it->position(0));
            size_t match_end = match_start + static_cast<size_t>(it->length(0));
            result.append(out, last_end, match_start - last_end);
            result += transform(*it);
            last_end = match_end;
            any = true;
        }
        if (!any) return out;
        result.append(out, last_end, out.size() - last_end);
        return result;
    };

    out = rewrite(with_re, expand_with);
    out = rewrite(is_defined_re, [&ctx](const std::smatch& m) {
        return ctx.contains(m[1].str()) ? "true" : "false";
    });

    return out;
}

std::string trim_copy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

std::string code_fence(const std::string& code) {
    size_t longest_run = 0;
    size_t run = 0;
    for (char c : code) {
        if (c == '`') {
            ++run;
            longest_run = std::max(longest_run, run);
        } else {
            run = 0;
        }
    }
    std::string fence(static_cast<size_t>(std::max<size_t>(longest_run + 1, 3)), '`');
    std::string body = code;
    while (!body.empty() && body.back() == '\n') body.pop_back();
    return fence + "\n" + body + "\n" + fence;
}

std::string sanitize_unicode_tags(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c >= 0xF3 && c <= 0xF4 && i + 3 < input.size()) {
            unsigned int cp = ((c & 0x07) << 18) |
                              ((static_cast<unsigned char>(input[i + 1]) & 0x3F) << 12) |
                              ((static_cast<unsigned char>(input[i + 2]) & 0x3F) << 6) |
                              (static_cast<unsigned char>(input[i + 3]) & 0x3F);
            if (cp >= 0xE0000 && cp <= 0xE007F) {
                i += 4;
                continue;
            }
        }
        out += input[i];
        ++i;
    }
    return out;
}

struct PromptTemplate::Impl {
    inja::Environment env;

    Impl() {
        env.set_trim_blocks(true);
        env.set_lstrip_blocks(true);
        env.set_line_statement("");
        env.add_callback("code_fence", 1, [](inja::Arguments& args) {
            return static_cast<inja::json>(code_fence(args.at(0)->get<std::string>()));
        });
    }

    const std::string_view* find_embedded(const std::string& name) const {
        for (const auto& t : embedded::kEmbeddedTemplates) {
            if (t.name == name) return &t.content;
        }
        return nullptr;
    }

    const char* find_embedded_description(const std::string& name) const {
        for (const auto& t : embedded::kEmbeddedTemplates) {
            if (t.name == name) return t.description.data();
        }
        return nullptr;
    }

    // Load template content: user override first, then embedded.
    Result<std::string> load_template(const std::string& name) const {
        std::filesystem::path user_file = user_prompts_dir() / name;
        if (std::filesystem::exists(user_file)) {
            std::ifstream ifs(user_file);
            return Result<std::string>::ok(
                std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>()));
        }
        const std::string_view* embedded_content = find_embedded(name);
        if (embedded_content) {
            return Result<std::string>::ok(std::string(*embedded_content));
        }
        return Result<std::string>::err(
            make_error(ErrorCode::ConfigError, "unknown template: " + name));
    }
};

PromptTemplate& PromptTemplate::global() {
    static PromptTemplate instance;
    return instance;
}

PromptTemplate::PromptTemplate() : impl_(std::make_unique<Impl>()) {}

std::string PromptTemplate::render_string(const std::string& tmpl, const nlohmann::json& context) {
    std::string normalized;
    try {
        normalized = normalize_template(sanitize_unicode_tags(tmpl), context);
        return trim_copy(impl_->env.render(normalized, context));
    } catch (const std::exception& e) {
        spdlog::warn("Template render failed: {}", e.what());
        return trim_copy(normalized.empty() ? tmpl : normalized);
    }
}

std::string PromptTemplate::render(const std::string& name, const nlohmann::json& context) {
    auto content = impl_->load_template(name);
    if (!content) {
        spdlog::warn("{}", content.error().message);
        return "";
    }
    return render_string(*content, context);
}

TemplateInfo PromptTemplate::get_template(const std::string& name) {
    TemplateInfo info;
    info.name = name;
    if (const char* desc = impl_->find_embedded_description(name)) {
        info.description = desc;
    }
    const std::string_view* embedded_content = impl_->find_embedded(name);
    if (embedded_content) info.default_content = std::string(*embedded_content);

    if (valid_template_name(name)) {
        std::filesystem::path user_file = user_prompts_dir() / name;
        if (std::filesystem::exists(user_file)) {
            std::ifstream ifs(user_file);
            info.user_content = std::string((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
            info.is_customized = true;
        }
    }
    return info;
}

void PromptTemplate::save_template(const std::string& name, const std::string& content) {
    if (!valid_template_name(name)) {
        spdlog::warn("Rejecting invalid template name '{}'", name);
        return;
    }
    auto dir = user_prompts_dir();
    std::filesystem::create_directories(dir);
    std::ofstream ofs(dir / name);
    if (!ofs.is_open()) {
        spdlog::error("Failed to save template {}: cannot open {}", name, (dir / name).string());
        return;
    }
    ofs << content;
    if (ofs.fail()) {
        spdlog::error("Failed to save template {}: write error", name);
    }
}

void PromptTemplate::reset_template(const std::string& name) {
    if (!valid_template_name(name)) {
        spdlog::warn("Rejecting invalid template name '{}'", name);
        return;
    }
    std::filesystem::path user_file = user_prompts_dir() / name;
    if (std::filesystem::exists(user_file)) {
        std::filesystem::remove(user_file);
    }
}

std::vector<TemplateInfo> PromptTemplate::list_templates() {
    std::vector<TemplateInfo> result;
    for (const auto& t : embedded::kEmbeddedTemplates) {
        TemplateInfo info;
        info.name = std::string(t.name);
        info.description = std::string(t.description);
        info.default_content = std::string(t.content);
        std::filesystem::path user_file = user_prompts_dir() / info.name;
        if (std::filesystem::exists(user_file)) {
            std::ifstream ifs(user_file);
            info.user_content = std::string((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
            info.is_customized = true;
        }
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace goose
