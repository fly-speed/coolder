#include <libai_cpp/agent.h>
#include <libai/sdk/agent_types.h>
#include <type_traits>

static_assert(!std::is_same<libai::cpp::Message, libai::message_t>::value,
    "Message must be independent");
static_assert(!std::is_same<libai::cpp::Model, libai::model_t>::value,
    "Model must be independent");
static_assert(
    !std::is_same<libai::cpp::Cancellation, libai::cancellation_t>::value,
    "Cancellation must be independent");
static_assert(
    !std::is_same<libai::cpp::RunOptions, libai::run_options_t>::value,
    "Options must be independent");
#ifdef LIBAI_CPP_FULL
#include <libai_cpp/coding.h>
#include <coding_host.h>
#include <libai/provider/ai_provider_client.h>
static_assert(!std::is_same<libai::cpp::ProviderConfig,
                  webcool::ai::provider_config_t>::value,
    "Provider config must be independent");
#endif
int main()
{
	return 0;
}
