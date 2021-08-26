#include "ppsspp_config.h"
#include "CommonWindows.h"

#include <WinUser.h>
#include <shellapi.h>
#include <commctrl.h>

#include "Misc.h"
#include "Common/Data/Encoding/Utf8.h"

bool KeyDownAsync(int vkey) {
#if PPSSPP_PLATFORM(UWP)
	return 0;
#else
	return (GetAsyncKeyState(vkey) & 0x8000) != 0;
#endif
}

namespace W32Util
{
	void CenterWindow(HWND hwnd)
	{
		HWND hwndParent;
		RECT rect, rectP;
		int width, height;      
		int screenwidth, screenheight;
		int x, y;

		//make the window relative to its parent
		hwndParent = GetParent(hwnd);
		if (!hwndParent)
			return;

		GetWindowRect(hwnd, &rect);
		GetWindowRect(hwndParent, &rectP);

		width  = rect.right  - rect.left;
		height = rect.bottom - rect.top;

		x = ((rectP.right-rectP.left) -  width) / 2 + rectP.left;
		y = ((rectP.bottom-rectP.top) - height) / 2 + rectP.top;

		screenwidth  = GetSystemMetrics(SM_CXSCREEN);
		screenheight = GetSystemMetrics(SM_CYSCREEN);

		//make sure that the dialog box never moves outside of
		//the screen
		if(x < 0) x = 0;
		if(y < 0) y = 0;
		if(x + width  > screenwidth)  x = screenwidth  - width;
		if(y + height > screenheight) y = screenheight - height;

		MoveWindow(hwnd, x, y, width, height, FALSE);
	}
 
	BOOL CopyTextToClipboard(HWND hwnd, const char *text) {
		std::wstring wtext = ConvertUTF8ToWString(text);
		return CopyTextToClipboard(hwnd, wtext);
	}

	BOOL CopyTextToClipboard(HWND hwnd, const std::wstring &wtext) {
		OpenClipboard(hwnd);
		EmptyClipboard();
		HANDLE hglbCopy = GlobalAlloc(GMEM_MOVEABLE, (wtext.size() + 1) * sizeof(wchar_t));
		if (hglbCopy == NULL) {
			CloseClipboard();
			return FALSE;
		}

		// Lock the handle and copy the text to the buffer.

		wchar_t *lptstrCopy = (wchar_t *)GlobalLock(hglbCopy);
		wcscpy(lptstrCopy, wtext.c_str());
		lptstrCopy[wtext.size()] = (wchar_t) 0;    // null character
		GlobalUnlock(hglbCopy);
		SetClipboardData(CF_UNICODETEXT, hglbCopy);
		CloseClipboard();
		return TRUE;
	}

	void MakeTopMost(HWND hwnd, bool topMost) {
		HWND style = HWND_NOTOPMOST;
		if (topMost) style = HWND_TOPMOST;
		SetWindowPos(hwnd, style, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE);
	}

	static const wchar_t *RemoveExecutableFromCommandLine(const wchar_t *cmdline) {
		if (!cmdline) {
			return L"";
		}

		switch (cmdline[0]) {
		case '"':
			// We don't need to handle escaped quotes, since filenames can't have that.
			cmdline = wcschr(cmdline + 1, '"');
			if (cmdline) {
				++cmdline;
				if (cmdline[0] == ' ') {
					++cmdline;
				}
			}
			break;

		default:
			cmdline = wcschr(cmdline, ' ');
			if (cmdline) {
				++cmdline;
			}
			break;
		}

		return cmdline;
	}

	void GetSelfExecuteParams(std::wstring &workingDirectory, std::wstring &moduleFilename) {
		workingDirectory.resize(MAX_PATH);
		size_t sz = GetCurrentDirectoryW((DWORD)workingDirectory.size(), &workingDirectory[0]);
		if (sz != 0 && sz < workingDirectory.size()) {
			// This means success, so now we can remove the null terminator.
			workingDirectory.resize(sz);
		} else if (sz > workingDirectory.size()) {
			// If insufficient, sz will include the null terminator, so we remove after.
			workingDirectory.resize(sz);
			sz = GetCurrentDirectoryW((DWORD)sz, &workingDirectory[0]);
			workingDirectory.resize(sz);
		}

		moduleFilename.clear();
		do {
			moduleFilename.resize(moduleFilename.size() + MAX_PATH);
			// On failure, this will return the same value as passed in, but success will always be one lower.
			sz = GetModuleFileName(GetModuleHandle(nullptr), &moduleFilename[0], (DWORD)moduleFilename.size());
		} while (sz >= moduleFilename.size());
		moduleFilename.resize(sz);
	}

