#include "LinearLighting.h"

#include "../I18n/I18n.h"
#include "CSUtility.h"
#include "Features/PostProcessing.h"
#include "GpuPass.h"
#include "LightLimitFix.h"
#include "ShaderCache.h"
#include "SkySync.h"
#include "State.h"
#include "Util.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#endif
#include "Globals.h"
#include "Utils/PointLightFlags.h"

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	enableACEScg)

namespace
{
	constexpr float kMinimumColorScale = 1.0e-5f;
	constexpr std::uint32_t kFirstPointLightSceneIndex = 1;
	constexpr std::uint32_t kMaxLightingPointLights = 7;
	constexpr std::uint32_t kMaxEffectPointLights = 4;
	constexpr std::uint32_t kMaxWaterPointLights = 7;
	constexpr std::uint32_t kEffectPointLightFlagWidth = 3;
	constexpr std::uint32_t kEffectPointLightLinear = 1u << 0;
	constexpr std::uint32_t kEffectPointLightSpot = 1u << 1;
	constexpr std::uint32_t kEffectPointLightOmnidirectional = 1u << 2;

	std::uint32_t PackEffectPointLightFlags(std::uint32_t a_flags)
	{
		std::uint32_t packed = 0;
		packed |= (a_flags & PointLightFlags::ToMask(PointLightFlags::Flags::Linear)) != 0 ? kEffectPointLightLinear : 0;
		packed |= (a_flags & PointLightFlags::ToMask(PointLightFlags::Flags::Spot)) != 0 ? kEffectPointLightSpot : 0;
		packed |= (a_flags & PointLightFlags::ToMask(PointLightFlags::Flags::OmniDirectional)) != 0 ? kEffectPointLightOmnidirectional : 0;
		return packed;
	}

	enum class ColorProducer
	{
		None,
		LightingMaterial,
		LightingGeometry,
		EffectMaterial,
		EffectGeometry,
		WaterMaterial,
		WaterGeometry,
	};

	struct ColorProducerContext
	{
		struct ScaledColor
		{
			RE::NiColor source{};
			RE::NiColor working{};
			bool replace{};
		};

		struct PointLight : ScaledColor
		{
			std::uint32_t flags{};
		};

		ColorProducer type{ ColorProducer::None };
		std::optional<RE::NiColor> hairTint;
		std::optional<RE::NiColor> lightingSpecular;
		std::optional<RE::NiColor> lightingEmission;
		std::optional<RE::NiColor> projectedMaterial;
		std::optional<RE::NiColor> effectBase;
		std::optional<RE::NiColor> effectRim;
		std::optional<RE::NiColor> effectFill;
		std::optional<RE::NiColor> effectEmittance;
		std::optional<RE::NiColor> waterShallow;
		std::optional<RE::NiColor> waterDeep;
		std::optional<RE::NiColor> waterReflection;
		ScaledColor directionalLight{};
		std::array<PointLight, kMaxLightingPointLights> pointLights{};
		std::uint32_t pointLightCount{};
	};

	thread_local ColorProducerContext currentColorProducer;

	ColorProducerContext PrepareColorProducer(
		ColorProducer a_type,
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		const RE::BSShaderMaterial* a_material);

	class ScopedColorProducer
	{
	public:
		ScopedColorProducer(
			ColorProducer a_type,
			RE::BSShader* a_shader,
			RE::BSRenderPass* a_pass = nullptr,
			const RE::BSShaderMaterial* a_material = nullptr) :
			previous(currentColorProducer)
		{
			currentColorProducer = PrepareColorProducer(a_type, a_shader, a_pass, a_material);
		}

		~ScopedColorProducer()
		{
			currentColorProducer = previous;
		}

