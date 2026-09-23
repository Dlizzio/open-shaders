#pragma once

#include <d3d11.h>

#include <array>
#include <initializer_list>

struct ID3D11DeviceContext;
struct ID3D11VertexShader;
struct ID3D11PixelShader;

namespace PostProcessingRaster
{
	/** Restores the D3D11 state changed by fullscreen post-processing raster passes. */
	struct RasterPass
	{
		/** Saves the touched state and binds fullscreen-triangle state with shared pixel constants. */
		explicit RasterPass(ID3D11DeviceContext* a_context);

		/** Restores saved pipeline state and releases its references. */
		~RasterPass();

		RasterPass(const RasterPass&) = delete;
		RasterPass& operator=(const RasterPass&) = delete;

		/** Binds render targets and a viewport matching the pass resolution. */
		void SetTargets(std::initializer_list<ID3D11RenderTargetView*> a_rtvs, float a_width, float a_height);

		/** Binds the fullscreen vertex shader and the pass pixel shader. */
		void SetShaders(ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps);

		/** Selects blending for subsequent draws. */
		void SetBlendState(ID3D11BlendState* a_blend, const float (&a_blendFactor)[4], UINT a_sampleMask = 0xFFFFFFFFu);

		/** Draws one fullscreen triangle without vertex buffers. */
		void Draw();

	private:
		static constexpr UINT kPSSRVCount = 6;
		static constexpr UINT kPSCBCount = 7;
		static constexpr UINT kPSSamplerCount = 1;
		static constexpr UINT kSharedCBStart = 5;

		ID3D11DeviceContext* context;

		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		ID3D11BlendState* savedBlendState = nullptr;
		FLOAT savedBlendFactor[4] = {};
		UINT savedSampleMask = 0;
		ID3D11DepthStencilState* savedDepthStencilState = nullptr;
		UINT savedStencilRef = 0;
		ID3D11RasterizerState* savedRasterizerState = nullptr;
		UINT savedViewportCount = 0;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> savedViewports = {};
		D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11InputLayout* savedInputLayout = nullptr;
		ID3D11VertexShader* savedVS = nullptr;
		ID3D11HullShader* savedHS = nullptr;
		ID3D11DomainShader* savedDS = nullptr;
		ID3D11GeometryShader* savedGS = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		std::array<ID3D11ShaderResourceView*, kPSSRVCount> savedPSSRVs = {};
		std::array<ID3D11Buffer*, kPSCBCount> savedPSCBs = {};
		std::array<ID3D11SamplerState*, kPSSamplerCount> savedPSSamplers = {};
	};
}
