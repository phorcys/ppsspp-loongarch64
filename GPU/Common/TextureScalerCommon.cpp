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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "GPU/Common/TextureScalerCommon.h"

#include "Core/Config.h"
#include "Common/Common.h"
#include "Common/Log.h"
#include "Common/CommonFuncs.h"
#include "Common/Thread/ParallelLoop.h"
#include "Core/ThreadPools.h"
#include "Common/CPUDetect.h"
#include "ext/xbrz/xbrz.h"

#if _M_SSE >= 0x401
#include <smmintrin.h>
#endif

// Report the time and throughput for each larger scaling operation in the log
//#define SCALING_MEASURE_TIME

//#define DEBUG_SCALER_OUTPUT

#ifdef SCALING_MEASURE_TIME
#include "Common/TimeUtil.h"
#endif

/////////////////////////////////////// Helper Functions (mostly math for parallelization)

namespace {
//////////////////////////////////////////////////////////////////// Various image processing

#define R(_col) ((_col>> 0)&0xFF)
#define G(_col) ((_col>> 8)&0xFF)
#define B(_col) ((_col>>16)&0xFF)
#define A(_col) ((_col>>24)&0xFF)

#define DISTANCE(_p1,_p2) ( abs(static_cast<int>(static_cast<int>(R(_p1))-R(_p2))) + abs(static_cast<int>(static_cast<int>(G(_p1))-G(_p2))) \
							  + abs(static_cast<int>(static_cast<int>(B(_p1))-B(_p2))) + abs(static_cast<int>(static_cast<int>(A(_p1))-A(_p2))) )

// this is sadly much faster than an inline function with a loop, at least in VC10
#define MIX_PIXELS(_p0, _p1, _factors) \
		( (R(_p0)*(_factors)[0] + R(_p1)*(_factors)[1])/255 <<  0 ) | \
		( (G(_p0)*(_factors)[0] + G(_p1)*(_factors)[1])/255 <<  8 ) | \
		( (B(_p0)*(_factors)[0] + B(_p1)*(_factors)[1])/255 << 16 ) | \
		( (A(_p0)*(_factors)[0] + A(_p1)*(_factors)[1])/255 << 24 )

#define BLOCK_SIZE 32

// 3x3 convolution with Neumann boundary conditions, parallelizable
// quite slow, could be sped up a lot
// especially handling of separable kernels
void convolve3x3(u32* data, u32* out, const int kernel[3][3], int width, int height, int l, int u) {
	for (int yb = 0; yb < (u - l) / BLOCK_SIZE + 1; ++yb) {
		for (int xb = 0; xb < width / BLOCK_SIZE + 1; ++xb) {
			for (int y = l + yb*BLOCK_SIZE; y < l + (yb + 1)*BLOCK_SIZE && y < u; ++y) {
				for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < width; ++x) {
					int val = 0;
					for (int yoff = -1; yoff <= 1; ++yoff) {
						int yy = std::max(std::min(y + yoff, height - 1), 0);
						for (int xoff = -1; xoff <= 1; ++xoff) {
							int xx = std::max(std::min(x + xoff, width - 1), 0);
							val += data[yy*width + xx] * kernel[yoff + 1][xoff + 1];
						}
					}
					out[y*width + x] = abs(val);
				}
			}
		}
	}
}

