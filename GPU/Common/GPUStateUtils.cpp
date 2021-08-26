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

#include <algorithm>
#include <limits>

#include "Common/System/Display.h"

#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"

#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/Math3D.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/VertexDecoderCommon.h"

#include "GPU/Common/GPUStateUtils.h"

bool IsStencilTestOutputDisabled() {
	// The mask applies on all stencil ops.
	if (gstate.isStencilTestEnabled() && (gstate.pmska & 0xFF) != 0xFF) {
		if (gstate.FrameBufFormat() == GE_FORMAT_565) {
			return true;
		}
		return gstate.getStencilOpZPass() == GE_STENCILOP_KEEP && gstate.getStencilOpZFail() == GE_STENCILOP_KEEP && gstate.getStencilOpSFail() == GE_STENCILOP_KEEP;
	}
	return true;
}

bool NeedsTestDiscard() {
	// We assume this is called only when enabled and not trivially true (may also be for color testing.)
	if (gstate.isStencilTestEnabled() && (gstate.pmska & 0xFF) != 0xFF)
		return true;
	if (gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled())
		return true;
	if (!gstate.isAlphaBlendEnabled())
		return true;
	if (gstate.getBlendFuncA() != GE_SRCBLEND_SRCALPHA && gstate.getBlendFuncA() != GE_SRCBLEND_DOUBLESRCALPHA)
		return true;
	// GE_DSTBLEND_DOUBLEINVSRCALPHA is actually inverse double src alpha, and doubling zero is still zero.
	if (gstate.getBlendFuncB() != GE_DSTBLEND_INVSRCALPHA && gstate.getBlendFuncB() != GE_DSTBLEND_DOUBLEINVSRCALPHA) {
		if (gstate.getBlendFuncB() != GE_DSTBLEND_FIXB || gstate.getFixB() != 0xFFFFFF)
			return true;
	}
	if (gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD && gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE)
		return true;
	if (gstate.isLogicOpEnabled() && gstate.getLogicOp() != GE_LOGIC_COPY)
		return true;

	return false;
}

bool IsAlphaTestTriviallyTrue() {
	switch (gstate.getAlphaTestFunction()) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_GEQUAL:
		if (gstate_c.vertexFullAlpha && (gstate_c.textureFullAlpha || !gstate.isTextureAlphaUsed()))
			return true;  // If alpha is full, it doesn't matter what the ref value is.
		return gstate.getAlphaTestRef() == 0;

		// Non-zero check. If we have no depth testing (and thus no depth writing), and an alpha func that will result in no change if zero alpha, get rid of the alpha test.
		// Speeds up Lumines by a LOT on PowerVR.
	case GE_COMP_NOTEQUAL:
		if (gstate.getAlphaTestRef() == 255) {
			// Likely to be rare. Let's just skip the vertexFullAlpha optimization here instead of adding
			// complicated code to discard the draw or whatnot.
			return false;
		}
		// Fallthrough on purpose

	case GE_COMP_GREATER:
	{
		// If the texture and vertex only use 1.0 alpha, then the ref value doesn't matter.
		if (gstate_c.vertexFullAlpha && (gstate_c.textureFullAlpha || !gstate.isTextureAlphaUsed()))
			return true;
		return gstate.getAlphaTestRef() == 0 && !NeedsTestDiscard();
	}

	case GE_COMP_LEQUAL:
		return gstate.getAlphaTestRef() == 255;

	case GE_COMP_EQUAL:
	case GE_COMP_LESS:
		return false;

	default:
		return false;
	}
}

bool IsAlphaTestAgainstZero() {
	return gstate.getAlphaTestRef() == 0 && gstate.getAlphaTestMask() == 0xFF;
}

bool IsColorTestAgainstZero() {
	return gstate.getColorTestRef() == 0 && gstate.getColorTestMask() == 0xFFFFFF;
}

bool IsColorTestTriviallyTrue() {
	switch (gstate.getColorTestFunction()) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
	case GE_COMP_NOTEQUAL:
		return false;
	default:
		return false;
	}
}

const bool nonAlphaSrcFactors[16] = {
	true,  // GE_SRCBLEND_DSTCOLOR,
	true,  // GE_SRCBLEND_INVDSTCOLOR,
	false, // GE_SRCBLEND_SRCALPHA,
	false, // GE_SRCBLEND_INVSRCALPHA,
	true,  // GE_SRCBLEND_DSTALPHA,
	true,  // GE_SRCBLEND_INVDSTALPHA,
	false, // GE_SRCBLEND_DOUBLESRCALPHA,
	false, // GE_SRCBLEND_DOUBLEINVSRCALPHA,
	true,  // GE_SRCBLEND_DOUBLEDSTALPHA,
	true,  // GE_SRCBLEND_DOUBLEINVDSTALPHA,
	true,  // GE_SRCBLEND_FIXA,
	true,
	true,
	true,
	true,
	true,
};

const bool nonAlphaDestFactors[16] = {
	true,  // GE_DSTBLEND_SRCCOLOR,
	true,  // GE_DSTBLEND_INVSRCCOLOR,
	false, // GE_DSTBLEND_SRCALPHA,
	false, // GE_DSTBLEND_INVSRCALPHA,
	true,  // GE_DSTBLEND_DSTALPHA,
	true,  // GE_DSTBLEND_INVDSTALPHA,
	false, // GE_DSTBLEND_DOUBLESRCALPHA,
	false, // GE_DSTBLEND_DOUBLEINVSRCALPHA,
	true,  // GE_DSTBLEND_DOUBLEDSTALPHA,
	true,  // GE_DSTBLEND_DOUBLEINVDSTALPHA,
	true,  // GE_DSTBLEND_FIXB,
	true,
	true,
	true,
	true,
	true,
};

ReplaceAlphaType ReplaceAlphaWithStencil(ReplaceBlendType replaceBlend) {
	if (IsStencilTestOutputDisabled() || gstate.isModeClear()) {
		return REPLACE_ALPHA_NO;
	}

	if (replaceBlend != REPLACE_BLEND_NO && replaceBlend != REPLACE_BLEND_COPY_FBO) {
		if (nonAlphaSrcFactors[gstate.getBlendFuncA()] && nonAlphaDestFactors[gstate.getBlendFuncB()]) {
			return REPLACE_ALPHA_YES;
		} else {
			if (gstate_c.Supports(GPU_SUPPORTS_DUALSOURCE_BLEND)) {
				return REPLACE_ALPHA_DUALSOURCE;
			} else {
				return REPLACE_ALPHA_NO;
			}
		}
	}

	return REPLACE_ALPHA_YES;
}

StencilValueType ReplaceAlphaWithStencilType() {
	switch (gstate.FrameBufFormat()) {
	case GE_FORMAT_565:
		// There's never a stencil value.  Maybe the right alpha is 1?
		return STENCIL_VALUE_ONE;

	case GE_FORMAT_5551:
		switch (gstate.getStencilOpZPass()) {
			// Technically, this should only ever use zero/one.
		case GE_STENCILOP_REPLACE:
			return (gstate.getStencilTestRef() & 0x80) != 0 ? STENCIL_VALUE_ONE : STENCIL_VALUE_ZERO;

			// Decrementing always zeros, since there's only one bit.
		case GE_STENCILOP_DECR:
		case GE_STENCILOP_ZERO:
			return STENCIL_VALUE_ZERO;

			// Incrementing always fills, since there's only one bit.
		case GE_STENCILOP_INCR:
			return STENCIL_VALUE_ONE;

		case GE_STENCILOP_INVERT:
			return STENCIL_VALUE_INVERT;

		case GE_STENCILOP_KEEP:
			return STENCIL_VALUE_KEEP;
		}
		break;

	case GE_FORMAT_4444:
	case GE_FORMAT_8888:
	case GE_FORMAT_INVALID:
	case GE_FORMAT_DEPTH16:
		switch (gstate.getStencilOpZPass()) {
		case GE_STENCILOP_REPLACE:
			// TODO: Could detect zero here and force ZERO - less uniform updates?
			return STENCIL_VALUE_UNIFORM;

		case GE_STENCILOP_ZERO:
			return STENCIL_VALUE_ZERO;

		case GE_STENCILOP_DECR:
			return gstate.FrameBufFormat() == GE_FORMAT_4444 ? STENCIL_VALUE_DECR_4 : STENCIL_VALUE_DECR_8;

		case GE_STENCILOP_INCR:
			return gstate.FrameBufFormat() == GE_FORMAT_4444 ? STENCIL_VALUE_INCR_4 : STENCIL_VALUE_INCR_8;

		case GE_STENCILOP_INVERT:
			return STENCIL_VALUE_INVERT;

		case GE_STENCILOP_KEEP:
			return STENCIL_VALUE_KEEP;
		}
		break;
	}

	return STENCIL_VALUE_KEEP;
}

