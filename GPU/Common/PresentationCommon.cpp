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

#include <cmath>
#include <set>
#include <cstdint>

#include "Common/GPU/thin3d.h"

#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "Core/HLE/sceDisplay.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/PresentationCommon.h"
#include "Common/GPU/ShaderTranslation.h"

struct Vertex {
	float x, y, z;
	float u, v;
	uint32_t rgba;
};

FRect GetScreenFrame(float pixelWidth, float pixelHeight) {
	FRect rc = FRect{
		0.0f,
		0.0f,
		pixelWidth,
		pixelHeight,
	};

	bool applyInset = !g_Config.bIgnoreScreenInsets;

	if (applyInset) {
		// Remove the DPI scale to get back to pixels.
		float left = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_LEFT) / g_dpi_scale_x;
		float right = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_RIGHT) / g_dpi_scale_x;
		float top = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_TOP) / g_dpi_scale_y;
		float bottom = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_BOTTOM) / g_dpi_scale_y;

		// Adjust left edge to compensate for cutouts (notches) if any.
		rc.x += left;
		rc.w -= (left + right);
		rc.y += top;
		rc.h -= (top + bottom);
	}
	return rc;
}

void CenterDisplayOutputRect(FRect *rc, float origW, float origH, const FRect &frame, int rotation) {
	float outW;
	float outH;

	bool rotated = rotation == ROTATION_LOCKED_VERTICAL || rotation == ROTATION_LOCKED_VERTICAL180;

	if (g_Config.iSmallDisplayZoomType == (int)SmallDisplayZoom::STRETCH) {
		outW = frame.w;
		outH = frame.h;
	} else {
		if (g_Config.iSmallDisplayZoomType == (int)SmallDisplayZoom::MANUAL) {
			float offsetX = (g_Config.fSmallDisplayOffsetX - 0.5f) * 2.0f * frame.w + frame.x;
			float offsetY = (g_Config.fSmallDisplayOffsetY - 0.5f) * 2.0f * frame.h + frame.y;
			// Have to invert Y for GL
			if (GetGPUBackend() == GPUBackend::OPENGL) {
				offsetY = offsetY * -1.0f;
			}
			float customZoom = g_Config.fSmallDisplayZoomLevel;
			float smallDisplayW = origW * customZoom;
			float smallDisplayH = origH * customZoom;
			if (!rotated) {
				rc->x = floorf(((frame.w - smallDisplayW) / 2.0f) + offsetX);
				rc->y = floorf(((frame.h - smallDisplayH) / 2.0f) + offsetY);
				rc->w = floorf(smallDisplayW);
				rc->h = floorf(smallDisplayH);
				return;
			} else {
				rc->x = floorf(((frame.w - smallDisplayH) / 2.0f) + offsetX);
				rc->y = floorf(((frame.h - smallDisplayW) / 2.0f) + offsetY);
				rc->w = floorf(smallDisplayH);
				rc->h = floorf(smallDisplayW);
				return;
			}
		} else if (g_Config.iSmallDisplayZoomType == (int)SmallDisplayZoom::AUTO) {
			// Stretch to 1080 for 272*4.  But don't distort if not widescreen (i.e. ultrawide of halfwide.)
			float pixelCrop = frame.h / 270.0f;
			float resCommonWidescreen = pixelCrop - floor(pixelCrop);
			if (!rotated && resCommonWidescreen == 0.0f && frame.w >= pixelCrop * 480.0f) {
				rc->x = floorf((frame.w - pixelCrop * 480.0f) * 0.5f + frame.x);
				rc->y = floorf(-pixelCrop + frame.y);
				rc->w = floorf(pixelCrop * 480.0f);
				rc->h = floorf(pixelCrop * 272.0f);
				return;
			}
		}

		float origRatio = !rotated ? origW / origH : origH / origW;
		float frameRatio = frame.w / frame.h;

		if (origRatio > frameRatio) {
			// Image is wider than frame. Center vertically.
			outW = frame.w;
			outH = frame.w / origRatio;
			// Stretch a little bit
			if (!rotated && g_Config.iSmallDisplayZoomType == (int)SmallDisplayZoom::PARTIAL_STRETCH)
				outH = (frame.h + outH) / 2.0f; // (408 + 720) / 2 = 564
		} else {
			// Image is taller than frame. Center horizontally.
			outW = frame.h * origRatio;
			outH = frame.h;
			if (rotated && g_Config.iSmallDisplayZoomType == (int)SmallDisplayZoom::PARTIAL_STRETCH)
				outW = (frame.h + outH) / 2.0f; // (408 + 720) / 2 = 564
		}
	}

	rc->x = floorf((frame.w - outW) / 2.0f + frame.x);
	rc->y = floorf((frame.h - outH) / 2.0f + frame.y);
	rc->w = floorf(outW);
	rc->h = floorf(outH);
}