	private:
		ColorProducerContext previous;
	};
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

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupMaterial
	{
		static void thunk(RE::BSShader* This, const RE::BSShaderMaterial* Material)
		{
			ScopedColorProducer producer(ColorProducer::LightingMaterial, This, nullptr, Material);
			func(This, Material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			ScopedColorProducer producer(ColorProducer::LightingGeometry, This, Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSEffectShader_SetupMaterial
	{
		static void thunk(RE::BSShader* This, const RE::BSShaderMaterial* Material)
		{
			ScopedColorProducer producer(ColorProducer::EffectMaterial, This, nullptr, Material);
			func(This, Material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSEffectShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			ScopedColorProducer producer(ColorProducer::EffectGeometry, This, Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_SetupMaterial
	{
		static void thunk(RE::BSShader* This, const RE::BSShaderMaterial* Material)
		{
			ScopedColorProducer producer(ColorProducer::WaterMaterial, This, nullptr, Material);
			func(This, Material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			ScopedColorProducer producer(ColorProducer::WaterGeometry, This, Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Clouds_Update
	{
		static void thunk(RE::Clouds* a_clouds, RE::Sky* a_sky, float a_delta)
		{
			func(a_clouds, a_sky, a_delta);
			globals::features::linearLighting.UpdateCloudColors(a_clouds);
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
		stl::write_vfunc<0x3, Clouds_Update>(RE::VTABLE_Clouds[0]);
		stl::write_vfunc<0x4, BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x4, BSEffectShader_SetupMaterial>(RE::VTABLE_BSEffectShader[0]);
		stl::write_vfunc<0x6, BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
		stl::write_vfunc<0x4, BSWaterShader_SetupMaterial>(RE::VTABLE_BSWaterShader[0]);
		stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
		stl::write_vfunc<0x1, BSImagespaceShaderRefraction_Render>(RE::VTABLE_BSImagespaceShaderRefraction[3]);
		logger::info("[LinearLighting] Installed color pipeline hooks");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
}

void LinearLighting::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening) {
		semanticColorCache.Clear();
		skyColorState = {};
		cloudColorState = {};
	}
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
	auto data = PerFrameData{};
	data.enableLinearLighting = IsLinearLightingActive();
	data.enableACEScg = IsACEScgActive();
	SynchronizeTransientColorDomains(data.enableLinearLighting != 0);

	data.effectLightingColor = { 1.0f, 1.0f, 1.0f };
	data.skyStaticsColor = { 1.0f, 1.0f, 1.0f };
	data.effectLightingReference = { 1.0f, 1.0f, 1.0f };
	data.skyStaticsReference = { 1.0f, 1.0f, 1.0f };
	data.directionalLightingReference = {};
	if (const auto* sky = globals::game::sky) {
		data.effectLightingColor = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kEffectLighting)];
		data.skyStaticsColor = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSkyStatics)];
		if (data.enableLinearLighting) {
			if (globals::game::imageSpaceManager) {
				const auto sunlightScale = globals::game::imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
				data.effectLightingColor *= sunlightScale;
				data.skyStaticsColor *= sunlightScale;
			}
			data.effectLightingReference = semanticColorCache.EncodeLinear(
				sky,
				LinearLightingColors::Semantic::EffectLightingReference,
				data.effectLightingColor);
			data.skyStaticsReference = semanticColorCache.EncodeLinear(
				sky,
				LinearLightingColors::Semantic::SkyStaticsReference,
				data.skyStaticsColor);
			data.effectLightingColor = semanticColorCache.ConvertLinear(
				sky,
				LinearLightingColors::Semantic::EffectLighting,
				data.effectLightingColor,
				data.enableACEScg != 0);
			data.skyStaticsColor = semanticColorCache.ConvertLinear(
				sky,
				LinearLightingColors::Semantic::SkyStatics,
				data.skyStaticsColor,
				data.enableACEScg != 0);
		}
	}

	if (data.enableLinearLighting) {
		const auto* shaderManager = globals::game::smState;
		const auto* shadowSceneNode = shaderManager ? shaderManager->shadowSceneNode[0] : nullptr;
		const auto* directionalLight = shadowSceneNode ?
		                                   skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get()) :
		                                   nullptr;
		if (directionalLight) {
			const auto& runtimeData = directionalLight->GetLightRuntimeData();
			RE::NiColor linearColor = runtimeData.diffuse;
			linearColor *= runtimeData.fade;
			if (globals::game::imageSpaceManager)
				linearColor *= globals::game::imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
			linearColor *= globals::features::csUtility.settings.directionalLightMult;
			data.directionalLightingReference = semanticColorCache.EncodeLinear(
				directionalLight,
				LinearLightingColors::Semantic::DirectionalLightingReference,
				linearColor);
		}
	}
	return data;
}

bool LinearLighting::IsLinearLightingActive() const
{
	if (!loaded || !settings.enableLinearLighting || !sceneGammaDecodeCS || !globals::state || globals::state->IsMainOrLoadingMenuOpen())
		return false;

#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded && globals::features::effects11.enableEffect)
		return false;
#endif

	return true;
}

bool LinearLighting::IsACEScgActive() const
{
	return settings.enableACEScg && IsLinearLightingActive() && globals::features::postProcessing.loaded;
}

void LinearLighting::BeginSkyColorUpdate(RE::Sky* a_sky)
{
	if (skyColorState.valid && skyColorState.owner == a_sky)
		PublishSkyColors(false);
}

void LinearLighting::UpdateSkyColors(RE::Sky* a_sky)
{
	if (!a_sky)
		return;

	if (skyColorState.owner != a_sky) {
		skyColorState = {};
		skyColorState.owner = a_sky;
	}

	for (std::size_t i = 0; i < skyColorState.authored.size(); ++i) {
		const auto& source = a_sky->skyColor[i];
		if (!skyColorState.valid ||
			std::memcmp(std::addressof(skyColorState.authored[i]), std::addressof(source), sizeof(source)) != 0) {
			skyColorState.authored[i] = source;
		}
	}
	skyColorState.valid = true;
	PublishSkyColors(IsLinearLightingActive());
}

void LinearLighting::UpdateCloudColors(RE::Clouds* a_clouds)
{
	if (!a_clouds)
		return;

	if (cloudColorState.owner != a_clouds) {
		cloudColorState = {};
		cloudColorState.owner = a_clouds;
	}

	cloudColorState.layerCount = std::min<std::size_t>(a_clouds->numLayers, RE::Clouds::kTotalLayers);
	for (std::size_t i = 0; i < cloudColorState.layerCount; ++i) {
		const auto& source = a_clouds->colors[i];
		if (!cloudColorState.valid ||
			std::memcmp(std::addressof(cloudColorState.authored[i]), std::addressof(source), sizeof(source)) != 0) {
			cloudColorState.authored[i] = source;
		}
	}
	cloudColorState.valid = true;
	PublishCloudColors(IsLinearLightingActive());
}

void LinearLighting::PublishSkyColors(bool a_linear)
{
	if (!skyColorState.valid || !skyColorState.owner)
		return;

	for (std::size_t i = 0; i < skyColorState.authored.size(); ++i) {
		if (a_linear && i != static_cast<std::size_t>(RE::TESWeather::ColorTypes::kUnknown)) {
			const auto semantic = static_cast<LinearLightingColors::Semantic>(
				static_cast<std::uint32_t>(LinearLightingColors::Semantic::SkyColorBase) + i);
			skyColorState.owner->skyColor[i] = semanticColorCache.ConvertAuthored(
				skyColorState.owner,
				semantic,
				skyColorState.authored[i],
				false);
		} else {
			skyColorState.owner->skyColor[i] = skyColorState.authored[i];
		}
	}
	skyColorState.publishedLinear = a_linear;
}

void LinearLighting::PublishCloudColors(bool a_linear)
{
	if (!cloudColorState.valid || !cloudColorState.owner)
		return;

	for (std::size_t i = 0; i < cloudColorState.layerCount; ++i) {
		auto* geometry = cloudColorState.owner->clouds[i].get();
		if (!geometry)
			continue;

		auto* property = netimmerse_cast<RE::BSSkyShaderProperty*>(
			geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (!property)
			continue;

		const auto& authored = cloudColorState.authored[i];
		const auto semantic = static_cast<LinearLightingColors::Semantic>(
			static_cast<std::uint32_t>(LinearLightingColors::Semantic::CloudColorBase) + i);
		const auto selected = a_linear ?
		                          semanticColorCache.ConvertAuthored(cloudColorState.owner, semantic, authored, false) :
		                          authored;
		property->kBlendColor.red = selected.red;
		property->kBlendColor.green = selected.green;
		property->kBlendColor.blue = selected.blue;
	}
	cloudColorState.publishedLinear = a_linear;
}

void LinearLighting::SynchronizeTransientColorDomains(bool a_linear)
{
	if (skyColorState.valid && skyColorState.publishedLinear != a_linear) {
		PublishSkyColors(a_linear);
		globals::features::skySync.ApplySunlightDimming(skyColorState.owner);
	}
	if (cloudColorState.valid && cloudColorState.publishedLinear != a_linear)
		PublishCloudColors(a_linear);
}

void LinearLighting::DecodeDirectionalAmbientColors(RE::NiColor (&a_colors)[3][2], RE::NiColor* a_specularTint)
{
	if (!IsLinearLightingActive())
		return;

	const void* owner = globals::game::sky ? static_cast<const void*>(globals::game::sky) : static_cast<const void*>(&a_colors);
	std::uint32_t colorIndex = 0;
	for (auto& axis : a_colors) {
		for (auto& color : axis) {
			const auto semantic = static_cast<LinearLightingColors::Semantic>(
				static_cast<std::uint32_t>(LinearLightingColors::Semantic::DirectionalAmbientBase) + colorIndex++);
			color = semanticColorCache.ConvertAuthored(owner, semantic, color, false);
		}
	}

	if (a_specularTint) {
		const auto semantic = static_cast<LinearLightingColors::Semantic>(
			static_cast<std::uint32_t>(LinearLightingColors::Semantic::DirectionalAmbientBase) + colorIndex);
		*a_specularTint = semanticColorCache.ConvertAuthored(owner, semantic, *a_specularTint, false);
	}
}

namespace
{
	ColorProducerContext PrepareColorProducer(
		ColorProducer a_type,
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		const RE::BSShaderMaterial* a_material)
	{
		auto& linearLighting = globals::features::linearLighting;
		if (!linearLighting.IsLinearLightingActive())
			return {};
		ColorProducerContext prepared{};
		prepared.type = a_type;

		auto* pixelShader = globals::game::currentPixelShader ? *globals::game::currentPixelShader : nullptr;
		if (!pixelShader)
			return prepared;

		auto preparePointLights = [&](std::uint32_t a_maxLights) {
			if (!a_pass || !a_pass->sceneLights)
				return;

			prepared.pointLightCount = a_pass->numLights > kFirstPointLightSceneIndex ?
			                               std::min<std::uint32_t>(a_pass->numLights - kFirstPointLightSceneIndex, a_maxLights) :
			                               0;
			for (std::uint32_t lightIndex = 0; lightIndex < prepared.pointLightCount; ++lightIndex) {
				auto* sceneLight = a_pass->sceneLights[lightIndex + kFirstPointLightSceneIndex];
				auto* light = sceneLight ? sceneLight->light.get() : nullptr;
				auto& pointLight = prepared.pointLights[lightIndex];
				pointLight.flags = PointLightFlags::GetVanillaPointLightFlags(sceneLight, light);
				if (!light)
					continue;

				const auto runtimeFlags = PointLightFlags::GetRuntimeLightFlags(light);
				const bool alreadyLinear =
					(runtimeFlags & PointLightFlags::ToMask(PointLightFlags::Flags::Linear)) != 0;
				pointLight.source = light->GetLightRuntimeData().diffuse;
				pointLight.working = linearLighting.GetWorkingPointLightColor(light, pointLight.source, alreadyLinear);
				pointLight.replace = true;
			}
		};
		auto prepareDirectionalLight = [&]() {
			if (!a_pass || !a_pass->sceneLights || a_pass->numLights == 0)
				return;

			auto* sceneLight = a_pass->sceneLights[0];
			auto* light = sceneLight ? sceneLight->light.get() : nullptr;
			if (!light)
				return;

			prepared.directionalLight.source = light->GetLightRuntimeData().diffuse;
			prepared.directionalLight.working = linearLighting.GetWorkingLinearColor(
				light,
				LinearLightingColors::Semantic::DirectionalLight,
				prepared.directionalLight.source);
			prepared.directionalLight.replace = true;
		};

		const auto shaderDescriptor = pixelShader->id;
		if (a_type == ColorProducer::LightingMaterial) {
			const auto* shader = static_cast<const RE::BSLightingShader*>(a_shader);
			if (!shader || !a_material || a_material->GetType() != RE::BSShaderMaterial::Type::kLighting)
				return prepared;

			const auto* lightingMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(a_material);
			const auto rawTechnique = static_cast<std::uint32_t>(shader->currentRawTechnique);
			const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((rawTechnique >> 24) & 0x3Fu);
			if (technique == SIE::ShaderCache::LightingShaderTechniques::Hair &&
				a_material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) {
				const auto* hairMaterial = static_cast<const RE::BSLightingShaderMaterialHairTint*>(a_material);
				prepared.hairTint = linearLighting.GetWorkingAuthoredColor(
					hairMaterial,
					LinearLightingColors::Semantic::HairTint,
					hairMaterial->tintColor);
			}

			const auto specularFlag = static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Specular);
			const auto truePBRFlag = static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr);
			if ((shaderDescriptor & specularFlag) != 0 && (shaderDescriptor & truePBRFlag) == 0) {
				prepared.lightingSpecular = linearLighting.GetWorkingAuthoredColorScaled(
					lightingMaterial,
					LinearLightingColors::Semantic::LightingSpecular,
					lightingMaterial->specularColor,
					lightingMaterial->specularColorScale);
			}
			return prepared;
		}

		if (a_type == ColorProducer::LightingGeometry) {
			if (!a_pass || !a_pass->shaderProperty ||
				a_pass->shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
				return prepared;

			const auto* property = static_cast<RE::BSLightingShaderProperty*>(a_pass->shaderProperty);
			if (std::abs(property->emissiveMult) <= kMinimumColorScale) {
				prepared.lightingEmission = RE::NiColor{};
			} else if (property->emissiveColor) {
				prepared.lightingEmission = linearLighting.GetWorkingAuthoredColorScaled(
					property,
					LinearLightingColors::Semantic::LightingEmission,
					*property->emissiveColor,
					property->emissiveMult);
			}

			const bool hasProjectedUV =
				(shaderDescriptor & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) != 0;
			if (hasProjectedUV && a_pass->geometry && !a_pass->geometry->AsMultiIndexTriShape()) {
				prepared.projectedMaterial = linearLighting.GetWorkingAuthoredColor(
					property,
					LinearLightingColors::Semantic::ProjectedMaterial,
					{ property->projectedUVColor.red, property->projectedUVColor.green, property->projectedUVColor.blue });
			}

			if (!globals::features::lightLimitFix.loaded)
				preparePointLights(kMaxLightingPointLights);
			prepareDirectionalLight();
			return prepared;
		}

		if (a_type == ColorProducer::WaterMaterial) {
			if (!a_material || a_material->GetType() != RE::BSShaderMaterial::Type::kWater)
				return prepared;

			const auto* waterMaterial = static_cast<const RE::BSWaterShaderMaterial*>(a_material);
			RE::NiColor waterMultiplier{ 1.0f, 1.0f, 1.0f };
			if (const auto* sky = globals::game::sky)
				waterMultiplier = sky->skyColor[static_cast<std::size_t>(RE::TESWeather::ColorTypes::kWaterMultiplier)];

			auto convertWater = [&](LinearLightingColors::Semantic a_semantic, const RE::NiColor& a_authored) {
				auto linear = linearLighting.GetLinearAuthoredColor(
					waterMaterial,
					a_semantic,
					a_authored);
				linear.red *= waterMultiplier.red;
				linear.green *= waterMultiplier.green;
				linear.blue *= waterMultiplier.blue;
				return linearLighting.GetWorkingLinearColor(
					waterMaterial,
					a_semantic,
					linear);
			};
			prepared.waterShallow = convertWater(LinearLightingColors::Semantic::WaterShallow, waterMaterial->shallowWaterColor);
			prepared.waterDeep = convertWater(
				LinearLightingColors::Semantic::WaterDeep,
				{ waterMaterial->deepWaterColor.red, waterMaterial->deepWaterColor.green, waterMaterial->deepWaterColor.blue });
			prepared.waterReflection = linearLighting.GetWorkingAuthoredColor(
				waterMaterial,
				LinearLightingColors::Semantic::WaterReflection,
				{ waterMaterial->reflectionColor.red, waterMaterial->reflectionColor.green, waterMaterial->reflectionColor.blue });
			return prepared;
		}

		if (a_type == ColorProducer::WaterGeometry) {
			if (!globals::features::lightLimitFix.loaded)
				preparePointLights(kMaxWaterPointLights);
			prepareDirectionalLight();
			return prepared;
		}

		auto hasEffectFlag = [&](SIE::ShaderCache::EffectShaderFlags a_flag) {
			return (shaderDescriptor & static_cast<std::uint32_t>(a_flag)) != 0;
		};
		const bool isMembrane = hasEffectFlag(SIE::ShaderCache::EffectShaderFlags::Membrane);
		const bool isGrayscaleToColor = hasEffectFlag(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor);
		const bool isBlood = hasEffectFlag(SIE::ShaderCache::EffectShaderFlags::Blood);
		const auto gammaRenderTarget = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
		const bool rendersToGamma =
			(globals::state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) != 0;

		if (a_type == ColorProducer::EffectMaterial) {
			if (rendersToGamma || !a_material || a_material->GetType() != RE::BSShaderMaterial::Type::kEffect ||
				isMembrane || isGrayscaleToColor || isBlood)
				return prepared;

			const auto* effectMaterial = static_cast<const RE::BSEffectShaderMaterial*>(a_material);
			prepared.effectBase = linearLighting.GetWorkingAuthoredColorScaled(
				effectMaterial,
				LinearLightingColors::Semantic::EffectBase,
				{ effectMaterial->baseColor.red, effectMaterial->baseColor.green, effectMaterial->baseColor.blue },
				effectMaterial->baseColorScale);
			return prepared;
		}

		if (a_type != ColorProducer::EffectGeometry)
			return prepared;

		preparePointLights(kMaxEffectPointLights);
		prepareDirectionalLight();
		if (rendersToGamma || !a_pass || !a_pass->shaderProperty ||
			a_pass->shaderProperty->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get() || isBlood)
			return prepared;

		const auto* property = static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty);
		if (isMembrane) {
			const auto* effectData = property->effectData.get();
			if (!effectData)
				return prepared;

			prepared.effectRim = linearLighting.GetWorkingAuthoredColor(
				effectData,
				LinearLightingColors::Semantic::EffectRim,
				{ effectData->rimColor.red, effectData->rimColor.green, effectData->rimColor.blue });
			if (!isGrayscaleToColor) {
				prepared.effectFill = linearLighting.GetWorkingAuthoredColorScaled(
					effectData,
					LinearLightingColors::Semantic::EffectFill,
					{ effectData->fillColor.red, effectData->fillColor.green, effectData->fillColor.blue },
					effectData->baseFillScale);
			}
			return prepared;
		}

		if (!isGrayscaleToColor) {
			prepared.effectEmittance = linearLighting.GetWorkingAuthoredColor(
				property,
				LinearLightingColors::Semantic::EffectEmittance,
				property->emittanceColor ? *property->emittanceColor : RE::NiColor{ 1.0f, 1.0f, 1.0f });
		}
		return prepared;
	}
}

void LinearLighting::PatchMappedPSConstants(ID3D11Resource* a_resource)
{
	if (!a_resource || currentColorProducer.type == ColorProducer::None)
		return;

	auto* shadowState = globals::game::shadowState;
	auto* pixelShader = globals::game::currentPixelShader ? *globals::game::currentPixelShader : nullptr;
	if (!shadowState || !pixelShader)
		return;

	RE::BSGraphics::ConstantGroupLevel level{};
	RE::BSGraphics::ConstantGroup* constantGroup = nullptr;
	for (const auto candidate : {
			 RE::BSGraphics::ConstantGroupLevel::PerTechnique,
			 RE::BSGraphics::ConstantGroupLevel::PerMaterial,
			 RE::BSGraphics::ConstantGroupLevel::PerGeometry }) {
		auto& group = shadowState->GetPSConstantGroup(candidate);
		if (reinterpret_cast<void*>(group.buffer) == reinterpret_cast<void*>(a_resource)) {
			level = candidate;
			constantGroup = std::addressof(group);
			break;
		}
	}
	if (!constantGroup || !constantGroup->data)
		return;

	auto getStagedConstant = [&](std::int32_t a_constantIndex) -> float* {
		if (a_constantIndex < 0 || static_cast<std::size_t>(a_constantIndex) >= pixelShader->constantTable.size())
			return nullptr;
		const auto constantOffset = pixelShader->constantTable[a_constantIndex];
		if (constantOffset < 0)
			return nullptr;
		return reinterpret_cast<float*>(constantGroup->data) + constantOffset;
	};
	auto getStagedColor = [&](std::int32_t a_constantIndex) -> RE::NiColor* {
		return reinterpret_cast<RE::NiColor*>(getStagedConstant(a_constantIndex));
	};
	auto copyPreparedColor = [&](std::int32_t a_constantIndex, const std::optional<RE::NiColor>& a_color) {
		if (!a_color)
			return;
		if (auto* stagedColor = getStagedColor(a_constantIndex))
			*stagedColor = *a_color;
	};
	auto applyPreparedScaledColor = [&](const ColorProducerContext::ScaledColor& a_color, float& a_red, float& a_green, float& a_blue) {
		if (!a_color.replace)
			return false;

		const std::array sourceChannels{ a_color.source.red, a_color.source.green, a_color.source.blue };
		const std::array stagedChannels{ a_red, a_green, a_blue };
		std::size_t scaleChannel = 0;
		for (std::size_t channel = 1; channel < sourceChannels.size(); ++channel) {
			if (std::abs(sourceChannels[channel]) > std::abs(sourceChannels[scaleChannel]))
				scaleChannel = channel;
		}

		if (sourceChannels[scaleChannel] == 0.0f) {
			a_red = 0.0f;
			a_green = 0.0f;
			a_blue = 0.0f;
			return true;
		}

		const float scale = stagedChannels[scaleChannel] / sourceChannels[scaleChannel];
		if (!std::isfinite(scale))
			return false;

		a_red = a_color.working.red * scale;
		a_green = a_color.working.green * scale;
		a_blue = a_color.working.blue * scale;
		return true;
	};
	auto applyPreparedPointLight = [&](const ColorProducerContext::PointLight& a_light, float& a_red, float& a_green, float& a_blue) {
		return applyPreparedScaledColor(a_light, a_red, a_green, a_blue);
	};
	auto applyPreparedDirectionalLight = [&](std::int32_t a_constantIndex) {
		if (auto* stagedColor = getStagedColor(a_constantIndex))
			applyPreparedScaledColor(
				currentColorProducer.directionalLight,
				stagedColor->red,
				stagedColor->green,
				stagedColor->blue);
	};

	if (currentColorProducer.type == ColorProducer::LightingMaterial &&
		level == RE::BSGraphics::ConstantGroupLevel::PerMaterial) {
		copyPreparedColor(ShaderConstants::LightingPS::Get().TintColor, currentColorProducer.hairTint);
		copyPreparedColor(ShaderConstants::LightingPS::Get().SpecularColor, currentColorProducer.lightingSpecular);
		return;
	}

	if (currentColorProducer.type == ColorProducer::LightingGeometry &&
		level == RE::BSGraphics::ConstantGroupLevel::PerGeometry) {
		applyPreparedDirectionalLight(ShaderConstants::LightingPS::Get().DirLightColor);
		copyPreparedColor(ShaderConstants::LightingPS::Get().EmitColor, currentColorProducer.lightingEmission);
		copyPreparedColor(ShaderConstants::LightingPS::Get().ProjectedUVParams2, currentColorProducer.projectedMaterial);
		if (auto* pointLightColors = getStagedConstant(ShaderConstants::LightingPS::Get().PointLightColor)) {
			for (std::uint32_t lightIndex = 0; lightIndex < currentColorProducer.pointLightCount; ++lightIndex) {
				auto* stagedColor = pointLightColors + lightIndex * 4;
				applyPreparedPointLight(currentColorProducer.pointLights[lightIndex], stagedColor[0], stagedColor[1], stagedColor[2]);
			}
		}
		return;
	}

	if (currentColorProducer.type == ColorProducer::WaterMaterial &&
		level == RE::BSGraphics::ConstantGroupLevel::PerMaterial) {
		const ShaderConstants::WaterPS waterConstants;
		copyPreparedColor(waterConstants.ShallowColor, currentColorProducer.waterShallow);
		copyPreparedColor(waterConstants.DeepColor, currentColorProducer.waterDeep);
		copyPreparedColor(waterConstants.ReflectionColor, currentColorProducer.waterReflection);
		return;
	}

	if (currentColorProducer.type == ColorProducer::WaterGeometry) {
		const ShaderConstants::WaterPS waterConstants;
		if (level == RE::BSGraphics::ConstantGroupLevel::PerTechnique) {
			applyPreparedDirectionalLight(waterConstants.SunColor);
			return;
		}
		if (level != RE::BSGraphics::ConstantGroupLevel::PerGeometry)
			return;
		if (auto* pointLightColors = getStagedConstant(waterConstants.LightColor)) {
			for (std::uint32_t lightIndex = 0; lightIndex < currentColorProducer.pointLightCount; ++lightIndex) {
				auto* stagedColor = pointLightColors + lightIndex * 4;
				applyPreparedPointLight(currentColorProducer.pointLights[lightIndex], stagedColor[0], stagedColor[1], stagedColor[2]);
			}
		}
		return;
	}

	if (currentColorProducer.type == ColorProducer::EffectGeometry &&
		level == RE::BSGraphics::ConstantGroupLevel::PerGeometry) {
		const auto& effectConstants = ShaderConstants::EffectPS::Get();
		applyPreparedDirectionalLight(effectConstants.DLightColor);
		auto* extendedFlags = getStagedConstant(effectConstants.ExtendedFlags);
		auto* pointLightRed = getStagedConstant(effectConstants.PLightColorR);
		auto* pointLightGreen = getStagedConstant(effectConstants.PLightColorG);
		auto* pointLightBlue = getStagedConstant(effectConstants.PLightColorB);
		if (extendedFlags)
			*reinterpret_cast<std::uint32_t*>(extendedFlags) = 0;
		if (extendedFlags && pointLightRed && pointLightGreen && pointLightBlue) {
			auto& packedFlags = *reinterpret_cast<std::uint32_t*>(extendedFlags);
			for (std::uint32_t lightIndex = 0; lightIndex < currentColorProducer.pointLightCount; ++lightIndex) {
				applyPreparedPointLight(
					currentColorProducer.pointLights[lightIndex],
					pointLightRed[lightIndex],
					pointLightGreen[lightIndex],
					pointLightBlue[lightIndex]);
				packedFlags |= PackEffectPointLightFlags(currentColorProducer.pointLights[lightIndex].flags) <<
				               (lightIndex * kEffectPointLightFlagWidth);
			}
		}
		copyPreparedColor(effectConstants.MembraneRimColor, currentColorProducer.effectRim);
		copyPreparedColor(effectConstants.PropertyColor, currentColorProducer.effectFill);
		copyPreparedColor(effectConstants.PropertyColor, currentColorProducer.effectEmittance);
		return;
	}

	if (currentColorProducer.type == ColorProducer::EffectMaterial &&
		level == RE::BSGraphics::ConstantGroupLevel::PerMaterial)
		copyPreparedColor(ShaderConstants::EffectPS::Get().BaseColor, currentColorProducer.effectBase);
}

RE::NiColor LinearLighting::GetWorkingPointLightColor(
	const RE::NiLight* a_light,
	const RE::NiColor& a_source,
	bool a_alreadyLinear)
{
	return a_alreadyLinear ?
	           semanticColorCache.ConvertLinear(
			   a_light,
			   LinearLightingColors::Semantic::PointLight,
			   a_source,
			   IsACEScgActive()) :
	           semanticColorCache.ConvertAuthored(
			   a_light,
			   LinearLightingColors::Semantic::PointLight,
			   a_source,
			   IsACEScgActive());
}

RE::NiColor LinearLighting::GetWorkingAuthoredColor(
	const void* a_owner,
	LinearLightingColors::Semantic a_semantic,
	const RE::NiColor& a_authored)
{
	return semanticColorCache.ConvertAuthored(a_owner, a_semantic, a_authored, IsACEScgActive());
}

RE::NiColor LinearLighting::GetWorkingAuthoredColorScaled(
	const void* a_owner,
	LinearLightingColors::Semantic a_semantic,
	const RE::NiColor& a_authored,
	float a_scale)
{
	return semanticColorCache.ConvertAuthoredScaled(a_owner, a_semantic, a_authored, a_scale, IsACEScgActive());
}

RE::NiColor LinearLighting::GetWorkingLinearColor(
	const void* a_owner,
	LinearLightingColors::Semantic a_semantic,
	const RE::NiColor& a_linear)
{
	if (!IsACEScgActive())
		return a_linear;
	return semanticColorCache.ConvertLinear(a_owner, a_semantic, a_linear, true);
}

RE::NiColor LinearLighting::GetLinearAuthoredColor(
	const void* a_owner,
	LinearLightingColors::Semantic a_semantic,
	const RE::NiColor& a_authored)
{
	return semanticColorCache.ConvertAuthored(a_owner, a_semantic, a_authored, false);
}

#undef I18N_KEY_PREFIX
