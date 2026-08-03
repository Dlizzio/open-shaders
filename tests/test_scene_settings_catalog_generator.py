import importlib.util
import math
import tempfile
import unittest
from pathlib import Path


GENERATOR_PATH = Path(__file__).parents[1] / "cmake" / "generate_scene_settings_catalog.py"
SPEC = importlib.util.spec_from_file_location("scene_catalog_generator", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GENERATOR)

KNOWN_PARSER_REGRESSION_SETTINGS = {
    ("ExtendedTranslucency", "", "AlphaMode"),
    ("ExtendedTranslucency", "", "AlphaReduction"),
    ("ExtendedTranslucency", "", "AlphaSoftness"),
    ("ExtendedTranslucency", "", "AlphaStrength"),
    ("LightLimitFix", "", "ContactShadowMaxSteps"),
    ("LightLimitFix", "", "ContactShadowMinIntensity"),
    ("ScreenSpaceGI", "", "EnableGI"),
    ("ScreenSpaceGI", "", "NumSlices"),
    ("ScreenSpaceGI", "", "NumSteps"),
    ("ScreenSpaceGI", "", "UseStereoReproject"),
    ("ScreenSpaceShadows", "", "EnableStereoSync"),
    ("ScreenSpaceShadows", "", "UseStereoReproject"),
    ("TerrainShadows", "", "EnableTerrainShadow"),
    ("Upscaling", "", "qualityMode"),
    ("Upscaling", "", "renderAtUpscaleRes"),
    ("Upscaling", "", "upscaleMethod"),
    ("Upscaling", "", "upscaleMethodNoDLSS"),
    ("VR", "", "EnableDepthBufferCullingExterior"),
    ("VR", "", "EnableDepthBufferCullingInterior"),
    ("VR", "", "EnableSSRFoveation"),
    ("VR", "", "EnableSSRFoveationHardCutoff"),
    ("VR", "", "EnableStereoBlend"),
    ("VR", "", "MinOccludeeBoxExtent"),
    ("VR", "", "StereoBlendColorThreshold"),
    ("VR", "", "StereoBlendDepthSigma"),
    ("VR", "", "StereoBlendMaxFactor"),
    ("VR", "", "mouseDeadzone"),
}


class SceneSettingsCatalogGeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = GENERATOR.build_entries(Path(__file__).parents[1])
        cls.entries_by_id = {
            (entry["feature"], entry["path"], entry["key"]): entry
            for entry in cls.entries
        }

    def test_inline_comment_does_not_hide_following_field(self):
        fields = GENERATOR.parse_struct_fields(
            "uint mode = 0; // explanation; (with parentheses)\n"
            "bool enabled = Runtime::IsEnabled() ? true : false;")
        self.assertEqual(fields["mode"], "uint")
        self.assertEqual(fields["enabled"], "bool")

    def test_multiline_and_cast_initializers_preserve_field_types(self):
        fields = GENERATOR.parse_struct_fields("""
            uint mode = (uint)Mode::Default;
            std::vector<int> values = {
                MakeValue(1),
                MakeValue(2)
            };
        """)
        self.assertEqual(fields["mode"], "uint")
        self.assertEqual(fields["values"], "std::vector<int>")

    def test_feature_matching_is_exact_and_inherited_fields_are_merged(self):
        feature_source = """
        struct VR : Feature {
            struct PerFrame { float strength = 1.0f; };
            struct Settings : PerFrame { bool enabled = true; } settings;
        };
        """
        prefixed_source = """
        struct VRStereoOptimizations {
            struct Settings { float wrong = 0.0f; } settings;
        };
        """
        with tempfile.TemporaryDirectory() as directory:
            feature_path = Path(directory) / "VR.h"
            prefixed_path = Path(directory) / "VRStereoOptimizations.h"
            feature_path.write_text(feature_source, encoding="utf-8")
            prefixed_path.write_text(prefixed_source, encoding="utf-8")
            features = {"VR": {"short": "VR", "name": "VR", "source": str(feature_path)}}
            paths = [feature_path, prefixed_path]
            fields = GENERATOR.collect_feature_struct_fields(paths, features)
            members = GENERATOR.collect_feature_member_fields(paths, features)
        self.assertEqual(fields["VR"]["Settings"], {
            "strength": "float",
            "enabled": "bool",
        })
        self.assertEqual(members["VR"]["settings"], "Settings")

    def test_feature_names_resolve_constants_wrappers_and_helpers(self):
        source = """
        struct Example : Feature {
            static constexpr std::string_view kShortName = "Example";
            static constexpr auto kDisplayName = "Example Feature";
            static std::string_view NameValue() { return kDisplayName; }
            std::string GetShortName() override { return std::string(kShortName); }
            std::string GetName() override { return std::string(NameValue()); }
        };
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Example.h"
            path.write_text(source, encoding="utf-8")
            features = GENERATOR.collect_features([path])
        self.assertEqual(features["Example"]["short"], "Example")
        self.assertEqual(features["Example"]["name"], "Example Feature")

    def test_catalog_validation_rejects_too_few_entries(self):
        with self.assertRaises(ValueError):
            GENERATOR.validate_entries([], 1)
        with self.assertRaises(ValueError):
            GENERATOR.validate_entries([{"feature": "One"}], 2)

    def test_i18n_keys_include_file_prefix(self):
        translated = GENERATOR.extract_i18n_call(
            'ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.strength)',
            "feature.example.")
        self.assertEqual(translated, ("feature.example.strength", "Strength"))

    def test_ui_labels_support_named_settings_objects(self):
        source = """
        #define I18N_KEY_PREFIX "feature.example."
        void Example::DrawSettings()
        {
            ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &bendSettings.Strength, 0.0f, 1.0f);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Example.cpp"
            path.write_text(source, encoding="utf-8")
            labels = GENERATOR.collect_ui_labels([path])
        self.assertEqual(
            labels[("Example", ("Strength",))],
            ("Strength", "", "feature.example.strength", "", "SliderFloat", 0.0, 1.0, 1.0))

    def test_diagnostic_text_does_not_replace_control_label(self):
        source = """
        #define I18N_KEY_PREFIX "feature.example."
        void Example::DrawSettings()
        {
            ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, 1.0f);
            ImGui::Text(T(TKEY("strength_debug"), "Strength: %.2f"), settings.Strength);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Example.cpp"
            path.write_text(source, encoding="utf-8")
            labels = GENERATOR.collect_ui_labels([path])
        self.assertEqual(
            labels[("Example", ("Strength",))],
            ("Strength", "", "feature.example.strength", "", "SliderFloat", 0.0, 1.0, 1.0))

    def test_checkbox_control_is_detected(self):
        control = GENERATOR.find_ui_control(
            'ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&settings.Enable)')
        self.assertIsNotNone(control)
        self.assertEqual(control[0], "Checkbox")

    def test_only_draw_settings_body_is_scanned(self):
        source = """
        #define I18N_KEY_PREFIX "feature.example."
        void Example::DrawSettings()
        {
            ImGui::SliderFloat(T(TKEY("inside"), "Inside"), &settings.Inside, 0.0f, 1.0f);
        }
        void Example::DrawDebug()
        {
            ImGui::SliderFloat(T(TKEY("outside"), "Outside"), &settings.Outside, 0.0f, 1.0f);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Example.cpp"
            path.write_text(source, encoding="utf-8")
            labels = GENERATOR.collect_ui_labels([path])
        self.assertIn(("Example", ("Inside",)), labels)
        self.assertNotIn(("Example", ("Outside",)), labels)

    def test_brace_scoped_categories_match_ibl(self):
        options = self.entries_by_id[("ImageBasedLighting", "", "DisableInInteriors")]
        dalc_amount = self.entries_by_id[("ImageBasedLighting", "", "DALCAmount")]
        self.assertEqual(options["displayPath"], "Enable IBL Options")
        self.assertEqual(dalc_amount["displayPath"], "")

    def test_brace_scoped_categories_match_sss(self):
        base = self.entries_by_id[("SubsurfaceScattering", "BaseProfile", "BlurRadius")]
        human = self.entries_by_id[("SubsurfaceScattering", "HumanProfile", "BlurRadius")]
        samples = self.entries_by_id[("SubsurfaceScattering", "", "BurleySamples")]
        self.assertEqual(base["displayPath"], "Base Profile")
        self.assertEqual(human["displayPath"], "Human Profile")
        self.assertEqual(samples["displayPath"], "Settings")

    def test_brace_scoped_categories_match_exponential_height_fog(self):
        root = self.entries_by_id[("ExponentialHeightFog", "", "fogDensity")]
        volumetric = self.entries_by_id[
            ("ExponentialHeightFog", "", "volumetricFogDistance")]
        debug = self.entries_by_id[
            ("ExponentialHeightFog", "", "volumetricGridSizeZ")]
        self.assertEqual(root["displayPath"], "")
        self.assertEqual(volumetric["displayPath"], "Volumetric Fog")
        self.assertEqual(debug["displayPath"], "Debug")
        self.assertIn("Hidden", debug["flags"])

    def test_safe_editor_metadata(self):
        ripple_lifetime = self.entries_by_id[
            ("WetnessEffects", "", "RippleLifetime")]
        resolution_mode = self.entries_by_id[
            ("ScreenSpaceGI", "", "ResolutionMode")]
        fog_density = self.entries_by_id[
            ("ExponentialHeightFog", "", "fogDensity")]
        self.assertIn("Hidden", ripple_lifetime["flags"])
        self.assertEqual(resolution_mode["editorSemantic"], "Choice")
        self.assertEqual([choice[0] for choice in resolution_mode["choices"]], [0, 1, 2])
        self.assertEqual((fog_density["minimum"], fog_density["maximum"]), (0.0, 1.0))

    def test_numeric_metadata_uses_raw_bounds_and_display_scale(self):
        percentage = self.entries_by_id[("ScreenSpaceGI", "", "GISaturation")]
        angle = self.entries_by_id[("Skylighting", "", "MaxZenith")]
        scalar = self.entries_by_id[("LightLimitFix", "", "ContactShadowMaxSteps")]

        self.assertEqual((percentage["minimum"], percentage["maximum"]), (0.0, 1.0))
        self.assertEqual(percentage["displayScale"], 100.0)
        self.assertAlmostEqual(angle["minimum"], 0.0)
        self.assertAlmostEqual(angle["maximum"], math.pi / 2.0)
        self.assertAlmostEqual(angle["displayScale"], 180.0 / math.pi)
        self.assertEqual((scalar["minimum"], scalar["maximum"]), (1.0, 16.0))
        self.assertEqual(scalar["displayScale"], 1.0)

    def test_known_parser_regressions_are_cataloged(self):
        discovered = {
            (entry["feature"], entry["path"], entry["key"])
            for entry in self.entries
        }
        self.assertTrue(KNOWN_PARSER_REGRESSION_SETTINGS <= discovered)
        GENERATOR.validate_entries(self.entries, 250)

    def test_directly_persisted_fields_are_cataloged_but_hidden(self):
        for key in ("EnableStereoSync", "UseStereoReproject"):
            entry = self.entries_by_id[("ScreenSpaceShadows", "", key)]
            self.assertEqual(entry["type"], "Boolean")
            self.assertEqual(entry["editorSemantic"], "None")
            self.assertIn("Hidden", entry["flags"])

    def test_generated_metadata_contains_display_scale(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            GENERATOR.write_catalog(self.entries, output)
            header = (output / "SceneSettingsCatalog.generated.h").read_text(encoding="utf-8")
            source = (output / "SceneSettingsCatalog.generated.cpp").read_text(encoding="utf-8")
        self.assertIn("double displayScale;", header)
        self.assertIn(repr(180.0 / math.pi), source)

    def test_feature_adapters_are_separate_from_catalog_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            GENERATOR.write_catalog(self.entries, output)
            source = (output / "SceneSettingsCatalog.generated.cpp").read_text(encoding="utf-8")
            adapters = (output / "FeatureSceneSettingsAdapters.generated.cpp").read_text(
                encoding="utf-8")
        self.assertNotIn('#include "Features/', source)
        self.assertNotIn("static_cast<", source)
        self.assertIn("RegisterControlResolver", adapters)

    def test_inverted_and_choice_exceptions_are_complete(self):
        for key in (
                "DisableInInteriors",
                "DisableInWorldMap",
                "DisableInLoadingScreen"):
            entry = self.entries_by_id[("ImageBasedLighting", "", key)]
            self.assertTrue(entry["invertedDisplay"])
            self.assertEqual(entry["editorSemantic"], "Toggle")

        expected_choices = {
            ("ScreenSpaceGI", "ResolutionMode"): [0, 1, 2],
            ("SubsurfaceScattering", "SSMode"): [0, 1],
            ("SubsurfaceScattering", "ScatterMode"): [0, 1, 2],
            ("ImageBasedLighting", "DALCMode"): [0, 1, 2, 3],
        }
        for (feature, key), expected_values in expected_choices.items():
            entry = self.entries_by_id[(feature, "", key)]
            self.assertEqual(entry["editorSemantic"], "Choice")
            self.assertEqual(
                [choice[0] for choice in entry["choices"]],
                expected_values)

    def test_cs_utility_exposes_only_safe_flat_numeric_controls(self):
        visible_numeric = [
            entry for entry in self.entries
            if entry["feature"] == "CSUtility" and
            entry["editorSemantic"] == "Numeric" and
            "Hidden" not in entry["flags"]
        ]
        self.assertEqual(len(visible_numeric), 8)
        sky = next(entry for entry in visible_numeric if entry["key"] == "skyBrightness")
        self.assertEqual((sky["minimum"], sky["maximum"]), (0.0, 2.0))
        multipliers = [entry for entry in visible_numeric if entry["key"] != "skyBrightness"]
        self.assertEqual(len(multipliers), 7)
        self.assertTrue(all(
            (entry["minimum"], entry["maximum"]) == (0.0, 5.0)
            for entry in multipliers))
        self.assertTrue(all(
            "Dof" not in entry["path"] and "dof" not in entry["path"]
            for entry in visible_numeric))

    def test_persisted_setting_without_supported_control_is_hidden(self):
        hidden_entries = [
            entry for entry in self.entries
            if entry["editorSemantic"] == "None"
        ]
        self.assertTrue(hidden_entries)
        self.assertTrue(all("Hidden" in entry["flags"] for entry in hidden_entries))

    def test_allow_and_deny_policy_are_explicit_and_exclusive(self):
        for entry in self.entries:
            allowed = "SceneControllable" in entry["flags"]
            denied = "Hidden" in entry["flags"]
            self.assertNotEqual(allowed, denied)
            self.assertEqual(allowed, entry["editorSemantic"] != "None" and
                             "debug" not in " ".join((
                                 entry["path"],
                                 entry["displayPath"],
                                 entry["key"])).lower())


if __name__ == "__main__":
    unittest.main()
