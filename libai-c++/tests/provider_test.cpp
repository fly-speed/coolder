#include <libai_cpp/provider_model.h>
#include <libai_cpp/coding_service.h>
#include <coding_host.h>
#include <cassert>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif

// Exercise both independent typed and JSON provider adapters against a local fixture.
int main(int argc, char **argv)
{
	assert(argc == 3);
	using namespace libai::cpp;
	ProviderConfig config;
	config.id = "fixture";
	config.protocol = "openai_chat";
	config.base_url = argv[1];
	config.model = "fixture";
	auto model = std::make_shared<NativeProviderModel>(config, "temporary");
	Agent agent(model);
	RunOptions options;
	options.max_model_retries = 0;
	RunResult result;
#ifndef _WIN32
	pthread_key_t foreign_key;
	assert(pthread_key_create(&foreign_key, nullptr) == 0);
#endif
	std::thread worker([&] {
#ifndef _WIN32
		// Foreign runtimes may occupy pthread keys before their first native call.
		assert(pthread_setspecific(
		           foreign_key, "foreign provider thread") == 0);
#endif
		result = agent.run("你好😀", options);
	});
	worker.join();
#ifndef _WIN32
	pthread_key_delete(foreign_key);
#endif
	assert(
	    result.status == RunStatus::completed && result.text == "native😀");
	assert(result.input_tokens == 3 && result.output_tokens == 2);
	CodingService service(argv[2]);
	auto saved = service.call("providers.save",
	    "{\"name\":\"fixture\",\"protocol\":\"openai_chat\",\"base_url\":\"" +
	        config.base_url +
	        "\",\"model\":\"fixture\",\"api_key\":\"temporary\"}");
	acl::json provider(saved.c_str());
	assert(provider.finish() &&
	    coding::json_values::boolean(provider["ok"], false));
	auto id = coding::json_values::text(provider["id"]);
	auto output = service.call("providers.complete",
	    "{\"id\":\"" + id +
	        "\",\"messages\":[{\"role\":\"user\",\"text\":\"你好😀\"}]}");
	acl::json completed(output.c_str());
	assert(completed.finish() &&
	    coding::json_values::boolean(completed["ok"], false));
	assert(coding::json_values::text(completed["text"]) == "native😀");
}
