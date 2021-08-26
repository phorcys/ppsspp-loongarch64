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
#include <string>
#include <atomic>

#include "ppsspp_config.h"

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Common/Thread/Promise.h"

#include "UI/MiscScreens.h"

// MemStickScreen - let's you configure your memory stick directory.
// Currently only useful for Android.
class MemStickScreen : public UIDialogScreenWithBackground {
public:
	MemStickScreen(bool initialSetup);
	~MemStickScreen() {}

	std::string tag() const override { return "game"; }

protected:
	void CreateViews() override;

	void sendMessage(const char *message, const char *value) override;
	void dialogFinished(const Screen *dialog, DialogResult result) override;
	void update() override;
	void render() override {
		// Simple anti-flicker due to delayed finish.
		if (!done_) {
			// render as usual.
			UIDialogScreenWithBackground::render();
		} else {
			// no render. black frame insertion is better than flicker.
		}
	}

private:
	// Event handlers
	UI::EventReturn OnBrowse(UI::EventParams &e);
	UI::EventReturn OnUseInternalStorage(UI::EventParams &params);
	UI::EventReturn OnUseStorageRoot(UI::EventParams &params);

	SettingInfoMessage *settingInfo_ = nullptr;

	bool initialSetup_;
	bool done_ = false;
};

class ProgressReporter {
public:
	void Set(std::string value) {
		std::lock_guard<std::mutex> guard(mutex_);
		progress_ = value;
	}

	std::string Get() {
		std::lock_guard<std::mutex> guard(mutex_);
		return progress_;
	}

private:
	std::string progress_;
	std::mutex mutex_;
};

class ConfirmMemstickMoveScreen : public UIDialogScreenWithBackground {
public:
	ConfirmMemstickMoveScreen(Path newMemstickFolder, bool initialSetup);
	~ConfirmMemstickMoveScreen();
protected:
	void update() override;
	void CreateViews() override;

private:
	UI::EventReturn OnMoveDataClick(UI::EventParams &params);
	void FinishFolderMove();

	UI::EventReturn OnConfirm(UI::EventParams &params);

	Path newMemstickFolder_;
	bool existingFilesInNewFolder_;
	bool moveData_ = true;
	bool initialSetup_;

	ProgressReporter progressReporter_;
	UI::TextView *progressView_ = nullptr;

	Promise<bool> *moveDataTask_ = nullptr;

	std::string error_;
};
