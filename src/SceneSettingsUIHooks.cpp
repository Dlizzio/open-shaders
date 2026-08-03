#include "SceneSettingsUIHooks.h"

#include "Feature.h"
#include "I18n/I18n.h"
#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

namespace
{
	using CheckboxFn = bool (*)(const char*, bool*);
	using CheckboxFlagsIntFn = bool (*)(const char*, int*, int);
	using CheckboxFlagsUIntFn = bool (*)(const char*, unsigned int*, unsigned int);
	using RadioButtonIntFn = bool (*)(const char*, int*, int);
	using RadioButtonBoolFn = bool (*)(const char*, bool);
	using ComboItemsFn = bool (*)(const char*, int*, const char* const*, int, int);
	using ComboStringFn = bool (*)(const char*, int*, const char*, int);
	using ComboGetterFn = bool (*)(const char*, int*, const char* (*)(void*, int), void*, int, int);
	using DragFloatFn = bool (*)(const char*, float*, float, float, float, const char*, ImGuiSliderFlags);
	using DragIntFn = bool (*)(const char*, int*, float, int, int, const char*, ImGuiSliderFlags);
	using SliderFloatFn = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
	using SliderIntFn = bool (*)(const char*, int*, int, int, const char*, ImGuiSliderFlags);
	using SliderAngleFn = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
	using PercentageSliderFn = bool (*)(const char*, float*, float, float, const char*);
	using InputFloatFn = bool (*)(const char*, float*, float, float, const char*, ImGuiInputTextFlags);
	using InputIntFn = bool (*)(const char*, int*, int, int, ImGuiInputTextFlags);
	using ColorEdit3Fn = bool (*)(const char*, float*, ImGuiColorEditFlags);
	using ColorEdit4Fn = bool (*)(const char*, float*, ImGuiColorEditFlags);
	using BeginComboFn = bool (*)(const char*, const char*, ImGuiComboFlags);

	CheckboxFn g_checkbox = static_cast<CheckboxFn>(&ImGui::Checkbox);
	CheckboxFlagsIntFn g_checkboxFlagsInt = static_cast<CheckboxFlagsIntFn>(&ImGui::CheckboxFlags);
	CheckboxFlagsUIntFn g_checkboxFlagsUInt = static_cast<CheckboxFlagsUIntFn>(&ImGui::CheckboxFlags);
	RadioButtonIntFn g_radioButtonInt = static_cast<RadioButtonIntFn>(&ImGui::RadioButton);
	RadioButtonBoolFn g_radioButtonBool = static_cast<RadioButtonBoolFn>(&ImGui::RadioButton);
	ComboItemsFn g_comboItems = static_cast<ComboItemsFn>(&ImGui::Combo);
	ComboStringFn g_comboString = static_cast<ComboStringFn>(&ImGui::Combo);
	ComboGetterFn g_comboGetter = static_cast<ComboGetterFn>(&ImGui::Combo);
	DragFloatFn g_dragFloat = static_cast<DragFloatFn>(&ImGui::DragFloat);
	DragIntFn g_dragInt = static_cast<DragIntFn>(&ImGui::DragInt);
	SliderFloatFn g_sliderFloat = static_cast<SliderFloatFn>(&ImGui::SliderFloat);
	SliderIntFn g_sliderInt = static_cast<SliderIntFn>(&ImGui::SliderInt);
	SliderAngleFn g_sliderAngle = static_cast<SliderAngleFn>(&ImGui::SliderAngle);
	PercentageSliderFn g_percentageSlider = &Util::PercentageSlider;
	InputFloatFn g_inputFloat = static_cast<InputFloatFn>(&ImGui::InputFloat);
	InputIntFn g_inputInt = static_cast<InputIntFn>(&ImGui::InputInt);
	ColorEdit3Fn g_colorEdit3 = static_cast<ColorEdit3Fn>(&ImGui::ColorEdit3);
	ColorEdit4Fn g_colorEdit4 = static_cast<ColorEdit4Fn>(&ImGui::ColorEdit4);
	BeginComboFn g_beginCombo = static_cast<BeginComboFn>(&ImGui::BeginCombo);

