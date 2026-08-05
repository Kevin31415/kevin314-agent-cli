#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tool_inspection.h"

namespace goose {

// Denies tool calls that repeat the same name+arguments more than
// max_repetitions times consecutively. Stateless per inspect() call: the
// running state is committed only for allowed calls, so a denied call does
// not advance the repeat counter (matches goose's tool_monitor behavior).
class RepetitionInspector : public ToolInspector {
public:
    explicit RepetitionInspector(std::optional<uint32_t> max_repetitions)
        : max_repetitions_(max_repetitions) {}

    std::string name() const override { return "repetition"; }

    std::vector<InspectionResult> inspect(
        const std::vector<ToolRequest>& requests,
        GooseMode /*mode*/,
        const ModelConfig* /*model_config*/) override;

    // Returns true if the call is allowed; records it as the last call.
    bool check_tool_call(const CallToolRequestParams& tool_call);

    void reset();

private:
    struct InternalToolCall {
        std::string name;
        nlohmann::json parameters;

        bool matches(const InternalToolCall& other) const {
            return name == other.name && parameters == other.parameters;
        }
    };

    static InternalToolCall to_internal(const CallToolRequestParams& tool_call);

    std::optional<uint32_t> max_repetitions_;
    std::optional<InternalToolCall> last_call_;
    uint32_t repeat_count_ = 0;
    std::unordered_map<std::string, uint32_t> call_counts_;
};

} // namespace goose
