#pragma once

namespace LinearLightingColors
{
	enum class Semantic : std::uint32_t
	{
		LightingSpecular,
		HairTint,
		LightingEmission,
		ProjectedMaterial,
		EffectBase,
		EffectRim,
		EffectFill,
		EffectEmittance,
		WaterShallow,
		WaterDeep,
		WaterReflection,
		PointLight,
		ProcessedPointLight,
		WaterForm,
		ExponentialFogInscattering,
		ExponentialFogTint,
		VolumetricFogEmissive,
		GeneratedParticleLight,
		DirectionalLight,
		EffectLighting,
		SkyStatics,
		EffectLightingReference,
		SkyStaticsReference,
		DirectionalLightingReference,
		SunColor,
		MasserColor,
		SecundaColor,
		TruePBRCoat,
		TruePBRSubsurface,
		TruePBRFuzz,
		SubsurfaceMeanFreePath,
		VolumetricFogAlbedo,
		SkyColorBase = 0x100,
		CloudColorBase = 0x200,
		DirectionalAmbientBase = 0x300,
	};

	/** @brief Caches exact semantic RGB conversions by owner, source encoding, and source bits. */
	class SemanticColorCache
	{
	public:
		/** @brief Converts an authored RGB value to the active linear working gamut. */
		RE::NiColor ConvertAuthored(
			const void* a_owner,
			Semantic a_semantic,
			const RE::NiColor& a_authored,
			bool a_acescg);
		/** @brief Converts and scales an authored RGB value in the active linear working gamut. */
		RE::NiColor ConvertAuthoredScaled(
			const void* a_owner,
			Semantic a_semantic,
			const RE::NiColor& a_authored,
			float a_scale,
			bool a_acescg);
		/** @brief Converts an already-linear sRGB value to the active linear working gamut. */
		RE::NiColor ConvertLinear(
			const void* a_owner,
			Semantic a_semantic,
			const RE::NiColor& a_linear,
			bool a_acescg);
		/** @brief Encodes a linear-sRGB value into the engine's authored transfer curve. */
		RE::NiColor EncodeLinear(
			const void* a_owner,
			Semantic a_semantic,
			const RE::NiColor& a_linear);
		/** @brief Drops transient owner entries during scene teardown. */
		void Clear();

	private:
		static constexpr std::size_t kMaximumEntries = 32768;

		enum class SourceEncoding : std::uint8_t
		{
			Authored,
			Linear,
		};

		struct Key
		{
			const void* owner{};
			Semantic semantic{};
			SourceEncoding encoding{};

			bool operator==(const Key&) const = default;
		};

		struct KeyHash
		{
			using is_avalanching = void;

			std::uint64_t operator()(const Key& a_key) const noexcept;
		};

		struct RawRGB
		{
			std::uint32_t red{};
			std::uint32_t green{};
			std::uint32_t blue{};

			bool operator==(const RawRGB&) const = default;
		};

		struct CachedColor
		{
			bool valid{};
			bool acescgValid{};
			RawRGB source{};
			RE::NiColor linearSRGB{};
			RE::NiColor acescg{};
		};

		struct CachedScaledColor
		{
			CachedColor color{};
			bool scaleValid{};
			bool scaledACEScgValid{};
			std::uint32_t scale{};
			RE::NiColor scaledLinearSRGB{};
			RE::NiColor scaledACEScg{};
		};

		struct CachedEncodedColor
		{
			bool valid{};
			RawRGB source{};
			RE::NiColor authored{};
		};

		static RawRGB Capture(const RE::NiColor& a_color);
		RE::NiColor Convert(
			const void* a_owner,
			Semantic a_semantic,
			SourceEncoding a_encoding,
			const RE::NiColor& a_source,
			bool a_acescg);
		RE::NiColor ConvertScaled(
			const void* a_owner,
			Semantic a_semantic,
			SourceEncoding a_encoding,
			const RE::NiColor& a_source,
			float a_scale,
			bool a_acescg);
		std::size_t Size() const;

		ankerl::unordered_dense::map<Key, CachedColor, KeyHash> colors;
		ankerl::unordered_dense::map<Key, CachedScaledColor, KeyHash> scaledColors;
		ankerl::unordered_dense::map<Key, CachedEncodedColor, KeyHash> encodedColors;
	};
}
