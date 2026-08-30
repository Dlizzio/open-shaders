#define COMPUTESHADER

#include "Common/Color.hlsli"

RWTexture2D<float4> SceneColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
	const uint2 dimensions = uint2(SharedData::BufferDim.xy);
	if (any(pixel >= dimensions))
		return;

	float4 color = SceneColor[pixel];
	color.rgb = Color::AuthoredGammaToWorkingLinear(color.rgb);
	SceneColor[pixel] = color;
}
