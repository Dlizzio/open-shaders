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
	waterGamma,
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
			ImGui::SliderFloat(T(TKEY("glowmap_gamma"), "Glowmap Gamma"), &settings.glowmapGamma, kGammaMin, kGammaMax, "%.2f");
			ImGui::SliderFloat(T(TKEY("light_gamma"), "Light Gamma"), &settings.lightGamma, kGammaMin, kGammaMax, "%.2f");
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
	sceneGammaDecodeCS.attach(static_cast<ID3D11ComputeShader*>(
		Util::CompileShader(L"Data\\Shaders\\LinearLighting\\SceneGammaDecodeCS.hlsl", {}, "cs_5_0")));
	sceneGammaActive = false;
	sceneGammaDecodedByRefraction = false;
}

void LinearLighting::BeginSceneGamma()
{
	sceneGammaActive = false;
	sceneGammaDecodedByRefraction = false;
	globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);

	const auto data = GetCommonBufferData();
	if (!globals::state->inWorld || !data.enableLinearLighting || !sceneGammaDecodeCS)
		return;

	sceneGammaActive = true;
	globals::state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
}

void LinearLighting::EndSceneGamma(RE::RENDER_TARGET a_renderTarget)
{
	if (!sceneGammaActive)
		return;

	sceneGammaActive = false;
	globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	if (sceneGammaDecodedByRefraction) {
		sceneGammaDecodedByRefraction = false;
		return;
	}

	const auto targetIndex = static_cast<size_t>(a_renderTarget);
	if (!sceneGammaDecodeCS || targetIndex >= Util::GetRenderTargetCount())
		return;

	auto& target = globals::game::renderer->GetRuntimeData().renderTargets[targetIndex];
	if (!target.texture || !target.UAV)
		return;

	CS_GPU_PASS("LinearLighting::DecodeScene");
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CSSetShader(sceneGammaDecodeCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &target.UAV, nullptr);

	const auto width = static_cast<uint32_t>(globals::state->screenSize.x);
	const auto height = static_cast<uint32_t>(globals::state->screenSize.y);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
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

	struct BSImagespaceShaderRefraction_Render
	{
		static void thunk(void* a_imageSpaceShader, RE::BSTriShape* a_shape, RE::ImageSpaceEffectParam* a_param)
		{
			auto& linearLighting = globals::features::linearLighting;
			if (linearLighting.sceneGammaActive) {
				CS_GPU_PASS("LinearLighting::DecodeScene");
				func(a_imageSpaceShader, a_shape, a_param);
				linearLighting.sceneGammaDecodedByRefraction = true;
				return;
			}

			func(a_imageSpaceShader, a_shape, a_param);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x1, BSImagespaceShaderRefraction_Render>(RE::VTABLE_BSImagespaceShaderRefraction[3]);
		logger::info("[LinearLighting] Installed shader hooks");
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
	data.enableLinearLighting = settings.enableLinearLighting && sceneGammaDecodeCS && !isMainLoadingMenu;
	data.enableACEScg = settings.enableACEScg && data.enableLinearLighting && globals::features::postProcessing.loaded;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.lightGamma = settings.lightGamma;
	data.colorGamma = settings.colorGamma;
	data.emitColorGamma = settings.emitColorGamma;
	data.glowmapGamma = settings.glowmapGamma;
	data.ambientGamma = settings.ambientGamma;
	data.waterGamma = settings.waterGamma;

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
			data.waterGamma = 1.0f;
		}
	}
#endif

	data.ambientMult = settings.ambientMult;
	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.emitColorMult = settings.emitColorMult;
	data.glowmapMult = settings.glowmapMult;
	if (!weatherLightingColorsInitialized)
		UpdateWeatherLightingColors(globals::game::sky);
	data.effectLightingColor = effectLightingColor;
	data.skyStaticsColor = skyStaticsColor;

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

void LinearLighting::UpdateWeatherLightingColors(RE::Sky* a_sky)
{
	if (!a_sky)
		return;

	effectLightingColor = ColorToLinear(
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kEffectLighting)], kEffectGamma);
	skyStaticsColor = ColorToLinear(
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSkyStatics)], kEffectGamma);
	weatherLightingColorsInitialized = true;
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
