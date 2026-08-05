#include "security/security_inspector.h"

#include <spdlog/spdlog.h>

#include <random>
#include <regex>

#include "../config/config.h"

namespace goose {

namespace {

enum class RiskLevel { Low, Medium, High, Critical };

float confidence_for(RiskLevel level) {
    switch (level) {
        case RiskLevel::Critical: return 0.95f;
        case RiskLevel::High: return 0.75f;
        case RiskLevel::Medium: return 0.60f;
        case RiskLevel::Low: return 0.45f;
    }
    return 0.45f;
}

struct ThreatPattern {
    const char* name;
    const char* pattern;
    const char* description;
    RiskLevel risk_level;
};

// Ported from goose's security/patterns.rs. Patterns target shell commands and
// injection attempts inside tool arguments.
const ThreatPattern kThreatPatterns[] = {
    {
        "rm_rf_root_bare",
        R"(rm\s+(-[rRfF]+\s+)*(-[rRfF]+|--recursive|--force|--no-preserve-root)(\s+(-[rRfF]+|--recursive|--force|--no-preserve-root))*\s+["']?/(\*)?["']?(\s|[;&|]|$))",
        "Recursive deletion of root filesystem",
        RiskLevel::Critical,
    },
    {
        "rm_rf_home_or_root",
        R"(rm\s+((--[a-zA-Z][a-zA-Z\-]*|--|-[a-zA-Z]+)\s+)*(-[a-zA-Z]*[rR][a-zA-Z]*|--recursive|--dir|-d)(\s+(--[a-zA-Z][a-zA-Z\-]*|--|-[a-zA-Z]+))*\s+["']?(~|\$HOME|\$\{HOME\}|/home|/root)/?(\*)?["']?(\s|[;&|]|$))",
        "Recursive deletion of home or root directory",
        RiskLevel::Critical,
    },
    {
        "dd_destruction",
        R"(dd\s+.*if=/dev/(zero|random|urandom).*of=/dev/[sh]d[a-z])",
        "Disk destruction using dd command",
        RiskLevel::Critical,
    },
    {
        "format_drive",
        R"((format|mkfs\.[a-z]+)\s+[/\\]dev[/\\][sh]d[a-z])",
        "Formatting system drives",
        RiskLevel::Critical,
    },
    {
        "curl_bash_execution",
        R"((curl|wget)\s+.*\|\s*(bash|sh|zsh|fish|csh|tcsh))",
        "Remote script execution via curl/wget piped to shell",
        RiskLevel::Critical,
    },
    {
        "bash_process_substitution",
        R"(bash\s*<\s*\(\s*(curl|wget))",
        "Bash process substitution with remote content",
        RiskLevel::High,
    },
    {
        "python_remote_exec",
        R"(python[23]?\s+-c\s+.*urllib|requests.*exec)",
        "Python remote code execution",
        RiskLevel::High,
    },
    {
        "powershell_download_exec",
        R"(powershell.*DownloadString.*Invoke-Expression)",
        "PowerShell remote script execution",
        RiskLevel::High,
    },
    {
        "ssh_key_exfiltration",
        R"((curl|wget).*-d.*\.ssh/(id_rsa|id_ed25519|id_ecdsa))",
        "SSH key exfiltration",
        RiskLevel::High,
    },
    {
        "password_file_access",
        R"((cat|grep|awk|sed).*(/etc/shadow|\.password))",
        "Password/shadow file access",
        RiskLevel::High,
    },
    {
        "history_exfiltration",
        R"((curl|wget).*-d.*\.(bash_history|zsh_history|history))",
        "Command history exfiltration",
        RiskLevel::High,
    },
    {
        "crontab_modification",
        R"((crontab\s+-e|echo.*>.*crontab|.*>\s*/var/spool/cron))",
        "Crontab modification for persistence",
        RiskLevel::High,
    },
    {
        "systemd_service_creation",
        R"(systemctl.*enable|.*\.service.*>/etc/systemd)",
        "Systemd service creation",
        RiskLevel::High,
    },
    {
        "hosts_file_modification",
        R"(echo.*>(>?)\s*(/etc/hosts|hosts\.txt)|echo.*\|\s*(sudo\s+)?tee\s+(-a\s+)?/etc/hosts)",
        "Hosts file modification",
        RiskLevel::Medium,
    },
    {
        "netcat_listener",
        R"(nc\s+(-l|-p)\s+\d+)",
        "Netcat listener creation",
        RiskLevel::High,
    },
    {
        "reverse_shell",
        R"((nc|netcat|bash|sh).*-e\s*(bash|sh|/bin/bash|/bin/sh))",
        "Reverse shell creation",
        RiskLevel::Critical,
    },
    {
        "ssh_tunnel",
        R"(ssh\s+.*-[LRD]\s+\d+:)",
        "SSH tunnel creation",
        RiskLevel::Medium,
    },
    {
        "kill_security_process",
        R"(kill(all)?\s+.*\b(antivirus|firewall|defender|security|monitor)\b)",
        "Killing security processes",
        RiskLevel::High,
    },
    {
        "process_injection",
        R"(gdb\s+.*attach|ptrace.*PTRACE_POKETEXT)",
        "Process injection techniques",
        RiskLevel::High,
    },
    {
        "sudo_without_password",
        R"(echo.*NOPASSWD.*>.*sudoers)",
        "Sudo privilege escalation",
        RiskLevel::Critical,
    },
    {
        "suid_binary_creation",
        R"(chmod\s+[47][0-7][0-7][0-7]|chmod\s+\+s)",
        "SUID binary creation",
        RiskLevel::High,
    },
    {
        "encoded_commands",
        R"((base64|hex|url).*decode.*\|\s*(bash|sh))",
        "Encoded command execution",
        RiskLevel::High,
    },
    {
        "base64_encoded_shell",
        R"((echo|printf)\s+[A-Za-z0-9+/=]{20,}\s*\|\s*base64\s+-d\s*\|\s*(bash|sh|zsh))",
        "Base64 encoded shell commands",
        RiskLevel::High,
    },
    {
        "hex_encoded_commands",
        R"((echo|printf)\s+[0-9a-fA-F\\x]{20,}\s*\|\s*(xxd|od).*\|\s*(bash|sh))",
        "Hex encoded command execution",
        RiskLevel::High,
    },
    {
        "eval_with_variables",
        R"(eval\s+\$[A-Za-z_][A-Za-z0-9_]*|\beval\s+.*\$\{)",
        "Eval with variable substitution",
        RiskLevel::High,
    },
    {
        "indirect_command_execution",
        R"(\$\([^)]*\$\([^)]*\)[^)]*\)|`[^`]*`[^`]*`)",
        "Nested command substitution",
        RiskLevel::Low,
    },
    {
        "unicode_obfuscation",
        R"((\\u[0-9a-fA-F]{4}|\\U[0-9a-fA-F]{8}){3,})",
        "Unicode character obfuscation (3+ consecutive escapes)",
        RiskLevel::Medium,
    },
    {
        "docker_privileged_exec",
        R"(docker\s+(run|exec).*--privileged)",
        "Docker privileged container execution",
        RiskLevel::High,
    },
    {
        "container_escape",
        R"((chroot|unshare|nsenter).*--mount|--pid|--net)",
        "Container escape techniques",
        RiskLevel::High,
    },
    {
        "kernel_module_manipulation",
        R"((insmod|rmmod|modprobe).*\.ko)",
        "Kernel module manipulation",
        RiskLevel::Critical,
    },
    {
        "memory_dump",
        R"((gcore|gdb.*dump|/proc/[0-9]+/mem))",
        "Memory dumping techniques",
        RiskLevel::High,
    },
    {
        "log_manipulation",
        R"((truncate.*log|rm\s+((--[a-zA-Z][a-zA-Z\-]*|--|-[a-zA-Z]+)\s+)*/var/log(/|\s|[;&|]|$)|echo\s*>\s*/var/log))",
        "Log file manipulation or deletion",
        RiskLevel::Medium,
    },
    {
        "file_timestamp_manipulation",
        R"(touch\s+-[amt]\s+|utimes|futimes)",
        "File timestamp manipulation",
        RiskLevel::Low,
    },
    {
        "steganography_tools",
        R"(\b(steghide|outguess|jphide|steganos)\b)",
        "Steganography tools usage",
        RiskLevel::Medium,
    },
    {
        "network_scanning",
        R"(\b(nmap|masscan|zmap|unicornscan)\b.*-[sS])",
        "Network scanning tools",
        RiskLevel::Medium,
    },
    {
        "password_cracking_tools",
        R"(\bjohn\s+--[a-z]|\b(hashcat|hydra|medusa|brutespray)\b)",
        "Password cracking tools",
        RiskLevel::High,
    },
};

