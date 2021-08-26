// Copyright (c) 2013- PPSSPP Project.

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
#include <functional>

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Math/curves.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "UI/SavedataScreen.h"
#include "UI/MainScreen.h"
#include "UI/GameInfoCache.h"
#include "UI/PauseScreen.h"

#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"
#include "Common/StringUtils.h"
#include "Core/Host.h"
#include "Core/Config.h"
#include "Core/Loaders.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Core/HLE/sceUtility.h"

class SavedataButton;

std::string GetFileDateAsString(const Path &filename) {
	tm time;
	if (File::GetModifTime(filename, time)) {
		char buf[256];
		switch (g_Config.iDateFormat) {
		case PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD:
			strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &time);
			break;
		case PSP_SYSTEMPARAM_DATE_FORMAT_MMDDYYYY:
			strftime(buf, sizeof(buf), "%m-%d-%Y %H:%M:%S", &time);
			break;
		case PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY:
			strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &time);
			break;
		default: // Should never happen
			return "";
		}
		return std::string(buf);
	}
	return "";
}

static std::string TrimString(const std::string &str) {
	size_t pos = str.find_last_not_of(" \r\n\t");
	if (pos != str.npos) {
		return str.substr(0, pos + 1);
	}
	return str;
}

class SavedataPopupScreen : public PopupScreen {
public:
	SavedataPopupScreen(std::string savePath, std::string title) : PopupScreen(TrimString(title)), savePath_(savePath) {
	}

	void CreatePopupContents(UI::ViewGroup *parent) override {
		using namespace UI;
		UIContext &dc = *screenManager()->getUIContext();
		const Style &textStyle = dc.theme->popupStyle;

		std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(screenManager()->getDrawContext(), savePath_, GAMEINFO_WANTBG | GAMEINFO_WANTSIZE);
		LinearLayout *content = new LinearLayout(ORIENT_VERTICAL);
		parent->Add(content);
		if (!ginfo)
			return;
		LinearLayout *toprow = new LinearLayout(ORIENT_HORIZONTAL, new LayoutParams(FILL_PARENT, WRAP_CONTENT));
		content->Add(toprow);

		auto sa = GetI18NCategory("Savedata");
		if (ginfo->fileType == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY) {
			std::string savedata_detail = ginfo->paramSFO.GetValueString("SAVEDATA_DETAIL");
			std::string savedata_title = ginfo->paramSFO.GetValueString("SAVEDATA_TITLE");

			if (ginfo->icon.texture) {
				toprow->Add(new GameIconView(savePath_, 2.0f, new LinearLayoutParams(Margins(10, 5))));
			}
			LinearLayout *topright = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, 1.0f));
			topright->SetSpacing(1.0f);
			topright->Add(new TextView(savedata_title, ALIGN_LEFT | FLAG_WRAP_TEXT, false))->SetTextColor(textStyle.fgColor);
			topright->Add(new TextView(StringFromFormat("%lld kB", ginfo->gameSize / 1024), 0, true))->SetTextColor(textStyle.fgColor);
			topright->Add(new TextView(GetFileDateAsString(savePath_ / "PARAM.SFO"), 0, true))->SetTextColor(textStyle.fgColor);
			toprow->Add(topright);
			content->Add(new Spacer(3.0));
			content->Add(new TextView(ReplaceAll(savedata_detail, "\r", ""), ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(Margins(10, 0))))->SetTextColor(textStyle.fgColor);
			content->Add(new Spacer(3.0));
		} else {
			Path image_path = savePath_.WithReplacedExtension(".ppst", ".jpg");
			if (File::Exists(image_path)) {
				toprow->Add(new AsyncImageFileView(image_path, IS_KEEP_ASPECT, new LinearLayoutParams(480, 272, Margins(10, 0))));
			} else {
				toprow->Add(new TextView(sa->T("No screenshot"), new LinearLayoutParams(Margins(10, 5))))->SetTextColor(textStyle.fgColor);
			}
			content->Add(new TextView(GetFileDateAsString(savePath_), 0, true, new LinearLayoutParams(Margins(10, 5))))->SetTextColor(textStyle.fgColor);
		}

		auto di = GetI18NCategory("Dialog");
		LinearLayout *buttons = new LinearLayout(ORIENT_HORIZONTAL);
		buttons->Add(new Button(di->T("Back"), new LinearLayoutParams(1.0)))->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);
		buttons->Add(new Button(di->T("Delete"), new LinearLayoutParams(1.0)))->OnClick.Handle(this, &SavedataPopupScreen::OnDeleteButtonClick);
		content->Add(buttons);
	}

