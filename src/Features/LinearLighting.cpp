#include "LinearLighting.h"

#include "../I18n/I18n.h"
#include "Features/PostProcessing.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#	include "Effects11/SettingManager.h"
#endif
#include "Globals.h"
#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	enableACEScg,
	lightGamma,
	colorGamma,
	emitColorGamma,
	glowmapGamma,
	ambientGamma,
	fogGamma,
	fogAlphaGamma,
	skyGamma,
	waterGamma,
	vlGamma,
	ambientMult,
	vanillaDiffuseColorMult,
	emitColorMult,
	glowmapMult)

namespace
{
	constexpr float kGammaMin = 0.1f;
	constexpr float kGammaMax = 3.0f;
	constexpr float kEffectGamma = 1.8f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;
	constexpr float kAmbientMultiplierMax = 5.0f;
	constexpr uint kEncodeEffectTarget = 0;
	constexpr uint kDecodeEffectTarget = 1;
}

void LinearLighting::DrawSettings()
{
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
			return;
		}
	}
#endif

	ImGui::Checkbox(T(TKEY("enable"), "Enable Linear Lighting"), (bool*)&settings.enableLinearLighting);
	ImGui::Checkbox(T(TKEY("enable_acescg"), "Enable ACEScg Wide Gamut"), (bool*)&settings.enableACEScg);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_acescg_tooltip"),
							  "Render in ACEScg color space for wider gamut and more accurate lighting.\n"
							  "Requires Linear Lighting and Post Processing enabled.\n"
							  "All sRGB-gamut textures and colors will be converted to ACEScg during shading."));

	if (ImGui::BeginTabBar("##LinearLightingTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_gamma"), "Gamma"))) {
			ImGui::SliderFloat(T(TKEY("ambient_gamma"), "Ambient Gamma"), &settings.ambientGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("color_gamma"), "Color Gamma"), &settings.colorGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_gamma"), "Emissive Color Gamma"), &settings.emitColorGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("fog_gamma"), "Fog Gamma"), &settings.fogGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("fog_transparency_gamma"), "Fog Transparency Gamma"), &settings.fogAlphaGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_gamma"), "Glowmap Gamma"), &settings.glowmapGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("light_gamma"), "Light Gamma"), &settings.lightGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("sky_gamma"), "Sky Gamma"), &settings.skyGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("vl_gamma"), "Volumetric Lighting Gamma"), &settings.vlGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("water_gamma"), "Water Gamma"), &settings.waterGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_multipliers"), "Multipliers"))) {
			ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, kMultiplierMin, kAmbientMultiplierMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("vanilla_diffuse_color_multiplier"), "Vanilla Diffuse Color Multiplier"), &settings.vanillaDiffuseColorMult, kMultiplierMin, kMultiplierMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_multiplier"), "Emissive Color Multiplier"), &settings.emitColorMult, kMultiplierMin, kMultiplierMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_multiplier"), "Glowmap Multiplier"), &settings.glowmapMult, kMultiplierMin, kMultiplierMax, "%.2f");

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LinearLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>(), "LinearLighting::PerGeometryCB");
	effectCompatibilityActive = false;
	effectCompatibilityTarget.reset();
	effectCompositeCB.reset();
	effectCompositeCS = nullptr;

	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !main.SRV || !main.RTV || !main.UAV) {
		logger::error("[LinearLighting] Effect compatibility requires kMAIN SRV, RTV, and UAV support");
		return;
	}

	D3D11_TEXTURE2D_DESC textureDesc{};
	main.texture->GetDesc(&textureDesc);
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	textureDesc.CPUAccessFlags = 0;
	textureDesc.MiscFlags = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	main.SRV->GetDesc(&srvDesc);
	main.RTV->GetDesc(&rtvDesc);
	main.UAV->GetDesc(&uavDesc);

	effectCompatibilityTarget = std::make_unique<Texture2D>(textureDesc, "LinearLighting::EffectCompatibility");
	effectCompatibilityTarget->CreateSRV(srvDesc);
	effectCompatibilityTarget->CreateRTV(rtvDesc);
	effectCompatibilityTarget->CreateUAV(uavDesc);
	effectCompositeCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<EffectCompositeData>(), "LinearLighting::EffectCompositeCB");

	effectCompositeCS.attach(static_cast<ID3D11ComputeShader*>(
		Util::CompileShader(L"Data\\Shaders\\LinearLighting\\EffectCompositeCS.hlsl", {}, "cs_5_0")));
	if (!effectCompositeCS) {
		logger::error("[LinearLighting] Failed to compile the effect compatibility shader");
		effectCompatibilityTarget.reset();
		effectCompositeCB.reset();
		return;
	}
	Util::SetResourceName(effectCompositeCS.get(), "LinearLighting::EffectCompositeCS");
}

bool LinearLighting::IsEffectCompatibilityReady() const
{
	return effectCompatibilityTarget && effectCompositeCB && effectCompositeCS;
}

