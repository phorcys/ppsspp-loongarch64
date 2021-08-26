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
#include <cstring>

#include "ext/xxhash.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Math/math_util.h"
#include "Common/Profiler/Profiler.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"

#include "Common/Data/Convert/ColorConv.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/Host.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "Core/System.h"

#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanImage.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Vulkan/VulkanContext.h"
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/FramebufferManagerVulkan.h"
#include "GPU/Vulkan/DepalettizeShaderVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/DrawEngineVulkan.h"

using namespace PPSSPP_VK;

#define TEXCACHE_MIN_SLAB_SIZE (8 * 1024 * 1024)
#define TEXCACHE_MAX_SLAB_SIZE (32 * 1024 * 1024)
#define TEXCACHE_SLAB_PRESSURE 4

const char *copyShader = R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

// No idea what's optimal here...
#define WORKGROUP_SIZE 16
layout (local_size_x = WORKGROUP_SIZE, local_size_y = WORKGROUP_SIZE, local_size_z = 1) in;

layout(std430, binding = 1) buffer Buf1 {
	uint data[];
} buf1;

layout(std430, binding = 2) buffer Buf2 {
	uint data[];
} buf2;

layout(push_constant) uniform Params {
	int width;
	int height;
	int scale;
	int fmt;
} params;

uint readColoru(uvec2 p) {
	// Note that if the pixels are packed, we can do multiple stores
	// and only launch this compute shader for every N pixels,
	// by slicing the width in half and multiplying x by 2, for example.
	if (params.fmt == 0) {
		return buf1.data[p.y * params.width + p.x];
	} else {
		uint offset = p.y * params.width + p.x;
		uint data = buf1.data[offset / 2];
		if ((offset & 1) != 0) {
			data = data >> 16;
		}
		if (params.fmt == 6) {
			uint r = ((data << 3) & 0xF8) | ((data >> 2) & 0x07);
			uint g = ((data >> 3) & 0xFC) | ((data >> 9) & 0x03);
			uint b = ((data >> 8) & 0xF8) | ((data >> 13) & 0x07);
			return 0xFF000000 | (b << 16) | (g << 8) | r;
		} else if (params.fmt == 5) {
			uint r = ((data << 3) & 0xF8) | ((data >> 2) & 0x07);
			uint g = ((data >> 2) & 0xF8) | ((data >> 7) & 0x07);
			uint b = ((data >> 7) & 0xF8) | ((data >> 12) & 0x07);
			uint a = ((data >> 15) & 0x01) == 0 ? 0x00 : 0xFF;
			return (a << 24) | (b << 16) | (g << 8) | r;
		} else if (params.fmt == 4) {
			uint r = (data & 0x0F) | ((data << 4) & 0xF0);
			uint g = (data & 0xF0) | ((data >> 4) & 0x0F);
			uint b = ((data >> 8) & 0x0F) | ((data >> 4) & 0xF0);
			uint a = ((data >> 12) & 0x0F) | ((data >> 8) & 0xF0);
			return (a << 24) | (b << 16) | (g << 8) | r;
		}
	}
}

vec4 readColorf(uvec2 p) {
	return unpackUnorm4x8(readColoru(p));
}

%s

void main() {
	uvec2 xy = gl_GlobalInvocationID.xy;
	// Kill off any out-of-image threads to avoid stray writes.
	// Should only happen on the tiniest mipmaps as PSP textures are power-of-2,
	// and we use a 16x16 workgroup size.
	if (xy.x >= params.width || xy.y >= params.height)
		return;

	uvec2 origxy = xy / params.scale;
	if (params.scale == 1) {
		buf2.data[xy.y * params.width + xy.x] = readColoru(origxy);
	} else {
		buf2.data[xy.y * params.width + xy.x] = applyScalingu(origxy, xy);
	}
}
)";

const char *uploadShader = R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

// No idea what's optimal here...
#define WORKGROUP_SIZE 16
layout (local_size_x = WORKGROUP_SIZE, local_size_y = WORKGROUP_SIZE, local_size_z = 1) in;

uniform layout(binding = 0, rgba8) writeonly image2D img;

layout(std430, binding = 1) buffer Buf {
	uint data[];
} buf;

layout(push_constant) uniform Params {
	int width;
	int height;
	int scale;
	int fmt;
} params;

uint readColoru(uvec2 p) {
	// Note that if the pixels are packed, we can do multiple stores
	// and only launch this compute shader for every N pixels,
	// by slicing the width in half and multiplying x by 2, for example.
	if (params.fmt == 0) {
		return buf.data[p.y * params.width + p.x];
	} else {
		uint offset = p.y * params.width + p.x;
		uint data = buf.data[offset / 2];
		if ((offset & 1) != 0) {
			data = data >> 16;
		}
		if (params.fmt == 6) {
			uint r = ((data << 3) & 0xF8) | ((data >> 2) & 0x07);
			uint g = ((data >> 3) & 0xFC) | ((data >> 9) & 0x03);
			uint b = ((data >> 8) & 0xF8) | ((data >> 13) & 0x07);
			return 0xFF000000 | (b << 16) | (g << 8) | r;
		} else if (params.fmt == 5) {
			uint r = ((data << 3) & 0xF8) | ((data >> 2) & 0x07);
			uint g = ((data >> 2) & 0xF8) | ((data >> 7) & 0x07);
			uint b = ((data >> 7) & 0xF8) | ((data >> 12) & 0x07);
			uint a = ((data >> 15) & 0x01) == 0 ? 0x00 : 0xFF;
			return (a << 24) | (b << 16) | (g << 8) | r;
		} else if (params.fmt == 4) {
			uint r = (data & 0x0F) | ((data << 4) & 0xF0);
			uint g = (data & 0xF0) | ((data >> 4) & 0x0F);
			uint b = ((data >> 8) & 0x0F) | ((data >> 4) & 0xF0);
			uint a = ((data >> 12) & 0x0F) | ((data >> 8) & 0xF0);
			return (a << 24) | (b << 16) | (g << 8) | r;
		}
	}
}

vec4 readColorf(uvec2 p) {
	// Unpack the color (we could look it up in a CLUT here if we wanted...)
	// It's a bit silly that we need to unpack to float and then have imageStore repack,
	// but the alternative is to store to a buffer, and then launch a vkCmdCopyBufferToImage instead.
	return unpackUnorm4x8(readColoru(p));
}

%s