PresentationCommon::PresentationCommon(Draw::DrawContext *draw) : draw_(draw) {
	CreateDeviceObjects();
}

PresentationCommon::~PresentationCommon() {
	DestroyDeviceObjects();
}

void PresentationCommon::GetCardboardSettings(CardboardSettings *cardboardSettings) {
	if (!g_Config.bEnableCardboardVR) {
		cardboardSettings->enabled = false;
		return;
	}
	// Calculate Cardboard Settings
	float cardboardScreenScale = g_Config.iCardboardScreenSize / 100.0f;
	float cardboardScreenWidth = pixelWidth_ / 2.0f * cardboardScreenScale;
	float cardboardScreenHeight = pixelHeight_ / 2.0f * cardboardScreenScale;
	float cardboardMaxXShift = (pixelWidth_ / 2.0f - cardboardScreenWidth) / 2.0f;
	float cardboardUserXShift = g_Config.iCardboardXShift / 100.0f * cardboardMaxXShift;
	float cardboardLeftEyeX = cardboardMaxXShift + cardboardUserXShift;
	float cardboardRightEyeX = pixelWidth_ / 2.0f + cardboardMaxXShift - cardboardUserXShift;
	float cardboardMaxYShift = pixelHeight_ / 2.0f - cardboardScreenHeight / 2.0f;
	float cardboardUserYShift = g_Config.iCardboardYShift / 100.0f * cardboardMaxYShift;
	float cardboardScreenY = cardboardMaxYShift + cardboardUserYShift;

	cardboardSettings->enabled = true;
	cardboardSettings->leftEyeXPosition = cardboardLeftEyeX;
	cardboardSettings->rightEyeXPosition = cardboardRightEyeX;
	cardboardSettings->screenYPosition = cardboardScreenY;
	cardboardSettings->screenWidth = cardboardScreenWidth;
	cardboardSettings->screenHeight = cardboardScreenHeight;
}

void PresentationCommon::CalculatePostShaderUniforms(int bufferWidth, int bufferHeight, int targetWidth, int targetHeight, const ShaderInfo *shaderInfo, PostShaderUniforms *uniforms) {
	float u_delta = 1.0f / bufferWidth;
	float v_delta = 1.0f / bufferHeight;
	float u_pixel_delta = 1.0f / targetWidth;
	float v_pixel_delta = 1.0f / targetHeight;
	int flipCount = __DisplayGetFlipCount();
	int vCount = __DisplayGetVCount();
	float time[4] = { (float)time_now_d(), (vCount % 60) * 1.0f / 60.0f, (float)vCount, (float)(flipCount % 60) };

	uniforms->texelDelta[0] = u_delta;
	uniforms->texelDelta[1] = v_delta;
	uniforms->pixelDelta[0] = u_pixel_delta;
	uniforms->pixelDelta[1] = v_pixel_delta;
	memcpy(uniforms->time, time, 4 * sizeof(float));
	uniforms->timeDelta[0] = time[0] - previousUniforms_.time[0];
	uniforms->timeDelta[1] = (time[2] - previousUniforms_.time[2]) * (1.0f / 60.0f);
	uniforms->timeDelta[2] = time[2] - previousUniforms_.time[2];
	uniforms->timeDelta[3] = time[3] != previousUniforms_.time[3] ? 1.0f : 0.0f;
	uniforms->video = hasVideo_ ? 1.0f : 0.0f;

	// The shader translator tacks this onto our shaders, if we don't set it they render garbage.
	uniforms->gl_HalfPixel[0] = u_pixel_delta * 0.5f;
	uniforms->gl_HalfPixel[1] = v_pixel_delta * 0.5f;

	uniforms->setting[0] = g_Config.mPostShaderSetting[shaderInfo->section + "SettingValue1"];;
	uniforms->setting[1] = g_Config.mPostShaderSetting[shaderInfo->section + "SettingValue2"];
	uniforms->setting[2] = g_Config.mPostShaderSetting[shaderInfo->section + "SettingValue3"];
	uniforms->setting[3] = g_Config.mPostShaderSetting[shaderInfo->section + "SettingValue4"];
}

static std::string ReadShaderSrc(const Path &filename) {
	size_t sz = 0;
	char *data = (char *)VFSReadFile(filename.c_str(), &sz);
	if (!data) {
		return "";
	}

	std::string src(data, sz);
	delete[] data;
	return src;
}

