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

#pragma once

#include <functional>

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/ViewGroup.h"
#include "UI/MiscScreens.h"
#include "Common/File/PathBrowser.h"

enum GameBrowserFlags {
	FLAG_HOMEBREWSTOREBUTTON = 1
};

enum class BrowseFlags {
	NONE = 0,
	NAVIGATE = 1,
	ARCHIVES = 2,
	PIN = 4,
	HOMEBREW_STORE = 8,
	STANDARD = 1 | 2 | 4,
};
ENUM_CLASS_BITOPS(BrowseFlags);

bool LaunchFile(ScreenManager *screenManager, const Path &path);

class GameBrowser : public UI::LinearLayout {
public:
	GameBrowser(const Path &path, BrowseFlags browseFlags, bool *gridStyle, ScreenManager *screenManager, std::string lastText, std::string lastLink, UI::LayoutParams *layoutParams = nullptr);

	UI::Event OnChoice;
	UI::Event OnHoldChoice;
	UI::Event OnHighlight;

	void FocusGame(const Path &gamePath);
	void SetPath(const Path &path);
	void Draw(UIContext &dc) override;
	void Update() override;

protected:
	virtual bool DisplayTopBar();
	virtual bool HasSpecialFiles(std::vector<Path> &filenames);
	virtual Path HomePath();

	void Refresh();

private:
	bool IsCurrentPathPinned();
	const std::vector<Path> GetPinnedPaths();
	const std::string GetBaseName(const std::string &path);

	UI::EventReturn GameButtonClick(UI::EventParams &e);
	UI::EventReturn GameButtonHoldClick(UI::EventParams &e);
	UI::EventReturn GameButtonHighlight(UI::EventParams &e);
	UI::EventReturn NavigateClick(UI::EventParams &e);
	UI::EventReturn LayoutChange(UI::EventParams &e);
	UI::EventReturn LastClick(UI::EventParams &e);
	UI::EventReturn BrowseClick(UI::EventParams &e);
	UI::EventReturn StorageClick(UI::EventParams &e);
	UI::EventReturn OnHomeClick(UI::EventParams &e);
	UI::EventReturn PinToggleClick(UI::EventParams &e);
	UI::EventReturn GridSettingsClick(UI::EventParams &e);
	UI::EventReturn OnRecentClear(UI::EventParams &e);
	UI::EventReturn OnHomebrewStore(UI::EventParams &e);

	UI::ViewGroup *gameList_ = nullptr;
	PathBrowser path_;
	bool *gridStyle_ = nullptr;
	BrowseFlags browseFlags_;
	std::string lastText_;
	std::string lastLink_;
	Path focusGamePath_;
	bool listingPending_ = false;
	float lastScale_ = 1.0f;
	bool lastLayoutWasGrid_ = true;
	ScreenManager *screenManager_;
};

class RemoteISOBrowseScreen;

class MainScreen : public UIScreenWithBackground {
public:
	MainScreen();
	~MainScreen();

	bool isTopLevel() const override { return true; }

	// Horrible hack to show the demos & homebrew tab after having installed a game from a zip file.
	static bool showHomebrewTab;

protected:
	void CreateViews() override;
	void DrawBackground(UIContext &dc) override;
	void update() override;
	void sendMessage(const char *message, const char *value) override;
	void dialogFinished(const Screen *dialog, DialogResult result) override;

	bool UseVerticalLayout() const;
	bool DrawBackgroundFor(UIContext &dc, const Path &gamePath, float progress);

	UI::EventReturn OnGameSelected(UI::EventParams &e);
	UI::EventReturn OnGameSelectedInstant(UI::EventParams &e);
	UI::EventReturn OnGameHighlight(UI::EventParams &e);
	// Event handlers
	UI::EventReturn OnLoadFile(UI::EventParams &e);
	UI::EventReturn OnGameSettings(UI::EventParams &e);
	UI::EventReturn OnCredits(UI::EventParams &e);
	UI::EventReturn OnSupport(UI::EventParams &e);
	UI::EventReturn OnPPSSPPOrg(UI::EventParams &e);
	UI::EventReturn OnForums(UI::EventParams &e);
	UI::EventReturn OnExit(UI::EventParams &e);
	UI::EventReturn OnDownloadUpgrade(UI::EventParams &e);
	UI::EventReturn OnDismissUpgrade(UI::EventParams &e);
	UI::EventReturn OnAllowStorage(UI::EventParams &e);
	UI::EventReturn OnFullScreenToggle(UI::EventParams &e);

	UI::LinearLayout *upgradeBar_ = nullptr;
	UI::TabHolder *tabHolder_ = nullptr;
	UI::Button *fullscreenButton_ = nullptr;

	Path restoreFocusGamePath_;
	std::vector<GameBrowser *> gameBrowsers_;

	Path highlightedGamePath_;
	Path prevHighlightedGamePath_;
	float highlightProgress_ = 0.0f;
	float prevHighlightProgress_ = 0.0f;
	bool backFromStore_ = false;
	bool lockBackgroundAudio_ = false;
	bool lastVertical_;
	bool confirmedTemporary_ = false;
	UI::ScrollView *scrollAllGames_ = nullptr;

	friend class RemoteISOBrowseScreen;
};

class UmdReplaceScreen : public UIDialogScreenWithBackground {
public:
	UmdReplaceScreen() {}

protected:
	void CreateViews() override;
	void update() override;
	//virtual void sendMessage(const char *message, const char *value);

private:
	UI::EventReturn OnGameSelected(UI::EventParams &e);
	UI::EventReturn OnGameSelectedInstant(UI::EventParams &e);

	UI::EventReturn OnCancel(UI::EventParams &e);
	UI::EventReturn OnGameSettings(UI::EventParams &e);
};

class GridSettingsScreen : public PopupScreen {
public:
	GridSettingsScreen(std::string label) : PopupScreen(label) {}
	void CreatePopupContents(UI::ViewGroup *parent) override;
	UI::Event OnRecentChanged;

private:
	UI::EventReturn GridPlusClick(UI::EventParams &e);
	UI::EventReturn GridMinusClick(UI::EventParams &e);
	UI::EventReturn OnRecentClearClick(UI::EventParams &e);
	const float MAX_GAME_GRID_SCALE = 3.0f;
	const float MIN_GAME_GRID_SCALE = 0.8f;
};
