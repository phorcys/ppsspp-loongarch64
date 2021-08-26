// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <mutex>
#include <thread>

#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPServer.h"
#include "Common/Net/Sinks.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/File/FileDescriptor.h"
#include "Common/TimeUtil.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/Debugger/WebSocket.h"
#include "Core/WebServer.h"

enum class ServerStatus {
	STOPPED,
	STARTING,
	RUNNING,
	STOPPING,
	FINISHED,
};

static const char *REPORT_HOSTNAME = "report.ppsspp.org";
static const int REPORT_PORT = 80;

static std::thread serverThread;
static ServerStatus serverStatus;
static std::mutex serverStatusLock;
static int serverFlags;

static void UpdateStatus(ServerStatus s) {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	serverStatus = s;
}

static ServerStatus RetrieveStatus() {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	return serverStatus;
}

// This reports the local IP address to report.ppsspp.org, which can then
// relay that address to a mobile device searching for the server.
static bool RegisterServer(int port) {
	bool success = false;
	http::Client http;
	http::RequestProgress progress;
	Buffer theVoid = Buffer::Void();

	http.SetUserAgent(StringFromFormat("PPSSPP/%s", PPSSPP_GIT_VERSION));

	char resource4[1024] = {};
	if (http.Resolve(REPORT_HOSTNAME, REPORT_PORT, net::DNSType::IPV4)) {
		if (http.Connect()) {
			std::string ip = fd_util::GetLocalIP(http.sock());
			snprintf(resource4, sizeof(resource4) - 1, "/match/update?local=%s&port=%d", ip.c_str(), port);

			if (http.GET(http::RequestParams(resource4), &theVoid, &progress) > 0)
				success = true;
			theVoid.Skip(theVoid.size());
			http.Disconnect();
		}
	}

	if (http.Resolve(REPORT_HOSTNAME, REPORT_PORT, net::DNSType::IPV6)) {
		// If IPv4 was successful, don't give this as much time (it blocks and sometimes IPv6 is broken.)
		double timeout = success ? 2.0 : 20.0;

		// We register both IPv4 and IPv6 in case the other client is using a different one.
		if (resource4[0] != 0 && http.Connect(timeout)) {
			if (http.GET(http::RequestParams(resource4), &theVoid, &progress) > 0)
				success = true;
			theVoid.Skip(theVoid.size());
			http.Disconnect();
		}

		// Currently, we're not using keepalive, so gotta reconnect...
		if (http.Connect(timeout)) {
			char resource6[1024] = {};
			std::string ip = fd_util::GetLocalIP(http.sock());
			snprintf(resource6, sizeof(resource6) - 1, "/match/update?local=%s&port=%d", ip.c_str(), port);

			if (http.GET(http::RequestParams(resource6), &theVoid, &progress) > 0)
				success = true;
			theVoid.Skip(theVoid.size());
			http.Disconnect();
		}
	}

	return success;
}

bool RemoteISOFileSupported(const std::string &filename) {
	// Disc-like files.
	if (endsWithNoCase(filename, ".cso") || endsWithNoCase(filename, ".iso")) {
		return true;
	}
	// May work - but won't have supporting files.
	if (endsWithNoCase(filename, ".pbp")) {
		return true;
	}
	// Debugging files.
	if (endsWithNoCase(filename, ".ppdmp")) {
		return true;
	}
	return false;
}

static std::string RemotePathForRecent(const std::string &filename) {
#ifdef _WIN32
	static const std::string sep = "\\/";
#else
	static const std::string sep = "/";
#endif
	size_t basepos = filename.find_last_of(sep);
	std::string basename = "/" + (basepos == filename.npos ? filename : filename.substr(basepos + 1));

	if (basename == "/EBOOT.PBP") {
		// Go up one more folder.
		size_t nextpos = filename.find_last_of(sep, basepos - 1);
		basename = "/" + (nextpos == filename.npos ? filename : filename.substr(nextpos + 1));
	}

	// Let's not serve directories, since they won't work.  Only single files.
	// Maybe can do PBPs and other files later.  Would be neat to stream virtual disc filesystems.
	if (RemoteISOFileSupported(basename)) {
		return ReplaceAll(basename, " ", "%20");
	}
	return "";
}

static Path LocalFromRemotePath(const std::string &path) {
	for (const std::string &filename : g_Config.recentIsos) {
		std::string basename = RemotePathForRecent(filename);
		if (basename == path) {
			return Path(filename);
		}
	}
	return Path();
}

