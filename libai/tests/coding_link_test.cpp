#include <libai/coding.h>
int main()
{
	const auto *agent =
	    libai::ai::agent_registry_t::instance().find("coding");
	if (!agent || agent->tools.empty())
		return 1;
	if (!libai::ai::coding::reviewable_agent_run_status("completed"))
		return 2;
	if (!libai::ai::coding::reviewable_agent_run_status("invalid"))
		return 0;
	return 3;
}