void LinearLighting::DispatchEffectComposite(
	uint a_mode, ID3D11ShaderResourceView* a_source, ID3D11UnorderedAccessView* a_destination)
{
	EffectCompositeData data{};
	data.mode = a_mode;
	data.enableACEScg = GetCommonBufferData().enableACEScg;
	data.width = effectCompatibilityTarget->desc.Width;
	data.height = effectCompatibilityTarget->desc.Height;
	effectCompositeCB->Update(data);

	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CSSetShader(effectCompositeCS.get(), nullptr, 0);
	ID3D11Buffer* constantBuffer = effectCompositeCB->CB();
	context->CSSetConstantBuffers(0, 1, &constantBuffer);
	context->CSSetShaderResources(0, 1, &a_source);
	context->CSSetUnorderedAccessViews(0, 1, &a_destination, nullptr);
	context->Dispatch((data.width + 7) / 8, (data.height + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);
}

void LinearLighting::BeginEffectCompatibility(EffectCompatibilityScope a_scope)
{
	const auto data = GetCommonBufferData();
	if (effectCompatibilityActive || !globals::state->inWorld || !data.enableLinearLighting || !IsEffectCompatibilityReady())
		return;

	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	savedMainTarget = main;
	if (a_scope == EffectCompatibilityScope::kBlendedDecals) {
		CS_GPU_PASS("LinearLighting::EncodeBlendedDecals");
		DispatchEffectComposite(kEncodeEffectTarget, savedMainTarget.SRV, effectCompatibilityTarget->uav.get());
	} else {
		CS_GPU_PASS("LinearLighting::EncodeEffects");
		DispatchEffectComposite(kEncodeEffectTarget, savedMainTarget.SRV, effectCompatibilityTarget->uav.get());
	}

	main.texture = effectCompatibilityTarget->resource.get();
	main.RTV = effectCompatibilityTarget->rtv.get();
	main.SRV = effectCompatibilityTarget->srv.get();
	main.UAV = effectCompatibilityTarget->uav.get();
	effectCompatibilityActive = true;
	effectCompatibilityScope = a_scope;
	globals::state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void LinearLighting::EndEffectCompatibility()
{
	if (!effectCompatibilityActive)
		return;

	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	main = savedMainTarget;
	effectCompatibilityActive = false;
	globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	if (effectCompatibilityScope == EffectCompatibilityScope::kBlendedDecals) {
		CS_GPU_PASS("LinearLighting::DecodeBlendedDecals");
		DispatchEffectComposite(kDecodeEffectTarget, effectCompatibilityTarget->srv.get(), main.UAV);
	} else {
		CS_GPU_PASS("LinearLighting::DecodeEffects");
		DispatchEffectComposite(kDecodeEffectTarget, effectCompatibilityTarget->srv.get(), main.UAV);
	}
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void LinearLighting::Prepass()
{
	bool isMainLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();
	dirLightMult = 1.0f;
	if (!settings.enableLinearLighting || isMainLoadingMenu)
		return;

	auto imageSpaceManager = globals::game::imageSpaceManager;
	if (!imageSpaceManager)
		return;

	dirLightMult = imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed hooks - BSLightingShader_SetupGeometry");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
	bool isMainLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();
	auto data = PerFrameData{};
	data.enableLinearLighting = settings.enableLinearLighting && IsEffectCompatibilityReady() && !isMainLoadingMenu;
	data.enableACEScg = settings.enableACEScg && data.enableLinearLighting && globals::features::postProcessing.loaded;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.lightGamma = settings.lightGamma;
	data.colorGamma = settings.colorGamma;
	data.emitColorGamma = settings.emitColorGamma;
	data.glowmapGamma = settings.glowmapGamma;
	data.ambientGamma = settings.ambientGamma;
	data.fogGamma = settings.fogGamma;
	data.fogAlphaGamma = settings.fogAlphaGamma;
	data.skyGamma = settings.skyGamma;
	data.waterGamma = settings.waterGamma;
	data.vlGamma = settings.vlGamma;

#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.enableLinearLighting = false;
			data.lightGamma = 1.0f;
			data.colorGamma = 1.0f;
			data.emitColorGamma = 1.0f;
			data.glowmapGamma = 1.0f;
			data.ambientGamma = 1.0f;
			data.fogGamma = 1.0f;
			data.fogAlphaGamma = 1.0f;
			data.skyGamma = 1.0f;
			data.waterGamma = 1.0f;
			data.vlGamma = 1.0f;
		}
	}
#endif

	data.ambientMult = settings.ambientMult;
	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.emitColorMult = settings.emitColorMult;
	data.glowmapMult = settings.glowmapMult;

	// Override multipliers to neutral values when ENB PP is active
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.vanillaDiffuseColorMult = 1.0f;
			data.dirLightMult = 1.0f;
			data.ambientMult = 1.0f;
			data.emitColorMult = 1.0f;
			data.glowmapMult = 1.0f;
		}
	}
#endif
	return data;
}

void LinearLighting::ConvertWeatherEffectLighting(RE::Sky* a_sky)
{
	if (!a_sky || !GetCommonBufferData().enableLinearLighting)
		return;

	auto& effectLighting = a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kEffectLighting)];
	effectLighting = ColorToLinear(effectLighting, kEffectGamma);
}

RE::NiColor LinearLighting::ColorToLinear(RE::NiColor inColor, float gamma)
{
	RE::NiColor outColor;
	outColor.red = std::pow(inColor.red, gamma);
	outColor.green = std::pow(inColor.green, gamma);
	outColor.blue = std::pow(inColor.blue, gamma);
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;

	if (lightProperty != nullptr) {
		float emissiveMult = 1.0f;
		if (settings.enableLinearLighting) {
			emissiveMult = lightProperty->emissiveMult;
			PerGeometryData perGeometryData{};
			perGeometryData.emissiveMult = emissiveMult;
			PerGeometryCB->Update(perGeometryData);

			ID3D11Buffer* buffer = { PerGeometryCB->CB() };
			auto context = globals::d3d::context;
			context->PSSetConstantBuffers(8, 1, &buffer);
		}
	}
}

#undef I18N_KEY_PREFIX