// deposterization: smoothes posterized gradients from low-color-depth (e.g. 444, 565, compressed) sources
void deposterizeH(u32* data, u32* out, int w, int l, int u) {
	static const int T = 8;
	for (int y = l; y < u; ++y) {
		for (int x = 0; x < w; ++x) {
			int inpos = y*w + x;
			u32 center = data[inpos];
			if (x == 0 || x == w - 1) {
				out[y*w + x] = center;
				continue;
			}
			u32 left = data[inpos - 1];
			u32 right = data[inpos + 1];
			out[y*w + x] = 0;
			for (int c = 0; c < 4; ++c) {
				u8 lc = ((left >> c * 8) & 0xFF);
				u8 cc = ((center >> c * 8) & 0xFF);
				u8 rc = ((right >> c * 8) & 0xFF);
				if ((lc != rc) && ((lc == cc && abs((int)((int)rc) - cc) <= T) || (rc == cc && abs((int)((int)lc) - cc) <= T))) {
					// blend this component
					out[y*w + x] |= ((rc + lc) / 2) << (c * 8);
				} else {
					// no change for this component
					out[y*w + x] |= cc << (c * 8);
				}
			}
		}
	}
}
void deposterizeV(u32* data, u32* out, int w, int h, int l, int u) {
	static const int T = 8;
	for (int xb = 0; xb < w / BLOCK_SIZE + 1; ++xb) {
		for (int y = l; y < u; ++y) {
			for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < w; ++x) {
				u32 center = data[y    * w + x];
				if (y == 0 || y == h - 1) {
					out[y*w + x] = center;
					continue;
				}
				u32 upper = data[(y - 1) * w + x];
				u32 lower = data[(y + 1) * w + x];
				out[y*w + x] = 0;
				for (int c = 0; c < 4; ++c) {
					u8 uc = ((upper >> c * 8) & 0xFF);
					u8 cc = ((center >> c * 8) & 0xFF);
					u8 lc = ((lower >> c * 8) & 0xFF);
					if ((uc != lc) && ((uc == cc && abs((int)((int)lc) - cc) <= T) || (lc == cc && abs((int)((int)uc) - cc) <= T))) {
						// blend this component
						out[y*w + x] |= ((lc + uc) / 2) << (c * 8);
					} else {
						// no change for this component
						out[y*w + x] |= cc << (c * 8);
					}
				}
			}
		}
	}
}

// generates a distance mask value for each pixel in data
// higher values -> larger distance to the surrounding pixels
void generateDistanceMask(u32* data, u32* out, int width, int height, int l, int u) {
	for (int yb = 0; yb < (u - l) / BLOCK_SIZE + 1; ++yb) {
		for (int xb = 0; xb < width / BLOCK_SIZE + 1; ++xb) {
			for (int y = l + yb*BLOCK_SIZE; y < l + (yb + 1)*BLOCK_SIZE && y < u; ++y) {
				for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < width; ++x) {
					const u32 center = data[y*width + x];
					u32 dist = 0;
					for (int yoff = -1; yoff <= 1; ++yoff) {
						int yy = y + yoff;
						if (yy == height || yy == -1) {
							dist += 1200; // assume distance at borders, usually makes for better result
							continue;
						}
						for (int xoff = -1; xoff <= 1; ++xoff) {
							if (yoff == 0 && xoff == 0) continue;
							int xx = x + xoff;
							if (xx == width || xx == -1) {
								dist += 400; // assume distance at borders, usually makes for better result
								continue;
							}
							dist += DISTANCE(data[yy*width + xx], center);
						}
					}
					out[y*width + x] = dist;
				}
			}
		}
	}
}

// mix two images based on a mask
void mix(u32* data, u32* source, u32* mask, u32 maskmax, int width, int l, int u) {
	for (int y = l; y < u; ++y) {
		for (int x = 0; x < width; ++x) {
			int pos = y*width + x;
			u8 mixFactors[2] = { 0, static_cast<u8>((std::min(mask[pos], maskmax) * 255) / maskmax) };
			mixFactors[0] = 255 - mixFactors[1];
			data[pos] = MIX_PIXELS(data[pos], source[pos], mixFactors);
			if (A(source[pos]) == 0) data[pos] = data[pos] & 0x00FFFFFF; // xBRZ always does a better job with hard alpha
		}
	}
}

//////////////////////////////////////////////////////////////////// Bicubic scaling

// generate the value of a Mitchell-Netravali scaling spline at distance d, with parameters A and B
// B=1 C=0   : cubic B spline (very smooth)
// B=C=1/3   : recommended for general upscaling
// B=0 C=1/2 : Catmull-Rom spline (sharp, ringing)
// see Mitchell & Netravali, "Reconstruction Filters in Computer Graphics"
inline float mitchell(float x, float B, float C) {
	float ax = fabs(x);
	if (ax >= 2.0f) return 0.0f;
	if (ax >= 1.0f) return ((-B - 6 * C)*(x*x*x) + (6 * B + 30 * C)*(x*x) + (-12 * B - 48 * C)*x + (8 * B + 24 * C)) / 6.0f;
	return ((12 - 9 * B - 6 * C)*(x*x*x) + (-18 + 12 * B + 6 * C)*(x*x) + (6 - 2 * B)) / 6.0f;
}

