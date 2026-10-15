#include "../validation/proposal_response_recovery.h"
#include <cassert>
#include <iostream>
int main() {
 using namespace libai::ai;
 const std::string raw=R"({"type":"tool_call","name":"workspace.propose_batch","arguments":{},"arguments":{"content":"[{\"path\":\"src/x.go\",\"content\":\"package x\\n\"}]"}})";
 assert(proposal_json_error(raw).find("duplicate JSON object key at byte")!=std::string::npos);
 std::string result,error;
 assert(recover_proposal_response(raw,R"({"raw_edits":[{"old_text":"\"arguments\":{},","new_text":""}],"decode_tool":true})",result,error));
 assert(result=="[{\"path\":\"src/x.go\",\"content\":\"package x\\n\"}]");
 assert(!recover_proposal_response(raw,R"({"decode_tool":true})",result,error));
 assert(!recover_proposal_response(raw,R"({"raw_edits":[{"old_text":"arguments","new_text":"x"}],"decode_tool":true})",result,error));
 assert(!recover_proposal_response(R"({"type":"tool_call","name":"workspace.propose_delete","arguments":{"path":"x"}})",R"({"decode_tool":true})",result,error));
 assert(!recover_proposal_response("[]",R"({"raw_edits":[{"old_text":"[]","new_text":"[{\"path\":\"a\",\"path\":\"b\"}]"}]})",result,error));
 assert(proposal_json_error("{\"x\":}").find("at byte")!=std::string::npos);
 std::cout<<"PASS explicit repairs preserve strict JSON and tool identity\n";
}