protected:
	UI::Size PopupWidth() const override { return 500; }

private:
	UI::EventReturn OnDeleteButtonClick(UI::EventParams &e);
	Path savePath_;
};

class SortedLinearLayout : public UI::LinearLayoutList {
public:
	typedef std::function<bool(const View *, const View *)> CompareFunc;
	typedef std::function<bool()> DoneFunc;

	SortedLinearLayout(UI::Orientation orientation, UI::LayoutParams *layoutParams = nullptr)
		: UI::LinearLayoutList(orientation, layoutParams) {
	}

	void SetCompare(CompareFunc lessFunc, DoneFunc doneFunc) {
		lessFunc_ = lessFunc;
		doneFunc_ = doneFunc;
	}

	void Update() override;

private:
	CompareFunc lessFunc_;
	DoneFunc doneFunc_;
};

void SortedLinearLayout::Update() {
	if (lessFunc_) {
		std::stable_sort(views_.begin(), views_.end(), lessFunc_);
	}
	if (doneFunc_ && doneFunc_()) {
		lessFunc_ = CompareFunc();
	}
	UI::LinearLayout::Update();
}

class SavedataButton : public UI::Clickable {
public:
	SavedataButton(const Path &gamePath, UI::LayoutParams *layoutParams = 0)
		: UI::Clickable(layoutParams), savePath_(gamePath) {
		SetTag(gamePath.ToString());
	}

	void Draw(UIContext &dc) override;
	bool UpdateText();
	std::string DescribeText() const override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = 500;
		h = 74;
	}

	const Path &GamePath() const { return savePath_; }

private:
	void UpdateText(const std::shared_ptr<GameInfo> &ginfo);

	Path savePath_;
	std::string title_;
	std::string subtitle_;
};

UI::EventReturn SavedataPopupScreen::OnDeleteButtonClick(UI::EventParams &e) {
	std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(nullptr, savePath_, GAMEINFO_WANTSIZE);
	ginfo->Delete();
	TriggerFinish(DR_NO);
	return UI::EVENT_DONE;
}

static std::string CleanSaveString(std::string str) {
	std::string s = ReplaceAll(str, "&", "&&");
	s = ReplaceAll(s, "\n", " ");
	s = ReplaceAll(s, "\r", " ");
	return s;
}

bool SavedataButton::UpdateText() {
	std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(nullptr, savePath_, GAMEINFO_WANTSIZE);
	if (!ginfo->pending) {
		UpdateText(ginfo);
		return true;
	}
	return false;
}

void SavedataButton::UpdateText(const std::shared_ptr<GameInfo> &ginfo) {
	const std::string currentTitle = ginfo->GetTitle();
	if (!currentTitle.empty()) {
		title_ = CleanSaveString(currentTitle);
	}
	if (subtitle_.empty() && ginfo->gameSize > 0) {
		std::string savedata_title = ginfo->paramSFO.GetValueString("SAVEDATA_TITLE");
		subtitle_ = CleanSaveString(savedata_title) + StringFromFormat(" (%lld kB)", ginfo->gameSize / 1024);
	}
}

