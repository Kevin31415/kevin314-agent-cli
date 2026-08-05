#include "security/repetition_inspector.h"

namespace goose {

RepetitionInspector::InternalToolCall RepetitionInspector::to_internal(
    const CallToolRequestParams& tool_call) {
    InternalToolCall call;
    call.name = tool_call.name;
    call.parameters = tool_call.arguments.is_null() ? nlohmann::json{nullptr}
                                                    : tool_call.arguments;
    return call;
}

bool RepetitionInspector::check_tool_call(const CallToolRequestParams& tool_call) {
    InternalToolCall internal_call = to_internal(tool_call);
    call_counts_[internal_call.name]++;

    if (!max_repetitions_) {
        last_call_ = std::move(internal_call);
        repeat_count_ = 1;
        return true;
    }

    if (last_call_ && last_call_->matches(internal_call)) {
        repeat_count_++;
        if (repeat_count_ > *max_repetitions_) {
            return false;
        }
    } else {
        repeat_count_ = 1;
    }

    last_call_ = std::move(internal_call);
    return true;
}

void RepetitionInspector::reset() {
    last_call_.reset();
    repeat_count_ = 0;
    call_counts_.clear();
}

std::vector<InspectionResult> RepetitionInspector::inspect(
    const std::vector<ToolRequest>& requests,
    GooseMode /*mode*/,
    const ModelConfig* /*model_config*/) {

    std::vector<InspectionResult> results;

    for (const auto& request : requests) {
        if (!std::holds_alternative<CallToolRequestParams>(request.tool_call)) continue;
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);

        // Evaluate against a temporary copy so a denied call does not advance
        // the repetition state; only allowed calls commit below.
        RepetitionInspector temp(max_repetitions_);
        temp.last_call_ = last_call_;
        temp.repeat_count_ = repeat_count_;
        temp.call_counts_ = call_counts_;

        if (!temp.check_tool_call(params)) {
            InspectionResult result;
            result.tool_request_id = request.id;
            result.action = InspectionAction::Deny;
            result.reason = "Tool '" + params.name + "' has exceeded maximum repetitions";
            result.confidence = 1.0f;
            result.inspector_name = name();
            result.finding_id = "REP-001";
            results.push_back(std::move(result));
            continue;
        }

        check_tool_call(params);
    }

    return results;
}

} // namespace goose
