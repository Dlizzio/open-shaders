#pragma once

#include "LinearLighting/SemanticColorCache.h"

struct LinearLighting : Feature
{
	static LinearLighting* GetSingleton()
	{
		static LinearLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Linear Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.linear_lighting.name", "Linear Lighting"); }
	virtual inline std::string GetShortName() override { return "LinearLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.linear_lighting.description", "Linear Lighting does internal color space conversion to improve lighting calculation accuracy."),
			{ T("feature.linear_lighting.key_feature_1", "Semantic authored-color conversion"),
				T("feature.linear_lighting.key_feature_2", "Corrects lighting calculations"),
				T("feature.linear_lighting.key_feature_3", "Makes PBR really work") } };
	};

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	struct Settings
	{
		uint enableLinearLighting = false;
		uint enableACEScg = false;
	} settings;

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableACEScg;
		uint pad0[2];
		RE::NiColor effectLightingColor;
		float pad1;
		RE::NiColor skyStaticsColor;
		float pad2;
		RE::NiColor effectLightingReference;
		float pad3;
		RE::NiColor skyStaticsReference;
		float pad4;
		RE::NiColor directionalLightingReference;
		float pad5;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 0x60);

	winrt::com_ptr<ID3D11ComputeShader> sceneGammaDecodeCS;
	bool sceneGammaActive = false;
	bool sceneGammaDecodedByRefraction = false;

	/** @brief Draws the ImGui settings UI for the linear working-space modes. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Installs typed color-producer and scene-boundary hooks. */
	virtual void PostPostLoad() override;
	/** @brief Clears semantic color sidecars before old scene objects are destroyed. */
	virtual void OnSceneTransitionReset(bool a_opening) override;

	/** @brief Creates the scene gamma decoder. */
	virtual void SetupResources() override;
	/** @brief Marks kMAIN as gamma-domain storage for the main world-rendering interval. */
	void BeginSceneGamma();
	/** @brief Decodes the completed gamma-domain scene before post-processing consumes it. */
	void EndSceneGamma(RE::RENDER_TARGET a_renderTarget);

	/** @brief Populates and returns the linear-lighting per-frame data. */
	PerFrameData GetCommonBufferData();
	/** @brief Returns whether the engine and shaders should currently use linear lighting data. */
	bool IsLinearLightingActive() const;
	/** @brief Returns whether the active linear pipeline uses the ACEScg working gamut. */
	bool IsACEScgActive() const;

	/** @brief Restores the authored sidecar before Skyrim updates a transient subset. */
	void BeginSkyColorUpdate(RE::Sky* a_sky);
	/** @brief Captures Skyrim's completed weather colors and publishes the selected domain. */
	void UpdateSkyColors(RE::Sky* a_sky);
	/** @brief Captures completed cloud colors and publishes the selected domain. */
	void UpdateCloudColors(RE::Clouds* a_clouds);
	/** @brief Decodes a transient copy of the final directional ambient colors. */
	void DecodeDirectionalAmbientColors(RE::NiColor (&a_colors)[3][2], RE::NiColor* a_specularTint);
	/** @brief Decodes typed colors in the engine's transient mapped PS constant group. */
	void PatchMappedPSConstants(ID3D11Resource* a_resource);
	/** @brief Returns a cached working-space copy of a non-photometric point-light color. */
	RE::NiColor GetWorkingPointLightColor(const RE::NiLight* a_light, const RE::NiColor& a_source, bool a_alreadyLinear);
	/** @brief Returns a cached working-space copy for a typed authored-color producer. */
	RE::NiColor GetWorkingAuthoredColor(
		const void* a_owner,
		LinearLightingColors::Semantic a_semantic,
		const RE::NiColor& a_authored);
	/** @brief Returns a cached scaled working-space copy for a typed authored-color producer. */
	RE::NiColor GetWorkingAuthoredColorScaled(
		const void* a_owner,
		LinearLightingColors::Semantic a_semantic,
		const RE::NiColor& a_authored,
		float a_scale);
	/** @brief Returns a cached working-space copy for an already-linear typed producer. */
	RE::NiColor GetWorkingLinearColor(
		const void* a_owner,
		LinearLightingColors::Semantic a_semantic,
		const RE::NiColor& a_linear);
	/** @brief Returns a cached linear-sRGB copy without applying the optional working-gamut transform. */
	RE::NiColor GetLinearAuthoredColor(
		const void* a_owner,
		LinearLightingColors::Semantic a_semantic,
		const RE::NiColor& a_authored);

	/** @brief Contains the BSLightingShader geometry setup hook implementation. */
	struct Hooks;

private:
	struct SkyColorState
	{
		RE::Sky* owner{};
		std::array<RE::NiColor, RE::TESWeather::ColorTypes::kTotal> authored{};
		bool valid{};
		bool publishedLinear{};
	};

	struct CloudColorState
	{
		RE::Clouds* owner{};
		std::array<RE::NiColor, RE::Clouds::kTotalLayers> authored{};
		std::size_t layerCount{};
		bool valid{};
		bool publishedLinear{};
	};

	void PublishSkyColors(bool a_linear);
	void PublishCloudColors(bool a_linear);
	void SynchronizeTransientColorDomains(bool a_linear);
	LinearLightingColors::SemanticColorCache semanticColorCache;
	SkyColorState skyColorState;
	CloudColorState cloudColorState;

};
