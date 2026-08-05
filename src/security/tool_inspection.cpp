#include "security/tool_inspection.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "security/permission_inspector.h"

namespace goose {

void ToolInspectionManager::add_inspector(std::unique_ptr<ToolInspector> inspector) {
    inspectors_.push_back(std::move(inspector));
}

std::vector<InspectionResult> ToolInspectionManager::inspect_tools(
    const std::vector<ToolRequest>& requests,
    GooseMode mode,
    const ModelConfig* model_config) const {

    std::vector<InspectionResult> all_results;
    for (const auto& inspector : inspectors_) {
        if (!inspector->is_enabled()) continue;
        spdlog::debug("Running tool inspector: {}", inspector->name());
        auto results = inspector->inspect(requests, mode, model_config);
        spdlog::debug("Tool inspector {} returned {} result(s)", inspector->name(), results.size());
        for (auto& result : results) {
            all_results.push_back(std::move(result));
        }
    }
    return all_results;
}

PermissionCheckResult ToolInspectionManager::check_all(
    const std::vector<ToolRequest>& requests,
    GooseMode mode,
    const ModelConfig* model_config) const {

    auto results = inspect_tools(requests, mode, model_config);
    return process_inspection_results(requests, results);
}

// Build the baseline from the permission inspector's decisions, defaulting to
// needs_approval for requests without a permission result, then apply the
// other inspectors' results as overrides.
PermissionCheckResult ToolInspectionManager::process_inspection_results(
    const std::vector<ToolRequest>& requests,
    const std::vector<InspectionResult>& results) const {

    PermissionCheckResult check_result;

    std::vector<const InspectionResult*> permission_results;
    for (const auto& r : results) {
        if (r.inspector_name == "permission") permission_results.push_back(&r);
    }

    for (const auto& request : requests) {
        const InspectionResult* permission_result = nullptr;
        for (const auto* r : permission_results) {
            if (r->tool_request_id == request.id) {
                permission_result = r;
                break;
            }
        }

        if (!permission_result) {
            check_result.needs_approval.push_back(request);
            continue;
        }

        switch (permission_result->action) {
            case InspectionAction::Allow:
                check_result.approved.push_back(request);
                break;
            case InspectionAction::Deny:
                check_result.denied.push_back(request);
                break;
            case InspectionAction::RequireApproval:
                check_result.needs_approval.push_back(request);
                break;
        }
    }

    return apply_inspection_results_to_permissions(std::move(check_result), results);
}

std::vector<std::string> ToolInspectionManager::inspector_names() const {
    std::vector<std::string> names;
    for (const auto& inspector : inspectors_) {
        names.push_back(inspector->name());
    }
    return names;
}

void ToolInspectionManager::apply_tool_annotations(const std::vector<Tool>& tools) {
    for (auto& inspector : inspectors_) {
        if (auto* permission = dynamic_cast<PermissionInspector*>(inspector.get())) {
            permission->apply_tool_annotations(tools);
            return;
        }
    }
}

PermissionCheckResult apply_inspection_results_to_permissions(
    PermissionCheckResult result,
    const std::vector<InspectionResult>& inspection_results) {

    if (inspection_results.empty()) return result;

    // Map of request id -> request, to restore full ToolRequest objects when
    // an override moves a request into a different bucket.
    std::vector<ToolRequest> all_requests = result.approved;
    all_requests.insert(all_requests.end(),
                        result.needs_approval.begin(), result.needs_approval.end());
    all_requests.insert(all_requests.end(),
                        result.denied.begin(), result.denied.end());

    auto find_request = [&](const std::string& id) -> const ToolRequest* {
        for (const auto& req : all_requests) {
            if (req.id == id) return &req;
        }
        return nullptr;
    };

    auto remove_from = [](std::vector<ToolRequest>& bucket, const std::string& id) {
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                    [&](const ToolRequest& t) { return t.id == id; }),
                     bucket.end());
    };

    for (const auto& r : inspection_results) {
        if (r.inspector_name == "permission") continue;

        const char* action_str = r.action == InspectionAction::Deny ? "BLOCK"
                               : r.action == InspectionAction::RequireApproval ? "ALERT"
                               : "ALLOW";
        spdlog::info(
            "inspection result applied: inspector={} action={} confidence={} finding={} request={} reason={}",
            r.inspector_name, action_str, r.confidence,
            r.finding_id.value_or(""), r.tool_request_id, r.reason);

        if (r.action == InspectionAction::Deny) {
            remove_from(result.approved, r.tool_request_id);
            remove_from(result.needs_approval, r.tool_request_id);
            if (std::none_of(result.denied.begin(), result.denied.end(),
                             [&](const ToolRequest& t) { return t.id == r.tool_request_id; })) {
                if (const auto* request = find_request(r.tool_request_id)) {
                    result.denied.push_back(*request);
                }
            }
        } else if (r.action == InspectionAction::RequireApproval) {
            remove_from(result.approved, r.tool_request_id);
            if (std::none_of(result.needs_approval.begin(), result.needs_approval.end(),
                             [&](const ToolRequest& t) { return t.id == r.tool_request_id; })) {
                if (const auto* request = find_request(r.tool_request_id)) {
                    result.needs_approval.push_back(*request);
                }
            }
        }
        // Allow never overrides other inspectors' decisions.
    }

    return result;
}

} // namespace goose