ReplaceBlendType ReplaceBlendWithShader(bool allowFramebufferRead, GEBufferFormat bufferFormat) {
	if (!gstate.isAlphaBlendEnabled() || gstate.isModeClear()) {
		return REPLACE_BLEND_NO;
	}

	GEBlendMode eq = gstate.getBlendEq();
	// Let's get the non-factor modes out of the way first.
	switch (eq) {
	case GE_BLENDMODE_ABSDIFF:
		return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;

	case GE_BLENDMODE_MIN:
	case GE_BLENDMODE_MAX:
		if (gstate_c.Supports(GPU_SUPPORTS_BLEND_MINMAX)) {
			return REPLACE_BLEND_STANDARD;
		} else {
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;
		}

	default:
		break;
	}

	GEBlendSrcFactor funcA = gstate.getBlendFuncA();
	GEBlendDstFactor funcB = gstate.getBlendFuncB();

	switch (funcA) {
	case GE_SRCBLEND_DOUBLESRCALPHA:
	case GE_SRCBLEND_DOUBLEINVSRCALPHA:
		// 2x alpha in the source function and not in the dest = source color doubling.
		// Even dest alpha is safe, since we're moving the * 2.0 into the src color.
		switch (funcB) {
		case GE_DSTBLEND_SRCCOLOR:
		case GE_DSTBLEND_INVSRCCOLOR:
			// When inversing, alpha clamping isn't an issue.
			if (funcA == GE_SRCBLEND_DOUBLEINVSRCALPHA)
				return REPLACE_BLEND_2X_ALPHA;
			// Can't double, we need the source color to be correct.
			// Doubling only alpha would clamp the src alpha incorrectly.
			return !allowFramebufferRead ? REPLACE_BLEND_2X_ALPHA : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLEDSTALPHA:
		case GE_DSTBLEND_DOUBLEINVDSTALPHA:
			if (bufferFormat == GE_FORMAT_565)
				return REPLACE_BLEND_2X_ALPHA;
			return !allowFramebufferRead ? REPLACE_BLEND_2X_ALPHA : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLESRCALPHA:
			// We can't technically do this correctly (due to clamping) without reading the dst color.
			// Using a copy isn't accurate either, though, when there's overlap.
			if (gstate_c.Supports(GPU_SUPPORTS_ANY_FRAMEBUFFER_FETCH))
				return !allowFramebufferRead ? REPLACE_BLEND_PRE_SRC_2X_ALPHA : REPLACE_BLEND_COPY_FBO;
			return REPLACE_BLEND_PRE_SRC_2X_ALPHA;

		case GE_DSTBLEND_DOUBLEINVSRCALPHA:
			// For the inverse, doubling alpha is safe, because it will clamp correctly.
			return REPLACE_BLEND_PRE_SRC_2X_ALPHA;

		case GE_DSTBLEND_SRCALPHA:
		case GE_DSTBLEND_INVSRCALPHA:
		case GE_DSTBLEND_DSTALPHA:
		case GE_DSTBLEND_INVDSTALPHA:
		case GE_DSTBLEND_FIXB:
		default:
			// TODO: Could use vertexFullAlpha, but it's not calculated yet.
			// This outputs the original alpha for the dest factor.
			return REPLACE_BLEND_PRE_SRC;
		}

	case GE_SRCBLEND_DOUBLEDSTALPHA:
		switch (funcB) {
		case GE_DSTBLEND_SRCCOLOR:
		case GE_DSTBLEND_INVSRCCOLOR:
			if (bufferFormat == GE_FORMAT_565) {
				// Dest alpha should be zero.
				return REPLACE_BLEND_STANDARD;
			}
			// Can't double, we need the source color to be correct.
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLEDSTALPHA:
		case GE_DSTBLEND_DOUBLEINVDSTALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				// Both blend factors are 0 or 1, no need to read it, since it's known.
				// Doubling will have no effect here.
				return REPLACE_BLEND_STANDARD;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_2X_SRC : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLESRCALPHA:
		case GE_DSTBLEND_DOUBLEINVSRCALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_2X_ALPHA;
			}
			// Double both src (for dst alpha) and alpha (for dst factor.)
			// But to be accurate (clamping), we need to read the dst color.
			return !allowFramebufferRead ? REPLACE_BLEND_PRE_SRC_2X_ALPHA : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_SRCALPHA:
		case GE_DSTBLEND_INVSRCALPHA:
		case GE_DSTBLEND_DSTALPHA:
		case GE_DSTBLEND_INVDSTALPHA:
		case GE_DSTBLEND_FIXB:
		default:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_STANDARD;
			}
			// We can't technically do this correctly (due to clamping) without reading the dst alpha.
			return !allowFramebufferRead ? REPLACE_BLEND_2X_SRC : REPLACE_BLEND_COPY_FBO;
		}

	case GE_SRCBLEND_DOUBLEINVDSTALPHA:
		// Inverse double dst alpha is tricky.  Doubling the src color is probably the wrong direction,
		// halving might be more correct.  We really need to read the dst color.
		switch (funcB) {
		case GE_DSTBLEND_SRCCOLOR:
		case GE_DSTBLEND_INVSRCCOLOR:
		case GE_DSTBLEND_DOUBLEDSTALPHA:
		case GE_DSTBLEND_DOUBLEINVDSTALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_STANDARD;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLESRCALPHA:
		case GE_DSTBLEND_DOUBLEINVSRCALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_2X_ALPHA;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_2X_ALPHA : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_SRCALPHA:
		case GE_DSTBLEND_INVSRCALPHA:
		case GE_DSTBLEND_DSTALPHA:
		case GE_DSTBLEND_INVDSTALPHA:
		case GE_DSTBLEND_FIXB:
		default:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_STANDARD;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;
		}

	case GE_SRCBLEND_FIXA:
	default:
		switch (funcB) {
		case GE_DSTBLEND_DOUBLESRCALPHA:
			// Can't safely double alpha, will clamp.
			return !allowFramebufferRead ? REPLACE_BLEND_2X_ALPHA : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_DOUBLEINVSRCALPHA:
			// Doubling alpha is safe for the inverse, will clamp to zero either way.
			return REPLACE_BLEND_2X_ALPHA;

		case GE_DSTBLEND_DOUBLEDSTALPHA:
		case GE_DSTBLEND_DOUBLEINVDSTALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_STANDARD;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;

		case GE_DSTBLEND_FIXB:
		default:
			if (gstate.getFixA() == 0xFFFFFF && gstate.getFixB() == 0x000000) {
				// Some games specify this.  Some cards may prefer blending off entirely.
				return REPLACE_BLEND_NO;
			} else if (gstate.getFixA() == 0xFFFFFF || gstate.getFixA() == 0x000000 || gstate.getFixB() == 0xFFFFFF || gstate.getFixB() == 0x000000) {
				return REPLACE_BLEND_STANDARD;
			} else {
				// Multiply the src color in the shader, that way it's always accurate.
				return REPLACE_BLEND_PRE_SRC;
			}

		case GE_DSTBLEND_SRCCOLOR:
		case GE_DSTBLEND_INVSRCCOLOR:
		case GE_DSTBLEND_SRCALPHA:
		case GE_DSTBLEND_INVSRCALPHA:
		case GE_DSTBLEND_DSTALPHA:
		case GE_DSTBLEND_INVDSTALPHA:
			return REPLACE_BLEND_STANDARD;
		}

	case GE_SRCBLEND_DSTCOLOR:
	case GE_SRCBLEND_INVDSTCOLOR:
	case GE_SRCBLEND_SRCALPHA:
	case GE_SRCBLEND_INVSRCALPHA:
	case GE_SRCBLEND_DSTALPHA:
	case GE_SRCBLEND_INVDSTALPHA:
		switch (funcB) {
		case GE_DSTBLEND_DOUBLESRCALPHA:
			if (funcA == GE_SRCBLEND_SRCALPHA || funcA == GE_SRCBLEND_INVSRCALPHA) {
				// Can't safely double alpha, will clamp.  However, a copy may easily be worse due to overlap.
				if (gstate_c.Supports(GPU_SUPPORTS_ANY_FRAMEBUFFER_FETCH))
					return !allowFramebufferRead ? REPLACE_BLEND_PRE_SRC_2X_ALPHA : REPLACE_BLEND_COPY_FBO;
				return REPLACE_BLEND_PRE_SRC_2X_ALPHA;
			} else {
				// This means dst alpha/color is used in the src factor.
				// Unfortunately, copying here causes overlap problems in Silent Hill games (it seems?)
				// We will just hope that doubling alpha for the dst factor will not clamp too badly.
				if (gstate_c.Supports(GPU_SUPPORTS_ANY_FRAMEBUFFER_FETCH))
					return !allowFramebufferRead ? REPLACE_BLEND_2X_ALPHA : REPLACE_BLEND_COPY_FBO;
				return REPLACE_BLEND_2X_ALPHA;
			}

		case GE_DSTBLEND_DOUBLEINVSRCALPHA:
			// For inverse, things are simpler.  Clamping isn't an issue, as long as we avoid
			// messing with the other factor's components.
			if (funcA == GE_SRCBLEND_SRCALPHA || funcA == GE_SRCBLEND_INVSRCALPHA) {
				return REPLACE_BLEND_PRE_SRC_2X_ALPHA;
			}
			return REPLACE_BLEND_2X_ALPHA;

		case GE_DSTBLEND_DOUBLEDSTALPHA:
		case GE_DSTBLEND_DOUBLEINVDSTALPHA:
			if (bufferFormat == GE_FORMAT_565) {
				return REPLACE_BLEND_STANDARD;
			}
			return !allowFramebufferRead ? REPLACE_BLEND_STANDARD : REPLACE_BLEND_COPY_FBO;

		default:
			return REPLACE_BLEND_STANDARD;
		}
	}

	// Should never get here.
	return REPLACE_BLEND_STANDARD;
}

