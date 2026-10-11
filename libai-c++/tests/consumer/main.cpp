#include <libai_cpp/agent.h>
#include <libai_cpp/coding_service.h>
#include <iostream>
#ifdef LIBAI_CPP_FULL
#include <libai_cpp/coding.h>
#include <libai_cpp/provider_model.h>
#endif

int main()
{
	using namespace libai::cpp;
	Agent agent([](const ModelRequest &request, const StreamSink &) {
		ModelResponse result;
		result.text = request.messages.back().text;
		return result;
	});
	auto result = agent.run("independent consumer");
	if (result.status != RunStatus::completed)
		return 1;
#ifdef LIBAI_CPP_FULL
	ProviderConfig config;
	config.protocol = "openai_chat";
	config.base_url = "http://127.0.0.1:1/v1";
	config.model = "example";
	NativeProviderModel model(config, "example-key");
	if (!CodingService::available())
		return 2;
#endif
	std::cout << result.text << '\n';
}
