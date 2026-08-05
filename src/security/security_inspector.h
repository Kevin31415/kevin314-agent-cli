#pragma once

#include <string>
#include <vector>

#include "tool_inspection.h"

namespace goose {

struct SecurityResult {
    bool is_malicious = false;
    float confidence = 0.0f;
    std::string explanation;
    bool should_ask_user = false;
    std::string finding_id;
    std::string tool_request_id;
};

// Pattern-matching prompt-injection scanner. Detection is off by default and
// can be enabled via the SECURITY_PROMPT_ENABLED config param or the
// SECURITY_PROMPT_ENABLED_OVERRIDE environment variable.
class SecurityManager {
public:
    static bool is_prompt_injection_detection_enabled();

    // Analyze tool requests against the threat pattern table. Only threats
    // above the ask-user threshold produce a result.
    std::vector<SecurityResult> analyze_tool_requests(
        const std::vector<ToolRequest>& requests) const;

private:
    static constexpr float kAskUserThreshold = 0.8f;
};

// Tool inspector wrapping SecurityManager; disabled unless prompt-injection
// detection is enabled.
class SecurityInspector : public ToolInspector {
public:
    SecurityInspector() = default;

    std::string name() const override { return "security"; }
    bool is_enabled() const override {
        return SecurityManager::is_prompt_injection_detection_enabled();
    }

    std::vector<InspectionResult> inspect(
        const std::vector<ToolRequest>& requests,
        GooseMode /*mode*/,
        const ModelConfig* /*model_config*/) override;
};

} // namespace goose