	void ExitAndRestart(bool overrideArgs, const std::string &args) {
		SpawnNewInstance(overrideArgs, args);

		ExitProcess(0);
	}

	void SpawnNewInstance(bool overrideArgs, const std::string &args) {
		// This preserves arguments (for example, config file) and working directory.
		std::wstring workingDirectory;
		std::wstring moduleFilename;
		GetSelfExecuteParams(workingDirectory, moduleFilename);

		const wchar_t *cmdline;
		std::wstring wargs;
		if (overrideArgs) {
			wargs = ConvertUTF8ToWString(args);
			cmdline = wargs.c_str();
		} else {
			cmdline = RemoveExecutableFromCommandLine(GetCommandLineW());
		}
		ShellExecute(nullptr, nullptr, moduleFilename.c_str(), cmdline, workingDirectory.c_str(), SW_SHOW);
	}
}

GenericListControl::GenericListControl(HWND hwnd, const GenericListViewDef& def)
	: handle(hwnd), columns(def.columns),columnCount(def.columnCount),valid(false),
	inResizeColumns(false),updating(false)
{
	DWORD style = GetWindowLong(handle,GWL_STYLE) | LVS_REPORT;
	SetWindowLong(handle, GWL_STYLE, style);

	SetWindowLongPtr(handle,GWLP_USERDATA,(LONG_PTR)this);
	oldProc = (WNDPROC) SetWindowLongPtr(handle,GWLP_WNDPROC,(LONG_PTR)wndProc);

	auto exStyle = LVS_EX_FULLROWSELECT;
	if (def.checkbox)
		exStyle |= LVS_EX_CHECKBOXES;
	SendMessage(handle, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, exStyle);

	LVCOLUMN lvc; 
	lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	lvc.iSubItem = 0;
	
	RECT rect;
	GetClientRect(handle,&rect);

	int totalListSize = rect.right-rect.left;
	for (int i = 0; i < columnCount; i++) {
		lvc.cx = (int)(columns[i].size * totalListSize);
		lvc.pszText = (LPTSTR)columns[i].name;

		if (columns[i].flags & GLVC_CENTERED)
			lvc.fmt = LVCFMT_CENTER;
		else
			lvc.fmt = LVCFMT_LEFT;

		ListView_InsertColumn(handle, i, &lvc);
	}

	if (def.columnOrder != NULL)
		ListView_SetColumnOrderArray(handle,columnCount,def.columnOrder);

	SetSendInvalidRows(false);
	valid = true;
}

void GenericListControl::HandleNotify(LPARAM lParam)
{
	LPNMHDR mhdr = (LPNMHDR) lParam;

	if (mhdr->code == NM_DBLCLK)
	{
		LPNMITEMACTIVATE item = (LPNMITEMACTIVATE) lParam;
		if ((item->iItem != -1 && item->iItem < GetRowCount()) || sendInvalidRows)
			OnDoubleClick(item->iItem,item->iSubItem);
		return;
	}

	if (mhdr->code == NM_RCLICK)
	{
		const LPNMITEMACTIVATE item = (LPNMITEMACTIVATE)lParam;
		if ((item->iItem != -1 && item->iItem < GetRowCount()) || sendInvalidRows)
			OnRightClick(item->iItem,item->iSubItem,item->ptAction);
		return;
	}

	if (mhdr->code == LVN_GETDISPINFO)
	{
		NMLVDISPINFO* dispInfo = (NMLVDISPINFO*)lParam;

		stringBuffer[0] = 0;
		GetColumnText(stringBuffer,dispInfo->item.iItem,dispInfo->item.iSubItem);
		
		if (stringBuffer[0] == 0)
			wcscat(stringBuffer,L"Invalid");

		dispInfo->item.pszText = stringBuffer;
		return;
	}
	 
	// handle checkboxes
	if (mhdr->code == LVN_ITEMCHANGED && updating == false)
	{
		NMLISTVIEW* item = (NMLISTVIEW*) lParam;
		if (item->iItem != -1 && (item->uChanged & LVIF_STATE) != 0)
		{
			// image is 1 if unchcked, 2 if checked
			int oldImage = (item->uOldState & LVIS_STATEIMAGEMASK) >> 12;
			int newImage = (item->uNewState & LVIS_STATEIMAGEMASK) >> 12;
			if (oldImage != newImage)
				OnToggle(item->iItem,newImage == 2);
		}

		return;
	}
}

