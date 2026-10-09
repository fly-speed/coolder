#include "stdafx.h"
#include "server/host.h"
#include "server/auth.h"
#include "libai/agent/ai_admin_policy.h"
#include "libai/runtime/coding_runtime.h"
#include <filesystem>
#include <iostream>
#include <csignal>
#ifndef _WIN32
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{

volatile std::sig_atomic_t stopping = 0;

void stop(int)
{
	stopping = 1;
}

class session final : public acl::session {
public:
	session()
		: acl::session(0)
	{
	}
	bool remove() override
	{
		return true;
	}

	bool get_attrs(std::map<acl::string, acl::session_string> &) override
	{
		return true;
	}

	bool
	set_attrs(const std::map<acl::string, acl::session_string> &) override
	{
		return true;
	}

	bool set_timeout(time_t) override
	{
		return true;
	}
};

class servlet final : public acl::HttpServlet {
public:
	servlet(acl::socket_stream *conn, acl::session *s)
		: acl::HttpServlet(conn, s)
	{
		setLocalCharset("utf8");
	}

	bool doGet(request_t &req, response_t &res) override
	{
		return coolder::dispatch(req, res, "GET");
	}

	bool doPost(request_t &req, response_t &res) override
	{
		return coolder::dispatch(req, res, "POST");
	}
};

}

int main(int argc, char **argv)
{
	try {
		unsigned port = 18095;
		coolder::data_dir = "var";
		coolder::html_dir = COOLDER_HTML_DIR;
		for (int i = 1; i < argc; ++i) {
			std::string arg = argv[i];
			if (arg == "--help") {
				std::cout
					<< "coolder [--port 18095] [--data DIR] [--workspace DIR] "
					   "[--html DIR]\nMulti-user browser AI coding workspace.\n";
				return 0;
			}
			if (i + 1 >= argc) {
				throw std::runtime_error(
					"option requires a value: " + arg);
			}
			std::string value = argv[++i];
			if (arg == "--port") {
				size_t n = 0;
				port = std::stoul(value, &n);
				if (n != value.size() || port == 0 ||
				    port > 65535) {
					throw std::runtime_error(
						"invalid port");
				}
			} else if (arg == "--data") {
				coolder::data_dir = value;
			} else if (arg == "--workspace") {
				coolder::workspace_dir = value;
			} else if (arg == "--html") {
				coolder::html_dir = value;
			} else {
				throw std::runtime_error("unknown option: " +
							 arg);
			}
		}

		namespace fs = std::filesystem;
		fs::create_directories(coolder::data_dir);
		coolder::data_dir = fs::canonical(coolder::data_dir).string();
		if (coolder::workspace_dir.empty()) {
			coolder::workspace_dir =
				coolder::data_dir + "/workspace";
		}
		fs::create_directories(coolder::workspace_dir);
		coolder::workspace_dir =
			fs::canonical(coolder::workspace_dir).string();
		if (coolder::workspace_dir == coolder::data_dir ||
		    coolder::data_dir.compare(
			    0, coolder::workspace_dir.size() + 1,
			    coolder::workspace_dir + "/") == 0) {
			throw std::runtime_error(
				"data directory must not be inside the workspace");
		}
		if (!fs::exists(coolder::html_dir + "/index.html")) {
			throw std::runtime_error(
				"UI files missing; set --html DIR");
		}
#ifndef _WIN32
		chmod(coolder::data_dir.c_str(), 0700);
		const int lock =
			open((coolder::data_dir + "/server.lock").c_str(),
			     O_CREAT | O_RDWR, 0600);
		if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
			throw std::runtime_error(
				"data directory already in use");
		}
#endif

#ifndef _WIN32
		std::signal(SIGPIPE, SIG_IGN);
#endif

		std::signal(SIGINT, stop);
		std::signal(SIGTERM, stop);
		acl::acl_cpp_init();
		acl::log::stdout_open(true);

		// ACL dynamically resolves TLS; optional overrides support nonstandard
		// installations.
		const char *crypto = std::getenv("COOLDER_LIBCRYPTO");
		const char *ssl = std::getenv("COOLDER_LIBSSL");
		if (crypto && ssl) {
			acl::openssl_conf::set_libpath(crypto, ssl);
		}
		if (!acl::openssl_conf::load()) {
			std::cerr
				<< "TLS runtime unavailable; set COOLDER_LIBCRYPTO and "
				   "COOLDER_LIBSSL for HTTPS providers.\n";
		}

		webcool::ai::ai_admin_policy_t policy;
		policy.language_tools = "python,javascript,go,java,rust,swift";
		policy.allow_browser_debug = false;
		webcool::ai::ai_admin_policy_store_t policy_store(
			coolder::data_dir);
		std::string policy_error;
		if (fs::exists(coolder::data_dir +
			       "/.webcool_settings/ai-policy.v1")) {
			if (!policy_store.load(policy, policy_error)) {
				throw std::runtime_error(policy_error);
			}
		} else if (!policy_store.save(policy, policy_error)) {
			throw std::runtime_error(policy_error);
		}
		policy.allow_browser_debug = false;
		policy.allow_users_shared_projects = false;
		policy.allow_users_local_projects = false;
		webcool::ai::ai_runtime_policy_set(policy);
		coolder::accounts_init();

		coolder::authority = "127.0.0.1:" + std::to_string(port);
		acl::server_socket server;
		if (!server.open(coolder::authority.c_str())) {
			throw std::runtime_error("cannot listen: " +
						 coolder::authority);
		}
		std::cout
			<< "coolder: http://" << coolder::authority
			<< "\nOpen the browser to initialize or sign in to an account."
			<< "\nWorkspace: " << coolder::workspace_dir
			<< std::endl;

		acl::gofiber([&] {
			while (!stopping) {
				bool timed = false;
				acl::socket_stream *conn =
					server.accept(1, &timed);
				if (!conn) {
					if (timed) {
						continue;
					}
					break;
				}
				acl::gofiber_stack(
					[conn] {
						std::unique_ptr<
							acl::socket_stream>
							owned(conn);
						conn->set_rw_timeout(30);
						session s;
						servlet handler(conn, &s);
						handler.setRwTimeout(30);
						handler.doRun();
					},
					1024 * 1024);
			}
			// Workers persist checkpoints at safe boundaries; cancel pending provider
			// I/O.
			{
				std::lock_guard<webcool::mutex> guard(
					action::agent_detail::
						g_agent_runtime_mutex);
				for (auto &item : action::agent_detail::
					     g_agent_runtime_tasks) {
					item.second->cancel_requested = true;
					if (item.second->worker) {
						item.second->worker->kill();
					}
				}
			}
			acl::fiber::schedule_stop();
		});

		acl::fiber::schedule_with(acl::FIBER_EVENT_T_KERNEL);
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "coolder: " << ex.what() << '\n';
		return 1;
	}
}