void SavedataButton::Draw(UIContext &dc) {
	std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(dc.GetDrawContext(), savePath_, GAMEINFO_WANTSIZE);
	Draw::Texture *texture = 0;
	u32 color = 0, shadowColor = 0;
	using namespace UI;

	if (ginfo->icon.texture) {
		texture = ginfo->icon.texture->GetTexture();
	}

	int x = bounds_.x;
	int y = bounds_.y;
	int w = 144;
	int h = bounds_.h;

	UI::Style style = dc.theme->itemStyle;
	if (down_)
		style = dc.theme->itemDownStyle;

	h = bounds_.h;
	if (HasFocus())
		style = down_ ? dc.theme->itemDownStyle : dc.theme->itemFocusedStyle;

	Drawable bg = style.background;

	dc.Draw()->Flush();
	dc.RebindTexture();
	dc.FillRect(bg, bounds_);
	dc.Draw()->Flush();

	if (texture) {
		color = whiteAlpha(ease((time_now_d() - ginfo->icon.timeLoaded) * 2));
		shadowColor = blackAlpha(ease((time_now_d() - ginfo->icon.timeLoaded) * 2));
		float tw = texture->Width();
		float th = texture->Height();

		// Adjust position so we don't stretch the image vertically or horizontally.
		// TODO: Add a param to specify fit?  The below assumes it's never too wide.
		float nw = h * tw / th;
		x += (w - nw) / 2.0f;
		w = nw;
	}

	int txOffset = down_ ? 4 : 0;
	txOffset = 0;

	Bounds overlayBounds = bounds_;

	// Render button
	int dropsize = 10;
	if (texture) {
		if (txOffset) {
			dropsize = 3;
			y += txOffset * 2;
			overlayBounds.y += txOffset * 2;
		}
		if (HasFocus()) {
			dc.Draw()->Flush();
			dc.RebindTexture();
			float pulse = sin(time_now_d() * 7.0) * 0.25 + 0.8;
			dc.Draw()->DrawImage4Grid(dc.theme->dropShadow4Grid, x - dropsize*1.5f, y - dropsize*1.5f, x + w + dropsize*1.5f, y + h + dropsize*1.5f, alphaMul(color, pulse), 1.0f);
			dc.Draw()->Flush();
		} else {
			dc.Draw()->Flush();
			dc.RebindTexture();
			dc.Draw()->DrawImage4Grid(dc.theme->dropShadow4Grid, x - dropsize, y - dropsize*0.5f, x + w + dropsize, y + h + dropsize*1.5, alphaMul(shadowColor, 0.5f), 1.0f);
			dc.Draw()->Flush();
		}
	}

	if (texture) {
		dc.Draw()->Flush();
		dc.GetDrawContext()->BindTexture(0, texture);
		dc.Draw()->DrawTexRect(x, y, x + w, y + h, 0, 0, 1, 1, color);
		dc.Draw()->Flush();
	}

	dc.Draw()->Flush();
	dc.RebindTexture();
	dc.SetFontStyle(dc.theme->uiFont);

	float tw, th;
	dc.Draw()->Flush();
	dc.PushScissor(bounds_);

	UpdateText(ginfo);
	dc.MeasureText(dc.GetFontStyle(), 1.0f, 1.0f, title_.c_str(), &tw, &th, 0);

	int availableWidth = bounds_.w - 150;
	float sineWidth = std::max(0.0f, (tw - availableWidth)) / 2.0f;

	float tx = 150;
	if (availableWidth < tw) {
		float overageRatio = 1.5f * availableWidth * 1.0f / tw;
		tx -= (1.0f + sin(time_now_d() * overageRatio)) * sineWidth;
		Bounds tb = bounds_;
		tb.x = bounds_.x + 150;
		tb.w = bounds_.w - 150;
		dc.PushScissor(tb);
	}
	dc.DrawText(title_.c_str(), bounds_.x + tx, bounds_.y + 4, style.fgColor, ALIGN_TOPLEFT);
	dc.SetFontScale(0.6f, 0.6f);
	dc.DrawText(subtitle_.c_str(), bounds_.x + tx, bounds_.y2() - 7, style.fgColor, ALIGN_BOTTOM);
	dc.SetFontScale(1.0f, 1.0f);

	if (availableWidth < tw) {
		dc.PopScissor();
	}
	dc.Draw()->Flush();
	dc.PopScissor();

	dc.RebindTexture();
}

std::string SavedataButton::DescribeText() const {
	auto u = GetI18NCategory("UI Elements");
	return ReplaceAll(u->T("%1 button"), "%1", title_) + "\n" + subtitle_;
}

SavedataBrowser::SavedataBrowser(const Path &path, UI::LayoutParams *layoutParams)
	: LinearLayout(UI::ORIENT_VERTICAL, layoutParams), path_(path) {
	Refresh();
}

