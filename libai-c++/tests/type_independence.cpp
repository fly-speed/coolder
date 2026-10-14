#include <libai_cpp/agent.h>
#include <libai/sdk/agent_types.h>
#include <type_traits>
#include <cassert>

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
static_assert(!std::is_same<libai::cpp::ExecutionStore, libai::execution_store_t>::value,
    "Execution store must be independent");
static_assert(!std::is_same<libai::cpp::ExecutionSnapshot, libai::execution_snapshot_t>::value,
    "Execution snapshot must be independent");
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
    libai::cpp::TaskContract contract;
    contract.loop_finished=true;contract.loop_outcome="completed";contract.artifact_revision="r1";
    contract.generation={"completed","r1","host","draft"};contract.build={"passed","r1","host","compiler"};
    contract.tests={"passed","r1","host","tests"};contract.review={"accepted","r1","human","review"};
    assert(contract.accepted());
    assert(!contract.proves(contract.build,nullptr,"host"));
    for(const auto &bad:std::vector<std::string>{std::string("nul\0",4),std::string("\xc0\xaf",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4)}) {
        contract.review.reference=bad;assert(!contract.accepted());
        contract.require_review=false;assert(!contract.accepted());contract.require_review=true;
    }
    contract.review.reference="Unicode \xf0\x9f\x8c\x8f";assert(contract.accepted());
    contract.review.authority="invalid";contract.require_review=false;assert(!contract.accepted());
    return 0;
}