	thread_local Feature* g_currentFeature = nullptr;
	thread_local bool g_sceneSettingsActive = false;
	bool g_installed = false;

	const SceneSettingsCatalog::SettingMetadata* FindControlSetting(const void* valueAddress)
	{
		if (!g_sceneSettingsActive || !g_currentFeature)
			return nullptr;

		const void* settingAddress = Util::GetActiveControlStorageAddress();
		if (!settingAddress)
			settingAddress = valueAddress;
		auto* setting = SceneSettingsCatalog::FindSettingForControl(g_currentFeature, settingAddress);
		if (!setting || !SceneSettingsManager::IsSceneSettingAllowed(
				setting->featureShortName, setting->settingPath, setting->settingKey))
			return nullptr;

		auto* sceneManager = SceneSettingsManager::GetSingleton();
		if (!sceneManager->IsActiveSceneSetting(setting->featureShortName, setting->settingPath, setting->settingKey))
			return nullptr;

		return setting;
	}

	void DrawSceneSettingTooltip()
	{
		if (auto tooltip = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"%s",
				T("feature.scene_manager.controlled_tooltip",
					"Controlled by active scene settings. Disable Scene Specific Settings for this feature to edit this setting."));
		}
	}

	template <class Draw>
	bool DrawControl(const char*, const void* valueAddress, Draw draw)
	{
		if (!FindControlSetting(valueAddress))
			return draw();

		ImGui::BeginDisabled();
		draw();
		ImGui::EndDisabled();
		DrawSceneSettingTooltip();
		return false;
	}

	template <class Target>
	bool Attach(Target& target, Target detour)
	{
		return DetourAttach(reinterpret_cast<PVOID*>(&target), reinterpret_cast<PVOID>(detour)) == NO_ERROR;
	}

	bool CheckboxDetour(const char* label, bool* value)
	{
		return DrawControl(label, value, [&] { return g_checkbox(label, value); });
	}

	bool CheckboxFlagsIntDetour(const char* label, int* flags, int flagsValue)
	{
		return DrawControl(label, flags, [&] { return g_checkboxFlagsInt(label, flags, flagsValue); });
	}

	bool CheckboxFlagsUIntDetour(const char* label, unsigned int* flags, unsigned int flagsValue)
	{
		return DrawControl(label, flags, [&] { return g_checkboxFlagsUInt(label, flags, flagsValue); });
	}

	bool RadioButtonIntDetour(const char* label, int* value, int buttonValue)
	{
		return DrawControl(label, value, [&] { return g_radioButtonInt(label, value, buttonValue); });
	}

	bool RadioButtonBoolDetour(const char* label, bool active)
	{
		return DrawControl(label, nullptr, [&] { return g_radioButtonBool(label, active); });
	}

	bool ComboItemsDetour(const char* label, int* currentItem, const char* const* items, int itemsCount, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboItems(label, currentItem, items, itemsCount, popupMaxHeightInItems); });
	}

	bool ComboStringDetour(const char* label, int* currentItem, const char* itemsSeparatedByZeros, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboString(label, currentItem, itemsSeparatedByZeros, popupMaxHeightInItems); });
	}

	bool ComboGetterDetour(const char* label, int* currentItem, const char* (*getter)(void*, int), void* userData, int itemsCount, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboGetter(label, currentItem, getter, userData, itemsCount, popupMaxHeightInItems); });
	}

	bool DragFloatDetour(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragFloat(label, value, speed, min, max, format, flags); });
	}

	bool DragIntDetour(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragInt(label, value, speed, min, max, format, flags); });
	}

	bool SliderFloatDetour(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderFloat(label, value, min, max, format, flags); });
	}

	bool SliderIntDetour(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderInt(label, value, min, max, format, flags); });
	}

	bool SliderAngleDetour(const char* label, float* value, float minDegrees, float maxDegrees,
		const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_sliderAngle(label, value, minDegrees, maxDegrees, format, flags); });
	}

	bool PercentageSliderDetour(const char* label, float* value, float minPercent, float maxPercent,
		const char* format)
	{
		return DrawControl(label, value,
			[&] { return g_percentageSlider(label, value, minPercent, maxPercent, format); });
	}

	bool InputFloatDetour(const char* label, float* value, float step, float stepFast, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputFloat(label, value, step, stepFast, format, flags); });
	}

	bool InputIntDetour(const char* label, int* value, int step, int stepFast, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputInt(label, value, step, stepFast, flags); });
	}

	bool ColorEdit3Detour(const char* label, float* color, ImGuiColorEditFlags flags)
	{
		return DrawControl(label, color, [&] { return g_colorEdit3(label, color, flags); });
	}

	bool ColorEdit4Detour(const char* label, float* color, ImGuiColorEditFlags flags)
	{
		return DrawControl(label, color, [&] { return g_colorEdit4(label, color, flags); });
	}

	bool BeginComboDetour(const char* label, const char* previewValue, ImGuiComboFlags flags)
	{
		if (!FindControlSetting(nullptr))
			return g_beginCombo(label, previewValue, flags);

		ImGui::BeginDisabled();
		bool opened = g_beginCombo(label, previewValue, flags);
		if (opened)
			ImGui::EndCombo();
		ImGui::EndDisabled();
		DrawSceneSettingTooltip();
		return false;
	}
}