void main() {
	uvec2 xy = gl_GlobalInvocationID.xy;
	// Kill off any out-of-image threads to avoid stray writes.
	// Should only happen on the tiniest mipmaps as PSP textures are power-of-2,
	// and we use a 16x16 workgroup size.
	if (xy.x >= params.width || xy.y >= params.height)
		return;

	uvec2 origxy = xy / params.scale;
	if (params.scale == 1) {
		imageStore(img, ivec2(xy.x, xy.y), readColorf(origxy));
	} else {
		imageStore(img, ivec2(xy.x, xy.y), applyScalingf(origxy, xy));
	}
}
)";

SamplerCache::~SamplerCache() {
	DeviceLost();
}

VkSampler SamplerCache::GetOrCreateSampler(const SamplerCacheKey &key) {
	VkSampler sampler = cache_.Get(key);
	if (sampler != VK_NULL_HANDLE)
		return sampler;

	VkSamplerCreateInfo samp = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = key.sClamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeV = key.tClamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeW = samp.addressModeU;  // irrelevant, but Mali recommends that all clamp modes are the same if possible.
	samp.compareOp = VK_COMPARE_OP_ALWAYS;
	samp.flags = 0;
	samp.magFilter = key.magFilt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	samp.minFilter = key.minFilt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	samp.mipmapMode = key.mipFilt ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (key.aniso) {
		// Docs say the min of this value and the supported max are used.
		samp.maxAnisotropy = 1 << g_Config.iAnisotropyLevel;
		samp.anisotropyEnable = true;
	} else {
		samp.maxAnisotropy = 1.0f;
		samp.anisotropyEnable = false;
	}
	samp.maxLod = (float)(int32_t)key.maxLevel * (1.0f / 256.0f);
	samp.minLod = (float)(int32_t)key.minLevel * (1.0f / 256.0f);
	samp.mipLodBias = (float)(int32_t)key.lodBias * (1.0f / 256.0f);

	VkResult res = vkCreateSampler(vulkan_->GetDevice(), &samp, nullptr, &sampler);
	_assert_(res == VK_SUCCESS);
	cache_.Insert(key, sampler);
	return sampler;
}

std::string SamplerCache::DebugGetSamplerString(std::string id, DebugShaderStringType stringType) {
	SamplerCacheKey key;
	key.FromString(id);
	return StringFromFormat("%s/%s mag:%s min:%s mip:%s maxLod:%f minLod:%f bias:%f",
		key.sClamp ? "Clamp" : "Wrap",
		key.tClamp ? "Clamp" : "Wrap",
		key.magFilt ? "Linear" : "Nearest",
		key.minFilt ? "Linear" : "Nearest",
		key.mipFilt ? "Linear" : "Nearest",
		key.maxLevel / 256.0f,
		key.minLevel / 256.0f,
		key.lodBias / 256.0f);
}

void SamplerCache::DeviceLost() {
	cache_.Iterate([&](const SamplerCacheKey &key, VkSampler sampler) {
		vulkan_->Delete().QueueDeleteSampler(sampler);
	});
	cache_.Clear();
}

void SamplerCache::DeviceRestore(VulkanContext *vulkan) {
	vulkan_ = vulkan;
}

std::vector<std::string> SamplerCache::DebugGetSamplerIDs() const {
	std::vector<std::string> ids;
	cache_.Iterate([&](const SamplerCacheKey &id, VkSampler sampler) {
		std::string idstr;
		id.ToString(&idstr);
		ids.push_back(idstr);
	});
	return ids;
}

TextureCacheVulkan::TextureCacheVulkan(Draw::DrawContext *draw, VulkanContext *vulkan)
	: TextureCacheCommon(draw),
		vulkan_(vulkan),
		computeShaderManager_(vulkan),
		samplerCache_(vulkan) {
	DeviceRestore(vulkan, draw);
	SetupTextureDecoder();
}

TextureCacheVulkan::~TextureCacheVulkan() {
	DeviceLost();
}

void TextureCacheVulkan::SetFramebufferManager(FramebufferManagerVulkan *fbManager) {
	framebufferManagerVulkan_ = fbManager;
	framebufferManager_ = fbManager;
}

void TextureCacheVulkan::SetVulkan2D(Vulkan2D *vk2d) {
	vulkan2D_ = vk2d;
	depalShaderCache_->SetVulkan2D(vk2d);
}