void SavedataBrowser::Update() {
	LinearLayout::Update();
	if (searchPending_) {
		searchPending_ = false;

		int n = gameList_->GetNumSubviews();
		bool matches = searchFilter_.empty();
		for (int i = 0; i < n; ++i) {
			SavedataButton *v = static_cast<SavedataButton *>(gameList_->GetViewByIndex(i));

			// Note: might be resetting to empty string.  Can do that right away.
			if (searchFilter_.empty()) {
				v->SetVisibility(UI::V_VISIBLE);
				continue;
			}

			if (!v->UpdateText()) {
				// We'll need to wait until the text is loaded.
				searchPending_ = true;
				v->SetVisibility(UI::V_GONE);
				continue;
			}

			std::string label = v->DescribeText();
			std::transform(label.begin(), label.end(), label.begin(), tolower);
			bool match = label.find(searchFilter_) != label.npos;
			matches = matches || match;
			v->SetVisibility(match ? UI::V_VISIBLE : UI::V_GONE);
		}

		if (searchingView_) {
			bool show = !searchFilter_.empty() && (matches || searchPending_);
			searchingView_->SetVisibility(show ? UI::V_VISIBLE : UI::V_GONE);
		}
		if (noMatchView_)
			noMatchView_->SetVisibility(matches || searchPending_ ? UI::V_GONE : UI::V_VISIBLE);
	}
}

void SavedataBrowser::SetSearchFilter(const std::string &filter) {
	auto sa = GetI18NCategory("Savedata");

	searchFilter_.resize(filter.size());
	std::transform(filter.begin(), filter.end(), searchFilter_.begin(), tolower);

	if (gameList_)
		searchPending_ = true;
	if (noMatchView_)
		noMatchView_->SetText(ReplaceAll(sa->T("Nothing matching '%1' was found."), "%1", filter));
	if (searchingView_)
		searchingView_->SetText(ReplaceAll(sa->T("Showing matches for '%1'."), "%1", filter));
}

void SavedataBrowser::SetSortOption(SavedataSortOption opt) {
	sortOption_ = opt;
	if (gameList_) {
		SortedLinearLayout *gl = static_cast<SortedLinearLayout *>(gameList_);
		if (sortOption_ == SavedataSortOption::FILENAME) {
			gl->SetCompare(&ByFilename, &SortDone);
		} else if (sortOption_ == SavedataSortOption::SIZE) {
			gl->SetCompare(&BySize, &SortDone);
		} else if (sortOption_ == SavedataSortOption::DATE) {
			gl->SetCompare(&ByDate, &SortDone);
		}
	}
}

bool SavedataBrowser::ByFilename(const UI::View *v1, const UI::View *v2) {
	const SavedataButton *b1 = static_cast<const SavedataButton *>(v1);
	const SavedataButton *b2 = static_cast<const SavedataButton *>(v2);

	return strcmp(b1->GamePath().c_str(), b2->GamePath().c_str()) < 0;
}

static time_t GetTotalSize(const SavedataButton *b) {
	auto fileLoader = std::unique_ptr<FileLoader>(ConstructFileLoader(b->GamePath()));
	std::string errorString;
	switch (Identify_File(fileLoader.get(), &errorString)) {
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
	case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		return File::GetDirectoryRecursiveSize(ResolvePBPDirectory(b->GamePath()), nullptr, File::GETFILES_GETHIDDEN);

	default:
		return fileLoader->FileSize();
	}
}

bool SavedataBrowser::BySize(const UI::View *v1, const UI::View *v2) {
	const SavedataButton *b1 = static_cast<const SavedataButton *>(v1);
	const SavedataButton *b2 = static_cast<const SavedataButton *>(v2);

	if (GetTotalSize(b1) > GetTotalSize(b2))
		return true;
	return strcmp(b1->GamePath().c_str(), b2->GamePath().c_str()) < 0;
}

static time_t GetDateSeconds(const SavedataButton *b) {
	auto fileLoader = std::unique_ptr<FileLoader>(ConstructFileLoader(b->GamePath()));
	tm datetm;
	bool success;
	std::string errorString;
	if (Identify_File(fileLoader.get(), &errorString) == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY) {
		success = File::GetModifTime(b->GamePath() / "PARAM.SFO", datetm);
	} else {
		success = File::GetModifTime(b->GamePath(), datetm);
	}

	if (success) {
		return mktime(&datetm);
	}
	return (time_t)0;
}