// arrays for pre-calculating weights and sums (~20KB)
// Dimensions:
//   0: 0 = BSpline, 1 = mitchell
//   2: 2-5x scaling
// 2,3: 5x5 generated pixels 
// 4,5: 5x5 pixels sampled from
float bicubicWeights[2][4][5][5][5][5];
float bicubicInvSums[2][4][5][5];

// initialize pre-computed weights array
void initBicubicWeights() {
	float B[2] = { 1.0f, 0.334f };
	float C[2] = { 0.0f, 0.334f };
	for (int type = 0; type < 2; ++type) {
		for (int factor = 2; factor <= 5; ++factor) {
			for (int x = 0; x < factor; ++x) {
				for (int y = 0; y < factor; ++y) {
					float sum = 0.0f;
					for (int sx = -2; sx <= 2; ++sx) {
						for (int sy = -2; sy <= 2; ++sy) {
							float dx = (x + 0.5f) / factor - (sx + 0.5f);
							float dy = (y + 0.5f) / factor - (sy + 0.5f);
							float dist = sqrt(dx*dx + dy*dy);
							float weight = mitchell(dist, B[type], C[type]);
							bicubicWeights[type][factor - 2][x][y][sx + 2][sy + 2] = weight;
							sum += weight;
						}
					}
					bicubicInvSums[type][factor - 2][x][y] = 1.0f / sum;
				}
			}
		}
	}
}

// perform bicubic scaling by factor f, with precomputed spline type T
template<int f, int T>
void scaleBicubicT(u32* data, u32* out, int w, int h, int l, int u) {
	int outw = w*f;
	for (int yb = 0; yb < (u - l)*f / BLOCK_SIZE + 1; ++yb) {
		for (int xb = 0; xb < w*f / BLOCK_SIZE + 1; ++xb) {
			for (int y = l*f + yb*BLOCK_SIZE; y < l*f + (yb + 1)*BLOCK_SIZE && y < u*f; ++y) {
				for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < w*f; ++x) {
					float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
					int cx = x / f, cy = y / f;
					// sample supporting pixels in original image
					for (int sx = -2; sx <= 2; ++sx) {
						for (int sy = -2; sy <= 2; ++sy) {
							float weight = bicubicWeights[T][f - 2][x%f][y%f][sx + 2][sy + 2];
							if (weight != 0.0f) {
								// clamp pixel locations
								int csy = std::max(std::min(sy + cy, h - 1), 0);
								int csx = std::max(std::min(sx + cx, w - 1), 0);
								// sample & add weighted components
								u32 sample = data[csy*w + csx];
								r += weight*R(sample);
								g += weight*G(sample);
								b += weight*B(sample);
								a += weight*A(sample);
							}
						}
					}
					// generate and write result
					float invSum = bicubicInvSums[T][f - 2][x%f][y%f];
					int ri = std::min(std::max(static_cast<int>(ceilf(r*invSum)), 0), 255);
					int gi = std::min(std::max(static_cast<int>(ceilf(g*invSum)), 0), 255);
					int bi = std::min(std::max(static_cast<int>(ceilf(b*invSum)), 0), 255);
					int ai = std::min(std::max(static_cast<int>(ceilf(a*invSum)), 0), 255);
					out[y*outw + x] = (ai << 24) | (bi << 16) | (gi << 8) | ri;
				}
			}
		}
	}
}
#if _M_SSE >= 0x401
template<int f, int T>
void scaleBicubicTSSE41(u32* data, u32* out, int w, int h, int l, int u) {
	int outw = w*f;
	for (int yb = 0; yb < (u - l)*f / BLOCK_SIZE + 1; ++yb) {
		for (int xb = 0; xb < w*f / BLOCK_SIZE + 1; ++xb) {
			for (int y = l*f + yb*BLOCK_SIZE; y < l*f + (yb + 1)*BLOCK_SIZE && y < u*f; ++y) {
				for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < w*f; ++x) {
					__m128 result = _mm_set1_ps(0.0f);
					int cx = x / f, cy = y / f;
					// sample supporting pixels in original image
					for (int sx = -2; sx <= 2; ++sx) {
						for (int sy = -2; sy <= 2; ++sy) {
							float weight = bicubicWeights[T][f - 2][x%f][y%f][sx + 2][sy + 2];
							if (weight != 0.0f) {
								// clamp pixel locations
								int csy = std::max(std::min(sy + cy, h - 1), 0);
								int csx = std::max(std::min(sx + cx, w - 1), 0);
								// sample & add weighted components
								__m128i sample = _mm_cvtsi32_si128(data[csy*w + csx]);
								sample = _mm_cvtepu8_epi32(sample);
								__m128 col = _mm_cvtepi32_ps(sample);
								col = _mm_mul_ps(col, _mm_set1_ps(weight));
								result = _mm_add_ps(result, col);
							}
						}
					}
					// generate and write result
					__m128i pixel = _mm_cvtps_epi32(_mm_mul_ps(result, _mm_set1_ps(bicubicInvSums[T][f - 2][x%f][y%f])));
					pixel = _mm_packs_epi32(pixel, pixel);
					pixel = _mm_packus_epi16(pixel, pixel);
					out[y*outw + x] = _mm_cvtsi128_si32(pixel);
				}
			}
		}
	}
}
#endif

