#include "Features/TerrainVariation/MeshRules.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <process.h>

using namespace TerrainVariationTextures;
namespace fs = std::filesystem;

namespace
{
	struct RulesDirectory
	{
		fs::path path;

		RulesDirectory()
		{
			static std::atomic<unsigned> counter{ 0 };
			path = fs::temp_directory_path() / std::format("terrain_rules_{}_{}_{}", ::_getpid(),
												   std::chrono::high_resolution_clock::now().time_since_epoch().count(), counter++);
			REQUIRE(fs::create_directory(path));
		}

		~RulesDirectory()
		{
			std::error_code error;
			fs::remove_all(path, error);
		}

		void Write(const fs::path& a_relativePath, std::string_view a_contents) const
		{
			const auto file = path / a_relativePath;
			fs::create_directories(file.parent_path());
			std::ofstream stream(file);
			stream << a_contents;
			REQUIRE(stream.good());
		}
	};
}

TEST_CASE("Terrain mesh rules combine shipped defaults and nested mod files", "[terrainvariation]")
{
	RulesDirectory directory;
	const auto repository = fs::path(__FILE__).parent_path().parent_path().parent_path();
	fs::copy_file(repository / "features/Terrain Variation/SKSE/Plugins/CommunityShaders/TerrainVariation/MeshRules/Default.json", directory.path / "Default.json");
	directory.Write("Mod/Patch.JSON", R"({"exclude":{"paths":["Data\\Textures\\Landscape\\Custom\\Roots.dds"],"directories":["Textures/LANDSCAPE/CustomPlants"]}})");
	directory.Write("Notes.txt", "This is not JSON");

	MeshRules rules;
	const auto result = rules.Load(directory.path);
	CHECK(result.loadedFiles == 2);
	CHECK(result.errors.empty());
	CHECK(rules.IsExcluded(CanonicaliseTexturePath("Data\\Textures\\Landscape\\DirtCliffs\\DirtCliffsRoots01.DDS")));
	CHECK(rules.IsExcluded("pbr/landscape/dirtcliffs/dirtcliffsroots01.dds"));
	CHECK(rules.IsExcluded("dirtcliffsroots01.dds"));
	CHECK(rules.IsExcluded("landscape/trees/pine/bark.dds"));
	CHECK(rules.IsExcluded("landscape/custom/roots.dds"));
	CHECK(rules.IsExcluded("landscape/customplants/fern.dds"));
	CHECK_FALSE(rules.IsExcluded("landscape/dirtcliffs/dirtcliffs01.dds"));
	CHECK_FALSE(rules.IsExcluded("landscape/fieldgrass02.dds"));
	CHECK_FALSE(rules.IsExcluded("landscape/trees2/bark.dds"));
	CHECK_FALSE(rules.IsExcluded("landscape/customplants2/fern.dds"));
	CHECK_FALSE(rules.IsExcluded("other/roots.dds"));
	CHECK_FALSE(rules.IsExcluded("landscape/custom/roots.dds.backup"));
	CHECK(rules.IsEligible("landscape/mountains/mountainslab01.dds", false));
	CHECK(rules.IsEligible("landscape/mountains/mountainslab02.dds", false));
	CHECK(rules.IsEligible("pbr/landscape/mountains/mountainslab01.dds", false));
	CHECK(rules.IsEligible("landscape/dirtcliffs/dirtcliffs01.dds", false));
	CHECK(rules.IsEligible("pbr/landscape/dirtcliffs/dirtcliffs01.dds", false));
	CHECK_FALSE(rules.IsEligible("landscape/dirtcliffs/dirtcliffsroots01.dds", false));
	CHECK_FALSE(rules.IsEligible("pbr/landscape/dirtcliffs/dirtcliffsroots01.dds", true));
	CHECK_FALSE(rules.IsEligible("landscape/trees/pine/bark.dds", true));
	CHECK_FALSE(rules.IsEligible("landscape/mountains2/mountainslab01.dds", false));
}

TEST_CASE("Terrain automatic mesh matching keeps the upstream subfolder restriction", "[terrainvariation]")
{
	MeshRules rules;
	CHECK(rules.IsEligible("landscape/fieldgrass02.dds", false));
	CHECK_FALSE(rules.IsEligible("landscape/mountains/mountainslab01.dds", false));
	CHECK_FALSE(rules.IsEligible("landscape/dirtcliffs/dirtcliffs01.dds", false));
	CHECK_FALSE(rules.IsEligible("pbr/landscape/mountains/mountainslab01.dds", false));
	CHECK(rules.IsEligible("landscape/mountains/mountainslab01.dds", true));
	CHECK(rules.IsEligible("custom/ground.dds", true));
	CHECK_FALSE(rules.IsEligible("custom/ground.dds", false));
}