bool SavedataBrowser::ByDate(const UI::View *v1, const UI::View *v2) {
	const SavedataButton *b1 = static_cast<const SavedataButton *>(v1);
	const SavedataButton *b2 = static_cast<const SavedataButton *>(v2);

	if (GetDateSeconds(b1) > GetDateSeconds(b2))
		return true;
	return strcmp(b1->GamePath().c_str(), b2->GamePath().c_str()) < 0;
}

bool SavedataBrowser::SortDone() {
	return true;
}

void SavedataBrowser::Refresh() {
	using namespace UI;

	// Kill all the contents
	Clear();

	Add(new Spacer(1.0f));
	auto mm = GetI18NCategory("MainMenu");
	auto sa = GetI18NCategory("Savedata");

	// Find games in the current directory and create new ones.
	std::vector<SavedataButton *> savedataButtons;

	std::vector<File::FileInfo> fileInfo;
	GetFilesInDir(path_, &fileInfo, "ppst:");

	for (size_t i = 0; i < fileInfo.size(); i++) {
		bool isState = !fileInfo[i].isDirectory;
		bool isSaveData = false;
		
		if (!isState && File::Exists(path_ / fileInfo[i].name / "PARAM.SFO"))
			isSaveData = true;

		if (isSaveData || isState) {
			savedataButtons.push_back(new SavedataButton(fileInfo[i].fullName, new UI::LinearLayoutParams(UI::FILL_PARENT, UI::WRAP_CONTENT)));
		}
	}

	ViewGroup *group = new LinearLayout(ORIENT_VERTICAL, new UI::LinearLayoutParams(UI::Margins(12, 0)));
	Add(group);

	if (savedataButtons.empty()) {
		group->Add(new TextView(sa->T("None yet. Things will appear here after you save.")));
		gameList_ = nullptr;
		noMatchView_ = nullptr;
		searchingView_ = nullptr;
	} else {
		noMatchView_ = group->Add(new TextView(sa->T("Nothing matching '%1' was found")));
		noMatchView_->SetVisibility(UI::V_GONE);
		searchingView_ = group->Add(new TextView(sa->T("Showing matches for '%1'")));
		searchingView_->SetVisibility(UI::V_GONE);

		SortedLinearLayout *gl = new SortedLinearLayout(UI::ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		gl->SetSpacing(4.0f);
		gameList_ = gl;
		Add(gameList_);

		for (size_t i = 0; i < savedataButtons.size(); i++) {
			SavedataButton *b = gameList_->Add(savedataButtons[i]);
			b->OnClick.Handle(this, &SavedataBrowser::SavedataButtonClick);
		}
	}

	// Reapply.
	SetSortOption(sortOption_);
	if (!searchFilter_.empty())
		SetSearchFilter(searchFilter_);
}

UI::EventReturn SavedataBrowser::SavedataButtonClick(UI::EventParams &e) {
	SavedataButton *button = static_cast<SavedataButton *>(e.v);
	UI::EventParams e2{};
	e2.v = e.v;
	e2.s = button->GamePath().ToString();
	// Insta-update - here we know we are already on the right thread.
	OnChoice.Trigger(e2);
	return UI::EVENT_DONE;
}

SavedataScreen::SavedataScreen(const Path &gamePath) : UIDialogScreenWithGameBackground(gamePath) {
}

SavedataScreen::~SavedataScreen() {
	if (g_gameInfoCache) {
		g_gameInfoCache->PurgeType(IdentifiedFileType::PPSSPP_SAVESTATE);
		g_gameInfoCache->PurgeType(IdentifiedFileType::PSP_SAVEDATA_DIRECTORY);
	}
}

void SavedataScreen::CreateViews() {
	using namespace UI;
	auto sa = GetI18NCategory("Savedata");
	auto di = GetI18NCategory("Dialog");
	Path savedata_dir = GetSysDirectory(DIRECTORY_SAVEDATA);
	Path savestate_dir = GetSysDirectory(DIRECTORY_SAVESTATE);

	gridStyle_ = false;
	root_ = new AnchorLayout();

	// Make space for buttons.
	LinearLayout *main = new LinearLayout(ORIENT_VERTICAL, new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, 0, 0, 0, 84.0f));

	TabHolder *tabs = new TabHolder(ORIENT_HORIZONTAL, 64, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f));
	tabs->SetTag("Savedata");
	ScrollView *scroll = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	scroll->SetTag("SavedataBrowser");
	dataBrowser_ = scroll->Add(new SavedataBrowser(savedata_dir, new LayoutParams(FILL_PARENT, FILL_PARENT)));
	dataBrowser_->SetSortOption(sortOption_);
	if (!searchFilter_.empty())
		dataBrowser_->SetSearchFilter(searchFilter_);
	dataBrowser_->OnChoice.Handle(this, &SavedataScreen::OnSavedataButtonClick);

	tabs->AddTab(sa->T("Save Data"), scroll);

	ScrollView *scroll2 = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	scroll2->SetTag("SavedataStatesBrowser");
	stateBrowser_ = scroll2->Add(new SavedataBrowser(savestate_dir));
	stateBrowser_->SetSortOption(sortOption_);
	if (!searchFilter_.empty())
		stateBrowser_->SetSearchFilter(searchFilter_);
	stateBrowser_->OnChoice.Handle(this, &SavedataScreen::OnSavedataButtonClick);
	tabs->AddTab(sa->T("Save States"), scroll2);

	main->Add(tabs);

	ChoiceStrip *sortStrip = new ChoiceStrip(ORIENT_HORIZONTAL, new AnchorLayoutParams(NONE, 0, 0, NONE));
	sortStrip->AddChoice(sa->T("Filename"));
	sortStrip->AddChoice(sa->T("Size"));
	sortStrip->AddChoice(sa->T("Date"));
	sortStrip->SetSelection((int)sortOption_, false);
	sortStrip->OnChoice.Handle<SavedataScreen>(this, &SavedataScreen::OnSortClick);

	AddStandardBack(root_);