static void DiscHandler(const http::Request &request, const Path &filename) {
	s64 sz = File::GetFileSize(filename);

	std::string range;
	if (request.Method() == http::RequestHeader::HEAD) {
		request.WriteHttpResponseHeader("1.0", 200, sz, "application/octet-stream", "Accept-Ranges: bytes\r\n");
	} else if (request.GetHeader("range", &range)) {
		s64 begin = 0, last = 0;
		if (sscanf(range.c_str(), "bytes=%lld-%lld", &begin, &last) != 2) {
			request.WriteHttpResponseHeader("1.0", 400, -1, "text/plain");
			request.Out()->Push("Could not understand range request.");
			return;
		}

		if (begin < 0 || begin > last || last >= sz) {
			request.WriteHttpResponseHeader("1.0", 416, -1, "text/plain");
			request.Out()->Push("Range goes outside of file.");
			return;
		}

		FILE *fp = File::OpenCFile(filename, "rb");
		if (!fp || fseek(fp, begin, SEEK_SET) != 0) {
			request.WriteHttpResponseHeader("1.0", 500, -1, "text/plain");
			request.Out()->Push("File access failed.");
			if (fp) {
				fclose(fp);
			}
			return;
		}

		s64 len = last - begin + 1;
		char contentRange[1024];
		sprintf(contentRange, "Content-Range: bytes %lld-%lld/%lld\r\n", begin, last, sz);
		request.WriteHttpResponseHeader("1.0", 206, len, "application/octet-stream", contentRange);

		const size_t CHUNK_SIZE = 16 * 1024;
		char *buf = new char[CHUNK_SIZE];
		for (s64 pos = 0; pos < len; pos += CHUNK_SIZE) {
			s64 chunklen = std::min(len - pos, (s64)CHUNK_SIZE);
			if (fread(buf, chunklen, 1, fp) != 1)
				break;
			request.Out()->Push(buf, chunklen);
		}
		fclose(fp);
		delete[] buf;
		request.Out()->Flush();
	} else {
		request.WriteHttpResponseHeader("1.0", 418, -1, "text/plain");
		request.Out()->Push("This server only supports range requests.");
	}
}

static void HandleListing(const http::Request &request) {
	request.WriteHttpResponseHeader("1.0", 200, -1, "text/plain");
	request.Out()->Printf("/\n");
	if (serverFlags & (int)WebServerFlags::DISCS) {
		// List the current discs in their recent order.
		for (const std::string &filename : g_Config.recentIsos) {
			std::string basename = RemotePathForRecent(filename);
			if (!basename.empty()) {
				request.Out()->Printf("%s\n", basename.c_str());
			}
		}
	}
	if (serverFlags & (int)WebServerFlags::DEBUGGER) {
		request.Out()->Printf("/debugger\n");
	}
}

static void HandleFallback(const http::Request &request) {
	if (serverFlags & (int)WebServerFlags::DISCS) {
		Path filename = LocalFromRemotePath(request.resource());
		if (!filename.empty()) {
			DiscHandler(request, filename);
			return;
		}
	}

	static const std::string payload = "404 not found\r\n";
	request.WriteHttpResponseHeader("1.0", 404, (int)payload.size(), "text/plain");
	request.Out()->Push(payload);
}

static void ForwardDebuggerRequest(const http::Request &request) {
	if (serverFlags & (int)WebServerFlags::DEBUGGER) {
		HandleDebuggerRequest(request);
	} else {
		HandleFallback(request);
	}
}

static void ExecuteWebServer() {
	SetCurrentThreadName("HTTPServer");

	auto http = new http::Server(new NewThreadExecutor());
	http->RegisterHandler("/", &HandleListing);
	// This lists all the (current) recent ISOs.
	http->SetFallbackHandler(&HandleFallback);
	http->RegisterHandler("/debugger", &ForwardDebuggerRequest);

	if (!http->Listen(g_Config.iRemoteISOPort)) {
		if (!http->Listen(0)) {
			ERROR_LOG(FILESYS, "Unable to listen on any port");
			UpdateStatus(ServerStatus::FINISHED);
			return;
		}
	}
	UpdateStatus(ServerStatus::RUNNING);

	g_Config.iRemoteISOPort = http->Port();
	RegisterServer(http->Port());
	double lastRegister = time_now_d();
	while (RetrieveStatus() == ServerStatus::RUNNING) {
		http->RunSlice(1.0);

		double now = time_now_d();
		if (now > lastRegister + 540.0) {
			RegisterServer(http->Port());
			lastRegister = now;
		}
	}

	http->Stop();
	StopAllDebuggers();
	delete http;

	UpdateStatus(ServerStatus::FINISHED);
}

bool StartWebServer(WebServerFlags flags) {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	switch (serverStatus) {
	case ServerStatus::RUNNING:
		if ((serverFlags & (int)flags) == (int)flags) {
			return false;
		}
		serverFlags |= (int)flags;
		return true;

	case ServerStatus::FINISHED:
		serverThread.join();
		// Intentional fallthrough.
	case ServerStatus::STOPPED:
		serverStatus = ServerStatus::STARTING;
		serverFlags = (int)flags;
		serverThread = std::thread(&ExecuteWebServer);
		return true;

	default:
		return false;
	}
}

bool StopWebServer(WebServerFlags flags) {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	if (serverStatus != ServerStatus::RUNNING) {
		return false;
	}

	serverFlags &= ~(int)flags;
	if (serverFlags == 0) {
		serverStatus = ServerStatus::STOPPING;
	}
	return true;
}

bool WebServerStopping(WebServerFlags flags) {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	return serverStatus == ServerStatus::STOPPING;
}

bool WebServerStopped(WebServerFlags flags) {
	std::lock_guard<std::mutex> guard(serverStatusLock);
	if (serverStatus == ServerStatus::RUNNING) {
		return (serverFlags & (int)flags) == 0;
	}
	return serverStatus == ServerStatus::STOPPED || serverStatus == ServerStatus::FINISHED;
}

void ShutdownWebServer() {
	StopWebServer(WebServerFlags::ALL);

	if (serverStatus != ServerStatus::STOPPED)
		serverThread.join();
	serverStatus = ServerStatus::STOPPED;
}
