import os
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


@unittest.skipUnless(os.name == "nt", "Uses the Windows build's native C++ dependencies")
class PostProcessingLifecycleTests(unittest.TestCase):
    def compile_and_run(self, source):
        dependencies = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source, imgui_root=dependencies)

    def test_resource_recreation_preserves_live_settings_and_applies_pending_changes(self):
        implementation = (ROOT / "src/Features/PostProcessing.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/PostProcessing.h").read_text(encoding="utf-8")
        methods = "\n".join(braced(implementation, declaration) for declaration in (
            "void PostProcessing::LoadSettings(",
            "void PostProcessing::ProcessSettings(",
            "void PostProcessing::SaveSettings(",
            "void PostProcessing::SaveActiveSettings(",
            "bool PostProcessing::ApplyPendingSettings(",
            "void PostProcessing::RestorePipelineDefaultEnablement(",
            "void PostProcessing::SetupResources(",
            "void PostProcessing::Reset(",
        ))
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace eastl { using std::make_unique; }
namespace logger {
template<class... T> void debug(T&&...) {}
template<class... T> void warn(T&&...) {}
template<class... T> void error(T&&...) {}
}
enum class SetupFailure { None, Texture, Sampler, Effect };
SetupFailure setupFailure = SetupFailure::None;
namespace DX {
struct com_exception : std::runtime_error { com_exception() : std::runtime_error("Injected Direct3D failure") {} };
void ThrowIfFailed(int result) { if (result) throw com_exception{}; }
}
struct ID3D11ShaderResourceView {};
struct D3D11_TEXTURE2D_DESC { int Format = 1, MipLevels = 1, BindFlags = 0, MiscFlags = 0; unsigned Width = 1920, Height = 1080; };
struct D3D11_RENDER_TARGET_VIEW_DESC { int Format, ViewDimension; struct { int MipSlice; } Texture2D; };
struct D3D11_SHADER_RESOURCE_VIEW_DESC { int Format, ViewDimension; struct { int MostDetailedMip, MipLevels; } Texture2D; };
constexpr int D3D11_BIND_RENDER_TARGET = 1, D3D11_BIND_SHADER_RESOURCE = 2;
constexpr int D3D11_RTV_DIMENSION_TEXTURE2D = 1, D3D11_SRV_DIMENSION_TEXTURE2D = 1;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT = 2;
struct Resource { void GetDesc(D3D11_TEXTURE2D_DESC* desc) { *desc = {}; } } resource;
struct Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    Texture2D(D3D11_TEXTURE2D_DESC value, const char* name) : desc(value) {
        if (setupFailure == SetupFailure::Texture && std::string(name) == "PostProcessing::Output")
            throw DX::com_exception{};
    }
    void CreateRTV(D3D11_RENDER_TARGET_VIEW_DESC) {}
    void CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC) {}
};
struct Sampler {};
template<class T> struct View {
    T* value = nullptr;
    T* get() { return value; }
    T** put() { return &value; }
    explicit operator bool() const { return value != nullptr; }
    void operator=(std::nullptr_t) { value = nullptr; }
};
struct D3D11_SAMPLER_DESC { int Filter, AddressU, AddressV, AddressW, MaxAnisotropy; float MaxLOD; };
constexpr int D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0, D3D11_TEXTURE_ADDRESS_CLAMP = 0;
constexpr float D3D11_FLOAT32_MAX = 1e30f;
namespace globals::d3d {
struct Device {
    int CreateSamplerState(D3D11_SAMPLER_DESC*, Sampler**) { return setupFailure == SetupFailure::Sampler ? -1 : 0; }
} deviceStorage;
auto* device = &deviceStorage;
}
struct Feature {
    struct PostProcessingInput { unsigned width = 0, height = 0; Resource* texture = nullptr; ID3D11ShaderResourceView* srv = nullptr; } input;
    bool loaded = true;
    PostProcessingInput GetPostProcessingInput() const { return input; }
    static inline Feature* provider = nullptr;
    template<class Pred> static Feature* FindLoadedFeature(Pred pred) { return provider && provider->loaded && pred(provider) ? provider : nullptr; }
};
struct ConstantBuffer { ConstantBuffer(int, const char*) {} };
template<class T> int ConstantBufferDesc() { return 0; }
namespace RE::RENDER_TARGETS { enum { kMAIN, kMAIN_COPY }; }
struct Renderer {
    struct Target { Resource* texture = &resource; };
    struct Data { std::array<Target, 2> renderTargets; } data;
    Data& GetRuntimeData() { return data; }
} renderer;
namespace globals::game { auto* renderer = &::renderer; }
namespace Util {
template<class T> T* AsW32(T* value) { return value; }
void RequestTargetLockAPI() {}
void SetResourceName(Sampler*, const char*) {}
}
struct Effect {
    virtual ~Effect() = default;
    virtual std::string GetType() const = 0;
    bool enabled = true;
    void* owner = nullptr;
    json settings = {{"currentTonemapper", "GT7"}, {"strength", 1.0}};
    bool IsAutoEnabled() const { return false; }
    void LoadSettings(json& value) { settings = value; }
    void SaveSettings(json& value) { value = settings; }
    void SetupResources() { if (setupFailure == SetupFailure::Effect) throw DX::com_exception{}; }
    void Reset() {}
};
struct PostProcessing : Feature {
    PIPELINE_INDEX;
    std::array<std::shared_ptr<Effect>, static_cast<size_t>(FeaturePipelineIndex::COUNT)> pipeline;
    json settings = {{"DisableVanillaTonemapping", 1}}, pendingSettings;
    struct CinematicCamera {
        json settings = {{"fov", 75}};
        void LoadSettings(json& value) { settings = value; }
        void SaveSettings(json& value) { value = settings; }
    } cinematicCamera;
    struct Bokeh { void Setup() {} } bokehResources;
    struct CopyCB {};
    std::unique_ptr<Texture2D> texCopyMain, texCopyMainCopy, texInput, texOutput;
    View<Sampler> copySampler;
    View<Resource> fullscreenVS, copyPS;
    bool resourcesReady = false;
    Feature* inputProvider = nullptr;
    D3D11_TEXTURE2D_DESC pipelineTextureDesc;
    std::unique_ptr<ConstantBuffer> copyCB;
    ID3D11ShaderResourceView* postProcessingOutput = nullptr;
    bool isrefraction = false;
    void CompileCopyShaders() { fullscreenVS.value = copyPS.value = &resource; }
    void RestoreDefaultSettings() { std::abort(); }
    void RestorePipelineDefaultEnablement();
    void LoadSettings(json&);
    void ProcessSettings(json&);
    void SaveSettings(json&);
    void SaveActiveSettings(json&);
    bool ApplyPendingSettings();
    void SetupResources();
    void Reset();
};
template<PostProcessing::FeaturePipelineIndex Index> struct PipelineEffect : Effect {
    std::string GetType() const override { return std::to_string(static_cast<int>(Index)); }
};
EFFECT_TYPES
using HistogramAutoExposure = AutoExposure;
DEFAULT_ENABLEMENT
METHODS
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    PostProcessing pp;
    const auto gradingIndex = static_cast<size_t>(PostProcessing::FeaturePipelineIndex::ColorGrading);
    const auto bloomIndex = static_cast<size_t>(PostProcessing::FeaturePipelineIndex::CODBloom);
    const auto grading = std::to_string(gradingIndex);
    const auto bloom = std::to_string(bloomIndex);
    json preset = {
        {grading, {{"enabled", true}, {"settings", {{"currentTonemapper", "AgX"}, {"strength", 2.5}}}}},
        {bloom, {{"enabled", false}, {"settings", {{"strength", 0.2}}}}},
        {"cinematic_camera", {{"fov", 90}}},
        {"ppsettings", {{"DisableVanillaTonemapping", 0}}}
    };
    pp.LoadSettings(preset);
    pp.SetupResources();
    check(pp.pendingSettings.empty(), "Initial setup consumes the pending preset");
    check(pp.pipeline[gradingIndex]->settings["currentTonemapper"] == "AgX", "Initial setup applies AgX");
    auto previousEffect = pp.pipeline[gradingIndex];
    pp.pipeline[gradingIndex]->settings["strength"] = 4.0;
    pp.pipeline[bloomIndex]->enabled = true;
    pp.cinematicCamera.settings["fov"] = 100;
    json before;
    pp.SaveSettings(before);
    ID3D11ShaderResourceView staleOutput;
    pp.postProcessingOutput = &staleOutput;
    pp.SetupResources();
    json after;
    pp.SaveSettings(after);
    check(before == after, "Resource recreation preserves live settings and effect enablement");
    check(previousEffect != pp.pipeline[gradingIndex], "Resource recreation replaces the effect objects");
    check(!pp.postProcessingOutput, "Resource recreation invalidates the previous scene SRV");
    json update = {{grading, {{"enabled", false}, {"settings", {{"currentTonemapper", "GT7"}}}}}};
    pp.LoadSettings(update);
    json queued;
    pp.SaveSettings(queued);
    check(queued == update, "Saving a queued update retains existing pending-settings semantics");
    pp.SetupResources();
    after = {};
    pp.SaveSettings(after);
    before[grading] = update[grading];
    check(before == after, "Pending changes override live settings without resetting unrelated effects");
    pp.postProcessingOutput = &staleOutput;
    pp.Reset();
    check(!pp.postProcessingOutput, "Frame reset invalidates the previous scene SRV");
    check(pp.pipelineTextureDesc.Width == 1920 && !pp.texOutput, "Default pipeline matches the engine size");
    Feature provider;
    provider.input.width = 2880;
    provider.input.height = 1620;
    Feature::provider = &provider;
    pp.SetupResources();
    check(pp.pipelineTextureDesc.Width == 2880 && pp.pipelineTextureDesc.Height == 1620,
          "Replacement resolution is available before the provider allocates its texture");
    check(pp.texInput->desc.Width == 2880 && pp.texOutput->desc.Width == 2880,
          "Input decoding and output conversion use the replacement resolution");
    check(pp.texCopyMain->desc.Width == 1920, "Engine copy fallback retains the engine resolution");
    for (auto failure : {SetupFailure::Texture, SetupFailure::Sampler, SetupFailure::Effect}) {
        for (bool queuedUpdate : {false, true}) {
            json expected;
            pp.SaveSettings(expected);
            if (queuedUpdate) {
                json change = {{bloom, {{"enabled", false}, {"settings", {{"strength", 0.75}}}}}};
                pp.LoadSettings(change);
                expected.update(change);
            }
            setupFailure = failure;
            pp.postProcessingOutput = &staleOutput;
            pp.SetupResources();
            check(!pp.resourcesReady && !pp.postProcessingOutput, "Failed setup disables the pipeline and its published output");
            check(!pp.texInput && !pp.texOutput && !pp.copyCB && !pp.copySampler && !pp.fullscreenVS && !pp.copyPS,
                  "Failed setup releases partial resources");
            for (auto& effect : pp.pipeline)
                check(!effect, "Failed setup releases partial effects");
            check(!pp.ApplyPendingSettings(), "Unavailable effects cannot consume saved settings");
            json saved;
            pp.SaveSettings(saved);
            check(saved == expected, "Failed setup preserves active settings and queued overrides");
            pp.SetupResources();
            pp.SaveSettings(saved);
            check(saved == expected, "Repeated setup failure preserves the recovery snapshot");
            setupFailure = SetupFailure::None;
            pp.SetupResources();
            saved = {};
            pp.SaveSettings(saved);
            check(pp.resourcesReady && pp.pendingSettings.empty() && saved == expected,
                  "Successful retry restores settings and reenables processing");
        }
    }
    provider.loaded = false;
    pp.SetupResources();
    check(pp.pipelineTextureDesc.Width == 1920 && !pp.texOutput && !pp.inputProvider,
          "An unavailable provider restores engine resolution and drops replacement resources");
}
'''
        index = braced(header, "enum class FeaturePipelineIndex")
        effect_names = [line.strip().rstrip(",") for line in index.splitlines()[2:-1]]
        effects = "\n".join(
            f"using {name} = PipelineEffect<PostProcessing::FeaturePipelineIndex::{name}>;"
            for name in effect_names if name and name != "COUNT"
        )
        source = source.replace("PIPELINE_INDEX", index).replace("EFFECT_TYPES", effects)
        source = source.replace("DEFAULT_ENABLEMENT", braced(implementation, "constexpr bool IsPipelineFeatureEnabledByDefault("))
        self.compile_and_run(source.replace("METHODS", methods))

    def test_tonemap_consumes_processed_scene_and_restores_engine_bindings(self):
        implementation = (ROOT / "src/Features/Upscaling/PerfMode/PostIntercept.cpp").read_text(encoding="utf-8")
        feature = (ROOT / "src/Feature.h").read_text(encoding="utf-8")
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CS_GPU_PASS(name)
struct ID3D11ShaderResourceView {};
struct DepthView {};
template<class T> struct View {
    T* value = nullptr;
    T* get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
namespace Util {
template<class T> T* AsW32(T* value) { return value; }
template<class T> T* AsReal(T* value) { return value; }
}
namespace RE {
struct BSTriShape {}; struct ImageSpaceEffectParam {};
namespace RENDER_TARGETS { enum { kMAIN, kMAIN_COPY, kTOTAL }; }
namespace RENDER_TARGETS_DEPTHSTENCIL { enum { kMAIN }; }
namespace BSGraphics {
struct Renderer {
    struct Target { ID3D11ShaderResourceView* SRV = nullptr; };
    struct Data { std::array<Target, 2> renderTargets; } data;
    struct Depth { DepthView* views[8]{}; DepthView* readOnlyViews[8]{}; };
    struct DepthData { std::array<Depth, 1> depthStencils; } depthData;
    Data& GetRuntimeData() { return data; }
    DepthData& GetDepthStencilData() { return depthData; }
    static Renderer* GetSingleton() { static Renderer renderer; return &renderer; }
};
}
}
struct Feature {
    bool loaded = true;
    ID3D11ShaderResourceView* output = nullptr;
    ID3D11ShaderResourceView* GetPostProcessingOutput() const { return output; }
    static auto& GetFeatureList() { static std::vector<Feature*> features; return features; }
    template<class Pred> FIND_FEATURE
};
struct PerfMode {
    bool hookActive = true;
    View<ID3D11ShaderResourceView> testTextureSRV;
    View<DepthView> fakeDSV;
    ID3D11ShaderResourceView *savedKMainSRV = nullptr, *savedKMainCopySRV = nullptr;
    DepthView* savedKMainViews[8]{};
    DepthView* savedKMainReadOnlyViews[8]{};
    void MaybeBlitMenuBG(int) {}
    template<class Hook> static void RenderTonemapWithSwap(void*, RE::BSTriShape*, RE::ImageSpaceEffectParam*);
};
namespace globals {
struct State { bool worldRenderedThisFrame = true; bool IsMainOrLoadingMenuOpen() { return false; } } stateStorage;
auto* state = &stateStorage;
namespace features { struct Upscaling { PerfMode perfMode; } upscaling; }
}
template<class Hook> TONEMAP
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
ID3D11ShaderResourceView* expected;
struct Hook {
    static void func(void*, RE::BSTriShape*, RE::ImageSpaceEffectParam*) {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        check(renderer->data.renderTargets[0].SRV == expected, "Tonemap receives the completed scene");
        check(renderer->data.renderTargets[1].SRV == expected, "Refraction uses the same completed scene");
        check(renderer->depthData.depthStencils[0].views[0] == globals::features::upscaling.perfMode.fakeDSV.get(),
              "Display-resolution depth remains active during tonemap");
    }
};
int main() {
    ID3D11ShaderResourceView raw, main, mainCopy, processed;
    DepthView engineDepth, displayDepth;
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    renderer->data.renderTargets = {{{&main}, {&mainCopy}}};
    renderer->depthData.depthStencils[0].views[0] = &engineDepth;
    renderer->depthData.depthStencils[0].readOnlyViews[0] = &engineDepth;
    auto& perfMode = globals::features::upscaling.perfMode;
    perfMode.testTextureSRV.value = &raw;
    perfMode.fakeDSV.value = &displayDepth;
    Feature inactive, producer;
    Feature::GetFeatureList() = {&inactive, &producer};
    for (bool loaded : {false, true}) for (bool hasOutput : {false, true}) {
        producer.loaded = loaded;
        producer.output = hasOutput ? &processed : nullptr;
        expected = loaded && hasOutput ? &processed : &raw;
        PerfMode::RenderTonemapWithSwap<Hook>(nullptr, nullptr, nullptr);
        check(renderer->data.renderTargets[0].SRV == &main && renderer->data.renderTargets[1].SRV == &mainCopy,
              "Tonemap restores both engine SRVs");
        check(renderer->depthData.depthStencils[0].views[0] == &engineDepth &&
              renderer->depthData.depthStencils[0].readOnlyViews[0] == &engineDepth,
              "Tonemap restores writable and read-only depth views");
        check(!renderer->depthData.depthStencils[0].views[1], "Absent depth views stay absent");
    }
}
'''
        source = source.replace("FIND_FEATURE", braced(feature, "static Feature* FindLoadedFeature("))
        source = source.replace("TONEMAP", braced(implementation, "void PerfMode::RenderTonemapWithSwap("))
        self.compile_and_run(source)

    def test_pipeline_preserves_display_resolution_and_disabled_effects(self):
        implementation = (ROOT / "src/Features/PostProcessing.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/PostProcessing.h").read_text(encoding="utf-8")
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#define CS_GPU_PASS(name)
struct ID3D11Texture2D { unsigned width, height; };
struct ID3D11ShaderResourceView { ID3D11Texture2D* texture; };
template<class T> struct View { T* value; T* get() const { return value; } };
struct Texture2D { View<ID3D11ShaderResourceView> srv; };
namespace Util { template<class T> T* AsReal(T* value) { return value; } }
namespace RE {
using RENDER_TARGET = int;
namespace RENDER_TARGETS { enum { kMAIN, kMAIN_COPY }; }
namespace BSGraphics { struct ShaderFlags { enum { DIRTY_RENDERTARGET }; }; }
}
struct Target { ID3D11Texture2D* texture; ID3D11ShaderResourceView* SRV; };
struct State {
    enum class TonemapOwner { kPostProcessing, kEffects11, kVanilla } owner = TonemapOwner::kPostProcessing;
    bool menu = false;
    bool IsMainOrLoadingMenuOpen() { return menu; }
    TonemapOwner GetTonemapOwner() { return owner; }
    void SetOutputRenderTarget(int) {}
};
namespace globals {
State storage; auto* state = &storage;
namespace game {
struct Renderer {
    struct Data { std::array<Target, 2> renderTargets; } data;
    Data& GetRuntimeData() { return data; }
} rendererStorage;
auto* renderer = &rendererStorage;
struct Flags { void set(int) {} } flags;
auto* stateUpdateFlags = &flags;
}
namespace d3d {
struct Context { void OMSetRenderTargets(int, void*, void*) {} } storage;
auto* context = &storage;
}
namespace features {
struct Upscaling { bool loaded = true; } upscaling;
struct LinearLighting {
    struct Settings { bool enableACEScg = false; } settings;
    bool active = true;
    bool IsLinearLightingActive() const { return active; }
} linearLighting;
}
}
struct PostProcessFeature {
    enum class Gamut { Rec709, ACEScg, Rec2020 };
    struct TextureInfo { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; Gamut gamut = Gamut::Rec709; };
    bool enabled = true;
    bool IsAutoEnabled() { return false; }
    void UpdateAutoEnabled() {}
    bool IsActive() { return enabled; }
    bool DrawAfterColorGrading() { return false; }
    bool DisableInMainLoadingMenu() { return false; }
    bool DrawBeforeUpscaling() { return false; }
};
struct ColorGrading : PostProcessFeature {
    struct Settings { bool enableTonemap = true; } settings;
    Gamut GetDisplayGamut() const { return Gamut::Rec709; }
    bool IsReadyForTonemapping() const { return true; }
};
struct Feature {
    struct PostProcessingInput { ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* srv = nullptr; } input;
    bool loaded = true;
    PostProcessingInput GetPostProcessingInput() { return input; }
};
struct PostProcessing : Feature {
    using Gamut = PostProcessFeature::Gamut;
    enum class FeaturePipelineIndex { ColorGrading };
    struct CopyCB { Gamut inputGamut = Gamut::Rec709, outputGamut = Gamut::Rec709; float gamma = 1; };
    static constexpr float kLegacySceneGamma = 1.6f;
    bool bypass = false, fullscreenVS = true, copyPS = true, isrefraction = false;
    bool resourcesReady = true;
    PIPELINE_READY
    struct Settings { int DisableVanillaTonemapping = 1; } settings;
    bool WantsTonemapOwnership() const;
    Feature* inputProvider = nullptr;
    ID3D11ShaderResourceView* postProcessingOutput = nullptr;
    std::unique_ptr<Texture2D> texCopyMain, texCopyMainCopy, texOutput;
    std::array<std::shared_ptr<PostProcessFeature>, 2> pipeline;
    ColorGrading grading;
    ID3D11Texture2D* seenInput = nullptr;
    int draws = 0, copies = 0, conversions = 0;
    bool IsTonemapOwnedByEffects11() { return globals::state->owner == State::TonemapOwner::kEffects11; }
    template<class T> const T* GetPipelineFeature(FeaturePipelineIndex) const { return &grading; }
    void BeginLinearProcessing(PostProcessFeature::TextureInfo& input, bool = true) { seenInput = input.tex; }
    void DrawFeature(PostProcessFeature&, PostProcessFeature::TextureInfo&) { ++draws; }
    void CopyToRenderTarget(Target&, Texture2D*, ID3D11Texture2D*, ID3D11ShaderResourceView*, const CopyCB&) { ++copies; }
    void DrawCopy(Texture2D&, ID3D11Texture2D*, ID3D11ShaderResourceView*, CopyCB) { ++conversions; }
    void PreProcess(int, int);
};
PIPELINE
TONEMAP_OWNERSHIP
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    ID3D11Texture2D render{1920, 1080}, display{2880, 1620}, converted{2880, 1620};
    ID3D11ShaderResourceView renderSRV{&render}, displaySRV{&display}, convertedSRV{&converted};
    globals::game::renderer->data.renderTargets = {{{&render, &renderSRV}, {&render, &renderSRV}}};
    Feature provider;
    provider.input = {&display, &displaySRV};
    PostProcessing pp;
    pp.inputProvider = &provider;
    pp.texOutput = std::make_unique<Texture2D>(Texture2D{{&convertedSRV}});
    pp.pipeline[0] = std::make_shared<PostProcessFeature>();
    pp.pipeline[1] = std::make_shared<PostProcessFeature>();
    pp.pipeline[1]->enabled = false;
    for (bool refraction : {false, true}) {
        pp.isrefraction = refraction;
        pp.PreProcess(refraction ? RE::RENDER_TARGETS::kMAIN_COPY : RE::RENDER_TARGETS::kMAIN, 0);
        check(pp.seenInput == &display, "Effects consume the reconstructed display image, including refraction");
        check(pp.postProcessingOutput == &displaySRV, "Tonemap receives the display-sized output");
        check(pp.copies == 0 && pp.conversions == 0, "Completed output never passes through reduced engine targets");
    }
    check(pp.draws == 2, "Disabled effects never draw");
    globals::state->owner = State::TonemapOwner::kVanilla;
    globals::features::linearLighting.active = false;
    pp.PreProcess(0, 0);
    check(pp.postProcessingOutput == &convertedSRV && pp.conversions == 1 && pp.copies == 0,
          "Vanilla tonemap encoding stays at display resolution");
    provider.input = {};
    pp.PreProcess(0, 0);
    check(pp.seenInput == &render && pp.copies == 2 && !pp.postProcessingOutput,
          "Inactive replacement input uses the engine path without publishing a reduced scene");
    provider.input = {&display, &displaySRV};
    pp.PreProcess(0, 0);
    pp.bypass = true;
    pp.PreProcess(0, 0);
    check(!pp.postProcessingOutput, "Bypass cannot expose stale processed output");
    pp.bypass = false;
    const auto drawsBeforeFailure = pp.draws;
    for (bool shadersReady : {false, true}) {
        pp.resourcesReady = false;
        pp.fullscreenVS = pp.copyPS = shadersReady;
        pp.postProcessingOutput = &displaySRV;
        pp.PreProcess(0, 0);
        check(!pp.postProcessingOutput && pp.draws == drawsBeforeFailure,
              "Recompiling shaders cannot enable a pipeline with failed resource setup");
        check(!pp.WantsTonemapOwnership(), "Failed setup leaves tonemapping to the game pipeline");
    }
}
'''
        source = source.replace("PIPELINE_READY", braced(header, "bool IsPipelineReady("))
        source = source.replace("PIPELINE", braced(implementation, "void PostProcessing::PreProcess("))
        source = source.replace("TONEMAP_OWNERSHIP", braced(implementation, "bool PostProcessing::WantsTonemapOwnership("))
        self.compile_and_run(source)


if __name__ == "__main__":
    unittest.main()
