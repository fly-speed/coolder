#pragma once
#include "types.h"
#include <cstdint>
namespace libai { namespace cpp {
struct McpOptions {
    std::shared_ptr<Cancellation> cancellation;
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max();
    std::chrono::milliseconds timeout{30000};
    std::string idempotency_key;
};
struct McpRequestContext {
    ToolContext control;
    std::uint64_t request_id=0;
    std::string protocol_version;
};
// Synchronous host transport. close must be idempotent and noexcept.
class LIBAI_CPP_API McpTransport {
public:
    virtual ~McpTransport() {}
    virtual std::string request(const std::string &wire,const McpRequestContext &context)=0;
    virtual void notify(const std::string &wire,const McpRequestContext &context)=0;
    virtual void close() noexcept=0;
};
struct McpTool {
    std::string name,description,input_schema,output_schema;
    bool requires_tasks=false;
};
struct McpResult { std::string json;bool error=false; };
// Host-reviewed local Schema/identity; remote hints never grant authority.
struct McpBinding {
    std::string parameters_schema,output_schema,execution_revision;
    bool requires_approval=true,side_effect_free=false;
    std::chrono::milliseconds timeout{30000};
};
struct McpStdioOptions {
    std::string executable;
    std::vector<std::string> arguments;
    bool inherit_environment=true;
    std::vector<std::string> environment;
};
// Owns a tools client; no implicit reconnect or business-tool retry.
class LIBAI_CPP_API McpClient {
public:
    explicit McpClient(std::shared_ptr<McpTransport> transport);
    ~McpClient();
    McpClient(const McpClient&)=delete;
    McpClient &operator=(const McpClient&)=delete;
    static bool stdio_available() noexcept;
    static std::shared_ptr<McpClient> stdio(const McpStdioOptions &options);
    std::string initialize(const std::string &name,const std::string &version,
        const McpOptions &options=McpOptions());
    std::vector<McpTool> list_tools(const McpOptions &options=McpOptions());
    McpTool tool(const std::string &name) const;
    // Explicit host call. Agent binding adds Schema/authorization/budget controls.
    McpResult call_tool(const std::string &name,const std::string &arguments,
        const McpOptions &options=McpOptions());
    void close();
private:
    friend class Agent;
    struct Impl;
    McpClient(std::unique_ptr<Impl> impl,int);
    std::unique_ptr<Impl> impl_;
};
}}