// Note: called on resize and settings changes.
bool PresentationCommon::UpdatePostShader() {
	std::vector<const ShaderInfo *> shaderInfo;
	if (!g_Config.vPostShaderNames.empty()) {
		ReloadAllPostShaderInfo();
		shaderInfo = GetFullPostShadersChain(g_Config.vPostShaderNames);
	}

	DestroyPostShader();
	if (shaderInfo.empty())
		return false;

	bool usePreviousFrame = false;
	bool usePreviousAtOutputResolution = false;
	for (int i = 0; i < shaderInfo.size(); ++i) {
		const ShaderInfo *next = i + 1 < shaderInfo.size() ? shaderInfo[i + 1] : nullptr;
		if (!BuildPostShader(shaderInfo[i], next)) {
			DestroyPostShader();
			return false;
		}
		if (shaderInfo[i]->usePreviousFrame) {
			usePreviousFrame = true;
			usePreviousAtOutputResolution = shaderInfo[i]->outputResolution;
		}
	}

	if (usePreviousFrame) {
		int w = usePreviousAtOutputResolution ? pixelWidth_ : renderWidth_;
		int h = usePreviousAtOutputResolution ? pixelHeight_ : renderHeight_;

		static constexpr int FRAMES = 2;
		previousFramebuffers_.resize(FRAMES);
		previousIndex_ = 0;

		for (int i = 0; i < FRAMES; ++i) {
			previousFramebuffers_[i] = draw_->CreateFramebuffer({ w, h, 1, 1, false, "inter_presentation" });
			if (!previousFramebuffers_[i]) {
				DestroyPostShader();
				return false;
			}
		}
	}

	usePostShader_ = true;
	return true;
}

bool PresentationCommon::BuildPostShader(const ShaderInfo *shaderInfo, const ShaderInfo *next) {
	std::string vsSourceGLSL = ReadShaderSrc(shaderInfo->vertexShaderFile);
	std::string fsSourceGLSL = ReadShaderSrc(shaderInfo->fragmentShaderFile);
	if (vsSourceGLSL.empty() || fsSourceGLSL.empty()) {
		return false;
	}

	std::string vsError;
	std::string fsError;

	// All post shaders are written in GLSL 1.0 so that's what we pass in here as a "from" language.
	Draw::ShaderModule *vs = CompileShaderModule(ShaderStage::Vertex, GLSL_1xx, vsSourceGLSL, &vsError);
	Draw::ShaderModule *fs = CompileShaderModule(ShaderStage::Fragment, GLSL_1xx, fsSourceGLSL, &fsError);

	// Don't worry, CompileShaderModule makes sure they get freed if one succeeded.
	if (!fs || !vs) {
		std::string errorString = vsError + "\n" + fsError;
		// DO NOT turn this into an ERROR_LOG_REPORT, as it will pollute our logs with all kinds of
		// user shader experiments.
		ERROR_LOG(FRAMEBUF, "Failed to build post-processing program from %s and %s!\n%s", shaderInfo->vertexShaderFile.c_str(), shaderInfo->fragmentShaderFile.c_str(), errorString.c_str());
		ShowPostShaderError(errorString);
		return false;
	}

	UniformBufferDesc postShaderDesc{ sizeof(PostShaderUniforms), {
		{ "gl_HalfPixel", 0, -1, UniformType::FLOAT4, offsetof(PostShaderUniforms, gl_HalfPixel) },
		{ "u_texelDelta", 1, 1, UniformType::FLOAT2, offsetof(PostShaderUniforms, texelDelta) },
		{ "u_pixelDelta", 2, 2, UniformType::FLOAT2, offsetof(PostShaderUniforms, pixelDelta) },
		{ "u_time", 3, 3, UniformType::FLOAT4, offsetof(PostShaderUniforms, time) },
		{ "u_timeDelta", 4, 4, UniformType::FLOAT4, offsetof(PostShaderUniforms, timeDelta) },
		{ "u_setting", 5, 5, UniformType::FLOAT4, offsetof(PostShaderUniforms, setting) },
		{ "u_video", 6, 6, UniformType::FLOAT1, offsetof(PostShaderUniforms, video) },
	} };

	Draw::Pipeline *pipeline = CreatePipeline({ vs, fs }, true, &postShaderDesc);
	if (!pipeline)
		return false;

	if (!shaderInfo->outputResolution || next) {
		int nextWidth = renderWidth_;
		int nextHeight = renderHeight_;

		// When chaining, we use the previous resolution as a base, rather than the render resolution.
		if (!postShaderFramebuffers_.empty())
			draw_->GetFramebufferDimensions(postShaderFramebuffers_.back(), &nextWidth, &nextHeight);

		if (next && next->isUpscalingFilter) {
			// Force 1x for this shader, so the next can upscale.
			const bool isPortrait = g_Config.IsPortrait();
			nextWidth = isPortrait ? 272 : 480;
			nextHeight = isPortrait ? 480 : 272;
		} else if (next && next->SSAAFilterLevel >= 2) {
			// Increase the resolution this shader outputs for the next to SSAA.
			nextWidth *= next->SSAAFilterLevel;
			nextHeight *= next->SSAAFilterLevel;
		} else if (shaderInfo->outputResolution) {
			// If the current shader uses output res (not next), we will use output res for it.
			FRect rc;
			FRect frame = GetScreenFrame((float)pixelWidth_, (float)pixelHeight_);
			CenterDisplayOutputRect(&rc, 480.0f, 272.0f, frame, g_Config.iInternalScreenRotation);
			nextWidth = (int)rc.w;
			nextHeight = (int)rc.h;
		}

		if (!AllocateFramebuffer(nextWidth, nextHeight)) {
			pipeline->Release();
			return false;
		}
	}

	postShaderPipelines_.push_back(pipeline);
	postShaderInfo_.push_back(*shaderInfo);
	return true;
}

