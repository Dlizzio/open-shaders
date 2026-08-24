#define CSHADER

#include "Common/Color.hlsli"

Texture2D<float4> SourceTexture : register(t0);
RWTexture2D<float4> DestinationTexture : register(u0);

cbuffer EffectCompositeData : register(b0)
{
	uint Mode;
	uint EnableACEScg;
	uint2 TextureSize;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (any(dispatchThreadId.xy >= TextureSize))
		return;

	float4 color = SourceTexture.Load(int3(dispatchThreadId.xy, 0));
	if (Mode == 0) {
		float3 linearSrgb = EnableACEScg ? AP1TosRGB(color.rgb) : color.rgb;
		color.rgb = Color::LinearToEffectGamma(linearSrgb);
	} else {
		float3 linearSrgb = Color::EffectGammaToLinear(color.rgb);
		color.rgb = EnableACEScg ? sRGBToAP1(linearSrgb) : linearSrgb;
	}
	DestinationTexture[dispatchThreadId.xy] = color;
}