void TextureCacheVulkan::DeviceLost() {
	Clear(true);

	if (allocator_) {
		allocator_->Destroy();

		// We have to delete on queue, so this can free its queued deletions.
		vulkan_->Delete().QueueCallback([](void *ptr) {
			auto allocator = static_cast<VulkanDeviceAllocator *>(ptr);
			delete allocator;
		}, allocator_);
		allocator_ = nullptr;
	}

	samplerCache_.DeviceLost();

	if (samplerNearest_)
		vulkan_->Delete().QueueDeleteSampler(samplerNearest_);

	if (uploadCS_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeleteShaderModule(uploadCS_);
	if (copyCS_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeleteShaderModule(copyCS_);

	computeShaderManager_.DeviceLost();

	nextTexture_ = nullptr;
}

void TextureCacheVulkan::DeviceRestore(VulkanContext *vulkan, Draw::DrawContext *draw) {
	vulkan_ = vulkan;
	draw_ = draw;

	_assert_(!allocator_);

	allocator_ = new VulkanDeviceAllocator(vulkan_, TEXCACHE_MIN_SLAB_SIZE, TEXCACHE_MAX_SLAB_SIZE);
	samplerCache_.DeviceRestore(vulkan);

	VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	VkResult res = vkCreateSampler(vulkan_->GetDevice(), &samp, nullptr, &samplerNearest_);
	_assert_(res == VK_SUCCESS);

	CompileScalingShader();

	computeShaderManager_.DeviceRestore(vulkan);
}

void TextureCacheVulkan::NotifyConfigChanged() {
	TextureCacheCommon::NotifyConfigChanged();
	CompileScalingShader();
}

static std::string ReadShaderSrc(const Path &filename) {
	size_t sz = 0;
	char *data = (char *)VFSReadFile(filename.c_str(), &sz);
	if (!data)
		return "";

	std::string src(data, sz);
	delete[] data;
	return src;
}

void TextureCacheVulkan::CompileScalingShader() {
	if (!g_Config.bTexHardwareScaling || g_Config.sTextureShaderName != textureShader_) {
		if (uploadCS_ != VK_NULL_HANDLE)
			vulkan_->Delete().QueueDeleteShaderModule(uploadCS_);
		if (copyCS_ != VK_NULL_HANDLE)
			vulkan_->Delete().QueueDeleteShaderModule(copyCS_);
		textureShader_.clear();
		maxScaleFactor_ = 255;
	} else if (uploadCS_ || copyCS_) {
		// No need to recreate.
		return;
	}
	if (!g_Config.bTexHardwareScaling)
		return;

	ReloadAllPostShaderInfo();
	const TextureShaderInfo *shaderInfo = GetTextureShaderInfo(g_Config.sTextureShaderName);
	if (!shaderInfo || shaderInfo->computeShaderFile.empty())
		return;

	std::string shaderSource = ReadShaderSrc(shaderInfo->computeShaderFile);
	std::string fullUploadShader = StringFromFormat(uploadShader, shaderSource.c_str());
	std::string fullCopyShader = StringFromFormat(copyShader, shaderSource.c_str());

	std::string error;
	uploadCS_ = CompileShaderModule(vulkan_, VK_SHADER_STAGE_COMPUTE_BIT, fullUploadShader.c_str(), &error);
	_dbg_assert_msg_(uploadCS_ != VK_NULL_HANDLE, "failed to compile upload shader");
	copyCS_ = CompileShaderModule(vulkan_, VK_SHADER_STAGE_COMPUTE_BIT, fullCopyShader.c_str(), &error);
	_dbg_assert_msg_(copyCS_ != VK_NULL_HANDLE, "failed to compile copy shader");

	textureShader_ = g_Config.sTextureShaderName;
	maxScaleFactor_ = shaderInfo->maxScale;
}

void TextureCacheVulkan::ReleaseTexture(TexCacheEntry *entry, bool delete_them) {
	delete entry->vkTex;
	entry->vkTex = nullptr;
}

VkFormat getClutDestFormatVulkan(GEPaletteFormat format) {
	switch (format) {
	case GE_CMODE_16BIT_ABGR4444:
		return VULKAN_4444_FORMAT;
	case GE_CMODE_16BIT_ABGR5551:
		return VULKAN_1555_FORMAT;
	case GE_CMODE_16BIT_BGR5650:
		return VULKAN_565_FORMAT;
	case GE_CMODE_32BIT_ABGR8888:
		return VULKAN_8888_FORMAT;
	}
	return VK_FORMAT_UNDEFINED;
}

static const VkFilter MagFiltVK[2] = {
	VK_FILTER_NEAREST,
	VK_FILTER_LINEAR
};

void TextureCacheVulkan::StartFrame() {
	InvalidateLastTexture();
	depalShaderCache_->Decimate();

	timesInvalidatedAllThisFrame_ = 0;
	texelsScaledThisFrame_ = 0;

	if (clearCacheNextFrame_) {
		Clear(true);
		clearCacheNextFrame_ = false;
	} else {
		int slabPressureLimit = TEXCACHE_SLAB_PRESSURE;
		if (g_Config.iTexScalingLevel > 1) {
			// Since textures are 2D maybe we should square this, but might get too non-aggressive.
			slabPressureLimit *= g_Config.iTexScalingLevel;
		}
		Decimate(allocator_->GetSlabCount() > slabPressureLimit);
	}

	allocator_->Begin();
	computeShaderManager_.BeginFrame();
}

void TextureCacheVulkan::EndFrame() {
	allocator_->End();
	computeShaderManager_.EndFrame();

	if (texelsScaledThisFrame_) {
		VERBOSE_LOG(G3D, "Scaled %i texels", texelsScaledThisFrame_);
	}
}

void TextureCacheVulkan::UpdateCurrentClut(GEPaletteFormat clutFormat, u32 clutBase, bool clutIndexIsSimple) {
	const u32 clutBaseBytes = clutFormat == GE_CMODE_32BIT_ABGR8888 ? (clutBase * sizeof(u32)) : (clutBase * sizeof(u16));
	// Technically, these extra bytes weren't loaded, but hopefully it was loaded earlier.
	// If not, we're going to hash random data, which hopefully doesn't cause a performance issue.
	//
	// TODO: Actually, this seems like a hack.  The game can upload part of a CLUT and reference other data.
	// clutTotalBytes_ is the last amount uploaded.  We should hash clutMaxBytes_, but this will often hash
	// unrelated old entries for small palettes.
	// Adding clutBaseBytes may just be mitigating this for some usage patterns.
	const u32 clutExtendedBytes = std::min(clutTotalBytes_ + clutBaseBytes, clutMaxBytes_);

	if (replacer_.Enabled())
		clutHash_ = XXH32((const char *)clutBufRaw_, clutExtendedBytes, 0xC0108888);
	else
		clutHash_ = XXH3_64bits((const char *)clutBufRaw_, clutExtendedBytes) & 0xFFFFFFFF;
	clutBuf_ = clutBufRaw_;

	// Special optimization: fonts typically draw clut4 with just alpha values in a single color.
	clutAlphaLinear_ = false;
	clutAlphaLinearColor_ = 0;
	if (clutFormat == GE_CMODE_16BIT_ABGR4444 && clutIndexIsSimple) {
		const u16_le *clut = GetCurrentClut<u16_le>();
		clutAlphaLinear_ = true;
		clutAlphaLinearColor_ = clut[15] & 0x0FFF;
		for (int i = 0; i < 16; ++i) {
			u16 step = clutAlphaLinearColor_ | (i << 12);
			if (clut[i] != step) {
				clutAlphaLinear_ = false;
				break;
			}
		}
	}

	clutLastFormat_ = gstate.clutformat;
}

void TextureCacheVulkan::BindTexture(TexCacheEntry *entry) {
	_assert_(entry);
	_assert_(entry->vkTex);

	entry->vkTex->Touch();
	imageView_ = entry->vkTex->GetImageView();
	int maxLevel = (entry->status & TexCacheEntry::STATUS_BAD_MIPS) ? 0 : entry->maxLevel;
	SamplerCacheKey samplerKey = GetSamplingParams(maxLevel, entry);
	curSampler_ = samplerCache_.GetOrCreateSampler(samplerKey);
	drawEngine_->SetDepalTexture(VK_NULL_HANDLE);
	gstate_c.SetUseShaderDepal(false);
}

void TextureCacheVulkan::Unbind() {
	imageView_ = VK_NULL_HANDLE;
	curSampler_ = VK_NULL_HANDLE;
	InvalidateLastTexture();
}

void TextureCacheVulkan::ApplyTextureFramebuffer(VirtualFramebuffer *framebuffer, GETextureFormat texFormat, FramebufferNotificationChannel channel) {
	SamplerCacheKey samplerKey = GetFramebufferSamplingParams(framebuffer->bufferWidth, framebuffer->bufferHeight);

	DepalShaderVulkan *depalShader = nullptr;
	uint32_t clutMode = gstate.clutformat & 0xFFFFFF;

	bool expand32 = !gstate_c.Supports(GPU_SUPPORTS_16BIT_FORMATS);
	bool depth = channel == NOTIFY_FB_DEPTH;
	bool useShaderDepal = framebufferManager_->GetCurrentRenderVFB() != framebuffer && !depth;

	bool need_depalettize = IsClutFormat(texFormat);

	if (need_depalettize && !g_Config.bDisableSlowFramebufEffects) {
		if (useShaderDepal) {
			depalShaderCache_->SetPushBuffer(drawEngine_->GetPushBufferForTextureData());
			const GEPaletteFormat clutFormat = gstate.getClutPaletteFormat();
			VulkanTexture *clutTexture = depalShaderCache_->GetClutTexture(clutFormat, clutHash_, clutBuf_, expand32);
			drawEngine_->SetDepalTexture(clutTexture ? clutTexture->GetImageView() : VK_NULL_HANDLE);
			// Only point filtering enabled.
			samplerKey.magFilt = false;
			samplerKey.minFilt = false;
			samplerKey.mipFilt = false;
			// Make sure to update the uniforms, and also texture - needs a recheck.
			gstate_c.Dirty(DIRTY_DEPAL);
			gstate_c.SetUseShaderDepal(true);
			gstate_c.depalFramebufferFormat = framebuffer->drawnFormat;
			const u32 bytesPerColor = clutFormat == GE_CMODE_32BIT_ABGR8888 ? sizeof(u32) : sizeof(u16);
			const u32 clutTotalColors = clutMaxBytes_ / bytesPerColor;
			TexCacheEntry::TexStatus alphaStatus = CheckAlpha(clutBuf_, getClutDestFormatVulkan(clutFormat), clutTotalColors, clutTotalColors, 1);
			gstate_c.SetTextureFullAlpha(alphaStatus == TexCacheEntry::STATUS_ALPHA_FULL);
			curSampler_ = samplerCache_.GetOrCreateSampler(samplerKey);
			if (framebufferManagerVulkan_->BindFramebufferAsColorTexture(0, framebuffer, BINDFBCOLOR_MAY_COPY_WITH_UV | BINDFBCOLOR_APPLY_TEX_OFFSET)) {
				imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW);
			} else {
				imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::NULL_IMAGEVIEW);
			}
			return;
		} else {
			depalShader = depalShaderCache_->GetDepalettizeShader(clutMode, depth ? GE_FORMAT_DEPTH16 : framebuffer->drawnFormat);
			drawEngine_->SetDepalTexture(VK_NULL_HANDLE);
			gstate_c.SetUseShaderDepal(false);
		}
	}
	if (depalShader) {
		depalShaderCache_->SetPushBuffer(drawEngine_->GetPushBufferForTextureData());
		const GEPaletteFormat clutFormat = gstate.getClutPaletteFormat();
		VulkanTexture *clutTexture = depalShaderCache_->GetClutTexture(clutFormat, clutHash_, clutBuf_, expand32);

		Draw::Framebuffer *depalFBO = framebufferManager_->GetTempFBO(TempFBO::DEPAL, framebuffer->renderWidth, framebuffer->renderHeight);
		draw_->BindFramebufferAsRenderTarget(depalFBO, { Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE }, "Depal");

		Vulkan2D::Vertex verts[4] = {
			{ -1, -1, 0.0f, 0, 0 },
			{  1, -1, 0.0f, 1, 0 },
			{ -1,  1, 0.0f, 0, 1 },
			{  1,  1, 0.0f, 1, 1 },
		};

		// If min is not < max, then we don't have values (wasn't set during decode.)
		if (gstate_c.vertBounds.minV < gstate_c.vertBounds.maxV) {
			const float invWidth = 1.0f / (float)framebuffer->bufferWidth;
			const float invHeight = 1.0f / (float)framebuffer->bufferHeight;
			// Inverse of half = double.
			const float invHalfWidth = invWidth * 2.0f;
			const float invHalfHeight = invHeight * 2.0f;

			const int u1 = gstate_c.vertBounds.minU + gstate_c.curTextureXOffset;
			const int v1 = gstate_c.vertBounds.minV + gstate_c.curTextureYOffset;
			const int u2 = gstate_c.vertBounds.maxU + gstate_c.curTextureXOffset;
			const int v2 = gstate_c.vertBounds.maxV + gstate_c.curTextureYOffset;

			const float left = u1 * invHalfWidth - 1.0f;
			const float right = u2 * invHalfWidth - 1.0f;
			const float top = v1 * invHalfHeight - 1.0f;
			const float bottom = v2 * invHalfHeight - 1.0f;
			// Points are: BL, BR, TR, TL.
			verts[0].x = left;
			verts[0].y = bottom;
			verts[1].x = right;
			verts[1].y = bottom;
			verts[2].x = left;
			verts[2].y = top;
			verts[3].x = right;
			verts[3].y = top;

			// And also the UVs, same order.
			const float uvleft = u1 * invWidth;
			const float uvright = u2 * invWidth;
			const float uvtop = v1 * invHeight;
			const float uvbottom = v2 * invHeight;
			verts[0].u = uvleft;
			verts[0].v = uvbottom;
			verts[1].u = uvright;
			verts[1].v = uvbottom;
			verts[2].u = uvleft;
			verts[2].v = uvtop;
			verts[3].u = uvright;
			verts[3].v = uvtop;

			// We need to reapply the texture next time since we cropped UV.
			gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
		}

		VkBuffer pushed;
		uint32_t offset = push_->PushAligned(verts, sizeof(verts), 4, &pushed);

		draw_->BindFramebufferAsTexture(framebuffer->fbo, 0, depth ? Draw::FB_DEPTH_BIT : Draw::FB_COLOR_BIT, 0);
		VkImageView fbo = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW);

		VkDescriptorSet descSet = vulkan2D_->GetDescriptorSet(fbo, samplerNearest_, clutTexture->GetImageView(), samplerNearest_);
		VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
		renderManager->BindPipeline(depalShader->pipeline, (PipelineFlags)0);

		if (depth) {
			DepthScaleFactors scaleFactors = GetDepthScaleFactors();
			struct DepthPushConstants {
				float z_scale;
				float z_offset;
			};
			DepthPushConstants push;
			push.z_scale = scaleFactors.scale;
			push.z_offset = scaleFactors.offset;
			renderManager->PushConstants(vulkan2D_->GetPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DepthPushConstants), &push);
		}
		renderManager->SetScissor(VkRect2D{ {0, 0}, { framebuffer->renderWidth, framebuffer->renderHeight} });
		renderManager->SetViewport(VkViewport{ 0.f, 0.f, (float)framebuffer->renderWidth, (float)framebuffer->renderHeight, 0.f, 1.f });
		renderManager->Draw(vulkan2D_->GetPipelineLayout(), descSet, 0, nullptr, pushed, offset, 4);
		shaderManagerVulkan_->DirtyLastShader();

		const u32 bytesPerColor = clutFormat == GE_CMODE_32BIT_ABGR8888 ? sizeof(u32) : sizeof(u16);
		const u32 clutTotalColors = clutMaxBytes_ / bytesPerColor;

		TexCacheEntry::TexStatus alphaStatus = CheckAlpha(clutBuf_, getClutDestFormatVulkan(clutFormat), clutTotalColors, clutTotalColors, 1);
		gstate_c.SetTextureFullAlpha(alphaStatus == TexCacheEntry::STATUS_ALPHA_FULL);

		framebufferManager_->RebindFramebuffer("RebindFramebuffer - ApplyTextureFramebuffer");
		draw_->BindFramebufferAsTexture(depalFBO, 0, Draw::FB_COLOR_BIT, 0);
		imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW);

		// Need to rebind the pipeline since we switched it.
		drawEngine_->DirtyPipeline();
		// Since we may have switched render targets, we need to re-set depth/stencil etc states.
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_BLEND_STATE | DIRTY_RASTER_STATE);
	} else {
		if (framebufferManagerVulkan_->BindFramebufferAsColorTexture(0, framebuffer, BINDFBCOLOR_MAY_COPY_WITH_UV | BINDFBCOLOR_APPLY_TEX_OFFSET)) {
			imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW);
		} else {
			imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::NULL_IMAGEVIEW);
		}

		drawEngine_->SetDepalTexture(VK_NULL_HANDLE);
		gstate_c.SetUseShaderDepal(false);

		gstate_c.SetTextureFullAlpha(gstate.getTextureFormat() == GE_TFMT_5650);
	}

	curSampler_ = samplerCache_.GetOrCreateSampler(samplerKey);
}

