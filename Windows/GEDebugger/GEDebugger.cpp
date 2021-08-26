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
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Common/Data/Convert/ColorConv.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/Screenshot.h"
#include "Windows/GEDebugger/GEDebugger.h"
#include "Windows/GEDebugger/SimpleGLWindow.h"
#include "Windows/GEDebugger/CtrlDisplayListView.h"
#include "Windows/GEDebugger/TabDisplayLists.h"
#include "Windows/GEDebugger/TabState.h"
#include "Windows/GEDebugger/TabVertices.h"
#include "Windows/W32Util/ShellUtil.h"
#include "Windows/InputBox.h"
#include "Windows/MainWindow.h"
#include "Windows/main.h"
#include "GPU/GPUInterface.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/GPUState.h"
#include "GPU/Debugger/Breakpoints.h"
#include "GPU/Debugger/Debugger.h"
#include "GPU/Debugger/Record.h"
#include "GPU/Debugger/Stepping.h"
#include <windowsx.h>
#include <commctrl.h>

const int POPUP_SUBMENU_ID_GEDBG_PREVIEW = 10;

using namespace GPUBreakpoints;
using namespace GPUDebug;
using namespace GPUStepping;

enum PrimaryDisplayType {
	PRIMARY_FRAMEBUF,
	PRIMARY_DEPTHBUF,
	PRIMARY_STENCILBUF,
};

StepCountDlg::StepCountDlg(HINSTANCE _hInstance, HWND _hParent) : Dialog((LPCSTR)IDD_GEDBG_STEPCOUNT, _hInstance, _hParent) {
	DialogManager::AddDlg(this);

	for (int i = 0; i < 4; i++) // Add items 1, 10, 100, 1000
		SendMessageA(GetDlgItem(m_hDlg, IDC_GEDBG_STEPCOUNT_COMBO), CB_ADDSTRING, 0, (LPARAM)std::to_string((int)pow(10, i)).c_str());
	SetWindowTextA(GetDlgItem(m_hDlg, IDC_GEDBG_STEPCOUNT_COMBO), "1");
}

StepCountDlg::~StepCountDlg() {
	DialogManager::RemoveDlg(this);
}

void StepCountDlg::Jump(int count, bool relative) {
	if (relative && count == 0)
		return;
	SetBreakNext(BreakNext::COUNT);
	SetBreakCount(count, relative);
};