LogicOpReplaceType ReplaceLogicOpType() {
	if (!gstate_c.Supports(GPU_SUPPORTS_LOGIC_OP) && gstate.isLogicOpEnabled()) {
		switch (gstate.getLogicOp()) {
		case GE_LOGIC_COPY_INVERTED:
		case GE_LOGIC_AND_INVERTED:
		case GE_LOGIC_OR_INVERTED:
		case GE_LOGIC_NOR:
		case GE_LOGIC_NAND:
		case GE_LOGIC_EQUIV:
			return LOGICOPTYPE_INVERT;
		case GE_LOGIC_INVERTED:
			return LOGICOPTYPE_ONE;
		case GE_LOGIC_SET:
			return LOGICOPTYPE_ONE;
		default:
			return LOGICOPTYPE_NORMAL;
		}
	}
	return LOGICOPTYPE_NORMAL;
}

static const float DEPTH_SLICE_FACTOR_HIGH = 4.0f;
static const float DEPTH_SLICE_FACTOR_16BIT = 256.0f;

float DepthSliceFactor() {
	if (gstate_c.Supports(GPU_SCALE_DEPTH_FROM_24BIT_TO_16BIT)) {
		return DEPTH_SLICE_FACTOR_16BIT;
	}
	if (gstate_c.Supports(GPU_SUPPORTS_DEPTH_CLAMP)) {
		return 1.0f;
	}
	return DEPTH_SLICE_FACTOR_HIGH;
}

// This is used for float values which might not be integers, but are in the integer scale of 65535.
float ToScaledDepthFromIntegerScale(float z) {
	if (!gstate_c.Supports(GPU_SUPPORTS_ACCURATE_DEPTH)) {
		return z * (1.0f / 65535.0f);
	}

	const float depthSliceFactor = DepthSliceFactor();
	if (gstate_c.Supports(GPU_SCALE_DEPTH_FROM_24BIT_TO_16BIT)) {
		const double doffset = 0.5 * (depthSliceFactor - 1.0) * (1.0 / depthSliceFactor);
		// Use one bit for each value, rather than 1.0 / (25535.0 * 256.0).
		return (float)((double)z * (1.0 / 16777215.0) + doffset);
	} else {
		const float offset = 0.5f * (depthSliceFactor - 1.0f) * (1.0f / depthSliceFactor);
		return z * (1.0f / depthSliceFactor) * (1.0f / 65535.0f) + offset;
	}
}

// See struct DepthScaleFactors for how to apply.
DepthScaleFactors GetDepthScaleFactors() {
	DepthScaleFactors factors;
	if (!gstate_c.Supports(GPU_SUPPORTS_ACCURATE_DEPTH)) {
		factors.offset = 0;
		factors.scale = 65535.0f;
		return factors;
	}

	const float depthSliceFactor = DepthSliceFactor();
	const float offset = 0.5f * (depthSliceFactor - 1.0f) * (1.0f / depthSliceFactor);
	factors.scale = depthSliceFactor * 65535.0f;
	factors.offset = offset;
	return factors;
}

