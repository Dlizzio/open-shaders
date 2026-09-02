#include "Features/PostProcessing/ColorSpace.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

namespace
{
	DirectX::XMFLOAT3 Transform(const DirectX::SimpleMath::Matrix& matrix, const DirectX::XMFLOAT3& value)
	{
		return {
			matrix(0, 0) * value.x + matrix(0, 1) * value.y + matrix(0, 2) * value.z,
			matrix(1, 0) * value.x + matrix(1, 1) * value.y + matrix(1, 2) * value.z,
			matrix(2, 0) * value.x + matrix(2, 1) * value.y + matrix(2, 2) * value.z
		};
	}

	void RequireClose(const DirectX::XMFLOAT3& actual, const DirectX::XMFLOAT3& expected, float margin = 0.0001f)
	{
		REQUIRE(actual.x == Approx(expected.x).margin(margin));
		REQUIRE(actual.y == Approx(expected.y).margin(margin));
		REQUIRE(actual.z == Approx(expected.z).margin(margin));
	}
}

TEST_CASE("ACEScg conversion preserves neutral SDR and HDR values", "[colorspace]")
{
	const auto toACEScg = getRGBMatrix("sRGB", "ACEScg");
	const auto toSRGB = getRGBMatrix("ACEScg", "sRGB");

	for (const float value : { 0.18f, 1.0f, 4.0f, 10.0f }) {
		const DirectX::XMFLOAT3 neutral{ value, value, value };
		RequireClose(Transform(toACEScg, neutral), neutral);
		RequireClose(Transform(toSRGB, neutral), neutral);
	}
}

TEST_CASE("ACEScg conversion roundtrips linear RGB values", "[colorspace]")
{
	const auto toACEScg = getRGBMatrix("sRGB", "ACEScg");
	const auto toSRGB = getRGBMatrix("ACEScg", "sRGB");
	const std::array colors{
		DirectX::XMFLOAT3{ 0.2f, 0.7f, 0.3f },
		DirectX::XMFLOAT3{ 4.0f, 1.5f, 0.25f },
		DirectX::XMFLOAT3{ -0.1f, 0.4f, 1.2f }
	};

	for (const auto& color : colors)
		RequireClose(Transform(toSRGB, Transform(toACEScg, color)), color);
}

TEST_CASE("XYZ boundary matrices remain native", "[colorspace]")
{
	const auto toXYZ = getRGBMatrix("ACEScg", "XYZ");
	const auto fromXYZ = getRGBMatrix("XYZ", "ACEScg");

	REQUIRE(toXYZ(0, 0) == Approx(0.66245418f));
	REQUIRE(toXYZ(1, 1) == Approx(0.67408177f));
	REQUIRE(toXYZ(2, 2) == Approx(1.0103391f));
	REQUIRE(fromXYZ(0, 0) == Approx(1.64102338f));
	REQUIRE(fromXYZ(1, 1) == Approx(1.61533159f));
	REQUIRE(fromXYZ(2, 2) == Approx(0.98839486f));
}