BOOL StepCountDlg::DlgProc(UINT message, WPARAM wParam, LPARAM lParam) {
	int count;
	bool relative;
	auto GetValue = [&]() {
		char str[7]; // +/-99999\0
		GetWindowTextA(GetDlgItem(m_hDlg, IDC_GEDBG_STEPCOUNT_COMBO), str, 7);
		relative = str[0] == '+' || str[0] == '-';
		return TryParse(str, &count);
	};

	switch (message) {
	case WM_CLOSE:
		Show(false);
		return TRUE;
	case WM_COMMAND:
		switch (wParam) {
		case IDC_GEDBG_STEPCOUNT_DEC:
			if (GetValue())
				Jump(-abs(count), true);
			return TRUE;
		case IDC_GEDBG_STEPCOUNT_INC:
			if (GetValue())
				Jump(abs(count), true);
			return TRUE;
		case IDC_GEDBG_STEPCOUNT_JUMP:
			if (GetValue())
				Jump(abs(count), false);
			return TRUE;
		case IDOK:
			if (GetValue())
				Jump(count, relative);
			Show(false);
			return TRUE;
		case IDCANCEL:
			SetFocus(m_hParent);
			Show(false);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

void CGEDebugger::Init() {
	SimpleGLWindow::RegisterClass();
	CtrlDisplayListView::registerClass();
}

CGEDebugger::CGEDebugger(HINSTANCE _hInstance, HWND _hParent)
	: Dialog((LPCSTR)IDD_GEDEBUGGER, _hInstance, _hParent)
	, stepCountDlg(_hInstance, m_hDlg) {
	// minimum size = a little more than the default
	RECT windowRect;
	GetWindowRect(m_hDlg, &windowRect);
	minWidth_ = windowRect.right-windowRect.left + 10;
	minHeight_ = windowRect.bottom-windowRect.top + 10;

	// it's ugly, but .rc coordinates don't match actual pixels and it screws
	// up both the size and the aspect ratio
	RECT frameRect;
	HWND frameWnd = GetDlgItem(m_hDlg,IDC_GEDBG_FRAME);

	GetWindowRect(frameWnd,&frameRect);
	MapWindowPoints(HWND_DESKTOP,m_hDlg,(LPPOINT)&frameRect,2);
	MoveWindow(frameWnd,frameRect.left,frameRect.top,512,272,TRUE);

	tabs = new TabControl(GetDlgItem(m_hDlg,IDC_GEDBG_MAINTAB));
	HWND wnd = tabs->AddTabWindow(L"CtrlDisplayListView",L"Display List");
	displayList = CtrlDisplayListView::getFrom(wnd);

	fbTabs = new TabControl(GetDlgItem(m_hDlg, IDC_GEDBG_FBTABS));
	fbTabs->SetMinTabWidth(50);
	// Must be in the same order as PrimaryDisplayType.
	fbTabs->AddTab(NULL, L"Color");
	fbTabs->AddTab(NULL, L"Depth");
	fbTabs->AddTab(NULL, L"Stencil");
	fbTabs->ShowTab(0, true);

	flags = new TabStateFlags(_hInstance, m_hDlg);
	tabs->AddTabDialog(flags, L"Flags");

	lighting = new TabStateLighting(_hInstance, m_hDlg);
	tabs->AddTabDialog(lighting, L"Lighting");

	textureState = new TabStateTexture(_hInstance, m_hDlg);
	tabs->AddTabDialog(textureState, L"Texture");

	settings = new TabStateSettings(_hInstance, m_hDlg);
	tabs->AddTabDialog(settings, L"Settings");

	vertices = new TabVertices(_hInstance, m_hDlg);
	tabs->AddTabDialog(vertices, L"Vertices");

	matrices = new TabMatrices(_hInstance, m_hDlg);
	tabs->AddTabDialog(matrices, L"Matrices");

	lists = new TabDisplayLists(_hInstance, m_hDlg);
	tabs->AddTabDialog(lists, L"Lists");

	watch = new TabStateWatch(_hInstance, m_hDlg);
	tabs->AddTabDialog(watch, L"Watch");

	tabs->ShowTab(0, true);

	// set window position
	int x = g_Config.iGEWindowX == -1 ? windowRect.left : g_Config.iGEWindowX;
	int y = g_Config.iGEWindowY == -1 ? windowRect.top : g_Config.iGEWindowY;
	int w = g_Config.iGEWindowW == -1 ? minWidth_ : g_Config.iGEWindowW;
	int h = g_Config.iGEWindowH == -1 ? minHeight_ : g_Config.iGEWindowH;
	MoveWindow(m_hDlg,x,y,w,h,FALSE);

	SetTimer(m_hDlg, 1, USER_TIMER_MINIMUM, nullptr);

	UpdateTextureLevel(textureLevel_);
}

CGEDebugger::~CGEDebugger() {
	CleanupPrimPreview();
	delete flags;
	delete lighting;
	delete textureState;
	delete settings;
	delete vertices;
	delete matrices;
	delete lists;
	delete watch;
	delete tabs;
	delete fbTabs;
}

void CGEDebugger::SetupPreviews() {
	if (primaryWindow == nullptr) {
		HMENU subMenu = GetSubMenu(g_hPopupMenus, POPUP_SUBMENU_ID_GEDBG_PREVIEW);

		primaryWindow = SimpleGLWindow::GetFrom(GetDlgItem(m_hDlg, IDC_GEDBG_FRAME));
		primaryWindow->Initialize(SimpleGLWindow::ALPHA_IGNORE | SimpleGLWindow::RESIZE_SHRINK_CENTER);
		primaryWindow->SetHoverCallback([&] (int x, int y) {
			PrimaryPreviewHover(x, y);
		});
		primaryWindow->SetRightClickMenu(subMenu, [&] (int cmd) {
			HMENU subMenu = GetSubMenu(g_hPopupMenus, POPUP_SUBMENU_ID_GEDBG_PREVIEW);
			switch (cmd) {
			case 0:
				// Setup.
				CheckMenuItem(subMenu, ID_GEDBG_ENABLE_PREVIEW, MF_BYCOMMAND | ((previewsEnabled_ & 1) ? MF_CHECKED : MF_UNCHECKED));
				break;
			case ID_GEDBG_EXPORT_IMAGE:
				PreviewExport(primaryBuffer_);
				break;
			case ID_GEDBG_ENABLE_PREVIEW:
				previewsEnabled_ ^= 1;
				primaryWindow->Redraw();
			default:
				break;
			}

			return true;
		});
		primaryWindow->SetRedrawCallback([&] {
			HandleRedraw(1);
		});
		primaryWindow->Clear();
	}
	if (secondWindow == nullptr) {
		HMENU subMenu = GetSubMenu(g_hPopupMenus, POPUP_SUBMENU_ID_GEDBG_PREVIEW);

		secondWindow = SimpleGLWindow::GetFrom(GetDlgItem(m_hDlg, IDC_GEDBG_TEX));
		secondWindow->Initialize(SimpleGLWindow::ALPHA_BLEND | SimpleGLWindow::RESIZE_SHRINK_CENTER);
		secondWindow->SetHoverCallback([&] (int x, int y) {
			SecondPreviewHover(x, y);
		});
		secondWindow->SetRightClickMenu(subMenu, [&] (int cmd) {
			HMENU subMenu = GetSubMenu(g_hPopupMenus, POPUP_SUBMENU_ID_GEDBG_PREVIEW);
			switch (cmd) {
			case 0:
				// Setup.
				CheckMenuItem(subMenu, ID_GEDBG_ENABLE_PREVIEW, MF_BYCOMMAND | ((previewsEnabled_ & 2) ? MF_CHECKED : MF_UNCHECKED));
				break;
			case ID_GEDBG_EXPORT_IMAGE:
				PreviewExport(secondBuffer_);
				break;
			case ID_GEDBG_ENABLE_PREVIEW:
				previewsEnabled_ ^= 2;
				secondWindow->Redraw();
			default:
				break;
			}

			return true;
		});
		secondWindow->SetRedrawCallback([&] {
			HandleRedraw(2);
		});
		secondWindow->Clear();
	}
}

void CGEDebugger::DescribePrimaryPreview(const GPUgstate &state, char desc[256]) {
	if (showClut_) {
		// In this case, we're showing the texture here.
		snprintf(desc, 256, "Texture L%d: 0x%08x (%dx%d)", textureLevel_, state.getTextureAddress(textureLevel_), state.getTextureWidth(textureLevel_), state.getTextureHeight(textureLevel_));
		return;
	}

	_assert_msg_(primaryBuffer_ != nullptr, "Must have a valid primaryBuffer_");

	switch (PrimaryDisplayType(fbTabs->CurrentTabIndex())) {
	case PRIMARY_FRAMEBUF:
		snprintf(desc, 256, "Color: 0x%08x (%dx%d) fmt %s", state.getFrameBufRawAddress(), primaryBuffer_->GetStride(), primaryBuffer_->GetHeight(), GeBufferFormatToString(state.FrameBufFormat()));
		break;

	case PRIMARY_DEPTHBUF:
		snprintf(desc, 256, "Depth: 0x%08x (%dx%d)", state.getDepthBufRawAddress(), primaryBuffer_->GetStride(), primaryBuffer_->GetHeight());
		break;

	case PRIMARY_STENCILBUF:
		snprintf(desc, 256, "Stencil: 0x%08x (%dx%d)", state.getFrameBufRawAddress(), primaryBuffer_->GetStride(), primaryBuffer_->GetHeight());
		break;
	}
}

void CGEDebugger::DescribeSecondPreview(const GPUgstate &state, char desc[256]) {
	if (showClut_) {
		snprintf(desc, 256, "CLUT: 0x%08x (%d)", state.getClutAddress(), state.getClutPaletteFormat());
	} else {
		snprintf(desc, 256, "Texture L%d: 0x%08x (%dx%d)", textureLevel_, state.getTextureAddress(textureLevel_), state.getTextureWidth(textureLevel_), state.getTextureHeight(textureLevel_));
	}
}

void CGEDebugger::PreviewExport(const GPUDebugBuffer *dbgBuffer) {
	const TCHAR *filter = L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg)\0*.jpg\0All files\0*.*\0\0";
	std::string fn;
	if (W32Util::BrowseForFileName(false, GetDlgHandle(), L"Save Preview Image...", nullptr, filter, L"png", fn)) {
		ScreenshotFormat fmt = fn.find(".jpg") != fn.npos ? ScreenshotFormat::JPG : ScreenshotFormat::PNG;

		Path filename(fn);
		bool saveAlpha = fmt == ScreenshotFormat::PNG;

		u8 *flipbuffer = nullptr;
		u32 w = (u32)-1;
		u32 h = (u32)-1;
		const u8 *buffer = ConvertBufferToScreenshot(*dbgBuffer, saveAlpha, flipbuffer, w, h);
		if (buffer != nullptr) {
			if (saveAlpha) {
				Save8888RGBAScreenshot(filename, buffer, w, h);
			} else {
				Save888RGBScreenshot(filename, fmt, buffer, w, h);
			}
		}
		delete [] flipbuffer;
	}
}

void CGEDebugger::UpdatePreviews() {
	auto memLock = Memory::Lock();
	if (!PSP_IsInited()) {
		return;
	}

	GPUgstate state{};

	if (gpuDebug != nullptr) {
		state = gpuDebug->GetGState();
	}

	updating_ = true;
	UpdateTextureLevel(textureLevel_);
	UpdatePrimaryPreview(state);
	UpdateSecondPreview(state);

	u32 primOp = PrimPreviewOp();
	if (primOp != 0) {
		UpdatePrimPreview(primOp, 3);
	}

	DisplayList list;
	if (gpuDebug != nullptr && gpuDebug->GetCurrentDisplayList(list)) {
		displayList->setDisplayList(list);
	}

	wchar_t primCounter[1024]{};
	swprintf(primCounter, ARRAY_SIZE(primCounter), L"%d/%d", PrimsThisFrame(), PrimsLastFrame());
	SetDlgItemText(m_hDlg, IDC_GEDBG_PRIMCOUNTER, primCounter);

	flags->Update();
	lighting->Update();
	textureState->Update();
	settings->Update();
	vertices->Update();
	matrices->Update();
	lists->Update();
	watch->Update();
	updating_ = false;
}

u32 CGEDebugger::TexturePreviewFlags(const GPUgstate &state) {
	if (state.isTextureAlphaUsed() && !forceOpaque_) {
		return SimpleGLWindow::ALPHA_BLEND | SimpleGLWindow::RESIZE_BEST_CENTER;
	} else {
		return SimpleGLWindow::RESIZE_BEST_CENTER;
	}
}

void CGEDebugger::UpdatePrimaryPreview(const GPUgstate &state) {
	bool bufferResult = false;
	u32 flags = SimpleGLWindow::ALPHA_IGNORE | SimpleGLWindow::RESIZE_SHRINK_CENTER;

	SetupPreviews();

	primaryBuffer_ = nullptr;
	if (showClut_) {
		bufferResult = GPU_GetCurrentTexture(primaryBuffer_, textureLevel_);
		flags = TexturePreviewFlags(state);
		if (bufferResult) {
			UpdateLastTexture(state.getTextureAddress(textureLevel_));
		} else {
			UpdateLastTexture((u32)-1);
		}
	} else {
		switch (PrimaryDisplayType(fbTabs->CurrentTabIndex())) {
		case PRIMARY_FRAMEBUF:
			bufferResult = GPU_GetCurrentFramebuffer(primaryBuffer_, GPU_DBG_FRAMEBUF_RENDER);
			break;

		case PRIMARY_DEPTHBUF:
			bufferResult = GPU_GetCurrentDepthbuffer(primaryBuffer_);
			break;

		case PRIMARY_STENCILBUF:
			bufferResult = GPU_GetCurrentStencilbuffer(primaryBuffer_);
			break;
		}
	}

	if (bufferResult && primaryBuffer_ != nullptr) {
		auto fmt = SimpleGLWindow::Format(primaryBuffer_->GetFormat());
		primaryWindow->SetFlags(flags);
		primaryWindow->Draw(primaryBuffer_->GetData(), primaryBuffer_->GetStride(), primaryBuffer_->GetHeight(), primaryBuffer_->GetFlipped(), fmt);

		char desc[256];
		wchar_t w_desc[256];
		DescribePrimaryPreview(state, desc);
		ConvertUTF8ToWString(w_desc, ARRAY_SIZE(w_desc), desc);

		SetDlgItemText(m_hDlg, IDC_GEDBG_FRAMEBUFADDR, w_desc);
	} else if (primaryWindow != nullptr) {
		primaryWindow->Clear();
		primaryBuffer_ = nullptr;

		SetDlgItemText(m_hDlg, IDC_GEDBG_FRAMEBUFADDR, L"Failed");
	}
}

void CGEDebugger::UpdateSecondPreview(const GPUgstate &state) {
	bool bufferResult = false;

	SetupPreviews();

	secondBuffer_ = nullptr;
	if (showClut_) {
		bufferResult = GPU_GetCurrentClut(secondBuffer_);
	} else {
		bufferResult = GPU_GetCurrentTexture(secondBuffer_, textureLevel_);
		if (bufferResult) {
			UpdateLastTexture(state.getTextureAddress(textureLevel_));
		} else {
			UpdateLastTexture((u32)-1);
		}
	}

	if (bufferResult) {
		auto fmt = SimpleGLWindow::Format(secondBuffer_->GetFormat());
		secondWindow->SetFlags(TexturePreviewFlags(state));
		if (showClut_) {
			// Reduce the stride so it's easier to see.
			secondWindow->Draw(secondBuffer_->GetData(), secondBuffer_->GetStride() / 16, secondBuffer_->GetHeight() * 16, secondBuffer_->GetFlipped(), fmt);
		} else {
			secondWindow->Draw(secondBuffer_->GetData(), secondBuffer_->GetStride(), secondBuffer_->GetHeight(), secondBuffer_->GetFlipped(), fmt);
		}

		char desc[256];
		DescribeSecondPreview(state, desc);
		wchar_t w_desc[256];
		ConvertUTF8ToWString(w_desc, ARRAY_SIZE(w_desc), desc);
		SetDlgItemText(m_hDlg, IDC_GEDBG_TEXADDR, w_desc);
	} else if (secondWindow != nullptr) {
		secondWindow->Clear();
		secondBuffer_ = nullptr;

		if (gpuDebug == nullptr || state.isTextureMapEnabled()) {
			SetDlgItemText(m_hDlg, IDC_GEDBG_TEXADDR, L"Texture: failed");
		} else {
			SetDlgItemText(m_hDlg, IDC_GEDBG_TEXADDR, L"Texture: disabled");
		}
	}
}

void CGEDebugger::PrimaryPreviewHover(int x, int y) {
	if (primaryBuffer_ == nullptr) {
		return;
	}

	SetupPreviews();

	char desc[256] = {0};

	if (!primaryWindow->HasTex()) {
		desc[0] = 0;
	} else if (x < 0 || y < 0) {
		// This means they left the area.
		GPUgstate state{};
		if (gpuDebug != nullptr) {
			state = gpuDebug->GetGState();
		}
		DescribePrimaryPreview(state, desc);
	} else {
		// Coordinates are relative to actual framebuffer size.
		u32 pix = primaryBuffer_->GetRawPixel(x, y);
		DescribePixel(pix, primaryBuffer_->GetFormat(), x, y, desc);
	}

	wchar_t w_desc[256];
	ConvertUTF8ToWString(w_desc, ARRAY_SIZE(w_desc), desc);

	SetDlgItemText(m_hDlg, IDC_GEDBG_FRAMEBUFADDR, w_desc);
}

void CGEDebugger::SecondPreviewHover(int x, int y) {
	if (secondBuffer_ == nullptr) {
		return;
	}

	char desc[256] = {0};

	if (!secondWindow->HasTex()) {
		desc[0] = 0;
	} else if (x < 0 || y < 0) {
		// This means they left the area.
		GPUgstate state{};
		if (gpuDebug != nullptr) {
			state = gpuDebug->GetGState();
		}
		DescribeSecondPreview(state, desc);
	} else {
		u32 pix = secondBuffer_->GetRawPixel(x, y);
		if (showClut_) {
			// Show the clut index, rather than coords.
			DescribePixel(pix, secondBuffer_->GetFormat(), y * 16 + x, 0, desc);
		} else {
			DescribePixel(pix, secondBuffer_->GetFormat(), x, y, desc);
		}
	}
	wchar_t w_desc[256];
	ConvertUTF8ToWString(w_desc, ARRAY_SIZE(w_desc), desc);
	SetDlgItemText(m_hDlg, IDC_GEDBG_TEXADDR, w_desc);
}

void CGEDebugger::DescribePixel(u32 pix, GPUDebugBufferFormat fmt, int x, int y, char desc[256]) {
	switch (fmt) {
	case GPU_DBG_FORMAT_565:
	case GPU_DBG_FORMAT_565_REV:
	case GPU_DBG_FORMAT_5551:
	case GPU_DBG_FORMAT_5551_REV:
	case GPU_DBG_FORMAT_5551_BGRA:
	case GPU_DBG_FORMAT_4444:
	case GPU_DBG_FORMAT_4444_REV:
	case GPU_DBG_FORMAT_4444_BGRA:
	case GPU_DBG_FORMAT_8888:
	case GPU_DBG_FORMAT_8888_BGRA:
		DescribePixelRGBA(pix, fmt, x, y, desc);
		break;

	case GPU_DBG_FORMAT_16BIT:
		snprintf(desc, 256, "%d,%d: %d / %f", x, y, pix, pix * (1.0f / 65535.0f));
		break;

	case GPU_DBG_FORMAT_8BIT:
		snprintf(desc, 256, "%d,%d: %d / %f", x, y, pix, pix * (1.0f / 255.0f));
		break;

	case GPU_DBG_FORMAT_24BIT_8X:
	{
		DepthScaleFactors depthScale = GetDepthScaleFactors();
		// These are only ever going to be depth values, so let's also show scaled to 16 bit.
		snprintf(desc, 256, "%d,%d: %d / %f / %f", x, y, pix & 0x00FFFFFF, (pix & 0x00FFFFFF) * (1.0f / 16777215.0f), depthScale.Apply((pix & 0x00FFFFFF) * (1.0f / 16777215.0f)));
		break;
	}

	case GPU_DBG_FORMAT_24BIT_8X_DIV_256:
		{
			// These are only ever going to be depth values, so let's also show scaled to 16 bit.
			int z24 = pix & 0x00FFFFFF;
			int z16 = z24 - 0x800000 + 0x8000;
			snprintf(desc, 256, "%d,%d: %d / %f", x, y, z16, z16 * (1.0f / 65535.0f));
		}
		break;

	case GPU_DBG_FORMAT_24X_8BIT:
		snprintf(desc, 256, "%d,%d: %d / %f", x, y, (pix >> 24) & 0xFF, ((pix >> 24) & 0xFF) * (1.0f / 255.0f));
		break;

	case GPU_DBG_FORMAT_FLOAT: {
		float pixf = *(float *)&pix;
		DepthScaleFactors depthScale = GetDepthScaleFactors();
		snprintf(desc, 256, "%d,%d: %f / %f", x, y, pixf, depthScale.Apply(pixf));
		break;
	}

	case GPU_DBG_FORMAT_FLOAT_DIV_256:
		{
			double z = *(float *)&pix;
			int z24 = (int)(z * 16777215.0);

			DepthScaleFactors factors = GetDepthScaleFactors();
			// TODO: Use GetDepthScaleFactors here too, verify it's the same.
			int z16 = z24 - 0x800000 + 0x8000;

			int z16_2 = factors.Apply(z);

			snprintf(desc, 256, "%d,%d: %d / %f", x, y, z16, (z - 0.5 + (1.0 / 512.0)) * 256.0);
		}
		break;

	default:
		snprintf(desc, 256, "Unexpected format");
	}
}

void CGEDebugger::DescribePixelRGBA(u32 pix, GPUDebugBufferFormat fmt, int x, int y, char desc[256]) {
	u32 r = -1, g = -1, b = -1, a = -1;

	switch (fmt) {
	case GPU_DBG_FORMAT_565:
		r = Convert5To8((pix >> 0) & 0x1F);
		g = Convert6To8((pix >> 5) & 0x3F);
		b = Convert5To8((pix >> 11) & 0x1F);
		break;
	case GPU_DBG_FORMAT_565_REV:
		b = Convert5To8((pix >> 0) & 0x1F);
		g = Convert6To8((pix >> 5) & 0x3F);
		r = Convert5To8((pix >> 11) & 0x1F);
		break;
	case GPU_DBG_FORMAT_5551:
		r = Convert5To8((pix >> 0) & 0x1F);
		g = Convert5To8((pix >> 5) & 0x1F);
		b = Convert5To8((pix >> 10) & 0x1F);
		a = (pix >> 15) & 1 ? 255 : 0;
		break;
	case GPU_DBG_FORMAT_5551_REV:
		a = pix & 1 ? 255 : 0;
		b = Convert5To8((pix >> 1) & 0x1F);
		g = Convert5To8((pix >> 6) & 0x1F);
		r = Convert5To8((pix >> 11) & 0x1F);
		break;
	case GPU_DBG_FORMAT_5551_BGRA:
		b = Convert5To8((pix >> 0) & 0x1F);
		g = Convert5To8((pix >> 5) & 0x1F);
		r = Convert5To8((pix >> 10) & 0x1F);
		a = (pix >> 15) & 1 ? 255 : 0;
		break;
	case GPU_DBG_FORMAT_4444:
		r = Convert4To8((pix >> 0) & 0x0F);
		g = Convert4To8((pix >> 4) & 0x0F);
		b = Convert4To8((pix >> 8) & 0x0F);
		a = Convert4To8((pix >> 12) & 0x0F);
		break;
	case GPU_DBG_FORMAT_4444_REV:
		a = Convert4To8((pix >> 0) & 0x0F);
		b = Convert4To8((pix >> 4) & 0x0F);
		g = Convert4To8((pix >> 8) & 0x0F);
		r = Convert4To8((pix >> 12) & 0x0F);
		break;
	case GPU_DBG_FORMAT_4444_BGRA:
		b = Convert4To8((pix >> 0) & 0x0F);
		g = Convert4To8((pix >> 4) & 0x0F);
		r = Convert4To8((pix >> 8) & 0x0F);
		a = Convert4To8((pix >> 12) & 0x0F);
		break;
	case GPU_DBG_FORMAT_8888:
		r = (pix >> 0) & 0xFF;
		g = (pix >> 8) & 0xFF;
		b = (pix >> 16) & 0xFF;
		a = (pix >> 24) & 0xFF;
		break;
	case GPU_DBG_FORMAT_8888_BGRA:
		b = (pix >> 0) & 0xFF;
		g = (pix >> 8) & 0xFF;
		r = (pix >> 16) & 0xFF;
		a = (pix >> 24) & 0xFF;
		break;

	default:
		snprintf(desc, 256, "Unexpected format");
		return;
	}

	snprintf(desc, 256, "%d,%d: r=%d, g=%d, b=%d, a=%d", x, y, r, g, b, a);
}

void CGEDebugger::UpdateTextureLevel(int level) {
	GPUgstate state{};
	if (gpuDebug != nullptr) {
		state = gpuDebug->GetGState();
	}

	int maxValid = 0;
	for (int i = 1; i < state.getTextureMaxLevel() + 1; ++i) {
		if (state.getTextureAddress(i) != 0) {
			maxValid = i;
		}
	}

	textureLevel_ = std::min(std::max(0, level), maxValid);
	EnableWindow(GetDlgItem(m_hDlg, IDC_GEDBG_TEXLEVELDOWN), textureLevel_ > 0);
	EnableWindow(GetDlgItem(m_hDlg, IDC_GEDBG_TEXLEVELUP), textureLevel_ < maxValid);
}

void CGEDebugger::UpdateSize(WORD width, WORD height) {
	// only resize the tab for now
	HWND tabControl = GetDlgItem(m_hDlg, IDC_GEDBG_MAINTAB);

	RECT tabRect;
	GetWindowRect(tabControl,&tabRect);
	MapWindowPoints(HWND_DESKTOP,m_hDlg,(LPPOINT)&tabRect,2);

	tabRect.right = tabRect.left + (width-tabRect.left*2);				// assume same gap on both sides
	tabRect.bottom = tabRect.top + (height-tabRect.top-tabRect.left);	// assume same gap on bottom too
	MoveWindow(tabControl,tabRect.left,tabRect.top,tabRect.right-tabRect.left,tabRect.bottom-tabRect.top,TRUE);
}

void CGEDebugger::SavePosition()
{
	RECT rc;
	if (GetWindowRect(m_hDlg, &rc))
	{
		g_Config.iGEWindowX = rc.left;
		g_Config.iGEWindowY = rc.top;
		g_Config.iGEWindowW = rc.right - rc.left;
		g_Config.iGEWindowH = rc.bottom - rc.top;
	}
}

BOOL CGEDebugger::DlgProc(UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG:
		return TRUE;

	case WM_GETMINMAXINFO:
		{
			MINMAXINFO* minmax = (MINMAXINFO*) lParam;
			minmax->ptMinTrackSize.x = minWidth_;
			minmax->ptMinTrackSize.y = minHeight_;
		}
		return TRUE;

	case WM_SIZE:
		UpdateSize(LOWORD(lParam), HIWORD(lParam));
		SavePosition();
		return TRUE;
		
	case WM_MOVE:
		SavePosition();
		return TRUE;

	case WM_CLOSE:
		GPUDebug::SetActive(false);

		stepCountDlg.Show(false);
		Show(false);
		return TRUE;

	case WM_SHOWWINDOW:
		SetupPreviews();
		break;

	case WM_ACTIVATE:
		if (wParam == WA_ACTIVE || wParam == WA_CLICKACTIVE) {
			g_activeWindow = WINDOW_GEDEBUGGER;
		}
		break;

	case WM_TIMER:
		if (GPUStepping::IsStepping()) {
			static int lastCounter = 0;
			if (lastCounter != GPUStepping::GetSteppingCounter()) {
				UpdatePreviews();
				lastCounter = GPUStepping::GetSteppingCounter();
			}
		}
		break;

	case WM_NOTIFY:
		switch (wParam)
		{
		case IDC_GEDBG_MAINTAB:
			tabs->HandleNotify(lParam);
			if (gpuDebug != nullptr) {
				lists->Update();
			}
			break;
		case IDC_GEDBG_FBTABS:
			fbTabs->HandleNotify(lParam);
			if (GPUDebug::IsActive() && gpuDebug != nullptr) {
				UpdatePreviews();
			}
			break;
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_GEDBG_STEPDRAW:
			SetBreakNext(BreakNext::DRAW);
			break;

		case IDC_GEDBG_STEP:
			SetBreakNext(BreakNext::OP);
			break;

		case IDC_GEDBG_STEPTEX:
			SetBreakNext(BreakNext::TEX);
			break;

		case IDC_GEDBG_STEPFRAME:
			SetBreakNext(BreakNext::FRAME);
			break;

		case IDC_GEDBG_STEPPRIM:
			SetBreakNext(BreakNext::PRIM);
			break;

		case IDC_GEDBG_STEPCURVE:
			SetBreakNext(BreakNext::CURVE);
			break;

		case IDC_GEDBG_STEPCOUNT:
			stepCountDlg.Show(true);
			break;

		case IDC_GEDBG_BREAKTEX:
			{
				GPUDebug::SetActive(true);
				if (!gpuDebug) {
					break;
				}
				const auto state = gpuDebug->GetGState();
				u32 texAddr = state.getTextureAddress(textureLevel_);
				// TODO: Better interface that allows add/remove or something.
				if (InputBox_GetHex(GetModuleHandle(NULL), m_hDlg, L"Texture Address", texAddr, texAddr)) {
					if (IsTextureBreakpoint(texAddr)) {
						RemoveTextureBreakpoint(texAddr);
					} else {
						AddTextureBreakpoint(texAddr);
					}
				}
			}
			break;

		case IDC_GEDBG_BREAKTARGET:
			{
				GPUDebug::SetActive(true);
				if (!gpuDebug) {
					break;
				}
				const auto state = gpuDebug->GetGState();
				u32 fbAddr = state.getFrameBufRawAddress();
				// TODO: Better interface that allows add/remove or something.
				if (InputBox_GetHex(GetModuleHandle(NULL), m_hDlg, L"Framebuffer Address", fbAddr, fbAddr)) {
					if (IsRenderTargetBreakpoint(fbAddr)) {
						RemoveRenderTargetBreakpoint(fbAddr);
					} else {
						AddRenderTargetBreakpoint(fbAddr);
					}
				}
			}
			break;

		case IDC_GEDBG_TEXLEVELDOWN:
			UpdateTextureLevel(textureLevel_ - 1);
			if (GPUDebug::IsActive() && gpuDebug != nullptr) {
				UpdatePreviews();
			}
			break;

		case IDC_GEDBG_TEXLEVELUP:
			UpdateTextureLevel(textureLevel_ + 1);
			if (GPUDebug::IsActive() && gpuDebug != nullptr) {
				UpdatePreviews();
			}
			break;

		case IDC_GEDBG_RESUME:
			SetupPreviews();
			primaryWindow->Clear();
			secondWindow->Clear();
			SetDlgItemText(m_hDlg, IDC_GEDBG_FRAMEBUFADDR, L"");
			SetDlgItemText(m_hDlg, IDC_GEDBG_TEXADDR, L"");
			SetDlgItemText(m_hDlg, IDC_GEDBG_PRIMCOUNTER, L"");

			SetBreakNext(BreakNext::NONE);
			break;

		case IDC_GEDBG_RECORD:
			GPURecord::Activate();
			break;

		case IDC_GEDBG_FORCEOPAQUE:
			if (GPUDebug::IsActive() && gpuDebug != nullptr) {
				forceOpaque_ = SendMessage(GetDlgItem(m_hDlg, IDC_GEDBG_FORCEOPAQUE), BM_GETCHECK, 0, 0) != 0;
				UpdatePreviews();
			}
			break;

		case IDC_GEDBG_SHOWCLUT:
			if (GPUDebug::IsActive() && gpuDebug != nullptr) {
				showClut_ = SendMessage(GetDlgItem(m_hDlg, IDC_GEDBG_SHOWCLUT), BM_GETCHECK, 0, 0) != 0;
				UpdatePreviews();
			}
			break;
		}
		break;

	case WM_GEDBG_STEPDISPLAYLIST:
		SetBreakNext(BreakNext::OP);
		break;

	case WM_GEDBG_TOGGLEPCBREAKPOINT:
		{
			GPUDebug::SetActive(true);
			u32 pc = (u32)wParam;
			bool temp;
			bool isBreak = IsAddressBreakpoint(pc, temp);
			if (isBreak && !temp) {
				RemoveAddressBreakpoint(pc);
			} else {
				AddAddressBreakpoint(pc);
			}
		}
		break;

	case WM_GEDBG_RUNTOWPARAM:
		{
			GPUDebug::SetActive(true);
			u32 pc = (u32)wParam;
			AddAddressBreakpoint(pc, true);
			SendMessage(m_hDlg,WM_COMMAND,IDC_GEDBG_RESUME,0);
		}
		break;

	case WM_GEDBG_SETCMDWPARAM:
		GPU_SetCmdValue((u32)wParam);
		break;

	case WM_GEDBG_UPDATE_WATCH:
		// Just a notification to update.
		if (watch)
			watch->Update();
		break;
	}

	return FALSE;
}