void ConvertViewportAndScissor(bool useBufferedRendering, float renderWidth, float renderHeight, int bufferWidth, int bufferHeight, ViewportAndScissor &out) {
	bool throughmode = gstate.isModeThrough();
	out.dirtyProj = false;
	out.dirtyDepth = false;

	float renderWidthFactor, renderHeightFactor;
	float renderX = 0.0f, renderY = 0.0f;
	float displayOffsetX, displayOffsetY;
	if (useBufferedRendering) {
		displayOffsetX = 0.0f;
		displayOffsetY = 0.0f;
		renderWidthFactor = (float)renderWidth / (float)bufferWidth;
		renderHeightFactor = (float)renderHeight / (float)bufferHeight;
	} else {
		float pixelW = PSP_CoreParameter().pixelWidth;
		float pixelH = PSP_CoreParameter().pixelHeight;
		FRect frame = GetScreenFrame(pixelW, pixelH);
		FRect rc;
		CenterDisplayOutputRect(&rc, 480, 272, frame, ROTATION_LOCKED_HORIZONTAL);
		displayOffsetX = rc.x;
		displayOffsetY = rc.y;
		renderWidth = rc.w;
		renderHeight = rc.h;
		renderWidthFactor = renderWidth / 480.0f;
		renderHeightFactor = renderHeight / 272.0f;
	}

	renderX = gstate_c.curRTOffsetX;

	// Scissor
	int scissorX1 = gstate.getScissorX1();
	int scissorY1 = gstate.getScissorY1();
	int scissorX2 = gstate.getScissorX2() + 1;
	int scissorY2 = gstate.getScissorY2() + 1;

	out.scissorEnable = true;
	if (scissorX2 < scissorX1 || scissorY2 < scissorY1) {
		out.scissorX = 0;
		out.scissorY = 0;
		out.scissorW = 0;
		out.scissorH = 0;
	} else {
		out.scissorX = (renderX * renderWidthFactor) + displayOffsetX + scissorX1 * renderWidthFactor;
		out.scissorY = (renderY * renderHeightFactor) + displayOffsetY + scissorY1 * renderHeightFactor;
		out.scissorW = (scissorX2 - scissorX1) * renderWidthFactor;
		out.scissorH = (scissorY2 - scissorY1) * renderHeightFactor;
	}

	int curRTWidth = gstate_c.curRTWidth;
	int curRTHeight = gstate_c.curRTHeight;

	float offsetX = gstate.getOffsetX();
	float offsetY = gstate.getOffsetY();

	if (throughmode) {
		out.viewportX = renderX * renderWidthFactor + displayOffsetX;
		out.viewportY = renderY * renderHeightFactor + displayOffsetY;
		out.viewportW = curRTWidth * renderWidthFactor;
		out.viewportH = curRTHeight * renderHeightFactor;
		out.depthRangeMin = ToScaledDepthFromIntegerScale(0);
		out.depthRangeMax = ToScaledDepthFromIntegerScale(65536);
	} else {
		// These we can turn into a glViewport call, offset by offsetX and offsetY. Math after.
		float vpXScale = gstate.getViewportXScale();
		float vpXCenter = gstate.getViewportXCenter();
		float vpYScale = gstate.getViewportYScale();
		float vpYCenter = gstate.getViewportYCenter();

		// The viewport transform appears to go like this:
		// Xscreen = -offsetX + vpXCenter + vpXScale * Xview
		// Yscreen = -offsetY + vpYCenter + vpYScale * Yview
		// Zscreen = vpZCenter + vpZScale * Zview

		// The viewport is normally centered at 2048,2048 but can also be centered at other locations.
		// Offset is subtracted from the viewport center and is also set to values in those ranges, and is set so that the viewport will cover
		// the desired screen area ([0-480)x[0-272)), so 1808,1912.

		// This means that to get the analogue glViewport we must:
		float vpX0 = vpXCenter - offsetX - fabsf(vpXScale);
		float vpY0 = vpYCenter - offsetY - fabsf(vpYScale);
		gstate_c.vpWidth = vpXScale * 2.0f;
		gstate_c.vpHeight = vpYScale * 2.0f;

		float vpWidth = fabsf(gstate_c.vpWidth);
		float vpHeight = fabsf(gstate_c.vpHeight);

		float left = renderX + vpX0;
		float top = renderY + vpY0;
		float right = left + vpWidth;
		float bottom = top + vpHeight;

		float wScale = 1.0f;
		float xOffset = 0.0f;
		float hScale = 1.0f;
		float yOffset = 0.0f;

		// If we're within the bounds, we want clipping the viewport way.  So leave it be.
		{
			float overageLeft = std::max(-left, 0.0f);
			float overageRight = std::max(right - bufferWidth, 0.0f);

			// Expand viewport to cover scissor region. The viewport doesn't clip on the PSP.
			if (right < scissorX2) {
				overageRight -= scissorX2 - right;
			}
			if (left > scissorX1) {
				overageLeft += scissorX1 - left;
			}

			// Our center drifted by the difference in overages.
			float drift = overageRight - overageLeft;

			if (overageLeft != 0.0f || overageRight != 0.0f) {
				left += overageLeft;
				right -= overageRight;

				// Protect against the viewport being entirely outside the scissor.
				// Emit a tiny but valid viewport. Really, we should probably emit a flag to ignore draws.
				if (right <= left) {
					right = left + 1.0f;
				}

				wScale = vpWidth / (right - left);
				xOffset = drift / (right - left);
			}
		}

		{
			float overageTop = std::max(-top, 0.0f);
			float overageBottom = std::max(bottom - bufferHeight, 0.0f);

			// Expand viewport to cover scissor region. The viewport doesn't clip on the PSP.
			if (bottom < scissorY2) {
				overageBottom -= scissorY2 - bottom;
			}
			if (top > scissorY1) {
				overageTop += scissorY1 - top;
			}
			// Our center drifted by the difference in overages.
			float drift = overageBottom - overageTop;

			if (overageTop != 0.0f || overageBottom != 0.0f) {
				top += overageTop;
				bottom -= overageBottom;

				// Protect against the viewport being entirely outside the scissor.
				// Emit a tiny but valid  viewport. Really, we should probably emit a flag to ignore draws.
				if (bottom <= top) {
					bottom = top + 1.0f;
				}

				hScale = vpHeight / (bottom - top);
				yOffset = drift / (bottom - top);
			}
		}

		out.viewportX = left * renderWidthFactor + displayOffsetX;
		out.viewportY = top * renderHeightFactor + displayOffsetY;
		out.viewportW = (right - left) * renderWidthFactor;
		out.viewportH = (bottom - top) * renderHeightFactor;

		// The depth viewport parameters are the same, but we handle it a bit differently.
		// When clipping is enabled, depth is clamped to [0, 65535].  And minz/maxz discard.
		// So, we apply the depth range as minz/maxz, and transform for the viewport.
		float vpZScale = gstate.getViewportZScale();
		float vpZCenter = gstate.getViewportZCenter();
		// TODO: This clip the entire draw if minz > maxz.
		float minz = gstate.getDepthRangeMin();
		float maxz = gstate.getDepthRangeMax();

		if (gstate.isDepthClampEnabled() && (minz == 0 || maxz == 65535)) {
			// Here, we should "clamp."  But clamping per fragment would be slow.
			// So, instead, we just increase the available range and hope.
			// If depthSliceFactor is 4, it means (75% / 2) of the depth lies in each direction.
			float fullDepthRange = 65535.0f * (DepthSliceFactor() - 1.0f) * (1.0f / 2.0f);
			if (minz == 0) {
				minz -= fullDepthRange;
			}
			if (maxz == 65535) {
				maxz += fullDepthRange;
			}
		}
		// Okay.  So, in our shader, -1 will map to minz, and +1 will map to maxz.
		float halfActualZRange = (maxz - minz) * (1.0f / 2.0f);
		float zScale = halfActualZRange < std::numeric_limits<float>::epsilon() ? 1.0f : vpZScale / halfActualZRange;
		// This adjusts the center from halfActualZRange to vpZCenter.
		float zOffset = halfActualZRange < std::numeric_limits<float>::epsilon() ? 0.0f : (vpZCenter - (minz + halfActualZRange)) / halfActualZRange;

		if (!gstate_c.Supports(GPU_SUPPORTS_ACCURATE_DEPTH)) {
			zScale = 1.0f;
			zOffset = 0.0f;
			out.depthRangeMin = ToScaledDepthFromIntegerScale(vpZCenter - vpZScale);
			out.depthRangeMax = ToScaledDepthFromIntegerScale(vpZCenter + vpZScale);
		} else {
			out.depthRangeMin = ToScaledDepthFromIntegerScale(minz);
			out.depthRangeMax = ToScaledDepthFromIntegerScale(maxz);
		}

		// OpenGL will clamp these for us anyway, and Direct3D will error if not clamped.
		out.depthRangeMin = std::max(out.depthRangeMin, 0.0f);
		out.depthRangeMax = std::min(out.depthRangeMax, 1.0f);

		bool scaleChanged = gstate_c.vpWidthScale != wScale || gstate_c.vpHeightScale != hScale;
		bool offsetChanged = gstate_c.vpXOffset != xOffset || gstate_c.vpYOffset != yOffset;
		bool depthChanged = gstate_c.vpDepthScale != zScale || gstate_c.vpZOffset != zOffset;
		if (scaleChanged || offsetChanged || depthChanged) {
			gstate_c.vpWidthScale = wScale;
			gstate_c.vpHeightScale = hScale;
			gstate_c.vpDepthScale = zScale;
			gstate_c.vpXOffset = xOffset;
			gstate_c.vpYOffset = yOffset;
			gstate_c.vpZOffset = zOffset;
			out.dirtyProj = true;
			out.dirtyDepth = depthChanged;
		}
	}
}

static const BlendFactor genericALookup[11] = {
	BlendFactor::DST_COLOR,
	BlendFactor::ONE_MINUS_DST_COLOR,
	BlendFactor::SRC_ALPHA,
	BlendFactor::ONE_MINUS_SRC_ALPHA,
	BlendFactor::DST_ALPHA,
	BlendFactor::ONE_MINUS_DST_ALPHA,
	BlendFactor::SRC_ALPHA,			// GE_SRCBLEND_DOUBLESRCALPHA
	BlendFactor::ONE_MINUS_SRC_ALPHA,		// GE_SRCBLEND_DOUBLEINVSRCALPHA
	BlendFactor::DST_ALPHA,			// GE_SRCBLEND_DOUBLEDSTALPHA
	BlendFactor::ONE_MINUS_DST_ALPHA,		// GE_SRCBLEND_DOUBLEINVDSTALPHA
	BlendFactor::CONSTANT_COLOR,		// FIXA
};

