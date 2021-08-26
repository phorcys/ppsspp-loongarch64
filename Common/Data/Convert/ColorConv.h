// Copyright (c) 2015- PPSSPP Project.

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

#pragma once

#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Common/Data/Convert/ColorConvNEON.h"

void SetupColorConv();

inline u8 Convert4To8(u8 v) {
	// Swizzle bits: 00001234 -> 12341234
	return (v << 4) | (v);
}

inline u8 Convert5To8(u8 v) {
	// Swizzle bits: 00012345 -> 12345123
	return (v << 3) | (v >> 2);
}

inline u8 Convert6To8(u8 v) {
	// Swizzle bits: 00123456 -> 12345612
	return (v << 2) | (v >> 4);
}

inline u32 RGBA4444ToRGBA8888(u16 src) {
	const u32 r = (src & 0x000F) << 0;
	const u32 g = (src & 0x00F0) << 4;
	const u32 b = (src & 0x0F00) << 8;
	const u32 a = (src & 0xF000) << 12;

	const u32 c = r | g | b | a;
	return c | (c << 4);
}

inline u32 RGBA5551ToRGBA8888(u16 src) {
	u8 r = Convert5To8((src >> 0) & 0x1F);
	u8 g = Convert5To8((src >> 5) & 0x1F);
	u8 b = Convert5To8((src >> 10) & 0x1F);
	u8 a = (src >> 15) & 0x1;
	a = (a) ? 0xff : 0;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

inline u32 RGB565ToRGBA8888(u16 src) {
	u8 r = Convert5To8((src >> 0) & 0x1F);
	u8 g = Convert6To8((src >> 5) & 0x3F);
	u8 b = Convert5To8((src >> 11) & 0x1F);
	u8 a = 0xFF;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

inline u16 RGBA8888ToRGB565(u32 value) {
	u32 r = (value >> 3) & 0x1F;
	u32 g = (value >> 5) & (0x3F << 5);
	u32 b = (value >> 8) & (0x1F << 11);
	return (u16)(r | g | b);
}

inline u16 RGBA8888ToRGBA5551(u32 value) {
	u32 r = (value >> 3) & 0x1F;
	u32 g = (value >> 6) & (0x1F << 5);
	u32 b = (value >> 9) & (0x1F << 10);
	u32 a = (value >> 16) & 0x8000;
	return (u16)(r | g | b | a);
}

inline u16 RGBA8888ToRGBA4444(u32 value) {
	const u32 c = value >> 4;
	const u16 r = (c >> 0) & 0x000F;
	const u16 g = (c >> 4) & 0x00F0;
	const u16 b = (c >> 8) & 0x0F00;
	const u16 a = (c >> 12) & 0xF000;
	return r | g | b | a;
}

// convert image to 8888, parallelizable
// TODO: Implement these in terms of the conversion functions below.
void convert4444_gl(u16* data, u32* out, int width, int l, int u);
void convert565_gl(u16* data, u32* out, int width, int l, int u);
void convert5551_gl(u16* data, u32* out, int width, int l, int u);
void convert4444_dx9(u16* data, u32* out, int width, int l, int u);
void convert565_dx9(u16* data, u32* out, int width, int l, int u);
void convert5551_dx9(u16* data, u32* out, int width, int l, int u);

// "Complete" set of color conversion functions between the usual formats.

// TODO: Need to revisit the naming convention of these. Seems totally backwards
// now that we've standardized on Draw::DataFormat.

typedef void (*Convert16bppTo16bppFunc)(u16 *dst, const u16 *src, u32 numPixels);
typedef void (*Convert16bppTo32bppFunc)(u32 *dst, const u16 *src, u32 numPixels);
typedef void (*Convert32bppTo16bppFunc)(u16 *dst, const u32 *src, u32 numPixels);
typedef void (*Convert32bppTo32bppFunc)(u32 *dst, const u32 *src, u32 numPixels);

void ConvertBGRA8888ToRGBA8888(u32 *dst, const u32 *src, u32 numPixels);
#define ConvertRGBA8888ToBGRA8888 ConvertBGRA8888ToRGBA8888
void ConvertBGRA8888ToRGB888(u8 *dst, const u32 *src, u32 numPixels);

void ConvertRGBA8888ToRGBA5551(u16 *dst, const u32 *src, u32 numPixels);
void ConvertRGBA8888ToRGB565(u16 *dst, const u32 *src, u32 numPixels);
void ConvertRGBA8888ToRGBA4444(u16 *dst, const u32 *src, u32 numPixels);
void ConvertRGBA8888ToRGB888(u8 *dst, const u32 *src, u32 numPixels);

void ConvertBGRA8888ToRGBA5551(u16 *dst, const u32 *src, u32 numPixels);
void ConvertBGRA8888ToRGB565(u16 *dst, const u32 *src, u32 numPixels);
void ConvertBGRA8888ToRGBA4444(u16 *dst, const u32 *src, u32 numPixels);

void ConvertRGB565ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertRGBA5551ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertRGBA4444ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);

void ConvertBGR565ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertABGR1555ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertABGR4444ToRGBA8888(u32 *dst, const u16 *src, u32 numPixels);

void ConvertRGBA4444ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertRGBA5551ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels);
void ConvertRGB565ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels);

void ConvertRGBA4444ToABGR4444Basic(u16 *dst, const u16 *src, u32 numPixels);
void ConvertRGBA5551ToABGR1555Basic(u16 *dst, const u16 *src, u32 numPixels);
void ConvertRGB565ToBGR565Basic(u16 *dst, const u16 *src, u32 numPixels);
void ConvertBGRA5551ToABGR1555(u16 *dst, const u16 *src, u32 numPixels);

#if PPSSPP_ARCH(ARM64)
#define ConvertRGBA4444ToABGR4444 ConvertRGBA4444ToABGR4444NEON
#elif !PPSSPP_ARCH(ARM)
#define ConvertRGBA4444ToABGR4444 ConvertRGBA4444ToABGR4444Basic
#else
extern Convert16bppTo16bppFunc ConvertRGBA4444ToABGR4444;
#endif

#if PPSSPP_ARCH(ARM64)
#define ConvertRGBA5551ToABGR1555 ConvertRGBA5551ToABGR1555NEON
#elif !PPSSPP_ARCH(ARM)
#define ConvertRGBA5551ToABGR1555 ConvertRGBA5551ToABGR1555Basic
#else
extern Convert16bppTo16bppFunc ConvertRGBA5551ToABGR1555;
#endif

#if PPSSPP_ARCH(ARM64)
#define ConvertRGB565ToBGR565 ConvertRGB565ToBGR565NEON
#elif !PPSSPP_ARCH(ARM)
#define ConvertRGB565ToBGR565 ConvertRGB565ToBGR565Basic
#else
extern Convert16bppTo16bppFunc ConvertRGB565ToBGR565;
#endif
