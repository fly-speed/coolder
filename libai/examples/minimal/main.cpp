#include <libai/coding.h>
#include <iostream>

// No webcool application headers or implementation files are needed.
int main()
{
	const auto *coding =
		webcool::ai::agent_registry_t::instance().find("coding");
	if (!coding)
		return 1;
	std::cout << "Agent: " << coding->id
		  << "\nTools: " << coding->tools.size() << '\n';
	return coding->tools.empty() ? 2 : 0;
}
