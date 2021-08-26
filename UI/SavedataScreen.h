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

#include <functional>
#include <string>

#include "Common/File/Path.h"

#include "Common/UI/UIScreen.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"

#include "UI/MiscScreens.h"

enum class SavedataSortOption {
	FILENAME,
	SIZE,
	DATE,
};

class SavedataBrowser : public UI::LinearLayout {
public:
	SavedataBrowser(const Path &path, UI::LayoutParams *layoutParams = 0);

	void Update() override;

	void SetSortOption(SavedataSortOption opt);
	void SetSearchFilter(const std::string &filter);

	UI::Event OnChoice;

private:
	static bool ByFilename(const UI::View *, const UI::View *);
	static bool BySize(const UI::View *, const UI::View *);
	static bool ByDate(const UI::View *, const UI::View *);
	static bool SortDone();

	void Refresh();
	UI::EventReturn SavedataButtonClick(UI::EventParams &e);

	SavedataSortOption sortOption_ = SavedataSortOption::FILENAME;
	UI::ViewGroup *gameList_ = nullptr;
	UI::TextView *noMatchView_ = nullptr;
	UI::TextView *searchingView_ = nullptr;
	Path path_;
	std::string searchFilter_;
	bool searchPending_ = false;
};

class SavedataScreen : public UIDialogScreenWithGameBackground {
public:
	// gamePath can be empty, in that case this screen will show all savedata in the save directory.
	SavedataScreen(const Path &gamePath);
	~SavedataScreen();

	void dialogFinished(const Screen *dialog, DialogResult result) override;
	void sendMessage(const char *message, const char *value) override;

protected:
	UI::EventReturn OnSavedataButtonClick(UI::EventParams &e);
	UI::EventReturn OnSortClick(UI::EventParams &e);
	UI::EventReturn OnSearch(UI::EventParams &e);
	void CreateViews() override;

	bool gridStyle_;
	SavedataSortOption sortOption_ = SavedataSortOption::FILENAME;
	SavedataBrowser *dataBrowser_;
	SavedataBrowser *stateBrowser_;
	std::string searchFilter_;
};