void scaleBicubicBSpline(int factor, u32* data, u32* out, int w, int h, int l, int u) {
#if _M_SSE >= 0x401
	if (cpu_info.bSSE4_1) {
		switch (factor) {
		case 2: scaleBicubicTSSE41<2, 0>(data, out, w, h, l, u); break; // when I first tested this, 
		case 3: scaleBicubicTSSE41<3, 0>(data, out, w, h, l, u); break; // it was even slower than I had expected
		case 4: scaleBicubicTSSE41<4, 0>(data, out, w, h, l, u); break; // turns out I had not included
		case 5: scaleBicubicTSSE41<5, 0>(data, out, w, h, l, u); break; // any of these break statements
		default: ERROR_LOG(G3D, "Bicubic upsampling only implemented for factors 2 to 5");
		}
	} else {
#endif
		switch (factor) {
		case 2: scaleBicubicT<2, 0>(data, out, w, h, l, u); break; // when I first tested this, 
		case 3: scaleBicubicT<3, 0>(data, out, w, h, l, u); break; // it was even slower than I had expected
		case 4: scaleBicubicT<4, 0>(data, out, w, h, l, u); break; // turns out I had not included
		case 5: scaleBicubicT<5, 0>(data, out, w, h, l, u); break; // any of these break statements
		default: ERROR_LOG(G3D, "Bicubic upsampling only implemented for factors 2 to 5");
		}
#if _M_SSE >= 0x401
	}
#endif
}

void scaleBicubicMitchell(int factor, u32* data, u32* out, int w, int h, int l, int u) {
#if _M_SSE >= 0x401
	if (cpu_info.bSSE4_1) {
		switch (factor) {
		case 2: scaleBicubicTSSE41<2, 1>(data, out, w, h, l, u); break;
		case 3: scaleBicubicTSSE41<3, 1>(data, out, w, h, l, u); break;
		case 4: scaleBicubicTSSE41<4, 1>(data, out, w, h, l, u); break;
		case 5: scaleBicubicTSSE41<5, 1>(data, out, w, h, l, u); break;
		default: ERROR_LOG(G3D, "Bicubic upsampling only implemented for factors 2 to 5");
		}
	} else {
#endif
		switch (factor) {
		case 2: scaleBicubicT<2, 1>(data, out, w, h, l, u); break;
		case 3: scaleBicubicT<3, 1>(data, out, w, h, l, u); break;
		case 4: scaleBicubicT<4, 1>(data, out, w, h, l, u); break;
		case 5: scaleBicubicT<5, 1>(data, out, w, h, l, u); break;
		default: ERROR_LOG(G3D, "Bicubic upsampling only implemented for factors 2 to 5");
		}
#if _M_SSE >= 0x401
	}
#endif
}

//////////////////////////////////////////////////////////////////// Bilinear scaling