bool PresentationCommon::AllocateFramebuffer(int w, int h) {
	using namespace Draw;

	// First, let's try to find a framebuffer of the right size that is NOT the most recent.
	Framebuffer *last = postShaderFramebuffers_.empty() ? nullptr : postShaderFramebuffers_.back();
	for (const auto &prev : postShaderFBOUsage_) {
		if (prev.w == w && prev.h == h && prev.fbo != last) {
			// Great, this one's perfect.  Ref it for when we release.
			prev.fbo->AddRef();
			postShaderFramebuffers_.push_back(prev.fbo);
			return true;
		}
	}

	// No depth/stencil for post processing
	Draw::Framebuffer *fbo = draw_->CreateFramebuffer({ w, h, 1, 1, false, "presentation" });
	if (!fbo) {
		return false;
	}

	postShaderFBOUsage_.push_back({ fbo, w, h });
	postShaderFramebuffers_.push_back(fbo);
	return true;
}

void PresentationCommon::ShowPostShaderError(const std::string &errorString) {
	// let's show the first line of the error string as an OSM.
	std::set<std::string> blacklistedLines;
	// These aren't useful to show, skip to the first interesting line.
	blacklistedLines.insert("Fragment shader failed to compile with the following errors:");
	blacklistedLines.insert("Vertex shader failed to compile with the following errors:");
	blacklistedLines.insert("Compile failed.");
	blacklistedLines.insert("");

	std::string firstLine;
	size_t start = 0;
	for (size_t i = 0; i < errorString.size(); i++) {
		if (errorString[i] == '\n' && i == start) {
			start = i + 1;
		} else if (errorString[i] == '\n') {
			firstLine = errorString.substr(start, i - start);
			if (blacklistedLines.find(firstLine) == blacklistedLines.end()) {
				break;
			}
			start = i + 1;
			firstLine.clear();
		}
	}
	if (!firstLine.empty()) {
		host->NotifyUserMessage("Post-shader error: " + firstLine + "...:\n" + errorString, 10.0f, 0xFF3090FF);
	} else {
		host->NotifyUserMessage("Post-shader error, see log for details", 10.0f, 0xFF3090FF);
	}
}

void PresentationCommon::DeviceLost() {
	DestroyDeviceObjects();
}

void PresentationCommon::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	CreateDeviceObjects();
}

Draw::Pipeline *PresentationCommon::CreatePipeline(std::vector<Draw::ShaderModule *> shaders, bool postShader, const UniformBufferDesc *uniformDesc) {
	using namespace Draw;

	Semantic pos = SEM_POSITION;
	Semantic tc = SEM_TEXCOORD0;
	// Shader translation marks these both as "TEXCOORDs" on HLSL...
	if (postShader && (lang_ == HLSL_D3D11 || lang_ == HLSL_D3D9)) {
		pos = SEM_TEXCOORD0;
		tc = SEM_TEXCOORD1;
	}

	// TODO: Maybe get rid of color0.
	InputLayoutDesc inputDesc = {
		{
			{ sizeof(Vertex), false },
		},
		{
			{ 0, pos, DataFormat::R32G32B32_FLOAT, 0 },
			{ 0, tc, DataFormat::R32G32_FLOAT, 12 },
			{ 0, SEM_COLOR0, DataFormat::R8G8B8A8_UNORM, 20 },
		},
	};

	InputLayout *inputLayout = draw_->CreateInputLayout(inputDesc);
	DepthStencilState *depth = draw_->CreateDepthStencilState({ false, false, Comparison::LESS });
	BlendState *blendstateOff = draw_->CreateBlendState({ false, 0xF });
	RasterState *rasterNoCull = draw_->CreateRasterState({});

	PipelineDesc pipelineDesc{ Primitive::TRIANGLE_LIST, shaders, inputLayout, depth, blendstateOff, rasterNoCull, uniformDesc };
	Pipeline *pipeline = draw_->CreateGraphicsPipeline(pipelineDesc);

	inputLayout->Release();
	depth->Release();
	blendstateOff->Release();
	rasterNoCull->Release();

	return pipeline;
}

