#include <libai/agent.h>
#include <libai/task_contract.h>
#include <libai/sdk/governance.h>
#include <cassert>
int main()
{
    const auto eval=[](const std::string &wire){
        libai::governance_detail::json::Document d;
        libai::governance_detail::parse_json(libai::governance_call("tasks.evaluate",wire),d);
        return std::make_pair(d["ok"].GetBool(),d["accepted"].GetBool());
    };
    assert((eval("{}") == std::make_pair(true,false)));
    assert((eval(R"({"loop_finished":true,"loop_outcome":"recovery_required"})") == std::make_pair(true,false)));
    const std::string analysis=R"({"loop_finished":true,"loop_outcome":"completed","requirements":{"generation":false,"build":false,"tests":false,"review":false}})";
    assert((eval(analysis)==std::make_pair(true,true)));
    for(const std::string wire:{R"({"version":1.0})",R"({"loop_finished":"true"})",R"({"version":1,"version":1})",
        R"({"build":null})",R"({"requirements":{"tests":1}})",R"({"artifact_revision":"\u0000"})"})
        assert((eval(wire)==std::make_pair(false,false)));
    assert((eval(R"({"accepted":true,"checks":{"loop":true}})")==std::make_pair(true,false)));
    libai::task_contract_t c;
    assert(!c.accepted());
    c.loop_finished = true;
    c.loop_outcome = "completed";
    c.artifact_revision = "revision-1";
    c.generation = {"completed", "revision-1", "host", "draft-index"};
    c.build = {"passed", "revision-1", "host", "compiler-report"};
    c.tests = {"passed", "revision-1", "host", "test-report"};
    c.review = {"accepted", "revision-1", "human", "review-decision"};
    assert(c.accepted());
    assert(!c.proves(c.build,NULL,"host"));
    assert(!c.proves(c.build,"passed",NULL));
    const auto clean=c;
    for(const auto &bad:std::vector<std::string>{std::string("bad\0reference",13),std::string("\xc0\xaf",2),
            std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xf0\x9f",2)}) {
        c=clean;c.review.reference=bad;assert(!c.accepted());
        c.require_review=false;assert(!c.accepted());
        c=clean;c.artifact_revision=bad;c.generation.revision=bad;c.build.revision=bad;c.tests.revision=bad;c.review.revision=bad;assert(!c.accepted());
        c.require_generation=c.require_build=c.require_tests=c.require_review=false;assert(!c.accepted());
    }
    c=clean;c.review.state="passed";c.require_review=false;assert(!c.accepted());
    c=clean;c.build.authority="untrusted";c.require_build=false;assert(!c.accepted());
    c=clean;c.review.reference="Unicode reference \xf0\x9f\x8c\x8f";assert(c.accepted());
    c=clean;
    c.tests.revision = "revision-0";
    assert(!c.accepted());
    c.tests.revision = "revision-1";
    c.tests.authority = "model";
    assert(!c.accepted());
    c.tests.authority = "host";
    c.review.reference.clear();
    assert(!c.accepted());
    c.require_review = false;
    assert(c.accepted());
    c.loop_outcome = "model_error";
    assert(!c.accepted());
    auto model = std::make_shared<libai::function_model_t>(
        [](const libai::model_request_t &, const libai::stream_sink_t &) {
            libai::model_response_t r; r.text = "All tests passed!";
            return r;
        });
    libai::agent_t agent(model);
    libai::agent_session_t session;
    auto result = agent.run(session, "Implement a program");
    assert(result.task_contract.loop_finished);
    assert(result.task_contract.loop_outcome == "completed");
    assert(!result.task_contract.accepted());
}
