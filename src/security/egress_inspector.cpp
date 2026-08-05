#include "security/egress_inspector.h"

#include <spdlog/spdlog.h>

#include <regex>

namespace goose {

namespace {

struct EgressDestination {
    std::string kind;
    std::string destination;
    std::string domain;
};

// Extract destinations from a tool's arguments rendered as a single line.
std::vector<EgressDestination> extract_destinations(const std::string& text) {
    std::vector<EgressDestination> destinations;

    static const std::regex url_re(R"((https?|ftp)://[^\s'"<>|;&)]+)", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), url_re), end; it != end; ++it) {
        std::string url = it->str();
        std::string domain;
        size_t scheme_end = url.find("://");
        size_t path_start = url.find_first_of("/?", scheme_end + 3);
        domain = url.substr(scheme_end + 3,
                            path_start == std::string::npos ? std::string::npos
                                                            : path_start - scheme_end - 3);
        destinations.push_back(EgressDestination{"url", url, domain});
    }

    static const std::regex git_ssh_re(R"(git@([^:]+):([^\s'"]+))");
    for (std::sregex_iterator it(text.begin(), text.end(), git_ssh_re), end; it != end; ++it) {
        destinations.push_back(
            EgressDestination{"git_remote", it->str(), (*it)[1].str()});
    }

    static const std::regex s3_re(R"(s3://([^/\s'"]+)(/[^\s'"]*)?)");
    for (std::sregex_iterator it(text.begin(), text.end(), s3_re), end; it != end; ++it) {
        destinations.push_back(
            EgressDestination{"s3_bucket", it->str(), (*it)[1].str() + ".s3.amazonaws.com"});
    }

    static const std::regex gcs_re(R"(gs://([^/\s'"]+)(/[^\s'"]*)?)");
    for (std::sregex_iterator it(text.begin(), text.end(), gcs_re), end; it != end; ++it) {
        destinations.push_back(
            EgressDestination{"gcs_bucket", it->str(), (*it)[1].str() + ".storage.googleapis.com"});
    }

    static const std::regex scp_re(R"((scp|rsync)\s+.*?(\S+@)?([a-zA-Z0-9][\w.-]+):)");
    for (std::sregex_iterator it(text.begin(), text.end(), scp_re), end; it != end; ++it) {
        destinations.push_back(EgressDestination{"scp_target", it->str(), (*it)[3].str()});
    }

    static const std::regex ssh_re(R"(ssh\s+.*?(\S+@)?([a-zA-Z0-9][\w.-]+))");
    for (std::sregex_iterator it(text.begin(), text.end(), ssh_re), end; it != end; ++it) {
        std::string host = (*it)[2].str();
        if (host[0] == '-') continue;
        destinations.push_back(EgressDestination{"ssh_host", it->str(), host});
    }

    return destinations;
}

} // namespace

std::vector<InspectionResult> EgressInspector::inspect(
    const std::vector<ToolRequest>& requests,
    GooseMode /*mode*/,
    const ModelConfig* /*model_config*/) {

    std::vector<InspectionResult> results;

    for (const auto& request : requests) {
        if (!std::holds_alternative<CallToolRequestParams>(request.tool_call)) continue;
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);

        std::string text = params.name + " " + params.arguments.dump();
        auto destinations = extract_destinations(text);
        if (destinations.empty()) continue;

        for (const auto& dest : destinations) {
            spdlog::info(
                "egress: kind={} domain={} destination={} tool={} request={}",
                dest.kind, dest.domain, dest.destination, params.name, request.id);
        }

        InspectionResult result;
        result.tool_request_id = request.id;
        result.action = InspectionAction::Allow;
        result.reason = "Network egress logged";
        result.confidence = 1.0f;
        result.inspector_name = name();
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace goose
