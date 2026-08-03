#pragma once

#include <array>
#include <string_view>
#include <vector>

namespace SceneSettingsPolicy
{
	using SettingBlacklistPath = std::vector<std::string_view>;

	inline const std::vector<SettingBlacklistPath> kSettingBlacklist = {
		{ "CSUtility", "Scene Dof" },
		{ "CSUtility", "Underwater Dof" },
		{ "PostProcessing", "Border" },
		{ "PostProcessing", "LUT" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "Enable Tonemapping" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "Use OpenDRT" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "Tonemapper" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "Tonemapper Settings" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "ODRT1" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "ODRT2" },
	};

	inline constexpr std::array<std::string_view, 6> kLocationFeatureWhitelist = {
		"CSUtility",
		"ScreenSpaceGI",
		"ScreenSpaceShadows",
		"SubsurfaceScattering",
		"ImageBasedLighting",
		"VanillaFresnel",
	};

	inline constexpr std::array<std::string_view, 8> kTimeOfDayFeatureWhitelist = {
		"CSUtility",
		"CloudShadows",
		"ExponentialHeightFog",
		"GrassLighting",
		"ImageBasedLighting",
		"Skylighting",
		"SubsurfaceScattering",
		"WetnessEffects",
	};
}
