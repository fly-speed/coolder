#pragma once
#include <libai_cpp/mcp.h>
#include "value_bridge.h"
#include <libai/mcp.h>
namespace libai { namespace cpp {
struct McpClient::Impl {
    std::shared_ptr<::libai::mcp_client_t> native;
    class Adapter : public ::libai::mcp_transport_t {
        std::shared_ptr<McpTransport> owner_;
        static McpRequestContext context(const ::libai::mcp_request_context_t &source) {
            McpRequestContext target;target.request_id=source.request_id;
            target.protocol_version=source.protocol_version;
            target.control.deadline=source.control.deadline;
            target.control.idempotency_key=source.control.idempotency_key;
            if(source.control.cancellation){
                target.control.cancellation=std::make_shared<Cancellation>();
                target.control.cancellation->impl_->native=source.control.cancellation;
            }
            return target;
        }
    public:
        explicit Adapter(std::shared_ptr<McpTransport> owner):owner_(std::move(owner)) {
            if(!owner_)throw std::invalid_argument("MCP transport is null");
        }
        std::string request(const std::string &wire,const ::libai::mcp_request_context_t &source) override {return owner_->request(wire,context(source));}
        void notify(const std::string &wire,const ::libai::mcp_request_context_t &source) override {owner_->notify(wire,context(source));}
        void close() noexcept override {owner_->close();}
    };
    explicit Impl(std::shared_ptr<::libai::mcp_transport_t> transport)
        :native(std::make_shared<::libai::mcp_client_t>(std::move(transport))){}
    static ::libai::mcp_options_t options(const McpOptions &source) {
        ::libai::mcp_options_t target;target.deadline=source.deadline;
        target.timeout=source.timeout;target.idempotency_key=source.idempotency_key;
        if(source.cancellation)target.cancellation=source.cancellation->impl_->native;
        return target;
    }
    static McpTool tool(const ::libai::mcp_tool_t &source) {
        McpTool target;target.name=source.name;target.description=source.description;
        target.input_schema=source.input_schema;target.output_schema=source.output_schema;
        target.requires_tasks=source.requires_tasks;return target;
    }
};
}}
