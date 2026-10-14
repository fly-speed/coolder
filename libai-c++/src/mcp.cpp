#include "mcp_bridge.h"
#include <libai/mcp_stdio.h>
namespace libai { namespace cpp {
McpClient::McpClient(std::shared_ptr<McpTransport> transport)
    :impl_(new Impl(std::make_shared<Impl::Adapter>(std::move(transport)))){}
McpClient::McpClient(std::unique_ptr<Impl> impl,int):impl_(std::move(impl)){}
McpClient::~McpClient()=default;
bool McpClient::stdio_available() noexcept {
#if (defined(__unix__) || defined(__APPLE__)) && (!defined(__APPLE__) || !TARGET_OS_IPHONE) && (!defined(__ANDROID__) || __ANDROID_API__ >= 28)
    return true;
#else
    return false;
#endif
}
std::shared_ptr<McpClient> McpClient::stdio(const McpStdioOptions &options) {
#if (defined(__unix__) || defined(__APPLE__)) && (!defined(__APPLE__) || !TARGET_OS_IPHONE) && (!defined(__ANDROID__) || __ANDROID_API__ >= 28)
    ::libai::mcp_stdio_options_t native;native.executable=options.executable;
    native.arguments=options.arguments;native.inherit_environment=options.inherit_environment;
    native.environment=options.environment;
    return std::shared_ptr<McpClient>(new McpClient(std::unique_ptr<Impl>(new Impl(
        std::make_shared<::libai::mcp_stdio_transport_t>(native))),0));
#else
    (void)options;throw std::runtime_error("MCP stdio is unavailable on this platform");
#endif
}
std::string McpClient::initialize(const std::string &name,const std::string &version,const McpOptions &options) {return impl_->native->initialize(name,version,Impl::options(options));}
std::vector<McpTool> McpClient::list_tools(const McpOptions &options) {
    std::vector<McpTool> result;for(const auto &tool:impl_->native->list_tools(Impl::options(options)))result.push_back(Impl::tool(tool));return result;
}
McpTool McpClient::tool(const std::string &name) const {return Impl::tool(impl_->native->tool(name));}
McpResult McpClient::call_tool(const std::string &name,const std::string &arguments,const McpOptions &options) {
    const auto native=impl_->native->call_tool(name,arguments,Impl::options(options));
    McpResult result;result.json=native.json;result.error=native.error;return result;
}
void McpClient::close(){impl_->native->close();}
}}