const static u8 BILINEAR_FACTORS[4][3][2] = {
		{ { 44, 211 }, { 0, 0 }, { 0, 0 } }, // x2
		{ { 64, 191 }, { 0, 255 }, { 0, 0 } }, // x3
		{ { 77, 178 }, { 26, 229 }, { 0, 0 } }, // x4
		{ { 102, 153 }, { 51, 204 }, { 0, 255 } }, // x5
};
// integral bilinear upscaling by factor f, horizontal part
template<int f>
void bilinearHt(u32* data, u32* out, int w, int l, int u) {
	static_assert(f > 1 && f <= 5, "Bilinear scaling only implemented for factors 2 to 5");
	int outw = w*f;
	for (int y = l; y < u; ++y) {
		for (int x = 0; x < w; ++x) {
			int inpos = y*w + x;
			u32 left = data[inpos - (x == 0 ? 0 : 1)];
			u32 center = data[inpos];
			u32 right = data[inpos + (x == w - 1 ? 0 : 1)];
			int i = 0;
			for (; i < f / 2 + f % 2; ++i) { // first half of the new pixels + center, hope the compiler unrolls this
				out[y*outw + x*f + i] = MIX_PIXELS(left, center, BILINEAR_FACTORS[f - 2][i]);
			}
			for (; i < f; ++i) { // second half of the new pixels, hope the compiler unrolls this
				out[y*outw + x*f + i] = MIX_PIXELS(right, center, BILINEAR_FACTORS[f - 2][f - 1 - i]);
			}
		}
	}
}
void bilinearH(int factor, u32* data, u32* out, int w, int l, int u) {
	switch (factor) {
	case 2: bilinearHt<2>(data, out, w, l, u); break;
	case 3: bilinearHt<3>(data, out, w, l, u); break;
	case 4: bilinearHt<4>(data, out, w, l, u); break;
	case 5: bilinearHt<5>(data, out, w, l, u); break;
	default: ERROR_LOG(G3D, "Bilinear upsampling only implemented for factors 2 to 5");
	}
}
// integral bilinear upscaling by factor f, vertical part
// gl/gu == global lower and upper bound
template<int f>
void bilinearVt(u32* data, u32* out, int w, int gl, int gu, int l, int u) {
	static_assert(f>1 && f <= 5, "Bilinear scaling only implemented for 2x, 3x, 4x, and 5x");
	int outw = w*f;
	for (int xb = 0; xb < outw / BLOCK_SIZE + 1; ++xb) {
		for (int y = l; y < u; ++y) {
			u32 uy = y - (y == gl ? 0 : 1);
			u32 ly = y + (y == gu - 1 ? 0 : 1);
			for (int x = xb*BLOCK_SIZE; x < (xb + 1)*BLOCK_SIZE && x < outw; ++x) {
				u32 upper = data[uy * outw + x];
				u32 center = data[y * outw + x];
				u32 lower = data[ly * outw + x];
				int i = 0;
				for (; i < f / 2 + f % 2; ++i) { // first half of the new pixels + center, hope the compiler unrolls this
					out[(y*f + i)*outw + x] = MIX_PIXELS(upper, center, BILINEAR_FACTORS[f - 2][i]);
				}
				for (; i < f; ++i) { // second half of the new pixels, hope the compiler unrolls this
					out[(y*f + i)*outw + x] = MIX_PIXELS(lower, center, BILINEAR_FACTORS[f - 2][f - 1 - i]);
				}
			}
		}
	}
}
void bilinearV(int factor, u32* data, u32* out, int w, int gl, int gu, int l, int u) {
	switch (factor) {
	case 2: bilinearVt<2>(data, out, w, gl, gu, l, u); break;
	case 3: bilinearVt<3>(data, out, w, gl, gu, l, u); break;
	case 4: bilinearVt<4>(data, out, w, gl, gu, l, u); break;
	case 5: bilinearVt<5>(data, out, w, gl, gu, l, u); break;
	default: ERROR_LOG(G3D, "Bilinear upsampling only implemented for factors 2 to 5");
	}
}

#undef BLOCK_SIZE
#undef MIX_PIXELS
#undef DISTANCE
#undef R
#undef G
#undef B
#undef A

#ifdef DEBUG_SCALER_OUTPUT

// used for debugging texture scaling (writing textures to files)
static int g_imgCount = 0;
void dbgPPM(int w, int h, u8* pixels, const char* prefix = "dbg") { // 3 component RGB
	char fn[32];
	snprintf(fn, 32, "%s%04d.ppm", prefix, g_imgCount++);
	FILE *fp = fopen(fn, "wb");
	fprintf(fp, "P6\n%d %d\n255\n", w, h);
	for (int j = 0; j < h; ++j) {
		for (int i = 0; i < w; ++i) {
			static unsigned char color[3];
			color[0] = pixels[(j*w + i) * 4 + 0];  /* red */
			color[1] = pixels[(j*w + i) * 4 + 1];  /* green */
			color[2] = pixels[(j*w + i) * 4 + 2];  /* blue */
			fwrite(color, 1, 3, fp);
		}
	}
	fclose(fp);
}
void dbgPGM(int w, int h, u32* pixels, const char* prefix = "dbg") { // 1 component
	char fn[32];
	snprintf(fn, 32, "%s%04d.pgm", prefix, g_imgCount++);
	FILE *fp = fopen(fn, "wb");
	fprintf(fp, "P5\n%d %d\n65536\n", w, h);
	for (int j = 0; j < h; ++j) {
		for (int i = 0; i < w; ++i) {
			fwrite((pixels + (j*w + i)), 1, 2, fp);
		}
	}
	fclose(fp);
}

