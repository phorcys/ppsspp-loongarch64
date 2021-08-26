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

#include "TiltAnalogSettingsScreen.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Common/Data/Text/I18n.h"

void TiltAnalogSettingsScreen::CreateViews() {
	using namespace UI;

	auto co = GetI18NCategory("Controls");
	auto di = GetI18NCategory("Dialog");

	root_ = new ScrollView(ORIENT_VERTICAL);
	root_->SetTag("TiltAnalogSettings");

	LinearLayout *settings = new LinearLayoutList(ORIENT_VERTICAL);

	settings->SetSpacing(0);
	settings->Add(new ItemHeader(co->T("Invert Axes")));
	settings->Add(new CheckBox(&g_Config.bInvertTiltX, co->T("Invert Tilt along X axis")));
	settings->Add(new CheckBox(&g_Config.bInvertTiltY, co->T("Invert Tilt along Y axis")));
	static const char* tiltMode[] = { "Screen aligned to ground", "Screen at right angle to ground", "Auto-switch" };
	settings->Add(new PopupMultiChoice(&g_Config.iTiltOrientation, co->T("Base tilt position"), tiltMode, 0, ARRAY_SIZE(tiltMode), co->GetName(), screenManager()));

	settings->Add(new ItemHeader(co->T("Sensitivity")));
	//TODO: allow values greater than 100? I'm not sure if that's needed.
	settings->Add(new PopupSliderChoice(&g_Config.iTiltSensitivityX, 0, 100, co->T("Tilt Sensitivity along X axis"), screenManager(),"%"));
	settings->Add(new PopupSliderChoice(&g_Config.iTiltSensitivityY, 0, 100, co->T("Tilt Sensitivity along Y axis"), screenManager(),"%"));
	settings->Add(new PopupSliderChoiceFloat(&g_Config.fDeadzoneRadius, 0.0, 1.0, co->T("Deadzone Radius"), 0.01f, screenManager(),"/ 1.0"));
	settings->Add(new PopupSliderChoiceFloat(&g_Config.fTiltDeadzoneSkip, 0.0, 1.0, co->T("Tilt Base Radius"), 0.01f, screenManager(),"/ 1.0"));

	settings->Add(new ItemHeader(co->T("Calibration")));
	InfoItem *calibrationInfo = new InfoItem(co->T("To Calibrate", "To calibrate, keep device on a flat surface and press calibrate."), "");
	settings->Add(calibrationInfo);

	Choice *calibrate = new Choice(co->T("Calibrate D-Pad"));
	calibrate->OnClick.Handle(this, &TiltAnalogSettingsScreen::OnCalibrate);
	settings->Add(calibrate);

	root_->Add(settings);
	settings->Add(new ItemHeader(""));
	settings->Add(new Choice(di->T("Back")))->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);
}

bool TiltAnalogSettingsScreen::axis(const AxisInput &axis) {
	if (axis.deviceId == DEVICE_ID_ACCELEROMETER) {
		// Historically, we've had X and Y swapped, likely due to portrait vs landscape.
		// TODO: We may want to configure this based on screen orientation.
		if (axis.axisId == JOYSTICK_AXIS_ACCELEROMETER_X) {
			currentTiltY_ = axis.value;
		}
		if (axis.axisId == JOYSTICK_AXIS_ACCELEROMETER_Y) {
			currentTiltX_ = axis.value;
		}
	}
	return false;
}

UI::EventReturn TiltAnalogSettingsScreen::OnCalibrate(UI::EventParams &e) {
	g_Config.fTiltBaseX = currentTiltX_;
	g_Config.fTiltBaseY = currentTiltY_;

	return UI::EVENT_DONE;
}