#if PPSSPP_PLATFORM(WINDOWS) || defined(USING_QT_UI) || defined(__ANDROID__)
	root_->Add(new Choice(di->T("Search"), "", false, new AnchorLayoutParams(WRAP_CONTENT, 64, NONE, NONE, 10, 10)))->OnClick.Handle<SavedataScreen>(this, &SavedataScreen::OnSearch);
#endif

	root_->Add(main);
	root_->Add(sortStrip);
}

UI::EventReturn SavedataScreen::OnSortClick(UI::EventParams &e) {
	sortOption_ = SavedataSortOption(e.a);

	dataBrowser_->SetSortOption(sortOption_);
	stateBrowser_->SetSortOption(sortOption_);

	return UI::EVENT_DONE;
}

UI::EventReturn SavedataScreen::OnSearch(UI::EventParams &e) {
	auto di = GetI18NCategory("Dialog");
#if PPSSPP_PLATFORM(WINDOWS) || defined(USING_QT_UI) || defined(__ANDROID__)
	System_InputBoxGetString(di->T("Filter"), searchFilter_, [](bool result, const std::string &value) {
		if (result) {
			NativeMessageReceived("savedatascreen_search", value.c_str());
		}
	});
#endif
	return UI::EVENT_DONE;
}

UI::EventReturn SavedataScreen::OnSavedataButtonClick(UI::EventParams &e) {
	std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(screenManager()->getDrawContext(), Path(e.s), 0);
	SavedataPopupScreen *popupScreen = new SavedataPopupScreen(e.s, ginfo->GetTitle());
	if (e.v) {
		popupScreen->SetPopupOrigin(e.v);
	}
	screenManager()->push(popupScreen);
	// the game path: e.s;
	return UI::EVENT_DONE;
}

void SavedataScreen::dialogFinished(const Screen *dialog, DialogResult result) {
	if (result == DR_NO) {
		RecreateViews();
	}
}

void SavedataScreen::sendMessage(const char *message, const char *value) {
	UIDialogScreenWithGameBackground::sendMessage(message, value);
	if (!strcmp(message, "savedatascreen_search")) {
		searchFilter_ = value;
		dataBrowser_->SetSearchFilter(searchFilter_);
		stateBrowser_->SetSearchFilter(searchFilter_);
	}
}
