#pragma once

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

		// Lighting multipliers
		float ambientMult = 0.32f;
		float vanillaDiffuseColorMult = 1.5f;
	} settings;

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableACEScg;
		uint isDirLightLinear;
		float dirLightMult;
		float authoredColorGamma;
		float vanillaDiffuseColorMult;
		float pad0[2];
		RE::NiColor effectLightingColor;
		float ambientMult;
		RE::NiColor skyStaticsColor;
		float pad1;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 0x40);

	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		float pad0[3];
	};

	ConstantBuffer* PerGeometryCB = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> sceneGammaDecodeCS;
	bool sceneGammaActive = false;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;
	RE::NiColor effectLightingColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor skyStaticsColor{ 1.0f, 1.0f, 1.0f };
	RE::NiColor weatherEffectLightingSource{};
	RE::NiColor weatherSkyStaticsSource{};
	bool weatherLightingColorsInitialized = false;

	/** @brief Draws the Linear Lighting controls and lighting multipliers. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Reads the directional light multiplier from ImageSpaceManager during the prepass. */
	virtual void Prepass() override;
	/** @brief Installs the lighting geometry hook. */
	virtual void PostPostLoad() override;

	/** @brief Creates the emissive data buffer and compiles the scene gamma decode shader. */
	virtual void SetupResources() override;
	/** @brief Recompiles the scene gamma decode shader after a shader-cache clear. */
	virtual void ClearShaderCache() override;
	/** @brief Marks kMAIN as gamma-domain storage for the main world-rendering interval. */
	void BeginSceneGamma();
	/** @brief Decodes the completed gamma-domain world scene in place. */
	void EndSceneGamma(RE::RENDER_TARGET a_renderTarget);

	/** @brief Populates and returns the per-frame constant buffer data with gamma and multiplier settings. */
	PerFrameData GetCommonBufferData();
	/** @brief Returns whether the engine and shaders should currently use linear lighting data. */
	bool IsLinearLightingActive() const;
	/** @brief Compiles the scene gamma decode shader when the target format supports typed UAV loads. */
	void CompileSceneGammaDecodeShader();

	/** @brief Caches linear copies of the interpolated weather colors used by effect meshes. */
	void UpdateWeatherLightingColors(RE::Sky* a_sky);

	/**
	 * @brief Decodes an authored Skyrim color into linear sRGB.
	 * @param inColor The input color in gamma space.
	 * @return The color converted to linear space.
	 */
	static RE::NiColor DecodeAuthoredColor(RE::NiColor inColor);

	/**
	 * @brief Uploads emissive multiplier data to the per-geometry constant buffer during shader setup.
	 * @param a_pass The render pass whose lighting properties to read.
	 */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	/** @brief Contains the lighting shader hook implementation. */
	struct Hooks;
};
