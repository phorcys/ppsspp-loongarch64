// Copyright (c) 2012- PPSSPP Project.

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

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"

struct GPUDebugBuffer;

extern bool teamCityMode;
extern std::string currentTestName;
void TeamCityPrint(const char *fmt, ...);
void GitHubActionsPrint(const char *type, const char *fmt, ...);

Path ExpectedFromFilename(const Path &bootFilename);
Path ExpectedScreenshotFromFilename(const Path &bootFilename);
std::string GetTestName(const Path &bootFilename);

bool CompareOutput(const Path &bootFilename, const std::string &output, bool verbose);
std::vector<u32> TranslateDebugBufferToCompare(const GPUDebugBuffer *buffer, u32 stride, u32 h);
double CompareScreenshot(const std::vector<u32> &pixels, u32 stride, u32 w, u32 h, const Path &screenshotFilename, std::string &error);
