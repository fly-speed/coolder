# coolder 语言模板

界面支持中文（zh）、英文（en）、日文（ja）、韩文（ko）。用户在个人设置保存语言、配色、字号，服务端按账户持久保存。修改密码仍需验证原密码。

## 添加语言

1. 复制 `template.json` 为语言代码文件，例如 `fr.json`。代码限 1–24 个英文字母、数字或连字符，支持 `pt-BR` 等地区变体。
2. 保持 JSON 键不变，将空字符串填写为目标语言译文。键是中文源文案；空值或缺少的键会回退到中文。
3. 在 `manifest.json` 数组增加 `{ "code": "fr", "name": "Français" }`。名称使用语言本身的写法。
4. 随应用部署整个 `html/i18n` 目录。刷新页面后，新语言出现在个人设置中，无需修改后端或重新编译。

语言文件只能包含纯文本，不支持 HTML 或脚本。语言设置控制应用界面，不翻译用户名称、项目名称、文件内容、模型输出或服务端诊断原文；编程任务的自然语言由任务内容决定。

## 开发约定

静态文案使用 `data-i18n="中文源文案"`，属性用 `data-i18n-title`、`data-i18n-placeholder`、`data-i18n-aria-label`。动态文案使用 `t('中文源文案')`。需随语言实时更新的文本用 `uiText(element, () => t('文案'))`，或给 `node`/`button` 传入返回文案的函数。用户原文用 `plainText`，避免保留之前的翻译绑定。不要将 API 返回的用户内容加入翻译器。

添加文案时同时更新四种语言文件和模板。主题值：default、blue、green、purple、sand、gray、pink、ocean；字号值：sm（12px）、md（14px）、lg（16px），其他界面文字按比例缩放。

偏好接口：GET/POST `/api/v1/auth/preferences`，字段 `language`、`theme`、`font_size`；POST 支持局部更新。账户由登录会话确定，不接受指定其他账户。独立保存在 `auth/ui-<account-id>.v1`，不会覆盖模型偏好。
