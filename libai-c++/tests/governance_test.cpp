#include <libai_cpp/agent.h>
#include <cassert>
int main(){
    assert(libai::cpp::governance_call("tasks.evaluate",R"({"loop_finished":true,"loop_outcome":"completed"})").find("\"accepted\":false")!=std::string::npos);
    assert(libai::cpp::governance_call("tasks.evaluate",R"({"loop_finished":true,"loop_outcome":"completed","requirements":{"generation":false,"build":false,"tests":false,"review":false}})").find("\"accepted\":true")!=std::string::npos);
    libai::cpp::Agent agent([](const libai::cpp::ModelRequest &q,const libai::cpp::StreamSink &){
        assert(q.tools.empty() && q.max_output_tokens==7);
        assert(q.model_profile_json.find("tool_calling")!=std::string::npos);
        libai::cpp::ModelResponse r;r.text="done";return r;
    });
    agent.configure_model(R"({"version":1,"tool_calling":false,"max_output_tokens":7})");
    bool rejected=false;try{agent.configure_model(R"({"version":1,"max_output_tokens":0})");}catch(const std::exception&){rejected=true;}
    assert(rejected && agent.run("hello").status==libai::cpp::RunStatus::completed);
    auto result=libai::cpp::validate_json_schema(R"({"type":"integer"})",R"("bad")");
    assert(!result.valid && result.keyword=="type");
    assert(libai::cpp::validate_json_schema(R"({"type":"integer"})","2").valid);
    assert(libai::cpp::governance_call("capabilities").find("draft4")!=std::string::npos);
}
