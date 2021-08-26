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
#include <map>
#include <string>
#include <vector>

#include "Common/UI/UIScreen.h"
#include "Common/File/DirListing.h"
#include "Common/File/Path.h"

struct ShaderInfo;
struct TextureShaderInfo;

extern Path boot_filename;
void UIBackgroundInit(UIContext &dc);
void UIBackgroundShutdown();

inline void NoOpVoidBool(bool) {}

class UIScreenWithBackground : public UIScreen {
public:
	UIScreenWithBackground() : UIScreen() {}
protected:
	void DrawBackground(UIContext &dc) override;
	void sendMessage(const char *message, const char *value) override;
};

class UIScreenWithGameBackground : public UIScreenWithBackground {
public:
	UIScreenWithGameBackground(const std::string &gamePath)
		: UIScreenWithBackground(), gamePath_(gamePath) {}
	void DrawBackground(UIContext &dc) override;
	void sendMessage(const char *message, const char *value) override;
protected:
	Path gamePath_;
};

class UIDialogScreenWithBackground : public UIDialogScreen {
public:
	UIDialogScreenWithBackground() : UIDialogScreen() {}
protected:
	void DrawBackground(UIContext &dc) override;
	void sendMessage(const char *message, const char *value) override;

	void AddStandardBack(UI::ViewGroup *parent);
};

class UIDialogScreenWithGameBackground : public UIDialogScreenWithBackground {
public:
	UIDialogScreenWithGameBackground(const Path &gamePath)
		: UIDialogScreenWithBackground(), gamePath_(gamePath) {}
	void DrawBackground(UIContext &dc) override;
	void sendMessage(const char *message, const char *value) override;
protected:
	Path gamePath_;
};

class PromptScreen : public UIDialogScreenWithBackground {
public:
	PromptScreen(std::string message, std::string yesButtonText, std::string noButtonText,
		std::function<void(bool)> callback = &NoOpVoidBool);

	void CreateViews() override;

	void TriggerFinish(DialogResult result) override;

private:
	UI::EventReturn OnYes(UI::EventParams &e);
	UI::EventReturn OnNo(UI::EventParams &e);

	std::string message_;
	std::string yesButtonText_;
	std::string noButtonText_;
	std::function<void(bool)> callback_;
};

class NewLanguageScreen : public ListPopupScreen {
public:
	NewLanguageScreen(const std::string &title);

private:
	void OnCompleted(DialogResult result) override;
	bool ShowButtons() const override { return true; }
	std::map<std::string, std::pair<std::string, int>> langValuesMapping;
	std::map<std::string, std::string> titleCodeMapping;
	std::vector<File::FileInfo> langs_;
};

class PostProcScreen : public ListPopupScreen {
public:
	PostProcScreen(const std::string &title, int id);

private:
	void OnCompleted(DialogResult result) override;
	bool ShowButtons() const override { return true; }
	std::vector<ShaderInfo> shaders_;
	int id_;
};

class TextureShaderScreen : public ListPopupScreen {
public:
	TextureShaderScreen(const std::string &title);

private:
	void OnCompleted(DialogResult result) override;
	bool ShowButtons() const override { return true; }
	std::vector<TextureShaderInfo> shaders_;
};

class LogoScreen : public UIScreen {
public:
	LogoScreen(bool gotoGameSettings = false);
	bool key(const KeyInput &key) override;
	bool touch(const TouchInput &touch) override;
	void update() override;
	void render() override;
	void sendMessage(const char *message, const char *value) override;
	void CreateViews() override {}

private:
	void Next();
	int frames_ = 0;
	double sinceStart_ = 0.0;
	bool switched_ = false;
	bool gotoGameSettings_ = false;
};

class CreditsScreen : public UIDialogScreenWithBackground {
public:
	CreditsScreen();
	void update() override;
	void render() override;

	void CreateViews() override;

private:
	UI::EventReturn OnOK(UI::EventParams &e);

	UI::EventReturn OnSupport(UI::EventParams &e);
	UI::EventReturn OnPPSSPPOrg(UI::EventParams &e);
	UI::EventReturn OnPrivacy(UI::EventParams &e);
	UI::EventReturn OnForums(UI::EventParams &e);
	UI::EventReturn OnDiscord(UI::EventParams &e);
	UI::EventReturn OnShare(UI::EventParams &e);
	UI::EventReturn OnTwitter(UI::EventParams &e);

	double startTime_ = 0.0;
};

class SettingInfoMessage : public UI::LinearLayout {
public:
	SettingInfoMessage(int align, UI::AnchorLayoutParams *lp);

	void SetBottomCutoff(float y) {
		cutOffY_ = y;
	}
	void Show(const std::string &text, UI::View *refView = nullptr);

	void Draw(UIContext &dc);

private:
	UI::TextView *text_ = nullptr;
	double timeShown_ = 0.0;
	float cutOffY_;
};
