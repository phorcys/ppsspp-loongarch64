#include "ppsspp_config.h"
#ifdef _WIN32
#include <winsock2.h>
#undef min
#undef max
#else
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstring>

#ifndef MSG_NOSIGNAL
// Default value to 0x00 (do nothing) in systems where it's not supported.
#define MSG_NOSIGNAL 0x00
#endif

#include "Common/File/FileDescriptor.h"
#include "Common/Log.h"
#include "Common/Net/NetBuffer.h"
#include "Common/TimeUtil.h"

namespace net {

bool Buffer::FlushSocket(uintptr_t sock, double timeout, bool *cancelled) {
	static constexpr float CANCEL_INTERVAL = 0.25f;
	for (size_t pos = 0, end = data_.size(); pos < end; ) {
		bool ready = false;
		double endTimeout = time_now_d() + timeout;
		while (!ready) {
			if (cancelled && *cancelled)
				return false;
			ready = fd_util::WaitUntilReady(sock, CANCEL_INTERVAL, true);
			if (!ready && time_now_d() > endTimeout) {
				ERROR_LOG(IO, "FlushSocket timed out");
				return false;
			}
		}
		int sent = send(sock, &data_[pos], (int)(end - pos), MSG_NOSIGNAL);
		if (sent < 0) {
			ERROR_LOG(IO, "FlushSocket failed");
			return false;
		}
		pos += sent;
	}
	data_.resize(0);
	return true;
}

bool Buffer::ReadAllWithProgress(int fd, int knownSize, float *progress, float *kBps, bool *cancelled) {
	static constexpr float CANCEL_INTERVAL = 0.25f;
	std::vector<char> buf;
	// We're non-blocking and reading from an OS buffer, so try to read as much as we can at a time.
	if (knownSize >= 65536 * 16) {
		buf.resize(65536);
	} else if (knownSize >= 1024 * 16) {
		buf.resize(knownSize / 16);
	} else {
		buf.resize(1024);
	}

	double st = time_now_d();
	int total = 0;
	while (true) {
		bool ready = false;
		while (!ready && cancelled) {
			if (*cancelled)
				return false;
			ready = fd_util::WaitUntilReady(fd, CANCEL_INTERVAL, false);
		}
		int retval = recv(fd, &buf[0], (int)buf.size(), MSG_NOSIGNAL);
		if (retval == 0) {
			return true;
		} else if (retval < 0) {
#if PPSSPP_PLATFORM(WINDOWS)
			if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
			if (errno != EWOULDBLOCK)
#endif
				ERROR_LOG(IO, "Error reading from buffer: %i", retval);
			return false;
		}
		char *p = Append((size_t)retval);
		memcpy(p, &buf[0], retval);
		total += retval;
		if (progress)
			*progress = (float)total / (float)knownSize;
		if (kBps)
			*kBps = (float)(total / (time_now_d() - st)) / 1024.0f;
	}
	return true;
}

int Buffer::Read(int fd, size_t sz) {
	char buf[1024];
	int retval;
	size_t received = 0;
	while ((retval = recv(fd, buf, (int)std::min(sz, sizeof(buf)), MSG_NOSIGNAL)) > 0) {
		if (retval < 0) {
			return retval;
		}
		char *p = Append((size_t)retval);
		memcpy(p, buf, retval);
		sz -= retval;
		received += retval;
		if (sz == 0)
			return 0;
	}
	return (int)received;
}

}