#endif

}

/////////////////////////////////////// Texture Scaler

TextureScalerCommon::TextureScalerCommon() {
	initBicubicWeights();
}

TextureScalerCommon::~TextureScalerCommon() {
}

bool TextureScalerCommon::IsEmptyOrFlat(u32* data, int pixels, int fmt) {
	int pixelsPerWord = 4 / BytesPerPixel(fmt);
	u32 ref = data[0];
	if (pixelsPerWord > 1 && (ref & 0x0000FFFF) != (ref >> 16)) {
		return false;
	}
	for (int i = 0; i < pixels / pixelsPerWord; ++i) {
		if (data[i] != ref) return false;
	}
	return true;
}

void TextureScalerCommon::ScaleAlways(u32 *out, u32 *src, u32 &dstFmt, int &width, int &height, int factor) {
	if (IsEmptyOrFlat(src, width*height, dstFmt)) {
		// This means it was a flat texture.  Vulkan wants the size up front, so we need to make it happen.
		u32 pixel;
		// Since it's flat, one pixel is enough.  It might end up pointing to data, though.
		u32 *pixelPointer = &pixel;
		ConvertTo8888(dstFmt, src, pixelPointer, 1, 1);
		if (pixelPointer != &pixel) {
			pixel = *pixelPointer;
		}

		dstFmt = Get8888Format();
		width *= factor;
		height *= factor;

		// ABCD.  If A = D, and AB = CD, then they must all be equal (B = C, etc.)
		if ((pixel & 0x000000FF) == (pixel >> 24) && (pixel & 0x0000FFFF) == (pixel >> 16)) {
			memset(out, pixel & 0xFF, width * height * sizeof(u32));
		} else {
			// Let's hope this is vectorized.
			for (int i = 0; i < width * height; ++i) {
				out[i] = pixel;
			}
		}
	} else {
		ScaleInto(out, src, dstFmt, width, height, factor);
	}
}

bool TextureScalerCommon::ScaleInto(u32 *outputBuf, u32 *src, u32 &dstFmt, int &width, int &height, int factor) {
#ifdef SCALING_MEASURE_TIME
	double t_start = time_now_d();
#endif

	bufInput.resize(width*height); // used to store the input image image if it needs to be reformatted
	u32 *inputBuf = bufInput.data();

	// convert texture to correct format for scaling
	ConvertTo8888(dstFmt, src, inputBuf, width, height);

	// deposterize
	if (g_Config.bTexDeposterize) {
		bufDeposter.resize(width*height);
		DePosterize(inputBuf, bufDeposter.data(), width, height);
		inputBuf = bufDeposter.data();
	}

	// scale 
	switch (g_Config.iTexScalingType) {
	case XBRZ:
		ScaleXBRZ(factor, inputBuf, outputBuf, width, height);
		break;
	case HYBRID:
		ScaleHybrid(factor, inputBuf, outputBuf, width, height);
		break;
	case BICUBIC:
		ScaleBicubicMitchell(factor, inputBuf, outputBuf, width, height);
		break;
	case HYBRID_BICUBIC:
		ScaleHybrid(factor, inputBuf, outputBuf, width, height, true);
		break;
	default:
		ERROR_LOG(G3D, "Unknown scaling type: %d", g_Config.iTexScalingType);
	}

	// update values accordingly
	dstFmt = Get8888Format();
	width *= factor;
	height *= factor;

#ifdef SCALING_MEASURE_TIME
	if (width*height > 64 * 64 * factor*factor) {
		double t = time_now_d() - t_start;
		NOTICE_LOG(G3D, "TextureScaler: processed %9d pixels in %6.5lf seconds. (%9.2lf Mpixels/second)",
			width*height, t, (width*height) / (t * 1000 * 1000));
	}
#endif

	return true;
}

