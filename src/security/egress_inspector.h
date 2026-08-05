#pragma once

#include <string>
#include <vector>

#include "tool_inspection.h"

namespace goose {

// Logs network destinations referenced in tool requests (URLs, git remotes,
// cloud buckets, ssh/scp targets). Log-only: never blocks or alters decisions.
class EgressInspector : public ToolInspector {
public:
    std::string name() const override { return "egress"; }

    std::vector<InspectionResult> inspect(
        const std::vector<ToolRequest>& requests,
        GooseMode /*mode*/,
        const ModelConfig* /*model_config*/) override;
};

} // namespace goose
