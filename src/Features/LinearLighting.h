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
			{ T("feature.linear_lighting.key_feature_1", "Customizable gamma correction"),
				T("feature.linear_lighting.key_feature_2", "Corrects lighting calculations"),
				T("feature.linear_lighting.key_feature_3", "Makes PBR really work") } };
	};

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	struct Settings
	{
		uint enableLinearLighting = false;
		uint enableACEScg = false;
		float lightGamma = 1.8f;
		float colorGamma = 1.8f;
		float emitColorGamma = 1.8f;
		float glowmapGamma = 1.8f;
		float ambientGamma = 1.8f;
		float fogGamma = 1.97f;
		float fogAlphaGamma = 1.8f;
		float skyGamma = 1.8f;
		float waterGamma = 1.8f;
		float vlGamma = 1.8f;

		// Lighting multipliers
		float ambientMult = 1.0f;
		float vanillaDiffuseColorMult = 1.0f;
		float emitColorMult = 1.0f;
		float glowmapMult = 0.66f;
	} settings;

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableACEScg;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float ambientMult;
		float vanillaDiffuseColorMult;
		float emitColorMult;
		float glowmapMult;
		float pad0[2];
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);

	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		float pad0[3];
	};

	struct alignas(16) EffectCompositeData
	{
		uint mode;
		uint enableACEScg;
		uint width;
		uint height;
	};
	STATIC_ASSERT_ALIGNAS_16(EffectCompositeData);

	enum class EffectCompatibilityScope
	{
		kBlendedDecals,
		kEffects
	};

	ConstantBuffer* PerGeometryCB = nullptr;
	std::unique_ptr<ConstantBuffer> effectCompositeCB;
	std::unique_ptr<Texture2D> effectCompatibilityTarget;
	winrt::com_ptr<ID3D11ComputeShader> effectCompositeCS;
	RE::BSGraphics::RenderTargetData savedMainTarget{};
	bool effectCompatibilityActive = false;
	EffectCompatibilityScope effectCompatibilityScope = EffectCompatibilityScope::kEffects;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;

	/** @brief Draws the ImGui settings UI for gamma correction and lighting multiplier configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Reads the directional light multiplier from ImageSpaceManager during the prepass. */
	virtual void Prepass() override;
	/** @brief Installs the BSLightingShader geometry setup hook. */
	virtual void PostPostLoad() override;

	/** @brief Creates the per-geometry constant buffer for emissive multiplier data. */
	virtual void SetupResources() override;
	/** @brief Encodes kMAIN and redirects compatible alpha-blended passes to the gamma-domain target. */
	void BeginEffectCompatibility(EffectCompatibilityScope a_scope);
	/** @brief Decodes the gamma-domain compatibility result back into linear kMAIN. */
	void EndEffectCompatibility();

	/** @brief Populates and returns the per-frame constant buffer data with gamma and multiplier settings. */
	PerFrameData GetCommonBufferData();

	/** @brief Converts the interpolated weather effect-lighting color before the engine applies light intensity. */
	void ConvertWeatherEffectLighting(RE::Sky* a_sky);

	/**
	 * @brief Converts an NiColor from gamma space to linear space using the specified gamma value.
	 * @param inColor The input color in gamma space.
	 * @param gamma The gamma exponent to apply.
	 * @return The color converted to linear space.
	 */
	RE::NiColor ColorToLinear(RE::NiColor inColor, float gamma);

	/**
	 * @brief Uploads emissive multiplier data to the per-geometry constant buffer during shader setup.
	 * @param a_pass The render pass whose lighting properties to read.
	 */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	/** @brief Returns whether the gamma-domain effect compatibility pass is available. */
	bool IsEffectCompatibilityReady() const;

	/** @brief Contains the BSLightingShader geometry setup hook implementation. */
	struct Hooks;

private:
	/** @brief Converts between the linear scene target and gamma-domain effect target. */
	void DispatchEffectComposite(uint a_mode, ID3D11ShaderResourceView* a_source, ID3D11UnorderedAccessView* a_destination);
};
