#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#define LinearSampler defaultSampler
SamplerState defaultSampler : register(s0);

#include "Common/ShadowSampling.hlsli"

#include "VolumetricRaysCommon.hlsli"

struct VS_OUTPUT_POST
{
	float4 pos: SV_POSITION;
	float2 txcoord0: TEXCOORD0;
};

struct PS_OUTPUT
{
	float Scattering: SV_Target0;
	// Linear depth this texel raymarched with; the bilateral blur + upsample weight against it.
	float Depth: SV_Target1;
};

static const uint SampleCount = 16;
static const float RcpSampleCount = 1.0 / float(SampleCount);
static const float MaxRayLength = 154117.64;
static const float RcpShadowCoverageRadiusSq = 1.0 / (262000.0 * 262000.0);

float GetVolumetricRaysScattering(float3 positionMS, float noise, float3 cameraOffset)
{
	float pixelDistance = length(positionMS);
	float3 rayDirection = positionMS / max(pixelDistance, VolumetricRays::DepthEpsilon);
	float rayLength = min(pixelDistance, MaxRayLength);
	float3 sunDirection = SharedData::SunDirection.xyz;


	float visibility = 0.0;

	[unroll] for (uint i = 0; i < SampleCount; i++)
	{
		float3 samplePos = rayDirection * ((float(i) + noise) * RcpSampleCount * rayLength);

		float shadow = 1.0;

#if defined(TERRAIN_SHADOWS)
		shadow = TerrainShadows::GetTerrainShadow(samplePos + cameraOffset, LinearSampler);
#endif

#if defined(CLOUD_SHADOWS)
		shadow *= CloudShadows::GetCloudShadowMult(samplePos, LinearSampler);
#endif

		float alongSun = dot(samplePos, sunDirection);
		float offsetSq = max(dot(samplePos, samplePos) - alongSun * alongSun, 0.0);
		visibility += shadow * saturate(1.0 - offsetSq * RcpShadowCoverageRadiusSq);
	}

	return saturate(visibility * RcpSampleCount * rayLength / MaxRayLength);
}

PS_OUTPUT main(VS_OUTPUT_POST input)
{
	Stereo::EyeUV eye = Stereo::UnpackEyeUV(input.txcoord0);
	float2 uv = eye.uv;

	float depth = SharedData::GetDepth(uv, eye.index);
	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[eye.index], positionCS);
	positionMS.xyz /= positionMS.w;

	float noise = Random::InterleavedGradientNoise(Stereo::EyeStableNoiseCoord(input.pos.xy, ScreenSize), SharedData::FrameCount);
	float3 cameraOffset = FrameBuffer::CameraPosAdjust[eye.index].xyz;

	PS_OUTPUT output;
	output.Scattering = GetVolumetricRaysScattering(positionMS.xyz, noise, cameraOffset);
	output.Depth = SharedData::GetScreenDepth(depth);
	return output;
}