static const BlendFactor genericBLookup[11] = {
	BlendFactor::SRC_COLOR,
	BlendFactor::ONE_MINUS_SRC_COLOR,
	BlendFactor::SRC_ALPHA,
	BlendFactor::ONE_MINUS_SRC_ALPHA,
	BlendFactor::DST_ALPHA,
	BlendFactor::ONE_MINUS_DST_ALPHA,
	BlendFactor::SRC_ALPHA,			// GE_SRCBLEND_DOUBLESRCALPHA
	BlendFactor::ONE_MINUS_SRC_ALPHA,		// GE_SRCBLEND_DOUBLEINVSRCALPHA
	BlendFactor::DST_ALPHA,			// GE_SRCBLEND_DOUBLEDSTALPHA
	BlendFactor::ONE_MINUS_DST_ALPHA,		// GE_SRCBLEND_DOUBLEINVDSTALPHA
	BlendFactor::CONSTANT_COLOR,		// FIXB
};

static const BlendEq eqLookupNoMinMax[] = {
	BlendEq::ADD,
	BlendEq::SUBTRACT,
	BlendEq::REVERSE_SUBTRACT,
	BlendEq::ADD,			// GE_BLENDMODE_MIN
	BlendEq::ADD,			// GE_BLENDMODE_MAX
	BlendEq::ADD,			// GE_BLENDMODE_ABSDIFF
};

static const BlendEq eqLookup[] = {
	BlendEq::ADD,
	BlendEq::SUBTRACT,
	BlendEq::REVERSE_SUBTRACT,
	BlendEq::MIN,			// GE_BLENDMODE_MIN
	BlendEq::MAX,			// GE_BLENDMODE_MAX
	BlendEq::MAX,			// GE_BLENDMODE_ABSDIFF
};

static BlendFactor toDualSource(BlendFactor blendfunc) {
	switch (blendfunc) {
	case BlendFactor::SRC_ALPHA:
		return BlendFactor::SRC1_ALPHA;
	case BlendFactor::ONE_MINUS_SRC_ALPHA:
		return BlendFactor::ONE_MINUS_SRC1_ALPHA;
	default:
		return blendfunc;
	}
}

static BlendFactor blendColor2Func(u32 fix, bool &approx) {
	if (fix == 0xFFFFFF)
		return BlendFactor::ONE;
	if (fix == 0)
		return BlendFactor::ZERO;

	// Otherwise, it's approximate if we pick ONE/ZERO.
	approx = true;

	const Vec3f fix3 = Vec3f::FromRGB(fix);
	if (fix3.x >= 0.99 && fix3.y >= 0.99 && fix3.z >= 0.99)
		return BlendFactor::ONE;
	else if (fix3.x <= 0.01 && fix3.y <= 0.01 && fix3.z <= 0.01)
		return BlendFactor::ZERO;
	return BlendFactor::INVALID;
}

// abs is a quagmire of compiler incompatibilities, so...
inline int iabs(int x) {
	return x >= 0 ? x : -x;
}

static inline bool blendColorSimilar(uint32_t a, uint32_t b, int margin = 25) {   // 25 ~= 0.1 * 255
	int diffx = iabs((a & 0xff) - (b & 0xff));
	int diffy = iabs(((a >> 8) & 0xff) - ((b >> 8) & 0xff));
	int diffz = iabs(((a >> 16) & 0xff) - ((b >> 16) & 0xff));
	if (diffx <= margin && diffy <= margin && diffz <= margin)
		return true;
	return false;
}

