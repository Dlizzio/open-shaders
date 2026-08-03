import importlib.util
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
POLICY_PATH = ROOT / "src" / "SceneSettingsPolicy.h"
MANAGER_PATH = ROOT / "src" / "SceneSettingsManager.cpp"
GENERATOR_PATH = ROOT / "cmake" / "generate_scene_settings_catalog.py"

SPEC = importlib.util.spec_from_file_location("scene_catalog_generator", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GENERATOR)


def extract_block(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\{{(.*?)\n\t\}};", source, re.DOTALL)
    if not match:
        raise AssertionError(f"Could not find {name}")
    return match.group(1)


def extract_strings(source: str, name: str) -> list[str]:
    return re.findall(r'"([^"]+)"', extract_block(source, name))


class SceneSettingsPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = POLICY_PATH.read_text(encoding="utf-8")
        cls.manager = MANAGER_PATH.read_text(encoding="utf-8")
        cls.entries = GENERATOR.build_entries(ROOT)

    def test_policy_lists_are_present_and_unique(self):
        interior = extract_strings(self.policy, "kLocationFeatureWhitelist")
        time_of_day = extract_strings(self.policy, "kTimeOfDayFeatureWhitelist")
        blacklist_rows = re.findall(
            r"\{([^{}]+)\}", extract_block(self.policy, "kSettingBlacklist"))
        blacklist = [tuple(re.findall(r'"([^"]+)"', row)) for row in blacklist_rows]

        self.assertEqual(len(interior), 6)
        self.assertEqual(len(time_of_day), 8)
        self.assertEqual(len(blacklist), 10)
        self.assertEqual(len(interior), len(set(interior)))
        self.assertEqual(len(time_of_day), len(set(time_of_day)))
        self.assertEqual(len(blacklist), len(set(blacklist)))
        self.assertTrue(all(blacklist))

    def test_whitelisted_features_are_discovered(self):
        discovered = {entry["feature"] for entry in self.entries}
        for name in (
                extract_strings(self.policy, "kLocationFeatureWhitelist") +
                extract_strings(self.policy, "kTimeOfDayFeatureWhitelist")):
            self.assertIn(name, discovered)

    def test_manager_uses_every_policy_list(self):
        for name in (
                "kSettingBlacklist",
                "kLocationFeatureWhitelist",
                "kTimeOfDayFeatureWhitelist"):
            self.assertIn(f"SceneSettingsPolicy::{name}", self.manager)


if __name__ == "__main__":
    unittest.main()
