#include <libai_cpp/agent.h>
#include <cassert>
using namespace libai::cpp;
int main() {
    int calls=0,effects=0;std::string visible;
    const std::string schema=R"({"type":"object","required":["n"],"properties":{"n":{"type":"integer"}}})";
    Agent a([&](const ModelRequest &q,const StreamSink &sink){
        assert(q.instructions.find(schema)!=std::string::npos);
        ModelResponse r;
        if(++calls==1){ToolCall c;c.id="call";c.name="effect";r.calls.push_back(c);}
        else if(calls==2)r.text="{}";
        else {assert(q.tools.empty());r.text=R"({"n":2})";}
        if(sink)assert(sink("unvalidated delta"));
        return r;
    });
    a.configure_output(schema,1);
    bool rejected=false;try{a.configure_output(R"({"const":1})");}catch(const std::exception&){rejected=true;}assert(rejected);
    ToolSpec t;t.name="effect";a.add_tool(t,[&](const ToolCall &,const ToolContext &){++effects;return ToolResult::success("done");});
    RunOptions options;options.on_text=[&](const std::string &s){visible+=s;return true;};
    auto result=a.run("work",options);
    assert(a.output_validation().output_validated && a.output_validation().output_repairs==1);
    assert(result.status==RunStatus::completed && result.text==R"({"n":2})" && visible==result.text && calls==3 && effects==1);
}