// Try to simulate some common logic ops.
static void ApplyLogicOp(BlendFactor &srcBlend, BlendFactor &dstBlend, BlendEq &blendEq) {
	// Note: our shader solution applies logic ops BEFORE blending, not correctly after.
	// This is however fine for the most common ones, like CLEAR/NOOP/SET, etc.
	if (!gstate_c.Supports(GPU_SUPPORTS_LOGIC_OP)) {
		if (gstate.isLogicOpEnabled()) {
			switch (gstate.getLogicOp()) {
			case GE_LOGIC_CLEAR:
				srcBlend = BlendFactor::ZERO;
				dstBlend = BlendFactor::ZERO;
				blendEq = BlendEq::ADD;
				break;
			case GE_LOGIC_AND:
			case GE_LOGIC_AND_REVERSE:
				WARN_LOG_REPORT_ONCE(d3dLogicOpAnd, G3D, "Unsupported AND logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_COPY:
				// This is the same as off.
				break;
			case GE_LOGIC_COPY_INVERTED:
				// Handled in the shader.
				break;
			case GE_LOGIC_AND_INVERTED:
			case GE_LOGIC_NOR:
			case GE_LOGIC_NAND:
			case GE_LOGIC_EQUIV:
				// Handled in the shader.
				WARN_LOG_REPORT_ONCE(d3dLogicOpAndInverted, G3D, "Attempted invert for logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_INVERTED:
				srcBlend = BlendFactor::ONE;
				dstBlend = BlendFactor::ONE;
				blendEq = BlendEq::SUBTRACT;
				WARN_LOG_REPORT_ONCE(d3dLogicOpInverted, G3D, "Attempted inverse for logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_NOOP:
				srcBlend = BlendFactor::ZERO;
				dstBlend = BlendFactor::ONE;
				blendEq = BlendEq::ADD;
				break;
			case GE_LOGIC_XOR:
				WARN_LOG_REPORT_ONCE(d3dLogicOpOrXor, G3D, "Unsupported XOR logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_OR:
			case GE_LOGIC_OR_INVERTED:
				// Inverted in shader.
				dstBlend = BlendFactor::ONE;
				WARN_LOG_REPORT_ONCE(d3dLogicOpOr, G3D, "Attempted or for logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_OR_REVERSE:
				WARN_LOG_REPORT_ONCE(d3dLogicOpOrReverse, G3D, "Unsupported OR REVERSE logic op: %x", gstate.getLogicOp());
				break;
			case GE_LOGIC_SET:
				srcBlend = BlendFactor::ONE;
				dstBlend = BlendFactor::ONE;
				blendEq = BlendEq::ADD;
				WARN_LOG_REPORT_ONCE(d3dLogicOpSet, G3D, "Attempted set for logic op: %x", gstate.getLogicOp());
				break;
			}
		}
	}
}

// Try to simulate some common logic ops.
void ApplyStencilReplaceAndLogicOpIgnoreBlend(ReplaceAlphaType replaceAlphaWithStencil, GenericBlendState &blendState) {
	StencilValueType stencilType = STENCIL_VALUE_KEEP;
	if (replaceAlphaWithStencil == REPLACE_ALPHA_YES) {
		stencilType = ReplaceAlphaWithStencilType();
	}

	// Normally, we would add src + 0 with blending off, but the logic op may have us do differently.
	BlendFactor srcBlend = BlendFactor::ONE;
	BlendFactor dstBlend = BlendFactor::ZERO;
	BlendEq blendEq = BlendEq::ADD;
	ApplyLogicOp(srcBlend, dstBlend, blendEq);

	// We're not blending, but we may still want to "blend" for stencil.
	// This is only useful for INCR/DECR/INVERT.  Others can write directly.
	switch (stencilType) {
	case STENCIL_VALUE_INCR_4:
	case STENCIL_VALUE_INCR_8:
		// We'll add the incremented value output by the shader.
		blendState.enabled = true;
		blendState.setFactors(srcBlend, dstBlend, BlendFactor::ONE, BlendFactor::ONE);
		blendState.setEquation(blendEq, BlendEq::ADD);
		break;

	case STENCIL_VALUE_DECR_4:
	case STENCIL_VALUE_DECR_8:
		// We'll subtract the incremented value output by the shader.
		blendState.enabled = true;
		blendState.setFactors(srcBlend, dstBlend, BlendFactor::ONE, BlendFactor::ONE);
		blendState.setEquation(blendEq, BlendEq::SUBTRACT);
		break;

	case STENCIL_VALUE_INVERT:
		// The shader will output one, and reverse subtracting will essentially invert.
		blendState.enabled = true;
		blendState.setFactors(srcBlend, dstBlend, BlendFactor::ONE, BlendFactor::ONE);
		blendState.setEquation(blendEq, BlendEq::REVERSE_SUBTRACT);
		break;

	default:
		if (srcBlend == BlendFactor::ONE && dstBlend == BlendFactor::ZERO && blendEq == BlendEq::ADD) {
			blendState.enabled = false;
		} else {
			blendState.enabled = true;
			blendState.setFactors(srcBlend, dstBlend, BlendFactor::ONE, BlendFactor::ZERO);
			blendState.setEquation(blendEq, BlendEq::ADD);
		}
		break;
	}
}

bool IsColorWriteMaskComplex(bool allowFramebufferRead) {
	// Restrict to Outrun temporarily (by uglily reusing the ReinterpretFramebuffers flag)
	// This check must match the one in ConvertMaskState.
	if (!allowFramebufferRead || !PSP_CoreParameter().compat.flags().ShaderColorBitmask) {
		// Don't have a choice - we'll make do but it won't always be right.
		return false;
	}

	uint32_t colorMask = (gstate.pmskc & 0xFFFFFF) | (gstate.pmska << 24);

	for (int i = 0; i < 4; i++) {
		switch (colorMask & 0xFF) {
		case 0x0:
		case 0xFF:
			break;
		default:
			return true;
		}
		colorMask >>= 8;
	}
	return false;
}

// If we can we emulate the colorMask by simply toggling the full R G B A masks offered
// by modern hardware, we do that. This is 99.9% of the time.
// When that's not enough, we fall back on a technique similar to shader blending,
// we read from the framebuffer (or a copy of it).
void ConvertMaskState(GenericMaskState &maskState, bool allowFramebufferRead) {
	// Invert to convert masks from the PSP's format where 1 is don't draw to PC where 1 is draw.
	uint32_t colorMask = ~((gstate.pmskc & 0xFFFFFF) | (gstate.pmska << 24));

	maskState.applyFramebufferRead = false;
	for (int i = 0; i < 4; i++) {
		int channelMask = colorMask & 0xFF;
		switch (channelMask) {
		case 0x0:
			maskState.rgba[i] = false;
			break;
		case 0xFF:
			maskState.rgba[i] = true;
			break;
		default:
			if (allowFramebufferRead) {
				// Instead of just 'true', restrict shader bitmasks to Outrun temporarily.
				// TODO: This check must match the one in IsColorWriteMaskComplex.
				maskState.applyFramebufferRead = PSP_CoreParameter().compat.flags().ShaderColorBitmask;
				maskState.rgba[i] = true;
			} else {
				// Use the old heuristic.
				maskState.rgba[i] = channelMask >= 128;
			}
		}
		colorMask >>= 8;
	}

	// Let's not write to alpha if stencil isn't enabled.
	if (IsStencilTestOutputDisabled()) {
		maskState.rgba[3] = false;
	} else if (ReplaceAlphaWithStencilType() == STENCIL_VALUE_KEEP) {
		// If the stencil type is set to KEEP, we shouldn't write to the stencil/alpha channel.
		maskState.rgba[3] = false;
	}
}

// Called even if AlphaBlendEnable == false - it also deals with stencil-related blend state.
void ConvertBlendState(GenericBlendState &blendState, bool allowFramebufferRead) {
	// Blending is a bit complex to emulate.  This is due to several reasons:
	//
	//  * Doubled blend modes (src, dst, inversed) aren't supported in OpenGL.
	//    If possible, we double the src color or src alpha in the shader to account for these.
	//    These may clip incorrectly, so we avoid unfortunately.
	//  * OpenGL only has one arbitrary fixed color.  We premultiply the other in the shader.
	//  * The written output alpha should actually be the stencil value.  Alpha is not written.
	//
	// If we can't apply blending, we make a copy of the framebuffer and do it manually.

	blendState.applyFramebufferRead = false;
	blendState.dirtyShaderBlendFixValues = false;
	blendState.useBlendColor = false;
	blendState.replaceAlphaWithStencil = REPLACE_ALPHA_NO;

	ReplaceBlendType replaceBlend = ReplaceBlendWithShader(allowFramebufferRead, gstate.FrameBufFormat());
	ReplaceAlphaType replaceAlphaWithStencil = ReplaceAlphaWithStencil(replaceBlend);
	bool usePreSrc = false;

	switch (replaceBlend) {
	case REPLACE_BLEND_NO:
		blendState.resetFramebufferRead = true;
		// We may still want to do something about stencil -> alpha.
		ApplyStencilReplaceAndLogicOpIgnoreBlend(replaceAlphaWithStencil, blendState);
		return;

	case REPLACE_BLEND_COPY_FBO:
		blendState.applyFramebufferRead = true;
		blendState.resetFramebufferRead = false;
		blendState.replaceAlphaWithStencil = replaceAlphaWithStencil;
		break;

	case REPLACE_BLEND_PRE_SRC:
	case REPLACE_BLEND_PRE_SRC_2X_ALPHA:
		usePreSrc = true;
		break;

	case REPLACE_BLEND_STANDARD:
	case REPLACE_BLEND_2X_ALPHA:
	case REPLACE_BLEND_2X_SRC:
		break;
	}

	blendState.enabled = true;
	blendState.resetFramebufferRead = true;

	const GEBlendMode blendFuncEq = gstate.getBlendEq();
	GEBlendSrcFactor blendFuncA = gstate.getBlendFuncA();
	GEBlendDstFactor blendFuncB = gstate.getBlendFuncB();
	const u32 fixA = gstate.getFixA();
	const u32 fixB = gstate.getFixB();

	if (blendFuncA > GE_SRCBLEND_FIXA)
		blendFuncA = GE_SRCBLEND_FIXA;
	if (blendFuncB > GE_DSTBLEND_FIXB)
		blendFuncB = GE_DSTBLEND_FIXB;

	int constantAlpha = 255;
	BlendFactor constantAlphaGL = BlendFactor::ONE;
	if (!IsStencilTestOutputDisabled() && replaceAlphaWithStencil == REPLACE_ALPHA_NO) {
		switch (ReplaceAlphaWithStencilType()) {
		case STENCIL_VALUE_UNIFORM:
			constantAlpha = gstate.getStencilTestRef();
			break;

		case STENCIL_VALUE_INCR_4:
		case STENCIL_VALUE_DECR_4:
			constantAlpha = 16;
			break;

		case STENCIL_VALUE_INCR_8:
		case STENCIL_VALUE_DECR_8:
			constantAlpha = 1;
			break;

		default:
			break;
		}

		// Otherwise it will stay GL_ONE.
		if (constantAlpha <= 0) {
			constantAlphaGL = BlendFactor::ZERO;
		} else if (constantAlpha < 255) {
			constantAlphaGL = BlendFactor::CONSTANT_ALPHA;
		}
	}

	// Shortcut by using GL_ONE where possible, no need to set blendcolor
	bool approxFuncA = false;
	BlendFactor glBlendFuncA = blendFuncA == GE_SRCBLEND_FIXA ? blendColor2Func(fixA, approxFuncA) : genericALookup[blendFuncA];
	bool approxFuncB = false;
	BlendFactor glBlendFuncB = blendFuncB == GE_DSTBLEND_FIXB ? blendColor2Func(fixB, approxFuncB) : genericBLookup[blendFuncB];

	if (gstate.FrameBufFormat() == GE_FORMAT_565) {
		if (blendFuncA == GE_SRCBLEND_DSTALPHA || blendFuncA == GE_SRCBLEND_DOUBLEDSTALPHA) {
			glBlendFuncA = BlendFactor::ZERO;
		}
		if (blendFuncA == GE_SRCBLEND_INVDSTALPHA || blendFuncA == GE_SRCBLEND_DOUBLEINVDSTALPHA) {
			glBlendFuncA = BlendFactor::ONE;
		}
		if (blendFuncB == GE_DSTBLEND_DSTALPHA || blendFuncB == GE_DSTBLEND_DOUBLEDSTALPHA) {
			glBlendFuncB = BlendFactor::ZERO;
		}
		if (blendFuncB == GE_DSTBLEND_INVDSTALPHA || blendFuncB == GE_DSTBLEND_DOUBLEINVDSTALPHA) {
			glBlendFuncB = BlendFactor::ONE;
		}
	}

	if (usePreSrc) {
		glBlendFuncA = BlendFactor::ONE;
		// Need to pull in the fixed color. TODO: If it hasn't changed, no need to dirty.
		if (blendFuncA == GE_SRCBLEND_FIXA) {
			blendState.dirtyShaderBlendFixValues = true;
		}
	}

	if (replaceAlphaWithStencil == REPLACE_ALPHA_DUALSOURCE) {
		glBlendFuncA = toDualSource(glBlendFuncA);
		glBlendFuncB = toDualSource(glBlendFuncB);
	}

	if (blendFuncA == GE_SRCBLEND_FIXA || blendFuncB == GE_DSTBLEND_FIXB) {
		if (glBlendFuncA == BlendFactor::INVALID && glBlendFuncB != BlendFactor::INVALID) {
			// Can use blendcolor trivially.
			blendState.setBlendColor(fixA, constantAlpha);
			glBlendFuncA = BlendFactor::CONSTANT_COLOR;
		} else if (glBlendFuncA != BlendFactor::INVALID && glBlendFuncB == BlendFactor::INVALID) {
			// Can use blendcolor trivially.
			blendState.setBlendColor(fixB, constantAlpha);
			glBlendFuncB = BlendFactor::CONSTANT_COLOR;
		} else if (glBlendFuncA == BlendFactor::INVALID && glBlendFuncB == BlendFactor::INVALID) {
			if (blendColorSimilar(fixA, 0xFFFFFF ^ fixB)) {
				glBlendFuncA = BlendFactor::CONSTANT_COLOR;
				glBlendFuncB = BlendFactor::ONE_MINUS_CONSTANT_COLOR;
				blendState.setBlendColor(fixA, constantAlpha);
			} else if (blendColorSimilar(fixA, fixB)) {
				glBlendFuncA = BlendFactor::CONSTANT_COLOR;
				glBlendFuncB = BlendFactor::CONSTANT_COLOR;
				blendState.setBlendColor(fixA, constantAlpha);
			} else {
				DEBUG_LOG(G3D, "ERROR INVALID blendcolorstate: FixA=%06x FixB=%06x FuncA=%i FuncB=%i", fixA, fixB, blendFuncA, blendFuncB);
				// Let's approximate, at least.  Close is better than totally off.
				const bool nearZeroA = blendColorSimilar(fixA, 0, 64);
				const bool nearZeroB = blendColorSimilar(fixB, 0, 64);
				if (nearZeroA || blendColorSimilar(fixA, 0xFFFFFF, 64)) {
					glBlendFuncA = nearZeroA ? BlendFactor::ZERO : BlendFactor::ONE;
					glBlendFuncB = BlendFactor::CONSTANT_COLOR;
					blendState.setBlendColor(fixB, constantAlpha);
				} else {
					// We need to pick something.  Let's go with A as the fixed color.
					glBlendFuncA = BlendFactor::CONSTANT_COLOR;
					glBlendFuncB = nearZeroB ? BlendFactor::ZERO : BlendFactor::ONE;
					blendState.setBlendColor(fixA, constantAlpha);
				}
			}
		} else {
			// We optimized both, but that's probably not necessary, so let's pick one to be constant.
			if (blendFuncA == GE_SRCBLEND_FIXA && !usePreSrc && approxFuncA) {
				glBlendFuncA = BlendFactor::CONSTANT_COLOR;
				blendState.setBlendColor(fixA, constantAlpha);
			} else if (approxFuncB) {
				glBlendFuncB = BlendFactor::CONSTANT_COLOR;
				blendState.setBlendColor(fixB, constantAlpha);
			} else {
				if (constantAlphaGL == BlendFactor::CONSTANT_ALPHA) {
					blendState.defaultBlendColor(constantAlpha);
				}
			}
		}
	} else {
		if (constantAlphaGL == BlendFactor::CONSTANT_ALPHA) {
			blendState.defaultBlendColor(constantAlpha);
		}
	}

	// Some Android devices (especially old Mali, it seems) composite badly if there's alpha in the backbuffer.
	// So in non-buffered rendering, we will simply consider the dest alpha to be zero in blending equations.
#ifdef __ANDROID__
	if (g_Config.iRenderingMode == FB_NON_BUFFERED_MODE) {
		if (glBlendFuncA == BlendFactor::DST_ALPHA) glBlendFuncA = BlendFactor::ZERO;
		if (glBlendFuncB == BlendFactor::DST_ALPHA) glBlendFuncB = BlendFactor::ZERO;
		if (glBlendFuncA == BlendFactor::ONE_MINUS_DST_ALPHA) glBlendFuncA = BlendFactor::ONE;
		if (glBlendFuncB == BlendFactor::ONE_MINUS_DST_ALPHA) glBlendFuncB = BlendFactor::ONE;
	}
#endif

	// At this point, through all paths above, glBlendFuncA and glBlendFuncB will be set right somehow.
	BlendEq colorEq;
	if (gstate_c.Supports(GPU_SUPPORTS_BLEND_MINMAX)) {
		colorEq = eqLookup[blendFuncEq];
	} else {
		colorEq = eqLookupNoMinMax[blendFuncEq];
	}

	// Attempt to apply the logic op, if any.
	ApplyLogicOp(glBlendFuncA, glBlendFuncB, colorEq);

	// The stencil-to-alpha in fragment shader doesn't apply here (blending is enabled), and we shouldn't
	// do any blending in the alpha channel as that doesn't seem to happen on PSP.  So, we attempt to
	// apply the stencil to the alpha, since that's what should be stored.
	BlendEq alphaEq = BlendEq::ADD;
	if (replaceAlphaWithStencil != REPLACE_ALPHA_NO) {
		// Let the fragment shader take care of it.
		switch (ReplaceAlphaWithStencilType()) {
		case STENCIL_VALUE_INCR_4:
		case STENCIL_VALUE_INCR_8:
			// We'll add the increment value.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ONE);
			break;

		case STENCIL_VALUE_DECR_4:
		case STENCIL_VALUE_DECR_8:
			// Like add with a small value, but subtracting.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ONE);
			alphaEq = BlendEq::SUBTRACT;
			break;

		case STENCIL_VALUE_INVERT:
			// This will subtract by one, effectively inverting the bits.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ONE);
			alphaEq = BlendEq::REVERSE_SUBTRACT;
			break;

		default:
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ZERO);
			break;
		}
	} else if (!IsStencilTestOutputDisabled()) {
		StencilValueType stencilValue = ReplaceAlphaWithStencilType();
		if (stencilValue == STENCIL_VALUE_UNIFORM && constantAlpha == 0x00) {
			stencilValue = STENCIL_VALUE_ZERO;
		} else if (stencilValue == STENCIL_VALUE_UNIFORM && constantAlpha == 0xFF) {
			stencilValue = STENCIL_VALUE_ONE;
		}
		switch (stencilValue) {
		case STENCIL_VALUE_KEEP:
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ZERO, BlendFactor::ONE);
			break;
		case STENCIL_VALUE_ONE:
			// This won't give one but it's our best shot...
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ONE);
			break;
		case STENCIL_VALUE_ZERO:
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ZERO, BlendFactor::ZERO);
			break;
		case STENCIL_VALUE_UNIFORM:
			// This won't give a correct value (it multiplies) but it may be better than random values.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, constantAlphaGL, BlendFactor::ZERO);
			break;
		case STENCIL_VALUE_INCR_4:
		case STENCIL_VALUE_INCR_8:
			// This won't give a correct value always, but it will try to increase at least.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, constantAlphaGL, BlendFactor::ONE);
			break;
		case STENCIL_VALUE_DECR_4:
		case STENCIL_VALUE_DECR_8:
			// This won't give a correct value always, but it will try to decrease at least.
			blendState.setFactors(glBlendFuncA, glBlendFuncB, constantAlphaGL, BlendFactor::ONE);
			alphaEq = BlendEq::SUBTRACT;
			break;
		case STENCIL_VALUE_INVERT:
			blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ONE, BlendFactor::ONE);
			// If the output alpha is near 1, this will basically invert.  It's our best shot.
			alphaEq = BlendEq::REVERSE_SUBTRACT;
			break;
		}
	} else {
		// Retain the existing value when stencil testing is off.
		blendState.setFactors(glBlendFuncA, glBlendFuncB, BlendFactor::ZERO, BlendFactor::ONE);
	}

	blendState.setEquation(colorEq, alphaEq);
}

