#include "Color.h"

namespace Util::Color
{
	RE::NiColor DecodeAuthored(RE::NiColor a_color)
	{
		return {
			std::pow(std::abs(a_color.red), AuthoredGamma),
			std::pow(std::abs(a_color.green), AuthoredGamma),
			std::pow(std::abs(a_color.blue), AuthoredGamma)
		};
	}

	float3 DecodeAuthored(float3 a_color)
	{
		return {
			std::pow(std::abs(a_color.x), AuthoredGamma),
			std::pow(std::abs(a_color.y), AuthoredGamma),
			std::pow(std::abs(a_color.z), AuthoredGamma)
		};
	}

	float3 EncodeAuthored(float3 a_color)
	{
		constexpr float inverseAuthoredGamma = 1.0f / AuthoredGamma;
		return {
			std::pow(std::abs(a_color.x), inverseAuthoredGamma),
			std::pow(std::abs(a_color.y), inverseAuthoredGamma),
			std::pow(std::abs(a_color.z), inverseAuthoredGamma)
		};
	}

	float3 DecodeSRGB(float3 a_color)
	{
		return {
			std::pow(std::abs(a_color.x), 2.2f),
			std::pow(std::abs(a_color.y), 2.2f),
			std::pow(std::abs(a_color.z), 2.2f)
		};
	}

	float3 LinearSRGBToAP1(float3 a_color)
	{
		using Matrix3 = std::array<std::array<float, 3>, 3>;
		constexpr Matrix3 kLinearSRGBToXYZ{ {
			{ 0.4123907993f, 0.3575843394f, 0.1804807884f },
			{ 0.2126390059f, 0.7151686788f, 0.0721923154f },
			{ 0.0193308187f, 0.1191947798f, 0.9505321522f },
		} };
		constexpr Matrix3 kXYZToAP1{ {
			{ 1.6410233797f, -0.3248032942f, -0.2364246952f },
			{ -0.6636628587f, 1.6153315917f, 0.0167563477f },
			{ 0.0117218943f, -0.0082844420f, 0.9883948585f },
		} };

		const auto transform = [](const Matrix3& a_matrix, const float3& a_value) {
			return float3{
				a_matrix[0][0] * a_value.x + a_matrix[0][1] * a_value.y + a_matrix[0][2] * a_value.z,
				a_matrix[1][0] * a_value.x + a_matrix[1][1] * a_value.y + a_matrix[1][2] * a_value.z,
				a_matrix[2][0] * a_value.x + a_matrix[2][1] * a_value.y + a_matrix[2][2] * a_value.z
			};
		};

		return transform(kXYZToAP1, transform(kLinearSRGBToXYZ, a_color));
	}

}
