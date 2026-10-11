#include <libai_cpp/coding_service.h>
#include <coding_host.h>
#include <cassert>
#include <string>

int main(int argc, char **argv)
{
	assert(argc == 2 && libai::cpp::CodingService::available());
	libai::cpp::CodingService service(argv[1]);
	auto result = service.call("capabilities");
	acl::json json(result.c_str());
	assert(json.finish() &&
	    libai::cpp::coding::json_values::boolean(json["ok"], false));
	assert(json["operations"] &&
	    json["operations"]->first_child()->is_array());
	// Independent desktop declarations are linked through the compiled C++ facade.
	assert(
	    !libai::cpp::coding::agent_registry_t::instance().list().empty());
	libai::cpp::coding::agent_workspace_t workspace(argv[1], false);
	std::string error;
	assert(workspace.create_text_if_absent(
	    "cpp-facade.txt", "C++ facade", error));
	result =
	    service.call("workspace.read", "{\"path\":\"cpp-facade.txt\"}");
	acl::json read(result.c_str());
	assert(read.finish());
	assert(libai::cpp::coding::json_values::text(read["content"]) ==
	    "C++ facade");
	assert(workspace.delete_text_if_unchanged(
	    "cpp-facade.txt", workspace.content_sha256("C++ facade"), error));
}