static void ConvertStencilFunc5551(GenericStencilFuncState &state) {
	// Flaws:
	// - INVERT should convert 1, 5, 0xFF to 0.  Currently it won't always.
	// - INCR twice shouldn't change the value.
	// - REPLACE should write 0 for 0x00 - 0x7F, and non-zero for 0x80 - 0xFF.
	// - Write mask may need double checking, but likely only the top bit matters.

	const bool usesRef = state.sFail == GE_STENCILOP_REPLACE || state.zFail == GE_STENCILOP_REPLACE || state.zPass == GE_STENCILOP_REPLACE;
	const u8 maskedRef = state.testRef & state.testMask;
	const u8 usedRef = (state.testRef & 0x80) != 0 ? 0xFF : 0x00;

	auto rewriteFunc = [&](GEComparison func, u8 ref) {
		// We can only safely rewrite if it doesn't use the ref, or if the ref is the same.
		if (!usesRef || usedRef == ref) {
			state.testFunc = func;
			state.testRef = ref;
			state.testMask = 0xFF;
		}
	};
	auto rewriteRef = [&](bool always) {
		state.testFunc = always ? GE_COMP_ALWAYS : GE_COMP_NEVER;
		if (usesRef) {
			// Rewrite the ref (for REPLACE) to 0x00 or 0xFF (the "best" values) if safe.
			// This will only be called if the test doesn't need the ref.
			state.testRef = usedRef;
			// Nuke the mask as well, since this is always/never, just for consistency.
			state.testMask = 0xFF;
		} else {
			// Not used, so let's make the ref 0xFF which is a useful value later.
			state.testRef = 0xFF;
			state.testMask = 0xFF;
		}
	};

	// For 5551, we treat any non-zero value in the buffer as 255.  Only zero is treated as zero.
	// See: https://github.com/hrydgard/ppsspp/pull/4150#issuecomment-26211193
	switch (state.testFunc) {
	case GE_COMP_NEVER:
	case GE_COMP_ALWAYS:
		// Fine as is.
		rewriteRef(state.testFunc == GE_COMP_ALWAYS);
		break;
	case GE_COMP_EQUAL: // maskedRef == maskedBuffer
		if (maskedRef == 0) {
			// Remove any mask, we might have bits less than 255 but that should not match.
			rewriteFunc(GE_COMP_EQUAL, 0);
		} else if (maskedRef == (0xFF & state.testMask) && state.testMask != 0) {
			// Equal to 255, for our buffer, means not equal to zero.
			rewriteFunc(GE_COMP_NOTEQUAL, 0);
		} else {
			// This should never pass, regardless of buffer value.  Only 0 and 255 are directly equal.
			rewriteRef(false);
		}
		break;
	case GE_COMP_NOTEQUAL: // maskedRef != maskedBuffer
		if (maskedRef == 0) {
			// Remove the mask, since our buffer might not be exactly 255.
			rewriteFunc(GE_COMP_NOTEQUAL, 0);
		} else if (maskedRef == (0xFF & state.testMask) && state.testMask != 0) {
			// The only value != 255 is 0, in our buffer.
			rewriteFunc(GE_COMP_EQUAL, 0);
		} else {
			// Every other value evaluates as not equal, always.
			rewriteRef(true);
		}
		break;
	case GE_COMP_LESS: // maskedRef < maskedBuffer
		if (maskedRef == (0xFF & state.testMask) && state.testMask != 0) {
			// No possible value is less than 255.
			rewriteRef(false);
		} else {
			// "0 < (0 or 255)" and "254 < (0 or 255)" can only work for non zero.
			rewriteFunc(GE_COMP_NOTEQUAL, 0);
		}
		break;
	case GE_COMP_LEQUAL: // maskedRef <= maskedBuffer
		if (maskedRef == 0) {
			// 0 is <= every possible value.
			rewriteRef(true);
		} else {
			// "1 <= (0 or 255)" and "255 <= (0 or 255)" simply mean, anything but zero.
			rewriteFunc(GE_COMP_NOTEQUAL, 0);
		}
		break;
	case GE_COMP_GREATER: // maskedRef > maskedBuffer
		if (maskedRef > 0) {
			// "1 > (0 or 255)" and "255 > (0 or 255)" can only match 0.
			rewriteFunc(GE_COMP_EQUAL, 0);
		} else {
			// 0 is never greater than any possible value.
			rewriteRef(false);
		}
		break;
	case GE_COMP_GEQUAL: // maskedRef >= maskedBuffer
		if (maskedRef == (0xFF & state.testMask) && state.testMask != 0) {
			// 255 is >= every possible value.
			rewriteRef(true);
		} else {
			// "0 >= (0 or 255)" and "254 >= "(0 or 255)" are the same, equal to zero.
			rewriteFunc(GE_COMP_EQUAL, 0);
		}
		break;
	}

	auto rewriteOps = [&](GEStencilOp from, GEStencilOp to) {
		if (state.sFail == from)
			state.sFail = to;
		if (state.zFail == from)
			state.zFail = to;
		if (state.zPass == from)
			state.zPass = to;
	};

	// Decrement always zeros, so let's rewrite those to be safe (even if it's not 1.)
	rewriteOps(GE_STENCILOP_DECR, GE_STENCILOP_ZERO);

	if (state.testFunc == GE_COMP_NOTEQUAL && state.testRef == 0 && state.testMask != 0) {
		// If it's != 0 (as optimized above), then we can rewrite INVERT to ZERO.
		// With 1 bit of stencil, INVERT != 0 can only make it 0.
		rewriteOps(GE_STENCILOP_INVERT, GE_STENCILOP_ZERO);
	}
	if (state.testFunc == GE_COMP_EQUAL && state.testRef == 0 && state.testMask != 0) {
		// If it's == 0 (as optimized above), then we can rewrite INCR to INVERT.
		// Otherwise we get 1, which we mostly handle, but won't INVERT correctly.
		rewriteOps(GE_STENCILOP_INCR, GE_STENCILOP_INVERT);
	}
	if (!usesRef && state.testRef == 0xFF) {
		// Safe to use REPLACE instead of INCR.
		rewriteOps(GE_STENCILOP_INCR, GE_STENCILOP_REPLACE);
	}
}