namespace SceneSettingsUIHooks
{
	FeatureDrawGuard::FeatureDrawGuard(Feature* feature, bool enabled) :
		previousFeature(g_currentFeature),
		previousEnabled(g_sceneSettingsActive)
	{
		g_currentFeature = enabled ? feature : nullptr;
		g_sceneSettingsActive = enabled && feature != nullptr;
	}

	FeatureDrawGuard::~FeatureDrawGuard()
	{
		if (g_sceneSettingsActive && g_currentFeature)
			SceneSettingsManager::GetSingleton()->CaptureExternalFeatureChanges(g_currentFeature);
		g_currentFeature = previousFeature;
		g_sceneSettingsActive = previousEnabled;
	}

	void Install()
	{
		if (g_installed)
			return;

		auto result = DetourTransactionBegin();
		if (result != NO_ERROR) {
			logger::warn("[SceneSettings] Failed to begin ImGui hook transaction: {}", result);
			return;
		}

		result = DetourUpdateThread(GetCurrentThread());
		if (result != NO_ERROR) {
			DetourTransactionAbort();
			logger::warn("[SceneSettings] Failed to enlist the current thread for ImGui hooks: {}", result);
			return;
		}

		bool attached =
			Attach(g_checkbox, CheckboxDetour) &&
			Attach(g_checkboxFlagsInt, CheckboxFlagsIntDetour) &&
			Attach(g_checkboxFlagsUInt, CheckboxFlagsUIntDetour) &&
			Attach(g_radioButtonInt, RadioButtonIntDetour) &&
			Attach(g_radioButtonBool, RadioButtonBoolDetour) &&
			Attach(g_comboItems, ComboItemsDetour) &&
			Attach(g_comboString, ComboStringDetour) &&
			Attach(g_comboGetter, ComboGetterDetour) &&
			Attach(g_dragFloat, DragFloatDetour) &&
			Attach(g_dragInt, DragIntDetour) &&
			Attach(g_sliderFloat, SliderFloatDetour) &&
			Attach(g_sliderInt, SliderIntDetour) &&
			Attach(g_sliderAngle, SliderAngleDetour) &&
			Attach(g_percentageSlider, PercentageSliderDetour) &&
			Attach(g_inputFloat, InputFloatDetour) &&
			Attach(g_inputInt, InputIntDetour) &&
			Attach(g_colorEdit3, ColorEdit3Detour) &&
			Attach(g_colorEdit4, ColorEdit4Detour) &&
			Attach(g_beginCombo, BeginComboDetour);

		if (!attached) {
			DetourTransactionAbort();
			logger::warn("[SceneSettings] Failed to attach ImGui scene setting hooks");
			return;
		}

		result = DetourTransactionCommit();
		if (result != NO_ERROR) {
			logger::warn("[SceneSettings] Failed to install ImGui scene setting hooks: {}", result);
			return;
		}

		g_installed = true;
		logger::info("[SceneSettings] Installed ImGui scene setting hooks");
	}
}
