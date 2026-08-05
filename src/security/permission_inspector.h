#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "permission_manager.h"
#include "tool_inspection.h"
#include "../provider/base.h"

namespace goose {

// SmartApprove core logic. Decision order:
//   1. user permission -> AlwaysAllow/NeverAllow/AskBefore
//   2. SmartApprove + read-only annotation -> Allow
//   3. SmartApprove + no/legacy cache -> LLM read-only detection
//   4. default -> RequireApproval
// Non-read-only LLM verdicts are cached globally (smart_approve/AskBefore).
class PermissionInspector : public ToolInspector {
public:
    PermissionInspector(std::shared_ptr<PermissionManager> permission_manager,
                        std::shared_ptr<Provider> provider);

    std::string name() const override { return "permission"; }

    void apply_tool_annotations(const std::vector<Tool>& tools);
    bool is_readonly_annotated_tool(const std::string& tool_name) const;

    std::vector<InspectionResult> inspect(
        const std::vector<ToolRequest>& requests,
        GooseMode mode,
        const ModelConfig* model_config) override;

    // Combine all inspectors' results into approved/needs_approval/denied,
    // using this inspector's decisions as the baseline.
    PermissionCheckResult process_inspection_results(
        const std::vector<ToolRequest>& remaining_requests,
        const std::vector<InspectionResult>& inspection_results) const;

    std::shared_ptr<PermissionManager> permission_manager() const { return permission_manager_; }

    // Read-only detection that stays open for verification in tests.
    std::vector<std::string> detect_read_only_requests(
        const std::vector<ToolRequest>& tool_requests,
        const ModelConfig& model_config);

private:
    std::shared_ptr<PermissionManager> permission_manager_;
    std::shared_ptr<Provider> provider_;
    std::unordered_set<std::string> readonly_tools_;
};

} // namespace goose
