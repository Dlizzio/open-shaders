#pragma once

namespace Util::Color
{
	inline constexpr float AuthoredGamma = 1.8f;

	/** @brief Decodes an authored RGB color into linear sRGB. */
	RE::NiColor DecodeAuthored(RE::NiColor a_color);
	/** @brief Decodes an authored RGB vector into linear sRGB. */
	float3 DecodeAuthored(float3 a_color);
	/** @brief Encodes linear sRGB into the engine's authored RGB transfer curve. */
	float3 EncodeAuthored(float3 a_color);
	/** @brief Decodes an sRGB-authored RGB vector into linear sRGB. */
	float3 DecodeSRGB(float3 a_color);
	/** @brief Converts linear sRGB to ACEScg/AP1. */
	float3 LinearSRGBToAP1(float3 a_color);
}