bool TextureScalerCommon::Scale(u32* &data, u32 &dstFmt, int &width, int &height, int factor) {
	// prevent processing empty or flat textures (this happens a lot in some games)
	// doesn't hurt the standard case, will be very quick for textures with actual texture
	if (IsEmptyOrFlat(data, width*height, dstFmt)) {
		DEBUG_LOG(G3D, "TextureScaler: early exit -- empty/flat texture");
		return false;
	}

	bufOutput.resize(width*height*factor*factor); // used to store the upscaled image
	u32 *outputBuf = bufOutput.data();

	if (ScaleInto(outputBuf, data, dstFmt, width, height, factor)) {
		data = outputBuf;
		return true;
	}
	return false;
}

const int MIN_LINES_PER_THREAD = 4;

void TextureScalerCommon::ScaleXBRZ(int factor, u32* source, u32* dest, int width, int height) {
	xbrz::ScalerCfg cfg;
	ParallelRangeLoop(&g_threadManager, std::bind(&xbrz::scale, factor, source, dest, width, height, xbrz::ColorFormat::ARGB, cfg, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
}

void TextureScalerCommon::ScaleBilinear(int factor, u32* source, u32* dest, int width, int height) {
	bufTmp1.resize(width * height * factor);
	u32 *tmpBuf = bufTmp1.data();
	ParallelRangeLoop(&g_threadManager, std::bind(&bilinearH, factor, source, tmpBuf, width, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ParallelRangeLoop(&g_threadManager, std::bind(&bilinearV, factor, tmpBuf, dest, width, 0, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
}

void TextureScalerCommon::ScaleBicubicBSpline(int factor, u32* source, u32* dest, int width, int height) {
	ParallelRangeLoop(&g_threadManager,std::bind(&scaleBicubicBSpline, factor, source, dest, width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
}

void TextureScalerCommon::ScaleBicubicMitchell(int factor, u32* source, u32* dest, int width, int height) {
	ParallelRangeLoop(&g_threadManager,std::bind(&scaleBicubicMitchell, factor, source, dest, width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
}

void TextureScalerCommon::ScaleHybrid(int factor, u32* source, u32* dest, int width, int height, bool bicubic) {
	// Basic algorithm:
	// 1) determine a feature mask C based on a sobel-ish filter + splatting, and upscale that mask bilinearly
	// 2) generate 2 scaled images: A - using Bilinear filtering, B - using xBRZ
	// 3) output = A*C + B*(1-C)

	const static int KERNEL_SPLAT[3][3] = {
			{ 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }
	};

	bufTmp1.resize(width*height);
	bufTmp2.resize(width*height*factor*factor);
	bufTmp3.resize(width*height*factor*factor);

	ParallelRangeLoop(&g_threadManager,std::bind(&generateDistanceMask, source, bufTmp1.data(), width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ParallelRangeLoop(&g_threadManager,std::bind(&convolve3x3, bufTmp1.data(), bufTmp2.data(), KERNEL_SPLAT, width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ScaleBilinear(factor, bufTmp2.data(), bufTmp3.data(), width, height);
	// mask C is now in bufTmp3

	ScaleXBRZ(factor, source, bufTmp2.data(), width, height);
	// xBRZ upscaled source is in bufTmp2

	if (bicubic) ScaleBicubicBSpline(factor, source, dest, width, height);
	else ScaleBilinear(factor, source, dest, width, height);
	// Upscaled source is in dest

	// Now we can mix it all together
	// The factor 8192 was found through practical testing on a variety of textures
	ParallelRangeLoop(&g_threadManager,std::bind(&mix, dest, bufTmp2.data(), bufTmp3.data(), 8192, width*factor, std::placeholders::_1, std::placeholders::_2), 0, height*factor, MIN_LINES_PER_THREAD);
}

void TextureScalerCommon::DePosterize(u32* source, u32* dest, int width, int height) {
	bufTmp3.resize(width*height);
	ParallelRangeLoop(&g_threadManager,std::bind(&deposterizeH, source, bufTmp3.data(), width, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ParallelRangeLoop(&g_threadManager,std::bind(&deposterizeV, bufTmp3.data(), dest, width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ParallelRangeLoop(&g_threadManager,std::bind(&deposterizeH, dest, bufTmp3.data(), width, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
	ParallelRangeLoop(&g_threadManager,std::bind(&deposterizeV, bufTmp3.data(), dest, width, height, std::placeholders::_1, std::placeholders::_2), 0, height, MIN_LINES_PER_THREAD);
}
