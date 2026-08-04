#pragma once

#include <array>
#include <string_view>
#include <vector>

namespace SceneSettingsPolicy
{
	using SettingBlacklistPath = std::vector<std::string_view>;

	inline const std::vector<SettingBlacklistPath> kSettingBlacklist = {
		{ "CSUtility", "Scene Dof", "locked" },
		{ "CSUtility", "Scene Dof", "baseline" },
		{ "CSUtility", "Underwater Dof", "locked" },
		{ "CSUtility", "Underwater Dof", "baseline" },
		{ "CSUtility", "Underwater Dof", "values", "autoFocus" },
		{ "CSUtility", "Underwater Dof", "values", "autoFocusSettings" },
		{ "CSUtility", "Underwater Dof", "values", "mode" },
		{ "PostProcessing", "Border" },
		{ "PostProcessing", "LUT" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "enableTonemap" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "useOpenDrt" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "currentTonemapper" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "tonemapParams" },
	};

	inline constexpr std::array<std::string_view, 7> kLocationFeatureWhitelist = {
		"CSUtility",
		"PostProcessing",
		"ScreenSpaceGI",
		"ScreenSpaceShadows",
		"SubsurfaceScattering",
		"ImageBasedLighting",
		"VanillaFresnel",
	};

	inline constexpr std::array<std::string_view, 9> kTimeOfDayFeatureWhitelist = {
		"CSUtility",
		"CloudShadows",
		"ExponentialHeightFog",
		"GrassLighting",
		"ImageBasedLighting",
		"PostProcessing",
		"Skylighting",
		"SubsurfaceScattering",
		"WetnessEffects",
	};
}
