#include "stdafx.h"
#include "project_plan_template.h"

namespace webcool {
namespace ai {
namespace {

std::string join_relative(const std::string& parent, const std::string& child) {
	return parent.empty() ? child : parent + "/" + child;
}

agent_project_module_t module(const char* id, const char* name,
	const char* layer, const std::string& path)
{
	agent_project_module_t value;
	value.id = id;
	value.name = name;
	value.layer = layer;
	value.path = path;
	return value;
}

agent_project_task_t task(const char* id, const char* title,
	const char* module_id)
{
	agent_project_task_t value;
	value.id = id;
	value.title = title;
	value.module_id = module_id;
	value.status = "pending";
	return value;
}

bool contains_any(const std::string& text,
	const char* const* words, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		if (text.find(words[i]) != std::string::npos) return true;
	}
	return false;
}

// User goals are deliberately converted into observable acceptance criteria.
// A plan that only says "implement adapters" allowed a browser game to drift
// into a terminal program: both have adapters, but only one satisfies the
// requested delivery channel.  This small language-neutral classifier keeps
// the plan deterministic; the model may refine it later through the reviewed
// project-plan API.
void append_delivery_acceptance(const std::string& goal,
	std::vector<agent_project_task_t>& tasks)
{
	const char* const web_words[] = {
		"browser", "web", "http", "html", "浏览器", "网页", "网站"
	};
	const char* const realtime_words[] = {
		"websocket", "real-time", "realtime", "在线", "实时", "连线", "多人"
	};
	const char* const persistence_words[] = {
		"database", "storage", "persist", "save", "数据库", "存储", "持久化", "保存"
	};
	const char* const security_words[] = {
		"auth", "login", "permission", "multi-user", "登录", "鉴权", "权限", "多用户"
	};
	const char* const cli_words[] = {
		"command line", "terminal", "console", "cli", "命令行", "终端", "控制台"
	};
	const char* const performance_words[] = {
		"performance", "concurrent", "latency", "throughput", "性能", "并发", "延迟", "吞吐"
	};
	const bool web = contains_any(goal, web_words,
		sizeof(web_words) / sizeof(web_words[0]));
	const bool realtime = contains_any(goal, realtime_words,
		sizeof(realtime_words) / sizeof(realtime_words[0]));
	const bool persistence = contains_any(goal, persistence_words,
		sizeof(persistence_words) / sizeof(persistence_words[0]));
	const bool security = contains_any(goal, security_words,
		sizeof(security_words) / sizeof(security_words[0]));
	const bool cli = contains_any(goal, cli_words,
		sizeof(cli_words) / sizeof(cli_words[0]));
	const bool performance = contains_any(goal, performance_words,
		sizeof(performance_words) / sizeof(performance_words[0]));
	if (!web && !realtime && !persistence && !security && !cli
		&& !performance) return;
	for (size_t i = 0; i < tasks.size(); ++i) {
		if (tasks[i].id == "implement-adapters"
			|| tasks[i].id == "deliver-mvp") {
			if (web) {
				tasks[i].acceptance_criteria.push_back(
					"HTTP 服务可启动，浏览器入口返回成功状态且静态资源可加载");
				tasks[i].test_plan.push_back("启动服务并执行 HTTP 健康探测");
			}
			if (realtime) {
				tasks[i].acceptance_criteria.push_back(
					"至少两个独立客户端可连接并观察到一致的共享状态");
				tasks[i].test_plan.push_back("运行双客户端实时通信集成测试");
			}
			if (persistence) {
				tasks[i].acceptance_criteria.push_back(
					"保存后的状态在进程重启后仍可恢复，损坏或失败路径不会静默丢失数据");
				tasks[i].test_plan.push_back("执行保存、重启、恢复及受控写入失败测试");
			}
			if (security) {
				tasks[i].acceptance_criteria.push_back(
					"未授权访问被拒绝且不同用户的数据和运行状态相互隔离");
				tasks[i].test_plan.push_back("使用两个用户验证授权边界和跨用户重放拒绝");
			}
			if (cli) {
				tasks[i].acceptance_criteria.push_back(
					"命令行入口具有确定的参数、标准输出错误输出和退出码契约");
				tasks[i].test_plan.push_back("验证正常、非法参数及运行失败的退出码和输出");
			}
		}
		if (tasks[i].id == "quality-gate") {
			tasks[i].acceptance_criteria.push_back(
				"只有可运行交付物通过端到端验收后才可报告项目完成");
			if (performance) {
				tasks[i].acceptance_criteria.push_back(
					"在目标并发或数据规模下资源使用有界且延迟满足项目目标");
				tasks[i].test_plan.push_back("执行可重复的并发负载与资源上限测试");
			}
		}
	}
}

void build_quick_plan(const std::string& project_path,
	const std::string& language, const std::string& platform,
	const std::string& requested_goal, std::string& goal,
	std::vector<agent_project_module_t>& modules,
	std::vector<agent_project_task_t>& tasks)
{
	modules.clear();
	tasks.clear();
	goal = requested_goal.empty()
		? "以" + language + "先交付可运行、可验证的" + platform + "最小版本"
		: requested_goal;
	agent_project_module_t app = module("app", "可运行程序", "delivery",
		project_path);
	agent_project_module_t tests = module("tests", "自动化验证", "verification",
		join_relative(project_path, "tests"));
	tests.dependencies.push_back("app");
	modules.push_back(app);
	modules.push_back(tests);

	agent_project_task_t mvp = task("deliver-mvp",
		"贯通用户要求的最小可运行纵向流程", "app");
	mvp.acceptance_criteria.push_back("程序可以使用所选平台的标准入口启动");
	mvp.acceptance_criteria.push_back("用户要求的主流程可以实际操作而非仅有接口骨架");
	mvp.test_plan.push_back("构建并启动最小交付物，验证主流程");
	agent_project_task_t gate = task("quality-gate",
		"验证构建、测试和用户可见交付物", "tests");
	gate.dependencies.push_back("deliver-mvp");
	gate.acceptance_criteria.push_back("固定命令可重复完成构建和自动化测试");
	gate.test_plan.push_back("在干净的智能体草稿工作区执行完整质量门禁");
	tasks.push_back(mvp);
	tasks.push_back(gate);
	append_delivery_acceptance(goal, tasks);
}

} // namespace

void build_project_plan_template(const std::string& project_path,
	const std::string& language, const std::string& platform,
	std::string& goal, std::vector<agent_project_module_t>& modules,
	std::vector<agent_project_task_t>& tasks)
{
	modules.clear();
	tasks.clear();
	goal = "以" + language + "开发可模块化、可测试、可调试且可预测的"
		+ platform + "项目；每次只推进一个有明确验收标准的任务。";

	agent_project_module_t core = module("core", "核心领域", "domain",
		join_relative(project_path, "src/core"));
	agent_project_module_t application = module("application", "应用编排",
		"application", join_relative(project_path, "src/application"));
	application.dependencies.push_back("core");
	agent_project_module_t adapters = module("adapters", "平台与外部适配",
		"infrastructure", join_relative(project_path, "src/adapters"));
	adapters.dependencies.push_back("application");
	agent_project_module_t tests = module("tests", "自动化验证", "verification",
		join_relative(project_path, "tests"));
	tests.dependencies.push_back("core");
	tests.dependencies.push_back("application");
	tests.dependencies.push_back("adapters");
	modules.push_back(core);
	modules.push_back(application);
	modules.push_back(adapters);
	modules.push_back(tests);

	agent_project_task_t boundaries = task("define-boundaries",
		"确定模块职责、接口和依赖方向", "core");
	boundaries.acceptance_criteria.push_back("模块目录、公开接口和依赖方向已记录");
	boundaries.acceptance_criteria.push_back("核心领域不依赖平台适配代码");
	boundaries.test_plan.push_back("运行当前工程的基础构建命令");

	agent_project_task_t core_task = task("implement-core",
		"实现第一组核心领域能力", "core");
	core_task.dependencies.push_back("define-boundaries");
	core_task.acceptance_criteria.push_back("核心逻辑具有明确输入、输出和错误路径");
	core_task.test_plan.push_back("为核心逻辑增加无外部依赖的单元测试");

	agent_project_task_t application_task = task("implement-application",
		"实现应用层用例编排", "application");
	application_task.dependencies.push_back("implement-core");
	application_task.acceptance_criteria.push_back("应用层只通过稳定接口调用核心层");
	application_task.test_plan.push_back("覆盖成功路径和主要失败路径");

	agent_project_task_t adapter_task = task("implement-adapters",
		"实现目标平台适配层", "adapters");
	adapter_task.dependencies.push_back("implement-application");
	adapter_task.acceptance_criteria.push_back("平台细节不泄漏到核心领域模块");
	adapter_task.test_plan.push_back("在所选运行平台执行集成构建");

	agent_project_task_t verification = task("quality-gate",
		"建立可重复的构建、测试与调试入口", "tests");
	verification.dependencies.push_back("implement-adapters");
	verification.acceptance_criteria.push_back("一次固定命令可完成构建和自动化测试");
	verification.acceptance_criteria.push_back("失败输出可以定位到模块和测试用例");
	verification.test_plan.push_back("在干净工作区执行完整质量门禁");
	tasks.push_back(boundaries);
	tasks.push_back(core_task);
	tasks.push_back(application_task);
	tasks.push_back(adapter_task);
	tasks.push_back(verification);
}

bool build_project_plan_proposal(const std::string& project_path,
	const std::string& language, const std::string& platform,
	const std::string& requested_goal, const std::string& scale,
	std::string& goal, std::vector<agent_project_module_t>& modules,
	std::vector<agent_project_task_t>& tasks, std::string& err)
{
	if (scale != "quick" && scale != "standard" && scale != "large") {
		err = "project plan scale must be quick, standard or large";
		return false;
	}
	if (scale == "quick") {
		build_quick_plan(project_path, language, platform, requested_goal,
			goal, modules, tasks);
		return true;
	}
	build_project_plan_template(project_path, language, platform, goal,
		modules, tasks);
	if (!requested_goal.empty()) goal = requested_goal;
	append_delivery_acceptance(goal, tasks);
	if (scale == "standard") return true;

	// Large plans keep stable IDs from the compact template so completed task
	// state can survive replanning. New tasks form explicit, reviewable gates.
	agent_project_module_t ui = module("presentation", "交互与呈现",
		"presentation", join_relative(project_path, "src/presentation"));
	ui.dependencies.push_back("application");
	modules[3].dependencies.push_back("presentation");
	agent_project_module_t delivery = module("delivery", "交付与可观测性",
		"operations", join_relative(project_path, "ops"));
	delivery.dependencies.push_back("adapters");
	delivery.dependencies.push_back("presentation");
	delivery.dependencies.push_back("tests");
	modules.insert(modules.begin() + 3, ui);
	modules.push_back(delivery);

	agent_project_task_t architecture = task("architecture-review",
		"评审跨模块契约、数据流和失败边界", "core");
	architecture.dependencies.push_back("define-boundaries");
	architecture.acceptance_criteria.push_back("公开契约、数据所有权和错误模型可追踪");
	architecture.acceptance_criteria.push_back("依赖图不存在反向或循环依赖");
	architecture.test_plan.push_back("执行项目索引并核对模块依赖图");
	tasks[1].dependencies.clear();
	tasks[1].dependencies.push_back("architecture-review");

	agent_project_task_t presentation = task("implement-presentation",
		"实现可用、可访问且可调试的交互层", "presentation");
	presentation.dependencies.push_back("implement-application");
	presentation.acceptance_criteria.push_back("主要流程具备加载、空态和错误反馈");
	presentation.acceptance_criteria.push_back("交互状态可预测且不绕过应用层");
	presentation.test_plan.push_back("覆盖关键交互和边界状态");

	agent_project_task_t unit_tests = task("unit-test-gate",
		"补齐核心与应用层的确定性单元测试", "tests");
	unit_tests.dependencies.push_back("implement-application");
	unit_tests.acceptance_criteria.push_back("成功、边界和主要失败路径均有断言");
	unit_tests.test_plan.push_back("重复运行单元测试并确认结果稳定");

	agent_project_task_t integration_tests = task("integration-test-gate",
		"验证适配层、交互层和核心流程集成", "tests");
	integration_tests.dependencies.push_back("implement-adapters");
	integration_tests.dependencies.push_back("implement-presentation");
	integration_tests.dependencies.push_back("unit-test-gate");
	integration_tests.acceptance_criteria.push_back("关键端到端流程具备可重复验证入口");
	integration_tests.test_plan.push_back("在目标平台运行集成测试和失败注入");

	agent_project_task_t observability = task("observability-gate",
		"完善诊断日志、错误定位和运行健康信息", "delivery");
	observability.dependencies.push_back("integration-test-gate");
	observability.acceptance_criteria.push_back("错误包含操作、位置和可执行修复线索");
	observability.test_plan.push_back("触发受控失败并核对日志与界面诊断");

	agent_project_task_t release = task("release-readiness",
		"完成跨平台交付检查和回滚说明", "delivery");
	release.dependencies.push_back("quality-gate");
	release.dependencies.push_back("observability-gate");
	release.acceptance_criteria.push_back("构建、测试、调试和回滚入口均有文档");
	release.test_plan.push_back("在每个声明支持的平台执行发布验收脚本");

	// Insert architecture before implementation while keeping all dependency
	// references stable. The final quality gate waits for integration coverage.
	tasks.insert(tasks.begin() + 1, architecture);
	for (size_t i = 0; i < tasks.size(); ++i) {
		if (tasks[i].id == "quality-gate") {
			tasks[i].module_id = "delivery";
			tasks[i].dependencies.clear();
			tasks[i].dependencies.push_back("integration-test-gate");
		}
	}
	tasks.push_back(presentation);
	tasks.push_back(unit_tests);
	tasks.push_back(integration_tests);
	tasks.push_back(observability);
	tasks.push_back(release);
	return true;
}

} // namespace ai
} // namespace webcool
