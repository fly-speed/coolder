#include "coding_internal.h"
#include "../common/record_json.h"
#include "../common/legacy_branding.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include <fstream>
#include <vector>
#include <sys/stat.h>
#include <cstdio>
#include <fcntl.h>
#include <algorithm>
#include <memory>
#ifndef _WIN32
#include <unistd.h>
#include <sys/file.h>
#endif
#ifndef _WIN32
#include <dirent.h>
#else
#include "../common/platform_compat.h"
#endif
namespace libai
{
namespace ai
{
namespace coding_detail
{
namespace
{
std::vector<std::string> children(const std::string &path)
{
	std::vector<std::string> result;
#ifndef _WIN32
	DIR *dir = opendir(path.c_str());
	if (!dir)
		throw std::runtime_error(
		    "cannot enumerate migration directory");
	while (auto *entry = readdir(dir)) {
		const std::string name = entry->d_name;
		if (name != "." && name != "..")
			result.push_back(name);
	}
	closedir(dir);
#else
	std::wstring pattern;
	if (!libai_utf8_path_to_wide((path + "/*").c_str(), pattern))
		throw std::runtime_error("invalid migration directory path");
	WIN32_FIND_DATAW info;
	HANDLE search = FindFirstFileW(pattern.c_str(), &info);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			std::string name;
			if (libai_wide_to_utf8(info.cFileName, name) &&
			    name != "." && name != "..")
				result.push_back(name);
		} while (FindNextFileW(search, &info));
		FindClose(search);
	}
#endif
	return result;
}
bool regular(const std::string &path)
{
#ifndef _WIN32
	struct stat st;
	return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
	std::wstring wide;
	if (!libai_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const auto attrs = GetFileAttributesW(wide.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES &&
	    !(attrs &
	        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#endif
}
void private_write(const std::string &path, const std::string &bytes)
{
#ifndef _WIN32
	const int fd =
	    open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	FILE *file = fd < 0 ? nullptr : fdopen(fd, "wb");
	if (!file && fd >= 0)
		close(fd);
#else
	std::wstring wide;
	if (!libai_utf8_path_to_wide(path.c_str(), wide))
		throw std::runtime_error("invalid migration path");
	FILE *file = _wfopen(wide.c_str(), L"wbx");
#endif
	if (!file)
		throw std::runtime_error(
		    "cannot create private migration file");
	bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
	    fflush(file) == 0;
#ifndef _WIN32
	if (ok)
		ok = fsync(fileno(file)) == 0;
#else
	if (ok)
		ok = _commit(_fileno(file)) == 0;
#endif
	if (fclose(file) != 0)
		ok = false;
	if (!ok) {
		remove(path.c_str());
		throw std::runtime_error("cannot flush private migration file");
	}
}
std::string read_bytes(const std::string &path)
{
	std::ifstream source(path.c_str(), std::ios::binary);
	if (!source.good())
		throw std::runtime_error("cannot read migration source");
	std::string bytes((std::istreambuf_iterator<char>(source)), {});
	if (source.bad())
		throw std::runtime_error(
		    "cannot read complete migration source");
	return bytes;
}
bool provider_scope(const std::string &root) {
    return regular(root+"/.libai_agent/providers.v1") ||
        regular(root+"/"+legacy_branding::old_component(".libai_agent")+"/providers.v1");
}
struct migration_leases {
#ifndef _WIN32
    std::vector<int> files;
    ~migration_leases(){for(int fd:files){flock(fd,LOCK_UN);close(fd);}}
    void acquire(const std::string &path) {
        int fd=open(path.c_str(),O_RDWR|O_NOFOLLOW);
        if(fd<0)throw std::runtime_error("cannot open migration execution lease");
        if(flock(fd,LOCK_EX|LOCK_NB)!=0){close(fd);throw std::runtime_error("active execution prevents storage naming migration");}
        files.push_back(fd);
    }
#else
    void acquire(const std::string &) {throw std::runtime_error("execution lease migration is not supported on this platform");}
#endif
};
void plan_names(const std::string &root,size_t depth,bool metadata,
    std::vector<std::pair<std::string,std::string>> &plan,migration_leases &leases) {
    if(depth>32)throw std::runtime_error("storage naming migration depth exceeds limit");
    for(const auto &name:children(root)) {
        if(name==".git"||name=="node_modules"||name=="build"||name=="dist"||name=="target"||name==".libai-build"||name==legacy_branding::old_component(".libai-build"))continue;
        const auto path=root+"/"+name;auto next=legacy_branding::new_component(name);
        if((next=="libai_auth"||next=="libai_users") && depth!=0 && !metadata && !provider_scope(root))next=name;
        const bool own=next==".libai_agent"||next==".libai_settings"||next=="libai_auth"||next=="libai_users";
        if(file_ops::safe_directory(path))plan_names(path,depth+1,metadata||own,plan,leases);
        else if(metadata && name.size()>5 && name.substr(name.size()-5)==".lock") {
            if(!regular(path))throw std::runtime_error("unsafe migration lease");
            leases.acquire(path);
        }
        if(next!=name) {
            if(!file_ops::safe_directory(path)&&!regular(path))throw std::runtime_error("unsafe legacy storage path");
            if(file_ops::path_entry_exists(root+"/"+next))throw std::runtime_error("both old and new storage paths exist; reconcile before migration");
            plan.emplace_back(path,root+"/"+next);
            if(plan.size()>100000)throw std::runtime_error("storage naming migration limit exceeded");
        }
    }
}
void apply_names(const std::vector<std::pair<std::string,std::string>> &plan) {
    for(const auto &item:plan) {
        if(!file_ops::replace_file(item.first,item.second))throw std::runtime_error("cannot rename legacy storage path");
#ifndef _WIN32
        const auto parent=item.second.substr(0,item.second.find_last_of('/'));
        int fd=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(fd<0)throw std::runtime_error("cannot open renamed storage parent");
        const int status=fsync(fd);close(fd);if(status!=0)throw std::runtime_error("cannot sync renamed storage parent");
#endif
    }
}
void collect(const std::string &root, const std::string &base, size_t depth,
    std::vector<std::pair<std::string, std::string>> &plan)
{
	if (depth > 32)
		throw std::runtime_error(
		    "migration directory depth exceeds limit");
	for (const auto &name : children(root)) {
		const auto path = root + "/" + name;
		if (name == ".git" || name == "node_modules" ||
		    name == "build" || name == "dist" || name == "target" || name==".libai-build" || name==legacy_branding::old_component(".libai-build"))
			continue;
		if (file_ops::safe_directory(path)) {
			collect(path, base, depth + 1, plan);
			continue;
		}
		if (!regular(path) ||
		    name.find(".legacy") != std::string::npos ||
		    name.find(".tmp") != std::string::npos)
			continue;
		const auto user_prefix = base + "/users/";
		const bool preference = path == base + "/preferences" ||
		    (path.compare(0, user_prefix.size(), user_prefix) == 0 &&
		        path.size() == user_prefix.size() + 64 + 12 &&
		        path.substr(user_prefix.size() + 64) ==
		            "/preferences" &&
		        path.substr(user_prefix.size(), 64)
		                .find_first_not_of("0123456789abcdef") ==
		            std::string::npos);
		const auto canonical=legacy_branding::canonical_path(path);
		if (!preference &&
		    canonical.find("/.libai_agent/") == std::string::npos &&
		    canonical.find("/.libai_settings/") == std::string::npos &&
		    canonical.find("/libai_auth/") == std::string::npos &&
            path.find("/"+legacy_branding::old_component("libai_auth")+"/")==std::string::npos)
			continue;
		if(name=="ai_master.key") {
            const auto parent=root.substr(0,root.find_last_of('/'));
            if(parent!=base && !provider_scope(parent))continue;
        }
		const bool record = name.size() > 3 &&
		    (name.substr(name.size() - 3) == ".v1" ||
		        name.substr(name.size() - 3) == ".v2");
		if (!record && name != "ai_master.key" && !preference)
			continue;
		std::ifstream in(path.c_str(), std::ios::binary);
		if (!in.good())
			throw std::runtime_error(
			    "cannot read migration record");
		std::string source=legacy_branding::canonical_record(std::string((std::istreambuf_iterator<char>(in)), {}));
		const auto at = source.find_first_not_of(" \t\r\n");
		if (at != std::string::npos && source[at] == '{')
			continue;
		if (name == "accounts.v1" || name.compare(0, 3, "ui-") == 0 ||
		    name == "project-members.v1")
			continue;
		const bool known = name == "preferences" ||
		    name == "ai-policy.v1" || name == "ai_master.key" ||
		    name == "external-project-roots.v1" ||
		    source.compare(0, 6, "LIBAI_") == 0;
		if (!known)
			continue;
		const auto converted = record_json::encode(source, path);
		const auto normalized =
		    source.empty() || source.back() == '\n' ? source :
		                                              source + "\n";
		if (record_json::decode(converted) != normalized &&
		    (name != "ai-policy.v1" ||
		        record_json::encode(
		            record_json::decode(converted), path) != converted))
			throw std::runtime_error(
			    "migration record does not round-trip: " + name);
		plan.emplace_back(path, converted);
		if (plan.size() > 100000)
			throw std::runtime_error(
			    "migration record limit exceeded");
	}
}
}
bool migrate(const std::string &root, acl::json_node &, acl::json_node &out,
    std::string &error)
{
	try {
		migration_leases leases;std::vector<std::pair<std::string,std::string>> names;
        plan_names(root,0,false,names,leases);
		std::vector<std::pair<std::string, std::string>> plan;
		collect(root, root, 0, plan);
		// Validate all backup destinations before replacing any source file.
		for (const auto &item : plan) {
			const auto backup = item.first + ".legacy";
			if (file_ops::path_entry_exists(backup) &&
			    !regular(backup))
				throw std::runtime_error(
				    "invalid migration backup path");
			if (file_ops::path_entry_exists(backup) &&
			    read_bytes(backup) != read_bytes(item.first))
				throw std::runtime_error(
				    "migration backup differs from legacy source");
		}
		apply_names(names);
        for(auto &item:plan)for(const auto &move:names)if(item.first==move.first || item.first.compare(0,move.first.size()+1,move.first+"/")==0)item.first=move.second+item.first.substr(move.first.size());
		for (const auto &item : plan) {
			const auto &path = item.first;
			const auto backup = path + ".legacy";
			if (!file_ops::path_entry_exists(backup))
				private_write(backup, read_bytes(path));
			const auto temp =
			    path + ".migrate-" + identifiers::new_id();
			private_write(temp, item.second + "\n");
			if (!file_ops::replace_file(temp, path)) {
				remove(temp.c_str());
				throw std::runtime_error(
				    "cannot install JSON migration record");
			}
		}
		out.add_number("renamed_paths",static_cast<long long>(names.size()));
		out.add_number(
		    "migrated_files", static_cast<long long>(plan.size()));
		return true;
	} catch (const std::exception &e) {
		error = e.what();
		return false;
	}
}
}
}
}
