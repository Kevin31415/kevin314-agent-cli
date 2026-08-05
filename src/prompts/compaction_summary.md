{#
  此模板可由用户覆盖：将修改后的副本放置在
  ~/.kacli/prompts/compaction_summary.md 以实验后压缩上下文包含的内容（例如 `user_intent[:3]` 以仅保留最重要的三个目标），而无需重新构建 kacli。

  key_code 通过 code_fence 过滤器包裹，因此嵌入的代码块无法超出此代码块。
#}
# 对话总结

{% if user_intent %}
## 用户意图
{% for item in user_intent %}
- {{ item }}
{% endfor %}

{% endif %}
{% if technical_concepts %}
## 技术概念
{% for item in technical_concepts %}
- {{ item }}
{% endfor %}

{% endif %}
{% if files %}
## 文件 + 代码
{% for file in files %}
{% if file.path %}
### {{ file.path }}
{% endif %}
{{ file.summary }}
{% if file.key_code %}
{{ file.key_code | code_fence }}
{% endif %}

{% endfor %}
{% endif %}
{% if errors_and_fixes %}
## 错误 + 修复
{% for item in errors_and_fixes %}
- {{ item }}
{% endfor %}

{% endif %}
{% if problem_solving %}
## 问题解决
{% for item in problem_solving %}
- {{ item }}
{% endfor %}

{% endif %}
{% if user_messages %}
## 用户消息
{% for item in user_messages %}
- {{ item }}
{% endfor %}

{% endif %}
{% if pending_tasks %}
## 待办任务
{% for item in pending_tasks %}
- {{ item }}
{% endfor %}

{% endif %}
{% if current_work %}
## 当前工作
{{ current_work }}

{% endif %}
{% if next_step %}
## 下一步
{{ next_step }}
{% endif %}