void GenericListControl::Update()
{
	updating = true;
	int newRows = GetRowCount();

	int items = ListView_GetItemCount(handle);
	while (items < newRows)
	{
		LVITEM lvI;
		lvI.pszText   = LPSTR_TEXTCALLBACK; // Sends an LVN_GETDISPINFO message.
		lvI.mask      = LVIF_TEXT | LVIF_IMAGE |LVIF_STATE;
		lvI.stateMask = 0;
		lvI.iSubItem  = 0;
		lvI.state     = 0;
		lvI.iItem  = items;
		lvI.iImage = items;

		ListView_InsertItem(handle, &lvI);
		items++;
	}

	while (items > newRows)
	{
		ListView_DeleteItem(handle,--items);
	}

	ResizeColumns();

	InvalidateRect(handle,NULL,true);
	UpdateWindow(handle);
	updating = false;
}


void GenericListControl::SetCheckState(int item, bool state)
{
	updating = true;
	ListView_SetCheckState(handle,item,state ? TRUE : FALSE);
	updating = false;
}

void GenericListControl::ResizeColumns()
{
	if (inResizeColumns)
		return;
	inResizeColumns = true;

	RECT rect;
	GetClientRect(handle, &rect);

	int totalListSize = rect.right - rect.left;
	for (int i = 0; i < columnCount; i++)
	{
		ListView_SetColumnWidth(handle, i, columns[i].size * totalListSize);
	}
	inResizeColumns = false;
}

LRESULT CALLBACK GenericListControl::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	GenericListControl* list = (GenericListControl*) GetWindowLongPtr(hwnd,GWLP_USERDATA);

	LRESULT returnValue;
	if (list->valid && list->WindowMessage(msg,wParam,lParam,returnValue) == true)
		return returnValue;

	switch (msg)
	{
	case WM_SIZE:
		list->ResizeColumns();
		break;

	case WM_KEYDOWN:
		switch (wParam)
		{
		case VK_INSERT:
		case 'C':
			if (KeyDownAsync(VK_CONTROL))
				list->ProcessCopy();
			break;

		case 'A':
			if (KeyDownAsync(VK_CONTROL))
				list->SelectAll();
			break;
		}
		break;
	}

	return (LRESULT)CallWindowProc((WNDPROC)list->oldProc,hwnd,msg,wParam,lParam);
}

void GenericListControl::ProcessCopy()
{
	int start = GetSelectedIndex();
	int size;
	if (start == -1)
		size = GetRowCount();
	else
		size = ListView_GetSelectedCount(handle);

	CopyRows(start, size);
}

void GenericListControl::CopyRows(int start, int size)
{
	std::wstring data;

	if (start == 0 && size == GetRowCount())
	{
		// Let's also copy the header if everything is selected.
		for (int c = 0; c < columnCount; ++c)
		{
			data.append(columns[c].name);
			if (c < columnCount - 1)
				data.append(L"\t");
			else
				data.append(L"\r\n");
		}
	}

	for (int r = start; r < start + size; ++r)
	{
		for (int c = 0; c < columnCount; ++c)
		{
			stringBuffer[0] = 0;
			GetColumnText(stringBuffer, r, c);
			data.append(stringBuffer);
			if (c < columnCount - 1)
				data.append(L"\t");
			else
				data.append(L"\r\n");
		}
	}
	W32Util::CopyTextToClipboard(handle, data);
}

void GenericListControl::SelectAll()
{
	ListView_SetItemState(handle, -1, LVIS_SELECTED, LVIS_SELECTED);
}

int GenericListControl::GetSelectedIndex()
{
	return ListView_GetNextItem(handle, -1, LVNI_SELECTED);
}