ReplacedTextureFormat FromVulkanFormat(VkFormat fmt) {
	switch (fmt) {
	case VULKAN_565_FORMAT: return ReplacedTextureFormat::F_5650;
	case VULKAN_1555_FORMAT: return ReplacedTextureFormat::F_5551;
	case VULKAN_4444_FORMAT: return ReplacedTextureFormat::F_4444;
	case VULKAN_8888_FORMAT: default: return ReplacedTextureFormat::F_8888;
	}
}

VkFormat ToVulkanFormat(ReplacedTextureFormat fmt) {
	switch (fmt) {
	case ReplacedTextureFormat::F_5650: return VULKAN_565_FORMAT;
	case ReplacedTextureFormat::F_5551: return VULKAN_1555_FORMAT;
	case ReplacedTextureFormat::F_4444: return VULKAN_4444_FORMAT;
	case ReplacedTextureFormat::F_8888: default: return VULKAN_8888_FORMAT;
	}
}

void TextureCacheVulkan::BuildTexture(TexCacheEntry *const entry) {
	entry->status &= ~TexCacheEntry::STATUS_ALPHA_MASK;

	// For the estimate, we assume cluts always point to 8888 for simplicity.
	cacheSizeEstimate_ += EstimateTexMemoryUsage(entry);

	if ((entry->bufw == 0 || (gstate.texbufwidth[0] & 0xf800) != 0) && entry->addr >= PSP_GetKernelMemoryEnd()) {
		ERROR_LOG_REPORT(G3D, "Texture with unexpected bufw (full=%d)", gstate.texbufwidth[0] & 0xffff);
		// Proceeding here can cause a crash.
		return;
	}

	// Adjust maxLevel to actually present levels..
	bool badMipSizes = false;

	// maxLevel here is the max level to upload. Not the count.
	int maxLevel = entry->maxLevel;

	for (int i = 0; i <= maxLevel; i++) {
		// If encountering levels pointing to nothing, adjust max level.
		u32 levelTexaddr = gstate.getTextureAddress(i);
		if (!Memory::IsValidAddress(levelTexaddr)) {
			maxLevel = i - 1;
			break;
		}

		// If size reaches 1, stop, and override maxlevel.
		int tw = gstate.getTextureWidth(i);
		int th = gstate.getTextureHeight(i);
		if (tw == 1 || th == 1) {
			maxLevel = i;
			break;
		}

		if (i > 0 && gstate_c.Supports(GPU_SUPPORTS_TEXTURE_LOD_CONTROL)) {
			if (tw != 1 && tw != (gstate.getTextureWidth(i - 1) >> 1))
				badMipSizes = true;
			else if (th != 1 && th != (gstate.getTextureHeight(i - 1) >> 1))
				badMipSizes = true;
		}
	}

	// In addition, simply don't load more than level 0 if g_Config.bMipMap is false.
	if (badMipSizes) {
		maxLevel = 0;
	}

	// We generate missing mipmaps from maxLevel+1 up to this level. maxLevel can get overwritten below
	// such as when using replacement textures - but let's keep the same amount of levels.
	int maxLevelToGenerate = maxLevel;

	// If GLES3 is available, we can preallocate the storage, which makes texture loading more efficient.
	VkFormat dstFmt = GetDestFormat(GETextureFormat(entry->format), gstate.getClutPaletteFormat());

	int scaleFactor = standardScaleFactor_;
	if (scaleFactor > maxScaleFactor_)
		scaleFactor = maxScaleFactor_;

	// Rachet down scale factor in low-memory mode.
	if (lowMemoryMode_) {
		// Keep it even, though, just in case of npot troubles.
		scaleFactor = scaleFactor > 4 ? 4 : (scaleFactor > 2 ? 2 : 1);
	}

	u64 cachekey = replacer_.Enabled() ? entry->CacheKey() : 0;
	int w = gstate.getTextureWidth(0);
	int h = gstate.getTextureHeight(0);
	ReplacedTexture &replaced = replacer_.FindReplacement(cachekey, entry->fullhash, w, h);
	if (replaced.GetSize(0, w, h)) {
		// We're replacing, so we won't scale.
		scaleFactor = 1;
		entry->status |= TexCacheEntry::STATUS_IS_SCALED;
		maxLevel = replaced.MaxLevel();
		badMipSizes = false;
	}

	bool hardwareScaling = g_Config.bTexHardwareScaling && (uploadCS_ != VK_NULL_HANDLE || copyCS_ != VK_NULL_HANDLE);

	// Don't scale the PPGe texture.
	if (entry->addr > 0x05000000 && entry->addr < PSP_GetKernelMemoryEnd())
		scaleFactor = 1;
	if ((entry->status & TexCacheEntry::STATUS_CHANGE_FREQUENT) != 0 && scaleFactor != 1 && !hardwareScaling) {
		// Remember for later that we /wanted/ to scale this texture.
		entry->status |= TexCacheEntry::STATUS_TO_SCALE;
		scaleFactor = 1;
	}

	if (scaleFactor != 1) {
		if (texelsScaledThisFrame_ >= TEXCACHE_MAX_TEXELS_SCALED && !hardwareScaling) {
			entry->status |= TexCacheEntry::STATUS_TO_SCALE;
			scaleFactor = 1;
		} else {
			entry->status &= ~TexCacheEntry::STATUS_TO_SCALE;
			entry->status |= TexCacheEntry::STATUS_IS_SCALED;
			texelsScaledThisFrame_ += w * h;
		}
	}

	// TODO
	if (scaleFactor > 1) {
		maxLevel = 0;
	}

	VkFormat actualFmt = scaleFactor > 1 ? VULKAN_8888_FORMAT : dstFmt;
	if (replaced.Valid()) {
		actualFmt = ToVulkanFormat(replaced.Format(0));
	}

	bool computeUpload = false;
	bool computeCopy = false;
	VkCommandBuffer cmdInit = (VkCommandBuffer)draw_->GetNativeObject(Draw::NativeObject::INIT_COMMANDBUFFER);

	{
		delete entry->vkTex;
		entry->vkTex = new VulkanTexture(vulkan_);
		VulkanTexture *image = entry->vkTex;

		const VkComponentMapping *mapping;
		switch (actualFmt) {
		case VULKAN_4444_FORMAT:
			mapping = &VULKAN_4444_SWIZZLE;
			break;

		case VULKAN_1555_FORMAT:
			mapping = &VULKAN_1555_SWIZZLE;
			break;

		case VULKAN_565_FORMAT:
			mapping = &VULKAN_565_SWIZZLE;
			break;

		default:
			mapping = &VULKAN_8888_SWIZZLE;
			break;
		}

		VkImageLayout imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		// Compute experiment
		if (actualFmt == VULKAN_8888_FORMAT && scaleFactor > 1 && hardwareScaling) {
			// Enable the experiment you want.
			if (uploadCS_ != VK_NULL_HANDLE)
				computeUpload = true;
			else if (copyCS_ != VK_NULL_HANDLE)
				computeCopy = true;
		}

		if (computeUpload) {
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		}

		char texName[128]{};
		snprintf(texName, sizeof(texName), "tex_%08x_%s", entry->addr, GeTextureFormatToString((GETextureFormat)entry->format, gstate.getClutPaletteFormat()));
		image->SetTag(texName);

		bool allocSuccess = image->CreateDirect(cmdInit, allocator_, w * scaleFactor, h * scaleFactor, maxLevelToGenerate + 1, actualFmt, imageLayout, usage, mapping);
		if (!allocSuccess && !lowMemoryMode_) {
			WARN_LOG_REPORT(G3D, "Texture cache ran out of GPU memory; switching to low memory mode");
			lowMemoryMode_ = true;
			decimationCounter_ = 0;
			Decimate();
			// TODO: We should stall the GPU here and wipe things out of memory.
			// As is, it will almost definitely fail the second time, but next frame it may recover.

			auto err = GetI18NCategory("Error");
			if (scaleFactor > 1) {
				host->NotifyUserMessage(err->T("Warning: Video memory FULL, reducing upscaling and switching to slow caching mode"), 2.0f);
			} else {
				host->NotifyUserMessage(err->T("Warning: Video memory FULL, switching to slow caching mode"), 2.0f);
			}

			scaleFactor = 1;
			actualFmt = dstFmt;

			allocSuccess = image->CreateDirect(cmdInit, allocator_, w * scaleFactor, h * scaleFactor, maxLevelToGenerate + 1, actualFmt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, mapping);
		}

		if (!allocSuccess) {
			ERROR_LOG(G3D, "Failed to create texture (%dx%d)", w, h);
			delete entry->vkTex;
			entry->vkTex = nullptr;
		}
	}

	ReplacedTextureDecodeInfo replacedInfo;
	if (replacer_.Enabled() && !replaced.Valid()) {
		replacedInfo.cachekey = cachekey;
		replacedInfo.hash = entry->fullhash;
		replacedInfo.addr = entry->addr;
		replacedInfo.isVideo = IsVideo(entry->addr);
		replacedInfo.isFinal = (entry->status & TexCacheEntry::STATUS_TO_SCALE) == 0;
		replacedInfo.scaleFactor = scaleFactor;
		replacedInfo.fmt = FromVulkanFormat(actualFmt);
	}

	if (entry->vkTex) {
		// NOTE: Since the level is not part of the cache key, we assume it never changes.
		u8 level = std::max(0, gstate.getTexLevelOffset16() / 16);
		bool fakeMipmap = IsFakeMipmapChange() && level > 0;
		// Upload the texture data.
		for (int i = 0; i <= maxLevel; i++) {
			int mipWidth = gstate.getTextureWidth(i) * scaleFactor;
			int mipHeight = gstate.getTextureHeight(i) * scaleFactor;
			if (replaced.Valid()) {
				replaced.GetSize(i, mipWidth, mipHeight);
			}
			int srcBpp = dstFmt == VULKAN_8888_FORMAT ? 4 : 2;
			int srcStride = mipWidth * srcBpp;
			int srcSize = srcStride * mipHeight;
			int bpp = actualFmt == VULKAN_8888_FORMAT ? 4 : 2;
			int stride = (mipWidth * bpp + 15) & ~15;
			int size = stride * mipHeight;
			uint32_t bufferOffset;
			VkBuffer texBuf;
			// nvidia returns 1 but that can't be healthy... let's align by 16 as a minimum.
			int pushAlignment = std::max(16, (int)vulkan_->GetPhysicalDeviceProperties().properties.limits.optimalBufferCopyOffsetAlignment);
			void *data;
			bool dataScaled = true;
			if (replaced.Valid()) {
				// Directly load the replaced image.
				data = drawEngine_->GetPushBufferForTextureData()->PushAligned(size, &bufferOffset, &texBuf, pushAlignment);
				replaced.Load(i, data, stride);
				entry->vkTex->UploadMip(cmdInit, i, mipWidth, mipHeight, texBuf, bufferOffset, stride / bpp);
			} else {
				auto dispatchCompute = [&](VkDescriptorSet descSet) {
					struct Params { int x; int y; int s; int fmt; } params{ mipWidth, mipHeight, scaleFactor, 0 };
					if (dstFmt == VULKAN_4444_FORMAT) {
						params.fmt = 4;
					} else if (dstFmt == VULKAN_1555_FORMAT) {
						params.fmt = 5;
					} else if (dstFmt == VULKAN_565_FORMAT) {
						params.fmt = 6;
					}
					vkCmdBindDescriptorSets(cmdInit, VK_PIPELINE_BIND_POINT_COMPUTE, computeShaderManager_.GetPipelineLayout(), 0, 1, &descSet, 0, nullptr);
					vkCmdPushConstants(cmdInit, computeShaderManager_.GetPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
					vkCmdDispatch(cmdInit, (mipWidth + 15) / 16, (mipHeight + 15) / 16, 1);
				};

				if (fakeMipmap) {
					data = drawEngine_->GetPushBufferForTextureData()->PushAligned(size, &bufferOffset, &texBuf, pushAlignment);
					LoadTextureLevel(*entry, (uint8_t *)data, stride, level, scaleFactor, dstFmt);
					entry->vkTex->UploadMip(cmdInit, 0, mipWidth, mipHeight, texBuf, bufferOffset, stride / bpp);
					break;
				} else {
					if (computeUpload) {
						data = drawEngine_->GetPushBufferForTextureData()->PushAligned(srcSize, &bufferOffset, &texBuf, pushAlignment);
						dataScaled = false;
						LoadTextureLevel(*entry, (uint8_t *)data, srcStride, i, 1, dstFmt);
						// This format can be used with storage images.
						VkImageView view = entry->vkTex->CreateViewForMip(i);
						VkDescriptorSet descSet = computeShaderManager_.GetDescriptorSet(view, texBuf, bufferOffset, srcSize);
						vkCmdBindPipeline(cmdInit, VK_PIPELINE_BIND_POINT_COMPUTE, computeShaderManager_.GetPipeline(uploadCS_));
						dispatchCompute(descSet);
						vulkan_->Delete().QueueDeleteImageView(view);
					} else if (computeCopy) {
						data = drawEngine_->GetPushBufferForTextureData()->PushAligned(srcSize, &bufferOffset, &texBuf, pushAlignment);
						dataScaled = false;
						LoadTextureLevel(*entry, (uint8_t *)data, srcStride, i, 1, dstFmt);
						// Simple test of using a "copy shader" before the upload. This one could unswizzle or whatever
						// and will work for any texture format including 16-bit as long as the shader is written to pack it into int32 size bits
						// which is the smallest possible write.
						VkBuffer localBuf;
						uint32_t localOffset;
						uint32_t localSize = size;
						localOffset = (uint32_t)drawEngine_->GetPushBufferLocal()->Allocate(localSize, &localBuf);

						VkDescriptorSet descSet = computeShaderManager_.GetDescriptorSet(VK_NULL_HANDLE, texBuf, bufferOffset, srcSize, localBuf, localOffset, localSize);
						vkCmdBindPipeline(cmdInit, VK_PIPELINE_BIND_POINT_COMPUTE, computeShaderManager_.GetPipeline(copyCS_));
						dispatchCompute(descSet);

						// After the compute, before the copy, we need a memory barrier.
						VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
						barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
						barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
						barrier.buffer = localBuf;
						barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
						barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
						barrier.offset = localOffset;
						barrier.size = localSize;
						vkCmdPipelineBarrier(cmdInit, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
							0, 0, nullptr, 1, &barrier, 0, nullptr);

						entry->vkTex->UploadMip(cmdInit, i, mipWidth, mipHeight, localBuf, localOffset, stride / bpp);
					} else {
						data = drawEngine_->GetPushBufferForTextureData()->PushAligned(size, &bufferOffset, &texBuf, pushAlignment);
						LoadTextureLevel(*entry, (uint8_t *)data, stride, i, scaleFactor, dstFmt);
						entry->vkTex->UploadMip(cmdInit, i, mipWidth, mipHeight, texBuf, bufferOffset, stride / bpp);
					}
				}
				if (replacer_.Enabled()) {
					// When hardware texture scaling is enabled, this saves the original.
					int w = dataScaled ? mipWidth : mipWidth / scaleFactor;
					int h = dataScaled ? mipHeight : mipHeight / scaleFactor;
					replacer_.NotifyTextureDecoded(replacedInfo, data, stride, i, w, h);
				}
			}
		}

		// Generate any additional mipmap levels.
		for (int level = maxLevel + 1; level <= maxLevelToGenerate; level++) {
			entry->vkTex->GenerateMip(cmdInit, level, computeUpload ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}

		if (maxLevel == 0) {
			entry->status |= TexCacheEntry::STATUS_BAD_MIPS;
		} else {
			entry->status &= ~TexCacheEntry::STATUS_BAD_MIPS;
		}
		if (replaced.Valid()) {
			entry->SetAlphaStatus(TexCacheEntry::TexStatus(replaced.AlphaStatus()));
		}
		entry->vkTex->EndCreate(cmdInit, false, computeUpload ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
}

VkFormat TextureCacheVulkan::GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) const {
	if (!gstate_c.Supports(GPU_SUPPORTS_16BIT_FORMATS)) {
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	switch (format) {
	case GE_TFMT_CLUT4:
	case GE_TFMT_CLUT8:
	case GE_TFMT_CLUT16:
	case GE_TFMT_CLUT32:
		return getClutDestFormatVulkan(clutFormat);
	case GE_TFMT_4444:
		return VULKAN_4444_FORMAT;
	case GE_TFMT_5551:
		return VULKAN_1555_FORMAT;
	case GE_TFMT_5650:
		return VULKAN_565_FORMAT;
	case GE_TFMT_8888:
	case GE_TFMT_DXT1:
	case GE_TFMT_DXT3:
	case GE_TFMT_DXT5:
	default:
		return VULKAN_8888_FORMAT;
	}
}

TexCacheEntry::TexStatus TextureCacheVulkan::CheckAlpha(const u32 *pixelData, VkFormat dstFmt, int stride, int w, int h) {
	CheckAlphaResult res;
	switch (dstFmt) {
	case VULKAN_4444_FORMAT:
		res = CheckAlphaRGBA4444Basic(pixelData, stride, w, h);
		break;
	case VULKAN_1555_FORMAT:
		res = CheckAlphaRGBA5551Basic(pixelData, stride, w, h);
		break;
	case VULKAN_565_FORMAT:
		// Never has any alpha.
		res = CHECKALPHA_FULL;
		break;
	default:
		res = CheckAlphaRGBA8888Basic(pixelData, stride, w, h);
		break;
	}

	return (TexCacheEntry::TexStatus)res;
}

void TextureCacheVulkan::LoadTextureLevel(TexCacheEntry &entry, uint8_t *writePtr, int rowPitch, int level, int scaleFactor, VkFormat dstFmt) {
	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);

	{
		PROFILE_THIS_SCOPE("decodetex");

		GETextureFormat tfmt = (GETextureFormat)entry.format;
		GEPaletteFormat clutformat = gstate.getClutPaletteFormat();
		u32 texaddr = gstate.getTextureAddress(level);
		int bufw = GetTextureBufw(level, texaddr, tfmt);
		int bpp = dstFmt == VULKAN_8888_FORMAT ? 4 : 2;

		u32 *pixelData = (u32 *)writePtr;
		int decPitch = rowPitch;
		if (scaleFactor > 1) {
			tmpTexBufRearrange_.resize(std::max(bufw, w) * h);
			pixelData = tmpTexBufRearrange_.data();
			// We want to end up with a neatly packed texture for scaling.
			decPitch = w * bpp;
		}

		bool expand32 = !gstate_c.Supports(GPU_SUPPORTS_16BIT_FORMATS);
		DecodeTextureLevel((u8 *)pixelData, decPitch, tfmt, clutformat, texaddr, level, bufw, false, false, expand32);
		gpuStats.numTexturesDecoded++;

		// We check before scaling since scaling shouldn't invent alpha from a full alpha texture.
		if ((entry.status & TexCacheEntry::STATUS_CHANGE_FREQUENT) == 0) {
			// TODO: When we decode directly, this can be more expensive (maybe not on mobile?)
			// This does allow us to skip alpha testing, though.
			TexCacheEntry::TexStatus alphaStatus = CheckAlpha(pixelData, dstFmt, decPitch / bpp, w, h);
			entry.SetAlphaStatus(alphaStatus, level);
		} else {
			entry.SetAlphaStatus(TexCacheEntry::STATUS_ALPHA_UNKNOWN);
		}

		if (scaleFactor > 1) {
			u32 fmt = dstFmt;
			// CPU scaling reads from the destination buffer so we want cached RAM.
			uint8_t *rearrange = (uint8_t *)AllocateAlignedMemory(w * scaleFactor * h * scaleFactor * 4, 16);
			scaler.ScaleAlways((u32 *)rearrange, pixelData, fmt, w, h, scaleFactor);
			pixelData = (u32 *)writePtr;
			dstFmt = (VkFormat)fmt;

			// We always end up at 8888.  Other parts assume this.
			_assert_(dstFmt == VULKAN_8888_FORMAT);
			bpp = sizeof(u32);
			decPitch = w * bpp;

			if (decPitch != rowPitch) {
				for (int y = 0; y < h; ++y) {
					memcpy(writePtr + rowPitch * y, rearrange + decPitch * y, w * bpp);
				}
				decPitch = rowPitch;
			} else {
				memcpy(writePtr, rearrange, w * h * 4);
			}
			FreeAlignedMemory(rearrange);
		}
	}
}

bool TextureCacheVulkan::GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level) {
	SetTexture();
	if (!nextTexture_) {
		if (nextFramebufferTexture_) {
			VirtualFramebuffer *vfb = nextFramebufferTexture_;
			buffer.Allocate(vfb->bufferWidth, vfb->bufferHeight, GPU_DBG_FORMAT_8888, false);
			bool retval = draw_->CopyFramebufferToMemorySync(vfb->fbo, Draw::FB_COLOR_BIT, 0, 0, vfb->bufferWidth, vfb->bufferHeight, Draw::DataFormat::R8G8B8A8_UNORM, buffer.GetData(), vfb->bufferWidth, "GetCurrentTextureDebug");
			// Vulkan requires us to re-apply all dynamic state for each command buffer, and the above will cause us to start a new cmdbuf.
			// So let's dirty the things that are involved in Vulkan dynamic state. Readbacks are not frequent so this won't hurt other backends.
			gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE);
			// We may have blitted to a temp FBO.
			framebufferManager_->RebindFramebuffer("RebindFramebuffer - GetCurrentTextureDebug");
			return retval;
		} else {
			return false;
		}
	}

	// Apply texture may need to rebuild the texture if we're about to render, or bind a framebuffer.
	TexCacheEntry *entry = nextTexture_;
	ApplyTexture();

	if (!entry->vkTex)
		return false;
	VulkanTexture *texture = entry->vkTex;
	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	GPUDebugBufferFormat bufferFormat;
	Draw::DataFormat drawFormat;
	switch (texture->GetFormat()) {
	case VULKAN_565_FORMAT:
		bufferFormat = GPU_DBG_FORMAT_565;
		drawFormat = Draw::DataFormat::B5G6R5_UNORM_PACK16;
		break;
	case VULKAN_1555_FORMAT:
		bufferFormat = GPU_DBG_FORMAT_5551;
		drawFormat = Draw::DataFormat::B5G5R5A1_UNORM_PACK16;
		break;
	case VULKAN_4444_FORMAT:
		bufferFormat = GPU_DBG_FORMAT_4444;
		drawFormat = Draw::DataFormat::B4G4R4A4_UNORM_PACK16;
		break;
	case VULKAN_8888_FORMAT:
	default:
		bufferFormat = GPU_DBG_FORMAT_8888;
		drawFormat = Draw::DataFormat::R8G8B8A8_UNORM;
		break;
	}

	int w = texture->GetWidth();
	int h = texture->GetHeight();
	buffer.Allocate(w, h, bufferFormat);

	renderManager->CopyImageToMemorySync(texture->GetImage(), level, 0, 0, w, h, drawFormat, (uint8_t *)buffer.GetData(), w, "GetCurrentTextureDebug");

	// Vulkan requires us to re-apply all dynamic state for each command buffer, and the above will cause us to start a new cmdbuf.
	// So let's dirty the things that are involved in Vulkan dynamic state. Readbacks are not frequent so this won't hurt other backends.
	gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE);
	framebufferManager_->RebindFramebuffer("RebindFramebuffer - GetCurrentTextureDebug");
	return true;
}

void TextureCacheVulkan::GetStats(char *ptr, size_t size) {
	snprintf(ptr, size, "Alloc: %d slabs\nSlab min/max: %d/%d\nAlloc usage: %d%%",
		allocator_->GetSlabCount(), allocator_->GetMinSlabSize(), allocator_->GetMaxSlabSize(), allocator_->ComputeUsagePercent());
}

std::vector<std::string> TextureCacheVulkan::DebugGetSamplerIDs() const {
	return samplerCache_.DebugGetSamplerIDs();
}

std::string TextureCacheVulkan::DebugGetSamplerString(std::string id, DebugShaderStringType stringType) {
	return samplerCache_.DebugGetSamplerString(id, stringType);
}
