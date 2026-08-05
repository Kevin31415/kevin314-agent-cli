#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../core/goose_mode.h"
#include "../core/tool.h"
#include "../core/types.h"
#include "../provider/model_config.h"

namespace goose {

// ============ Inspection types ============

enum class InspectionAction { Allow, Deny, RequireApproval };

struct InspectionResult {
    std::string tool_request_id;
    InspectionAction action;
    std::string reason;
    float confidence = 1.0f;
    std::string inspector_name;
    std::optional<std::string> finding_id;
};

struct PermissionCheckResult {
    std::vector<ToolRequest> approved;
    std::vector<ToolRequest> needs_approval;
    std::vector<ToolRequest> denied;
};

// ============ ToolInspector 接口 ============

class ToolInspector {
public:
    virtual ~ToolInspector() = default;

    virtual std::string name() const = 0;

    virtual bool is_enabled() const { return true; }

    // Inspect the given tool requests. Returns one result per request that
    // needs a decision; requests without a result defer to other inspectors.
    virtual std::vector<InspectionResult> inspect(
        const std::vector<ToolRequest>& requests,
        GooseMode mode,
        const ModelConfig* model_config) = 0;
};

// Generic permission-mixing logic: non-permission inspection results override
// the permission baseline (Deny moves to denied, RequireApproval moves to
// needs_approval, Allow never overrides).
PermissionCheckResult apply_inspection_results_to_permissions(
    PermissionCheckResult result,
    const std::vector<InspectionResult>& inspection_results);

// ============ ToolInspectionManager ============

class ToolInspectionManager {
public:
    void add_inspector(std::unique_ptr<ToolInspector> inspector);

    // Run all enabled inspectors in registration order.
    std::vector<InspectionResult> inspect_tools(
        const std::vector<ToolRequest>& requests,
        GooseMode mode,
        const ModelConfig* model_config) const;

    // inspect_tools + combine results into approved/needs_approval/denied.
    PermissionCheckResult check_all(
        const std::vector<ToolRequest>& requests,
        GooseMode mode,
        const ModelConfig* model_config) const;

    // Combine inspection results into approved/needs_approval/denied using
    // the permission inspector's decisions as the baseline.
    PermissionCheckResult process_inspection_results(
        const std::vector<ToolRequest>& requests,
        const std::vector<InspectionResult>& results) const;

    std::vector<std::string> inspector_names() const;

    // Push tool annotations into the permission inspector's read-only set and
    // the permission manager's smart_approve cache.
    void apply_tool_annotations(const std::vector<Tool>& tools);

private:
    std::vector<std::unique_ptr<ToolInspector>> inspectors_;
};

} // namespace goose