constexpr size_t kPatternCount = sizeof(kThreatPatterns) / sizeof(kThreatPatterns[0]);

const std::vector<std::regex>& compiled_patterns() {
    static const auto patterns = [] {
        std::vector<std::regex> compiled;
        compiled.reserve(kPatternCount);
        for (const auto& threat : kThreatPatterns) {
            compiled.emplace_back(threat.pattern,
                                  std::regex::ECMAScript | std::regex::icase);
        }
        return compiled;
    }();
    return patterns;
}

std::string random_hex_id() {
    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    std::uniform_int_distribution<uint64_t> dist;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(rng)));
    return std::string(buf, 8);
}

// Serialize tool arguments to a single lowercase string for pattern matching.
std::string stringify_arguments(const nlohmann::json& args) {
    std::string out = args.dump();
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

bool SecurityManager::is_prompt_injection_detection_enabled() {
    const char* override = std::getenv("SECURITY_PROMPT_ENABLED_OVERRIDE");
    if (override) {
        std::string value(override);
        if (value == "true") return true;
        if (value == "false") return false;
    }
    auto param = Config::global().get_param("SECURITY_PROMPT_ENABLED");
    return param && *param == "true";
}

std::vector<SecurityResult> SecurityManager::analyze_tool_requests(
    const std::vector<ToolRequest>& requests) const {

    if (!is_prompt_injection_detection_enabled()) {
        return {};
    }

    const auto& patterns = compiled_patterns();
    std::vector<SecurityResult> results;

    for (const auto& request : requests) {
        if (!std::holds_alternative<CallToolRequestParams>(request.tool_call)) continue;
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);

        std::string text = params.name + " " + stringify_arguments(params.arguments);

        const ThreatPattern* best = nullptr;
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (std::regex_search(text, patterns[i])) {
                if (!best || kThreatPatterns[i].risk_level > best->risk_level) {
                    best = &kThreatPatterns[i];
                }
            }
        }

        if (!best) continue;

        float confidence = confidence_for(best->risk_level);
        std::string finding_id = "SEC-" + random_hex_id();

        std::string sanitized = std::string(best->description);
        std::replace(sanitized.begin(), sanitized.end(), '\n', ' ');

        spdlog::warn(
            "prompt injection scan: pattern={} confidence={} finding={} tool={} request={}",
            best->name, confidence, finding_id, params.name, request.id);

        if (confidence > kAskUserThreshold) {
            results.push_back(SecurityResult{
                true,
                confidence,
                sanitized,
                true,
                finding_id,
                request.id,
            });
        }
    }
    return results;
}

std::vector<InspectionResult> SecurityInspector::inspect(
    const std::vector<ToolRequest>& requests,
    GooseMode /*mode*/,
    const ModelConfig* /*model_config*/) {

    auto security_results = SecurityManager{}.analyze_tool_requests(requests);

    std::vector<InspectionResult> results;
    for (const auto& sr : security_results) {
        InspectionResult result;
        result.tool_request_id = sr.tool_request_id;
        result.action = sr.should_ask_user
            ? InspectionAction::RequireApproval
            : InspectionAction::Allow;
        result.reason = sr.explanation;
        result.confidence = sr.confidence;
        result.inspector_name = name();
        result.finding_id = sr.finding_id;
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace goose
