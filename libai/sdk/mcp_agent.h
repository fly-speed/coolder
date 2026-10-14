#pragma once
#include "agent.h"
#include "mcp.h"
namespace libai {
struct mcp_binding_t {
    // Required host-reviewed local validation. Never reinterpret remote 2020-12 as Draft-04.
    std::string parameters_schema, output_schema, execution_revision;
    bool requires_approval=true;
    bool side_effect_free=false;
    std::chrono::milliseconds timeout{30000};
};
inline agent_t &add_mcp_tool(agent_t &agent,std::shared_ptr<mcp_client_t> client,
    const std::string &remote_name,const std::string &local_name,const mcp_binding_t &policy)
{
    if(!client || !mcp_detail::identifier(local_name) || policy.parameters_schema.empty() ||
        policy.execution_revision.empty() || policy.execution_revision.size()>108 ||
        policy.timeout.count()<=0 || policy.timeout>std::chrono::hours(24))
        throw std::invalid_argument("MCP binding needs host schema/revision, identity and bounded timeout");
    const auto remote=client->tool(remote_name);
    if(remote.requires_tasks)throw std::invalid_argument("MCP tool requires unsupported asynchronous tasks");
    if(!remote.output_schema.empty() && policy.output_schema.empty())
        throw std::invalid_argument("MCP outputSchema needs a host-reviewed local validator");
    std::shared_ptr<const json_schema_t> output;
    if(!policy.output_schema.empty())output=std::make_shared<json_schema_t>(policy.output_schema);
    tool_spec_t spec;spec.name=local_name;spec.description=remote.description;spec.parameters=policy.parameters_schema;
    spec.requires_approval=policy.requires_approval;spec.side_effect_free=policy.side_effect_free;
    spec.execution_revision="mcp/2025-11-25/"+policy.execution_revision;
    return agent.add_tool(spec,[client,remote,policy,output](const tool_call_t &call,const tool_context_t &control){
        try {
            mcp_options_t options;options.cancellation=control.cancellation;options.deadline=control.deadline;
            options.idempotency_key=control.idempotency_key;options.timeout=policy.timeout;
            const auto result=client->call_tool(remote.name,call.arguments,options,&remote);
            if(!result.error && output) {
                mcp_detail::json::Document value;governance_detail::parse_json(result.json,value);
                if(!value.HasMember("structuredContent") || !output->validate(mcp_detail::wire(value["structuredContent"])).valid)
                    return tool_result_t::failure("MCP structuredContent violates host output contract; effect may require reconciliation");
            }
            tool_result_t response;response.text=result.json;response.error=result.error;return response;
        } catch(const std::exception &error) {return tool_result_t::failure(error.what());}
    });
}
}