TEST_CASE("Terrain whitelist accepts filenames and exact paths while blacklist wins", "[terrainvariation]")
{
	RulesDirectory directory;
	directory.Write("Whitelist.json", R"({"include":{"filenames":["CustomCliff.dds","Blocked.dds"],"paths":["Textures\\Custom\\Slab.dds"],"directories":["custom/rocks"]}})");
	directory.Write("Nested/Blacklist.json", R"({"exclude":{"filenames":["blocked.dds"],"paths":["custom/rocks/carving.dds"],"directories":["custom/forbidden"]}})");
	MeshRules rules;
	const auto result = rules.Load(directory.path);
	CHECK(result.loadedFiles == 2);
	CHECK(result.errors.empty());
	CHECK(rules.IsEligible("mod/customcliff.dds", false));
	CHECK(rules.IsEligible("custom/slab.dds", false));
	CHECK(rules.IsEligible("custom/rocks/granite.dds", false));
	CHECK_FALSE(rules.IsEligible("other/slab.dds", false));
	CHECK_FALSE(rules.IsEligible("custom/rocks2/granite.dds", false));
	CHECK_FALSE(rules.IsEligible("custom/blocked.dds", true));
	CHECK_FALSE(rules.IsEligible("custom/rocks/carving.dds", false));
	CHECK_FALSE(rules.IsEligible("custom/forbidden/customcliff.dds", true));
}

TEST_CASE("Terrain mesh rules reject invalid files atomically and keep valid files", "[terrainvariation]")
{
	RulesDirectory directory;
	directory.Write("Valid.json", R"({"exclude":{"filenames":["KeepExcluded.dds"]}})");
	const std::string_view invalidFiles[] = {
		"{",
		"[]",
		R"({"exclude":[]})",
		R"({"include":[]})",
		R"({"include":{"filenames":["partial.dds",5]}})",
		R"({"exclude":{"filenames":["partial.dds"]},"include":{"paths":5}})",
		R"({"include":{"filenames":["partial.dds"]},"unknown":{}})",
		R"({"exclude":{"filenames":["partial.dds",5]}})",
		R"({"exclude":{"filenames":["partial.dds"],"unknown":[]}})",
		R"({"exclude":{"directories":[""]}})",
		R"({"exclude":{"filenames":["folder/roots.dds"]}})",
		R"({"exclude":{"paths":["../roots.dds"]}})",
		R"({"exclude":{"paths":["C:/roots.dds"]}})",
		R"({"exclude":{"paths":["landscape/*.dds"]}})",
		R"({"exclude":{"paths":["landscape/root"]}})"
	};
	for (const auto invalid : invalidFiles) {
		INFO(invalid);
		directory.Write("Invalid.json", invalid);
		MeshRules rules;
		const auto result = rules.Load(directory.path);
		CHECK(result.loadedFiles == 1);
		REQUIRE(result.errors.size() == 1);
		CHECK(result.errors.front().find("Invalid.json") != std::string::npos);
		CHECK(rules.IsExcluded("landscape/keepexcluded.dds"));
		CHECK_FALSE(rules.IsExcluded("landscape/partial.dds"));
		CHECK_FALSE(rules.IsExcluded("landscape/dirtcliffs/dirtcliffs01.dds"));
		CHECK_FALSE(rules.IsEligible("custom/partial.dds", false));
	}
}

TEST_CASE("Terrain mesh rules replace old exclusions only when loaded again", "[terrainvariation]")
{
	RulesDirectory directory;
	directory.Write("Rules.json", R"({"include":{"filenames":["oldcliff.dds"]},"exclude":{"filenames":["first.dds"]}})");
	MeshRules rules;
	REQUIRE(rules.Load(directory.path).loadedFiles == 1);
	directory.Write("Rules.json", R"({"include":{"filenames":["newcliff.dds"]},"exclude":{"filenames":["second.dds"]}})");
	CHECK(rules.IsEligible("custom/oldcliff.dds", false));
	CHECK_FALSE(rules.IsEligible("custom/newcliff.dds", false));
	CHECK(rules.IsExcluded("first.dds"));
	CHECK_FALSE(rules.IsExcluded("second.dds"));
	REQUIRE(rules.Load(directory.path).loadedFiles == 1);
	CHECK_FALSE(rules.IsEligible("custom/oldcliff.dds", false));
	CHECK(rules.IsEligible("custom/newcliff.dds", false));
	CHECK_FALSE(rules.IsExcluded("first.dds"));
	CHECK(rules.IsExcluded("second.dds"));
	const auto missing = rules.Load(directory.path / "Missing");
	CHECK(missing.loadedFiles == 0);
	CHECK(missing.errors.empty());
	CHECK_FALSE(rules.IsExcluded("second.dds"));
	CHECK_FALSE(rules.IsEligible("custom/newcliff.dds", false));
}