static void ConvertStencilMask5551(GenericStencilFuncState &state) {
	state.writeMask = state.writeMask >= 0x80 ? 0xff : 0x00;
}

void ConvertStencilFuncState(GenericStencilFuncState &state) {
	// The PSP's mask is reversed (bits not to write.)  Ignore enabled, used for clears too.
	state.writeMask = (~gstate.getStencilWriteMask()) & 0xFF;
	state.enabled = gstate.isStencilTestEnabled();
	if (!state.enabled) {
		if (gstate.FrameBufFormat() == GE_FORMAT_5551)
			ConvertStencilMask5551(state);
		return;
	}

	state.sFail = gstate.getStencilOpSFail();
	state.zFail = gstate.getStencilOpZFail();
	state.zPass = gstate.getStencilOpZPass();

	state.testFunc = gstate.getStencilTestFunction();
	state.testRef = gstate.getStencilTestRef();
	state.testMask = gstate.getStencilTestMask();

	switch (gstate.FrameBufFormat()) {
	case GE_FORMAT_565:
		state.writeMask = 0;
		break;

	case GE_FORMAT_5551:
		ConvertStencilMask5551(state);
		ConvertStencilFunc5551(state);
		break;

	default:
		// Hard to do anything useful for 4444, and 8888 is fine.
		break;
	}
}
