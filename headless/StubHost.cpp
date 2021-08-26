// Copyright (c) 2017- PPSSPP Project.

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

#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/CoreParameter.h"
#include "Core/System.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "headless/Compare.h"
#include "headless/StubHost.h"

void HeadlessHost::SendOrCollectDebugOutput(const std::string &data)
{
	if (PSP_CoreParameter().printfEmuLog)
		SendDebugOutput(data);
	else if (PSP_CoreParameter().collectEmuLog)
		*PSP_CoreParameter().collectEmuLog += data;
	else
		DEBUG_LOG(COMMON, "%s", data.c_str());
}

void HeadlessHost::SendDebugScreenshot(const u8 *pixbuf, u32 w, u32 h) {
	// Only if we're actually comparing.
	if (comparisonScreenshot_.empty()) {
		return;
	}

	// We ignore the current framebuffer parameters and just grab the full screen.
	const static u32 FRAME_STRIDE = 512;
	const static u32 FRAME_WIDTH = 480;
	const static u32 FRAME_HEIGHT = 272;

	GPUDebugBuffer buffer;
	gpuDebug->GetCurrentFramebuffer(buffer, GPU_DBG_FRAMEBUF_RENDER);
	const std::vector<u32> pixels = TranslateDebugBufferToCompare(&buffer, 512, 272);

	std::string error;
	double errors = CompareScreenshot(pixels, FRAME_STRIDE, FRAME_WIDTH, FRAME_HEIGHT, comparisonScreenshot_, error);
	if (errors < 0)
		SendOrCollectDebugOutput(error + "\n");

	if (errors > 0)
	{
		char temp[256];
		snprintf(temp, sizeof(temp), "Screenshot error: %f%%\n", errors * 100.0f);
		SendOrCollectDebugOutput(temp);
	}

	if (errors > 0 && !teamCityMode && !getenv("GITHUB_ACTIONS")) {
		static const u8 header[14 + 40] = {
			0x42, 0x4D, 0x38, 0x80, 0x08, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
			0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x10, 0x01,
			0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x02, 0x80, 0x08, 0x00, 0x12, 0x0B,
			0x00, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		};

		FILE *saved = File::OpenCFile(Path("__testfailure.bmp"), "wb");
		if (saved) {
			fwrite(&header, sizeof(header), 1, saved);
			fwrite(pixels.data(), sizeof(u32), FRAME_STRIDE * FRAME_HEIGHT, saved);
			fclose(saved);

			SendOrCollectDebugOutput("Actual output written to: __testfailure.bmp\n");
		}
	}
}
