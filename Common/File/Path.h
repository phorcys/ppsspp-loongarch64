#pragma once

#include "ppsspp_config.h"

#include <string>

enum class PathType {
	UNDEFINED = 0,
	NATIVE = 1,  // Can be relative.
	CONTENT_URI = 2,  // Android only. Can only be absolute!
	HTTP = 3,  // http://, https://
};

// Windows paths are always stored with '/' slashes in a Path.
// On .ToWString(), they are flipped back to '\'.

class Path {
private:
	void Init(const std::string &str);

public:
	Path() : type_(PathType::UNDEFINED) {}
	explicit Path(const std::string &str);

#if PPSSPP_PLATFORM(WINDOWS)
	explicit Path(const std::wstring &str);
#endif

	PathType Type() const {
		return type_;
	}

	bool Valid() const { return !path_.empty(); }
	bool IsRoot() const { return path_ == "/"; }  // Special value - only path that can end in a slash.

	// Some std::string emulation for simplicity.
	bool empty() const { return !Valid(); }
	void clear() {
		type_ = PathType::UNDEFINED;
		path_.clear();
	}
	size_t size() const {
		return path_.size();
	}

	// WARNING: Potentially unsafe usage, if it's not NATIVE.
	const char *c_str() const {
		return path_.c_str();
	}

	bool IsAbsolute() const;

	// Returns a path extended with a subdirectory.
	Path operator /(const std::string &subdir) const;

	// Navigates down into a subdir.
	void operator /=(const std::string &subdir);

	// File extension manipulation.
	Path WithExtraExtension(const std::string &ext) const;
	Path WithReplacedExtension(const std::string &oldExtension, const std::string &newExtension) const;
	Path WithReplacedExtension(const std::string &newExtension) const;

	// Removes the last component.
	std::string GetFilename() const;  // Really, GetLastComponent. Could be a file or directory. Includes the extension.
	std::string GetFileExtension() const;  // Always lowercase return. Includes the dot.
	std::string GetDirectory() const;

	const std::string &ToString() const;

#if PPSSPP_PLATFORM(WINDOWS)
	std::wstring ToWString() const;
#endif

	std::string ToVisualString() const;

	bool CanNavigateUp() const;
	Path NavigateUp() const;

	// Navigates as far up as possible from this path. If not possible to navigate upwards, returns the same path.
	// Not actually always the root of the volume, especially on systems like Mac and Linux where things are often mounted.
	// For Android directory trees, navigates to the root of the tree.
	Path GetRootVolume() const;

	bool ComputePathTo(const Path &other, std::string &path) const;

	bool operator ==(const Path &other) const {
		return path_ == other.path_ && type_ == other.type_;
	}
	bool operator !=(const Path &other) const {
		return path_ != other.path_ || type_ != other.type_;
	}

	bool FilePathContains(const std::string &needle) const;

	bool StartsWith(const Path &other) const;

	bool operator <(const Path &other) const {
		return path_ < other.path_;
	}

private:
	// The internal representation is currently always the plain string.
	// For CPU efficiency we could keep an AndroidStorageContentURI too,
	// but I don't think the encode/decode cost is significant. We simply create
	// those for processing instead.
	std::string path_;

	PathType type_;
};
