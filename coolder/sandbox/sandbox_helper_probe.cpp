#include "sandbox_helper_main_internal.h"
namespace sandbox_helper
{
#ifndef _WIN32
int terminate_service(pid_t child, int result)
{
	(void)kill(-child, SIGTERM);
	(void)kill(child, SIGTERM);
	for (int attempt = 0; attempt < 20; ++attempt) {
		int status = 0;
		if (waitpid(child, &status, WNOHANG) == child)
			return result;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	(void)kill(-child, SIGKILL);
	(void)kill(child, SIGKILL);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return result;
}

// The probe runs inside the Linux network namespace and beside the macOS
// sandbox process. Project code can therefore expose only its isolated
// loopback listener; no host/LAN network access is required.
bool allocate_probe_port(unsigned short &port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t size = sizeof(address);
	const bool ok = bind(fd, reinterpret_cast<struct sockaddr *>(&address),
	                    size) == 0 &&
	    getsockname(
	        fd, reinterpret_cast<struct sockaddr *>(&address), &size) == 0;
	if (ok)
		port = ntohs(address.sin_port);
	close(fd);
	return ok && port >= 1024;
}

#ifdef __APPLE__
bool probe_port_available(unsigned short port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const bool available =
	    bind(fd, reinterpret_cast<struct sockaddr *>(&address),
	        sizeof(address)) == 0;
	close(fd);
	if (available)
		return available;
	fputs(
	    "webcool-sandbox-helper: validation blocked: service port already occupied; refusing to test an existing server\n",
	    stderr);
	return available;
}

#endif

int run_browser_probe(const options_t &options)
{
#if defined(__APPLE__) || defined(__linux__)
	if (access(options.browser_node.c_str(), X_OK) != 0 ||
	    access(options.browser_runner.c_str(), R_OK) != 0) {
		fputs(
		    "webcool-sandbox-helper: browser runtime unavailable; check Node and deployed browser runner\n",
		    stderr);
		return 78;
	}
	const std::string url =
	    "http://127.0.0.1:" + std::to_string(options.probe_port) +
	    options.probe_path;
#ifdef __linux__
	// Chromium's own namespace sandbox cannot create nested namespaces after
	// WebCool applies seccomp. The browser still runs inside WebCool's private
	// user, mount, PID, IPC, UTS and network namespaces, Landlock rules and
	// delegated cgroup, so tell the trusted runner to disable only that nested
	// Chromium layer.
	if (setenv("WEBCOOL_BROWSER_OUTER_SANDBOX", "linux", 1) != 0)
		return 126;
	if (setenv("MOZ_DISABLE_CONTENT_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_GMP_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1", 1) != 0)
		return 126;
#endif
	const pid_t browser = fork();
	if (browser < 0)
		return 126;
	if (browser == 0) {
		execl(options.browser_node.c_str(),
		    options.browser_node.c_str(),
		    options.browser_runner.c_str(), url.c_str(),
		    options.project_root.c_str(),
		    (options.browser_evidence_root.empty() ?
		            options.project_root :
		            options.browser_evidence_root)
		        .c_str(),
		    static_cast<char *>(NULL));
		fputs(
		    "webcool-sandbox-helper: browser runtime unavailable; install browser dependencies and Node\n",
		    stderr);
		_exit(78);
	}
	int status = 0;
	while (waitpid(browser, &status, 0) < 0) {
		if (!(errno != EINTR))
			continue;
		return 126;
	}
	return child_exit_status(status);
#else
	(void)options;
	return 78;
#endif
}

// Each readiness attempt owns its socket and closes it on every outcome; a
// failed attempt must not exhaust descriptors during the bounded retry loop.
static bool http_service_ready(unsigned short port, const std::string &path)
{
	const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0)
		return false;
	struct timeval receive_timeout = { 0, 250000 };
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
	    sizeof(receive_timeout));
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(descriptor, reinterpret_cast<struct sockaddr *>(&address),
	        sizeof(address)) != 0) {
		close(descriptor);
		return false;
	}
	const std::string request = "GET " + path +
	    " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	(void)send(descriptor, request.data(), request.size(), 0);
	char response[64];
	const ssize_t received =
	    recv(descriptor, response, sizeof(response) - 1, 0);
	close(descriptor);
	if (received <= 12)
		return false;
	response[received] = '\0';
	const char *space = strchr(response, ' ');
	const int code = space == NULL ? 0 : atoi(space + 1);
	return strncmp(response, "HTTP/", 5) == 0 && code >= 200 && code < 400;
}

int probe_http_service(pid_t child, unsigned short port,
    const std::string &path, const options_t *browser_options)
{
	const std::chrono::steady_clock::time_point deadline =
	    std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		int status = 0;
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			return child_exit_status(status);
		if (http_service_ready(port, path))
			return terminate_service(child,
			    browser_options ?
			        run_browser_probe(*browser_options) :
			        0);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	fputs(
	    "webcool-sandbox-helper: HTTP service readiness probe timed out\n",
	    stderr);
	return terminate_service(child, 70);
}

#endif
}