void PresentationCommon::CreateDeviceObjects() {
	using namespace Draw;
	_assert_(vdata_ == nullptr);

	vdata_ = draw_->CreateBuffer(sizeof(Vertex) * 8, BufferUsageFlag::DYNAMIC | BufferUsageFlag::VERTEXDATA);

	// TODO: Use a triangle strip? Makes the UV rotation slightly more complex.
	idata_ = draw_->CreateBuffer(sizeof(uint16_t) * 6, BufferUsageFlag::DYNAMIC | BufferUsageFlag::INDEXDATA);
	uint16_t indexes[] = { 0, 1, 2, 0, 2, 3 };
	draw_->UpdateBuffer(idata_, (const uint8_t *)indexes, 0, sizeof(indexes), Draw::UPDATE_DISCARD);

	samplerNearest_ = draw_->CreateSamplerState({ TextureFilter::NEAREST, TextureFilter::NEAREST, TextureFilter::NEAREST, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });
	samplerLinear_ = draw_->CreateSamplerState({ TextureFilter::LINEAR, TextureFilter::LINEAR, TextureFilter::LINEAR, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });

	texColor_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D) }, false, &vsTexColBufDesc);
	texColorRBSwizzle_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D_RB_SWIZZLE) }, false, &vsTexColBufDesc);

	if (restorePostShader_)
		UpdatePostShader();
	restorePostShader_ = false;
}

template <typename T>
static void DoRelease(T *&obj) {
	if (obj)
		obj->Release();
	obj = nullptr;
}

template <typename T>
static void DoReleaseVector(std::vector<T *> &list) {
	for (auto &obj : list)
		obj->Release();
	list.clear();
}

void PresentationCommon::DestroyDeviceObjects() {
	DoRelease(texColor_);
	DoRelease(texColorRBSwizzle_);
	DoRelease(samplerNearest_);
	DoRelease(samplerLinear_);
	DoRelease(vdata_);
	DoRelease(idata_);
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	restorePostShader_ = usePostShader_;
	DestroyPostShader();
}

void PresentationCommon::DestroyPostShader() {
	usePostShader_ = false;

	DoReleaseVector(postShaderModules_);
	DoReleaseVector(postShaderPipelines_);
	DoReleaseVector(postShaderFramebuffers_);
	DoReleaseVector(previousFramebuffers_);
	postShaderInfo_.clear();
	postShaderFBOUsage_.clear();
}

Draw::ShaderModule *PresentationCommon::CompileShaderModule(ShaderStage stage, ShaderLanguage lang, const std::string &src, std::string *errorString) {
	std::string translated = src;
	if (lang != lang_) {
		// Gonna have to upconvert the shader.
		if (!TranslateShader(&translated, lang_, draw_->GetShaderLanguageDesc(), nullptr, src, lang, stage, errorString)) {
			ERROR_LOG(FRAMEBUF, "Failed to translate post-shader. Error string: '%s'\nSource code:\n%s\n", errorString->c_str(), src.c_str());
			return nullptr;
		}
	}

	Draw::ShaderModule *shader = draw_->CreateShaderModule(stage, lang_, (const uint8_t *)translated.c_str(), translated.size(), "postshader");
	if (shader)
		postShaderModules_.push_back(shader);
	return shader;
}

