// Copyright (c) 2014- PPSSPP Project.

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

#include "Common/UI/UIScreen.h"
#include "Common/UI/ViewGroup.h"
#include "UI/MiscScreens.h"
#include "Common/File/Path.h"

enum class ReportingOverallScore : int {
	PERFECT = 0,
	PLAYABLE = 1,
	INGAME = 2,
	MENU = 3,
	NONE = 4,
	INVALID = -1,
};

class ReportScreen : public UIDialogScreenWithGameBackground {
public:
	ReportScreen(const Path &gamePath);

protected:
	void postRender() override;
	void update() override;
	void resized() override;
	void CreateViews() override;
	void UpdateSubmit();
	void UpdateCRCInfo();
	void UpdateOverallDescription();

	UI::EventReturn HandleChoice(UI::EventParams &e);
	UI::EventReturn HandleSubmit(UI::EventParams &e);
	UI::EventReturn HandleBrowser(UI::EventParams &e);
	UI::EventReturn HandleShowCRC(UI::EventParams &e);
	UI::EventReturn HandleReportingChange(UI::EventParams &e);

	UI::Choice *submit_ = nullptr;
	UI::View *screenshot_ = nullptr;
	UI::TextView *reportingNotice_ = nullptr;
	UI::TextView *overallDescription_ = nullptr;
	UI::TextView *crcInfo_ = nullptr;
	UI::Choice *showCrcButton_ = nullptr;
	Path screenshotFilename_;

	ReportingOverallScore overall_ = ReportingOverallScore::INVALID;
	int graphics_ = -1;
	int speed_ = -1;
	int gameplay_ = -1;
	bool enableReporting_;
	bool ratingEnabled_;
	bool tookScreenshot_ = false;
	bool includeScreenshot_ = true;
	bool showCRC_ = false;
};

class ReportFinishScreen : public UIDialogScreenWithGameBackground {
public:
	ReportFinishScreen(const Path &gamePath, ReportingOverallScore score);

protected:
	void update() override;
	void CreateViews() override;
	void ShowSuggestions();

	UI::EventReturn HandleViewFeedback(UI::EventParams &e);

	UI::TextView *resultNotice_ = nullptr;
	UI::LinearLayout *resultItems_ = nullptr;
	ReportingOverallScore score_;
	bool setStatus_ = false;
};
