#ifndef __GRASS_ALPHA_COVERAGE_HLSLI__
#define __GRASS_ALPHA_COVERAGE_HLSLI__

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

namespace GrassAlphaCoverage
{

	static const float MinimumDerivative = 1e-6;
	static const float MinimumThreshold = 1e-6;
	static const float HashRange = 16777216.0;
	static const float LogNoiseScale = -0.5;
	static const float FullCoverageMip = 6.0;

	float HashCell(float3 cell)
	{
		return (float(Random::pcg3d(asuint(cell)).x >> 8) + 0.5) / HashRange;
	}

	/** Applies surface-anchored grass coverage using the raster mip level without sharpening bias. */
	void ApplyAlphaTest(float3 position, float alpha, float alphaTestRef, float mipLevel)
	{
		float3 derivatives = max(abs(ddx(position)), abs(ddy(position)));
		float coverageWeight = saturate(mipLevel / FullCoverageMip);
		coverageWeight *= coverageWeight;
		float minimumThreshold = alphaTestRef <= 0 || alphaTestRef >= 1 ? alphaTestRef :
		                                                                  max(lerp(alphaTestRef, 0.0, coverageWeight), MinimumThreshold);
		if (alpha < minimumThreshold)
			discard;

		float3 logScale = LogNoiseScale - log2(max(derivatives, MinimumDerivative));
		float3 scale = exp2(floor(logScale));
		float3 fraction = frac(logScale);
		float weight = length(fraction) / (length(fraction) + length(1 - fraction));
		float sampleValue = lerp(HashCell(floor(position * scale)), HashCell(floor(position * scale * 2)), weight);
		float shoulder = min(weight, 1 - weight);
		float centeredSample = sampleValue - 0.5;
		float tail = 0.5 - abs(centeredSample);
		float denominator = max(shoulder * (1 - shoulder), 0.5 * EPSILON_DIVISION);
		float thresholdOffset = tail < shoulder ? 1 - tail * tail / denominator : 2 * abs(centeredSample) / (1 - shoulder);
		float threshold = alphaTestRef + thresholdOffset * (centeredSample < 0 ? -alphaTestRef : 1 - alphaTestRef);
		threshold = lerp(alphaTestRef, threshold, coverageWeight);
		threshold = alphaTestRef <= 0 || alphaTestRef >= 1 ? alphaTestRef : clamp(threshold, MinimumThreshold, 1.0);
		if (alpha < threshold)
			discard;
	}

}

#endif