void PresentationCommon::SourceTexture(Draw::Texture *texture, int bufferWidth, int bufferHeight) {
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	texture->AddRef();
	srcTexture_ = texture;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

void PresentationCommon::SourceFramebuffer(Draw::Framebuffer *fb, int bufferWidth, int bufferHeight) {
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	fb->AddRef();
	srcFramebuffer_ = fb;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

void PresentationCommon::BindSource(int binding) {
	if (srcTexture_) {
		draw_->BindTexture(binding, srcTexture_);
	} else if (srcFramebuffer_) {
		draw_->BindFramebufferAsTexture(srcFramebuffer_, binding, Draw::FB_COLOR_BIT, 0);
	} else {
		_assert_(false);
	}
}

void PresentationCommon::UpdateUniforms(bool hasVideo) {
	hasVideo_ = hasVideo;
}

void PresentationCommon::CopyToOutput(OutputFlags flags, int uvRotation, float u0, float v0, float u1, float v1) {
	draw_->InvalidateCachedState();

	// TODO: If shader objects have been created by now, we might have received errors.
	// GLES can have the shader fail later, shader->failed / shader->error.
	// This should auto-disable usePostShader_ and call ShowPostShaderError().

	bool useNearest = flags & OutputFlags::NEAREST;
	const bool usePostShader = usePostShader_ && !(flags & OutputFlags::RB_SWIZZLE);
	const bool isFinalAtOutputResolution = usePostShader && postShaderFramebuffers_.size() < postShaderPipelines_.size();
	Draw::Framebuffer *postShaderOutput = nullptr;
	int lastWidth = srcWidth_;
	int lastHeight = srcHeight_;

	// These are the output coordinates.
	FRect frame = GetScreenFrame((float)pixelWidth_, (float)pixelHeight_);
	FRect rc;
	CenterDisplayOutputRect(&rc, 480.0f, 272.0f, frame, uvRotation);

	if (GetGPUBackend() == GPUBackend::DIRECT3D9) {
		rc.x -= 0.5f;
		// This is plus because the top is larger y.
		rc.y += 0.5f;
	}

	if ((flags & OutputFlags::BACKBUFFER_FLIPPED) || (flags & OutputFlags::POSITION_FLIPPED)) {
		std::swap(v0, v1);
	}

	// To make buffer updates easier, we use one array of verts.
	int postVertsOffset = (int)sizeof(Vertex) * 4;
	Vertex verts[8] = {
		{ rc.x, rc.y, 0, u0, v0, 0xFFFFFFFF }, // TL
		{ rc.x, rc.y + rc.h, 0, u0, v1, 0xFFFFFFFF }, // BL
		{ rc.x + rc.w, rc.y + rc.h, 0, u1, v1, 0xFFFFFFFF }, // BR
		{ rc.x + rc.w, rc.y, 0, u1, v0, 0xFFFFFFFF }, // TR
	};

	float invDestW = 1.0f / (pixelWidth_ * 0.5f);
	float invDestH = 1.0f / (pixelHeight_ * 0.5f);
	for (int i = 0; i < 4; i++) {
		verts[i].x = verts[i].x * invDestW - 1.0f;
		verts[i].y = verts[i].y * invDestH - 1.0f;
	}

	if (uvRotation != ROTATION_LOCKED_HORIZONTAL) {
		struct {
			float u;
			float v;
		} temp[4];
		int rotation = 0;
		// Vertical and Vertical180 needed swapping after we changed the coordinate system.
		switch (uvRotation) {
		case ROTATION_LOCKED_HORIZONTAL180: rotation = 2; break;
		case ROTATION_LOCKED_VERTICAL: rotation = 3; break;
		case ROTATION_LOCKED_VERTICAL180: rotation = 1; break;
		}

		// If we flipped, we rotate the other way.
		if ((flags & OutputFlags::BACKBUFFER_FLIPPED) || (flags & OutputFlags::POSITION_FLIPPED)) {
			if ((rotation & 1) != 0)
				rotation ^= 2;
		}

		for (int i = 0; i < 4; i++) {
			temp[i].u = verts[(i + rotation) & 3].u;
			temp[i].v = verts[(i + rotation) & 3].v;
		}
		for (int i = 0; i < 4; i++) {
			verts[i].u = temp[i].u;
			verts[i].v = temp[i].v;
		}
	}

	if (isFinalAtOutputResolution) {
		// In this mode, we ignore the g_display_rot_matrix.  Apply manually.
		if (g_display_rotation != DisplayRotation::ROTATE_0) {
			for (int i = 0; i < 4; i++) {
				Lin::Vec3 v(verts[i].x, verts[i].y, verts[i].z);
				// Backwards notation, should fix that...
				v = v * g_display_rot_matrix;
				verts[i].x = v.x;
				verts[i].y = v.y;
			}
		}
	}

	if (flags & OutputFlags::PILLARBOX) {
		for (int i = 0; i < 4; i++) {
			// Looks about right.
			verts[i].x *= 0.75f;
		}
	}

	// Grab the previous framebuffer early so we can change previousIndex_ when we want.
	Draw::Framebuffer *previousFramebuffer = previousFramebuffers_.empty() ? nullptr : previousFramebuffers_[previousIndex_];

	PostShaderUniforms uniforms;
	const auto performShaderPass = [&](const ShaderInfo *shaderInfo, Draw::Framebuffer *postShaderFramebuffer, Draw::Pipeline *postShaderPipeline) {
		if (postShaderOutput) {
			draw_->BindFramebufferAsTexture(postShaderOutput, 0, Draw::FB_COLOR_BIT, 0);
		} else {
			BindSource(0);
		}
		BindSource(1);
		if (shaderInfo->usePreviousFrame)
			draw_->BindFramebufferAsTexture(previousFramebuffer, 2, Draw::FB_COLOR_BIT, 0);

		int nextWidth, nextHeight;
		draw_->GetFramebufferDimensions(postShaderFramebuffer, &nextWidth, &nextHeight);
		Draw::Viewport viewport{ 0, 0, (float)nextWidth, (float)nextHeight, 0.0f, 1.0f };
		draw_->SetViewports(1, &viewport);
		draw_->SetScissorRect(0, 0, nextWidth, nextHeight);

		CalculatePostShaderUniforms(lastWidth, lastHeight, nextWidth, nextHeight, shaderInfo, &uniforms);

		draw_->BindPipeline(postShaderPipeline);
		draw_->UpdateDynamicUniformBuffer(&uniforms, sizeof(uniforms));

		Draw::SamplerState *sampler = useNearest || shaderInfo->isUpscalingFilter ? samplerNearest_ : samplerLinear_;
		draw_->BindSamplerStates(0, 1, &sampler);
		draw_->BindSamplerStates(1, 1, &sampler);
		if (shaderInfo->usePreviousFrame)
			draw_->BindSamplerStates(2, 1, &sampler);

		draw_->BindVertexBuffers(0, 1, &vdata_, &postVertsOffset);
		draw_->BindIndexBuffer(idata_, 0);
		draw_->DrawIndexed(6, 0);
		draw_->BindIndexBuffer(nullptr, 0);

		postShaderOutput = postShaderFramebuffer;
		lastWidth = nextWidth;
		lastHeight = nextHeight;
	};

	if (usePostShader) {
		bool flipped = flags & OutputFlags::POSITION_FLIPPED;
		float post_v0 = !flipped ? 1.0f : 0.0f;
		float post_v1 = !flipped ? 0.0f : 1.0f;
		verts[4] = { -1, -1, 0, 0, post_v1, 0xFFFFFFFF }; // TL
		verts[5] = { -1, 1, 0, 0, post_v0, 0xFFFFFFFF }; // BL
		verts[6] = { 1, 1, 0, 1, post_v0, 0xFFFFFFFF }; // BR
		verts[7] = { 1, -1, 0, 1, post_v1, 0xFFFFFFFF }; // TR
		draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, sizeof(verts), Draw::UPDATE_DISCARD);

		for (size_t i = 0; i < postShaderFramebuffers_.size(); ++i) {
			Draw::Pipeline *postShaderPipeline = postShaderPipelines_[i];
			const ShaderInfo *shaderInfo = &postShaderInfo_[i];
			Draw::Framebuffer *postShaderFramebuffer = postShaderFramebuffers_[i];
			if (!isFinalAtOutputResolution && i == postShaderFramebuffers_.size() - 1 && !previousFramebuffers_.empty()) {
				// This is the last pass and we're going direct to the backbuffer after this.
				// Redirect output to a separate framebuffer to keep the previous frame.
				previousIndex_++;
				if (previousIndex_ >= previousFramebuffers_.size())
					previousIndex_ = 0;
				postShaderFramebuffer = previousFramebuffers_[previousIndex_];
			}

			draw_->BindFramebufferAsRenderTarget(postShaderFramebuffer, { Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "PostShader");
			performShaderPass(shaderInfo, postShaderFramebuffer, postShaderPipeline);
		}

		if (isFinalAtOutputResolution && postShaderInfo_.back().isUpscalingFilter)
			useNearest = true;
	} else {
		draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, postVertsOffset, Draw::UPDATE_DISCARD);
	}

	// If we need to save the previous frame, we have to save any final pass in a framebuffer.
	if (isFinalAtOutputResolution && !previousFramebuffers_.empty()) {
		Draw::Pipeline *postShaderPipeline = postShaderPipelines_.back();
		const ShaderInfo *shaderInfo = &postShaderInfo_.back();

		// Pick the next to render to.
		previousIndex_++;
		if (previousIndex_ >= previousFramebuffers_.size())
			previousIndex_ = 0;
		Draw::Framebuffer *postShaderFramebuffer = previousFramebuffers_[previousIndex_];

		draw_->BindFramebufferAsRenderTarget(postShaderFramebuffer, { Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "InterFrameBlit");
		performShaderPass(shaderInfo, postShaderFramebuffer, postShaderPipeline);
	}

	Draw::Pipeline *pipeline = flags & OutputFlags::RB_SWIZZLE ? texColorRBSwizzle_ : texColor_;
	if (isFinalAtOutputResolution && previousFramebuffers_.empty()) {
		pipeline = postShaderPipelines_.back();
	}

	draw_->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "FinalBlit");
	draw_->SetScissorRect(0, 0, pixelWidth_, pixelHeight_);

	draw_->BindPipeline(pipeline);

	if (postShaderOutput) {
		draw_->BindFramebufferAsTexture(postShaderOutput, 0, Draw::FB_COLOR_BIT, 0);
	} else {
		BindSource(0);
	}
	BindSource(1);

	if (isFinalAtOutputResolution && previousFramebuffers_.empty()) {
		CalculatePostShaderUniforms(lastWidth, lastHeight, (int)rc.w, (int)rc.h, &postShaderInfo_.back(), &uniforms);
		draw_->UpdateDynamicUniformBuffer(&uniforms, sizeof(uniforms));
	} else {
		Draw::VsTexColUB ub{};
		memcpy(ub.WorldViewProj, g_display_rot_matrix.m, sizeof(float) * 16);
		draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	}

	draw_->BindVertexBuffers(0, 1, &vdata_, nullptr);
	draw_->BindIndexBuffer(idata_, 0);

	Draw::SamplerState *sampler = useNearest ? samplerNearest_ : samplerLinear_;
	draw_->BindSamplerStates(0, 1, &sampler);
	draw_->BindSamplerStates(1, 1, &sampler);

	auto setViewport = [&](float x, float y, float w, float h) {
		Draw::Viewport viewport{ x, y, w, h, 0.0f, 1.0f };
		draw_->SetViewports(1, &viewport);
	};

	CardboardSettings cardboardSettings;
	GetCardboardSettings(&cardboardSettings);
	if (cardboardSettings.enabled) {
		// This is what the left eye sees.
		setViewport(cardboardSettings.leftEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->DrawIndexed(6, 0);

		// And this is the right eye, unless they're a pirate.
		setViewport(cardboardSettings.rightEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->DrawIndexed(6, 0);
	} else {
		setViewport(0.0f, 0.0f, (float)pixelWidth_, (float)pixelHeight_);
		draw_->DrawIndexed(6, 0);
	}

	draw_->BindIndexBuffer(nullptr, 0);

	DoRelease(srcFramebuffer_);
	DoRelease(srcTexture_);

	// Unbinds all textures and samplers too, needed since sometimes a MakePixelTexture is deleted etc.
	draw_->BindPipeline(nullptr);

	previousUniforms_ = uniforms;
}

void PresentationCommon::CalculateRenderResolution(int *width, int *height, int *scaleFactor, bool *upscaling, bool *ssaa) {
	// Check if postprocessing shader is doing upscaling as it requires native resolution
	std::vector<const ShaderInfo *> shaderInfo;
	if (!g_Config.vPostShaderNames.empty()) {
		ReloadAllPostShaderInfo();
		shaderInfo = GetFullPostShadersChain(g_Config.vPostShaderNames);
	}

	bool firstIsUpscalingFilter = shaderInfo.empty() ? false : shaderInfo.front()->isUpscalingFilter;
	int firstSSAAFilterLevel = shaderInfo.empty() ? 0 : shaderInfo.front()->SSAAFilterLevel;

	// Actually, auto mode should be more granular...
	// Round up to a zoom factor for the render size.
	int zoom = g_Config.iInternalResolution;
	if (zoom == 0 || firstSSAAFilterLevel >= 2) {
		// auto mode, use the longest dimension
		if (!g_Config.IsPortrait()) {
			zoom = (PSP_CoreParameter().pixelWidth + 479) / 480;
		} else {
			zoom = (PSP_CoreParameter().pixelHeight + 479) / 480;
		}
		if (firstSSAAFilterLevel >= 2)
			zoom *= firstSSAAFilterLevel;
	}
	if (zoom <= 1 || firstIsUpscalingFilter)
		zoom = 1;

	if (upscaling) {
		*upscaling = firstIsUpscalingFilter;
		for (auto &info : shaderInfo) {
			*upscaling = *upscaling || info->isUpscalingFilter;
		}
	}
	if (ssaa) {
		*ssaa = firstSSAAFilterLevel >= 2;
		for (auto &info : shaderInfo) {
			*ssaa = *ssaa || info->SSAAFilterLevel >= 2;
		}
	}

	if (g_Config.IsPortrait()) {
		*width = 272 * zoom;
		*height = 480 * zoom;
	} else {
		*width = 480 * zoom;
		*height = 272 * zoom;
	}

	*scaleFactor = zoom;
}
