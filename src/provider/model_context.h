#pragma once

#include <cstdint>
#include <string>

namespace goose {

/// 解析模型上下文窗口上限（tokens）。
/// 优先级: env GOOSE_CONTEXT_LIMIT > 内置模型映射表 > kDefaultContextLimit。
int64_t resolve_context_limit(const std::string& model_name);

} // namespace goose
