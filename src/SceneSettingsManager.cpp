#include "SceneSettingsManager.h"

#include "Feature.h"
#include "Globals.h"
#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsPolicy.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
	using SceneSettingControlType = SceneSettingsManager::SettingControlType;
	using ManagerAggregatePresentation = SceneSettingsManager::AggregatePresentation;
	using ManagerUnifiedEditMode = SceneSettingsManager::UnifiedEditMode;
	using ManagerSettingDescriptor = SceneSettingsManager::SettingDescriptor;

	void CombineHash(size_t& signature, size_t value)
	{
		signature ^= value + 0x9E3779B9u + (signature << 6) + (signature >> 2);
	}

	void HashSceneSettingValue(size_t& signature, const json& value)
	{
		CombineHash(signature, static_cast<size_t>(value.type()));
		if (value.is_boolean())
			CombineHash(signature, std::hash<bool>{}(value.get<bool>()));
		else if (value.is_number_unsigned())
			CombineHash(signature, std::hash<std::uint64_t>{}(value.get<std::uint64_t>()));
		else if (value.is_number_integer())
			CombineHash(signature, std::hash<std::int64_t>{}(value.get<std::int64_t>()));
		else if (value.is_number_float())
			CombineHash(signature, std::hash<double>{}(value.get<double>()));
		else if (value.is_string())
			CombineHash(signature, std::hash<std::string_view>{}(value.get_ref<const std::string&>()));
	}

	SceneSettingsManager* sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager::SceneSettingsManager()
{
	assert(!sceneSettingsManagerSingleton);
	sceneSettingsManagerSingleton = this;
}

SceneSettingsManager::~SceneSettingsManager()
{
	if (sceneSettingsManagerSingleton == this)
		sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager* SceneSettingsManager::GetSingleton()
{
	return sceneSettingsManagerSingleton;
}

namespace
{
	constexpr auto kOverwriteJsonIndent = 2;
	constexpr auto kMaxSceneOverwriteFileSize = 1024 * 1024;
	constexpr const char* kFeatureKey = "_feature";
	constexpr const char* kMetadataKey = "_metadata";
	constexpr const char* kMetadataDescriptionKey = "description";
	constexpr std::string_view kSceneSettingDisplaySeparator = " / ";
	constexpr std::string_view kImGuiIdSeparator = "##";

	bool IsSceneSettingPrimitive(const json& value)
	{
		return value.is_boolean() || value.is_number_integer() || value.is_number_float() || value.is_string();
	}

	bool IsEntryListSceneType(SceneSettingsManager::SceneType type)
	{
		return type == SceneSettingsManager::SceneType::InteriorOnly ||
		       type == SceneSettingsManager::SceneType::TimeOfDay;
	}

	bool IsValidLocationTargetType(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Region:
		case SceneSettingsManager::LocationTargetType::Location:
		case SceneSettingsManager::LocationTargetType::Cell:
			return true;
		default:
			return false;
		}
	}

	bool WriteJsonAtomically(const std::filesystem::path& path, const json& data, int indent,
		std::string_view context)
	{
		std::string serialized;
		try {
			serialized = data.dump(indent);
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Could not serialize {} '{}': {}", context, path.string(), e.what());
			return false;
		}

		std::error_code ec;
		if (!path.parent_path().empty()) {
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec) {
				logger::error("[SceneSettings] Could not create directory for {} '{}': {}",
					context, path.string(), ec.message());
				return false;
			}
		}

		auto temporaryPath = path;
		temporaryPath += std::format(".{}.tmp", ::GetCurrentProcessId());
		{
			std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				logger::error("[SceneSettings] Could not open temporary {} file '{}'", context, temporaryPath.string());
				return false;
			}
			file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
			file.flush();
			if (file.fail()) {
				logger::error("[SceneSettings] Could not write temporary {} file '{}'", context, temporaryPath.string());
				file.close();
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
			file.close();
			if (file.fail()) {
				logger::error("[SceneSettings] Could not close temporary {} file '{}'", context, temporaryPath.string());
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
		}

		if (!::MoveFileExW(temporaryPath.c_str(), path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			const auto error = ::GetLastError();
			logger::error("[SceneSettings] Could not replace {} '{}' (Win32 error {})",
				context, path.string(), error);
			std::filesystem::remove(temporaryPath, ec);
			return false;
		}
		return true;
	}

	std::string StripImGuiId(std::string_view label)
	{
		return std::string(label.substr(0, label.find(kImGuiIdSeparator)));
	}

	std::vector<std::filesystem::path> GetSortedDirectoryPaths(
		const std::filesystem::path& directory, bool directories, std::string_view context)
	{
		std::vector<std::filesystem::path> paths;
		std::error_code ec;
		std::filesystem::directory_iterator iterator(
			directory, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) {
			logger::error("[SceneSettings] Failed to enumerate {} '{}': {}", context, directory.string(), ec.message());
			return paths;
		}

		const std::filesystem::directory_iterator end;
		while (iterator != end) {
			const auto& entry = *iterator;
			std::error_code statusError;
			const bool matches = directories ? entry.is_directory(statusError) : entry.is_regular_file(statusError);
			if (statusError) {
				logger::warn("[SceneSettings] Could not inspect '{}': {}", entry.path().string(), statusError.message());
			} else if (matches) {
				paths.push_back(entry.path());
			}

			iterator.increment(ec);
			if (ec) {
				logger::error("[SceneSettings] Failed while enumerating {} '{}': {}", context, directory.string(), ec.message());
				break;
			}
		}

		std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.generic_string() < rhs.generic_string();
		});
		return paths;
	}

	std::vector<std::filesystem::path> GetSortedJsonFiles(
		const std::filesystem::path& directory, std::string_view context)
	{
		auto paths = GetSortedDirectoryPaths(directory, false, context);
		std::erase_if(paths, [](const auto& path) { return path.extension() != ".json"; });
		return paths;
	}

	std::string NormalizeLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.localFormId == 0)
			return std::string(formKey);
		if (components.pluginName.empty())
			return std::format("0x{:X}", components.localFormId);

		auto pluginName = components.pluginName;
		std::transform(pluginName.begin(), pluginName.end(), pluginName.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return std::format("0x{:X}~{}", components.localFormId, pluginName);
	}

	std::string CanonicalizeResolvedLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.pluginName.empty() || components.localFormId == 0)
			return std::string(formKey);
		if (!RE::TESDataHandler::GetSingleton())
			return std::string(formKey);
		const auto formId = Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? Util::FormIdToSpid(formId) : std::string(formKey);
	}

	bool ReadOptionalStringField(const json& object, std::string_view field, std::string& value,
		std::string_view context)
	{
		auto it = object.find(std::string(field));
		if (it == object.end())
			return true;
		if (!it->is_string()) {
			logger::warn("[SceneSettings] {} field '{}' must be a string", context, field);
			return false;
		}
		value = it->get<std::string>();
		return true;
	}

	bool IsSceneMetadataKey(std::string_view key)
	{
		return !key.empty() && key.front() == '_';
	}

	bool ReadBoundedSceneJson(const std::filesystem::path& path, json& data)
	{
		std::error_code ec;
		const auto fileSize = std::filesystem::file_size(path, ec);
		if (ec || fileSize > kMaxSceneOverwriteFileSize)
			return false;

		std::ifstream file(path);
		if (!file.is_open())
			return false;
		data = json::parse(file, nullptr, false);
		return data.is_object();
	}

	// TOD/weather can only interpolate float settings, not integer toggles or enum values.
	bool IsNumericValue(const json& value)
	{
		return value.is_number_float();
	}

	bool IsSceneSettingPathWrapper(std::string_view token)
	{
		return token == "settings";
	}

	std::string NormalizeSceneSettingAddressToken(std::string_view token)
	{
		auto normalized = token.find(' ') == std::string_view::npos ?
		                      Util::PrettifyIdentifier(token) :
		                      std::string(token);
		std::erase_if(normalized, [](unsigned char c) { return std::isspace(c); });
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return normalized;
	}

	bool SceneSettingAddressTokensEqual(std::string_view lhs, std::string_view rhs)
	{
		return NormalizeSceneSettingAddressToken(lhs) == NormalizeSceneSettingAddressToken(rhs);
	}

	bool IsSceneSettingPolicyPrefix(
		const std::vector<std::string>& address, const SceneSettingsPolicy::SettingPolicyPath& prefix)
	{
		if (prefix.size() > address.size())
			return false;

		for (size_t index = 0; index < prefix.size(); ++index)
			if (!SceneSettingAddressTokensEqual(address[index], prefix[index]))
				return false;
		return true;
	}

	bool MatchesSceneSettingPolicy(const std::vector<std::string>& address,
		const std::vector<SceneSettingsPolicy::SettingPolicyPath>& paths)
	{
		return std::any_of(paths.begin(), paths.end(),
			[&](const auto& prefix) { return IsSceneSettingPolicyPrefix(address, prefix); });
	}

	std::vector<std::string> GetSceneSettingAddress(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		std::vector<std::string> address{ featureShortName };
		address.reserve(settingPath.size() + 2);
		for (const auto& segment : settingPath)
			if (!IsSceneSettingPathWrapper(segment))
				address.push_back(segment);
		address.push_back(settingKey);
		return address;
	}

	bool IsBlacklistedSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kSettingBlacklist);
	}

	bool HasSceneOverwriteContent(const json& data)
	{
		if (!data.is_object())
			return false;

		for (const auto& [key, _] : data.items())
			if (!IsSceneMetadataKey(key))
				return true;
		return false;
	}

	bool IsCompatibleSceneSettingValue(const json& featureValue, const json& value)
	{
		if (featureValue.type() == value.type())
			return true;
		if (featureValue.is_number() && value.is_number())
			return true;
		return false;
	}

	std::string JoinDisplayParts(const std::vector<std::string>& parts, std::string_view leaf)
	{
		std::string displayName;
		for (const auto& part : parts) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += part;
		}
		if (!leaf.empty()) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += leaf;
		}
		return displayName;
	}

	std::vector<std::string> SplitCatalogPath(std::string_view path)
	{
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < path.size()) {
			auto end = path.find('/', start);
			auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (!part.empty()) {
				std::string decoded(part);
				for (size_t pos = 0; (pos = decoded.find('~', pos)) != std::string::npos;) {
					if (pos + 1 < decoded.size() && decoded[pos + 1] == '1')
						decoded.replace(pos, 2, "/");
					else if (pos + 1 < decoded.size() && decoded[pos + 1] == '0')
						decoded.replace(pos, 2, "~");
					++pos;
				}
				parts.push_back(std::move(decoded));
			}
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return parts;
	}

	std::string ToCatalogPath(const std::vector<std::string>& path)
	{
		std::string result;
		for (const auto& part : path) {
			if (!result.empty())
				result += '/';
			for (const char ch : part) {
				if (ch == '~')
					result += "~0";
				else if (ch == '/')
					result += "~1";
				else
					result += ch;
			}
		}
		return result;
	}

	bool IsStructuralDisplayPart(std::string_view part)
	{
		std::string normalized;
		normalized.reserve(part.size());
		for (const char ch : part)
			if (std::isalnum(static_cast<unsigned char>(ch)))
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		return normalized == "settings" || normalized == "values" || normalized == "baseline";
	}

	std::string NormalizeDisplayPart(std::string part)
	{
		part = StripImGuiId(part);
		if (!part.empty() && std::all_of(part.begin(), part.end(), [](const char ch) {
				return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
			}))
			part = Util::PrettifyIdentifier(part);
		return part;
	}

	std::vector<std::string> GetCatalogDisplayPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto keys = SplitCatalogPath(setting.displayPathKeys);
		for (size_t index = 0; index < parts.size(); ++index) {
			if (index < keys.size() && keys[index] != "-")
				parts[index] = T(keys[index], parts[index].c_str());
			parts[index] = NormalizeDisplayPart(std::move(parts[index]));
		}
		std::erase_if(parts, [](const auto& part) { return part.empty() || IsStructuralDisplayPart(part); });
		return parts;
	}

	std::vector<std::string> GetCatalogSelectorPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.selectorPath);
		auto keys = SplitCatalogPath(setting.selectorPathKeys);
		for (size_t i = 0; i < parts.size(); ++i) {
			if (i < keys.size() && keys[i] != "-")
				parts[i] = T(keys[i], parts[i].c_str());
			parts[i] = StripImGuiId(parts[i]);
		}
		return parts;
	}

	bool EqualDisplayText(std::string_view lhs, std::string_view rhs)
	{
		return std::ranges::equal(lhs, rhs, [](const char a, const char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	}

	std::vector<std::string> GetCatalogContextPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = GetCatalogDisplayPath(setting);
		auto selectorDefaults = GetCatalogSelectorPath(setting);
		auto rawParts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto rawKeys = SplitCatalogPath(setting.displayPathKeys);
		const auto settingParts = SplitCatalogPath(setting.settingPath);
		const bool hasSelector = !selectorDefaults.empty();
		size_t rawOffset = 0;
		for (auto& part : selectorDefaults)
			part = NormalizeDisplayPart(std::move(part));
		while (!parts.empty() && !selectorDefaults.empty() &&
			   EqualDisplayText(parts.front(), selectorDefaults.front())) {
			parts.erase(parts.begin());
			selectorDefaults.erase(selectorDefaults.begin());
			++rawOffset;
		}
		if (hasSelector) {
			while (rawOffset < rawParts.size() && IsStructuralDisplayPart(rawParts[rawOffset]))
				++rawOffset;
			if (!parts.empty() && rawOffset < rawParts.size() && rawOffset < settingParts.size()) {
				const bool translated = rawOffset < rawKeys.size() && rawKeys[rawOffset] != "-";
				auto rawPart = NormalizeDisplayPart(rawParts[rawOffset]);
				auto settingPart = NormalizeDisplayPart(settingParts[rawOffset]);
				if (!translated && EqualDisplayText(parts.front(), rawPart) &&
					EqualDisplayText(rawPart, settingPart))
					parts.erase(parts.begin());
			}
		}
		return parts;
	}

	std::string GetCatalogLeafDisplayName(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		if (setting.displayName.empty() && setting.displayNameKey.empty() &&
			setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice)
			return T("feature.scene_manager.selection", "Selection");

		auto displayName = StripImGuiId(setting.displayName.empty() ? setting.settingKey : setting.displayName);
		if (!setting.displayNameKey.empty())
			displayName = StripImGuiId(T(setting.displayNameKey, displayName.c_str()));
		return displayName;
	}

	double GetCatalogNumericDisplayScale(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return std::isfinite(setting.displayScale) && setting.displayScale > 0.0 ?
		           setting.displayScale :
		           1.0;
	}

	bool ConvertCatalogNumericStoredToDisplay(const SceneSettingsCatalog::SettingMetadata& setting,
		double storedValue, double& displayValue)
	{
		if (!std::isfinite(storedValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			displayValue = storedValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		double transformedValue = storedValue;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			if (storedValue <= 0.0)
				return false;
			transformedValue = std::log2(storedValue);
			break;
		default:
			return false;
		}

		displayValue = transformedValue * GetCatalogNumericDisplayScale(setting);
		return std::isfinite(displayValue);
	}

	bool ConvertCatalogNumericDisplayToStored(const SceneSettingsCatalog::SettingMetadata& setting,
		double displayValue, double& storedValue)
	{
		if (!std::isfinite(displayValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			storedValue = displayValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		const double transformedValue = displayValue / GetCatalogNumericDisplayScale(setting);
		if (!std::isfinite(transformedValue))
			return false;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			storedValue = transformedValue;
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			storedValue = std::exp2(transformedValue);
			break;
		default:
			return false;
		}
		return std::isfinite(storedValue) &&
		       (setting.numericTransform != SceneSettingsCatalog::NumericTransform::Log2 || storedValue > 0.0);
	}

	const SceneSettingsCatalog::SettingMetadata* FindStoredAllComponent(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto storedAllComponents = [] {
			using AggregateKey = std::tuple<std::string_view, std::string_view, std::string_view,
				SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;
			const auto makeKey = [](const auto& candidate) {
				return AggregateKey{ candidate.featureShortName, candidate.serializedPath,
					candidate.serializedKey, candidate.aggregateSemantic,
					candidate.aggregateStart, candidate.aggregateCount };
			};
			std::map<AggregateKey, const SceneSettingsCatalog::SettingMetadata*> storedAll;
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				if (candidate.aggregateAll)
					storedAll.try_emplace(makeKey(candidate), &candidate);
			std::vector<const SceneSettingsCatalog::SettingMetadata*> components(
				SceneSettingsCatalog::GetSettings().size(), nullptr);
			for (size_t index = 0; index < SceneSettingsCatalog::GetSettings().size(); ++index) {
				const auto& source = SceneSettingsCatalog::GetSettings()[index];
				if (auto component = storedAll.find(makeKey(source)); component != storedAll.end())
					components[index] = component->second;
			}
			return components;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < storedAllComponents.size());
		return index < storedAllComponents.size() ? storedAllComponents[index] : nullptr;
	}

	SceneSettingControlType GetCatalogControlType(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		using enum SceneSettingsCatalog::AggregateSemantic;
		switch (setting.aggregateSemantic) {
		case Color:
			return FindStoredAllComponent(setting) ?
			           SceneSettingControlType::Numeric :
			           SceneSettingControlType::Color;
		case Numeric:
			return SceneSettingControlType::Numeric;
		default:
			return SceneSettingControlType::Scalar;
		}
	}

	std::string GetSettingComponentName(SceneSettingControlType type, std::int8_t componentIndex)
	{
		if (componentIndex < 0 || componentIndex > 3)
			return {};
		if (type == SceneSettingControlType::Color) {
			switch (componentIndex) {
			case 0:
				return T("feature.scene_manager.channel.red", "R");
			case 1:
				return T("feature.scene_manager.channel.green", "G");
			case 2:
				return T("feature.scene_manager.channel.blue", "B");
			default:
				return T("feature.scene_manager.channel.alpha", "A");
			}
		}
		switch (componentIndex) {
		case 0:
			return T("feature.scene_manager.channel.x", "X");
		case 1:
			return T("feature.scene_manager.channel.y", "Y");
		case 2:
			return T("feature.scene_manager.channel.z", "Z");
		default:
			return T("feature.scene_manager.channel.w", "W");
		}
	}

	std::string GetCatalogComponentDisplayName(
		const SceneSettingsCatalog::SettingMetadata& setting, SceneSettingControlType controlType)
	{
		auto displayName = StripImGuiId(setting.componentDisplayName);
		if (!setting.componentDisplayNameKey.empty())
			displayName = StripImGuiId(T(setting.componentDisplayNameKey, displayName.c_str()));
		if (!displayName.empty())
			return displayName;
		if (setting.aggregateAll)
			return T("feature.scene_manager.channel.all", "All");

		auto componentIndex = static_cast<std::int8_t>(setting.aggregateCount > 1 ?
														   setting.serializedComponent - setting.aggregateStart :
														   setting.serializedComponent);
		const auto* storedAll = FindStoredAllComponent(setting);
		if (storedAll && storedAll->serializedComponent < setting.serializedComponent)
			--componentIndex;
		const auto componentType = setting.aggregateSemantic == SceneSettingsCatalog::AggregateSemantic::Color ?
		                               SceneSettingControlType::Color :
		                               controlType;
		return GetSettingComponentName(componentType, componentIndex);
	}

	SceneSettingsManager::SettingControlInfo MakeSettingControlInfo(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		SceneSettingsManager::SettingControlInfo info;
		info.controlType = GetCatalogControlType(setting);
		info.settingPath = info.controlType == SceneSettingControlType::Scalar ?
		                       SplitCatalogPath(setting.settingPath) :
		                       SplitCatalogPath(setting.serializedPath);
		info.settingKey = std::string(info.controlType == SceneSettingControlType::Scalar ?
										  setting.settingKey :
										  setting.serializedKey);
		info.displayName = GetCatalogLeafDisplayName(setting);
		info.componentDisplayName = GetCatalogComponentDisplayName(setting, info.controlType);
		info.displayPath = GetCatalogContextPath(setting);
		info.componentIndex = setting.serializedComponent;
		info.aggregateAll = setting.aggregateAll;
		if (info.controlType != SceneSettingControlType::Scalar) {
			info.componentStart = setting.aggregateStart;
			info.componentCount = setting.aggregateCount;
			info.aggregatePresentation =
				info.controlType == SceneSettingControlType::Color &&
						setting.aggregatePresentation == SceneSettingsCatalog::AggregatePresentation::ColorPicker ?
					ManagerAggregatePresentation::ColorPicker :
					ManagerAggregatePresentation::Components;
			switch (setting.unifiedEditMode) {
			case SceneSettingsCatalog::UnifiedEditMode::Always:
				info.unifiedEditMode = ManagerUnifiedEditMode::Always;
				break;
			case SceneSettingsCatalog::UnifiedEditMode::Shift:
				info.unifiedEditMode = ManagerUnifiedEditMode::Shift;
				break;
			default:
				info.unifiedEditMode = ManagerUnifiedEditMode::None;
				break;
			}
		}
		return info;
	}

	bool IsCatalogValueCompatible(const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		using enum SceneSettingsCatalog::ValueType;
		switch (setting.valueType) {
		case Boolean:
			return value.is_boolean();
		case Integer:
			return value.is_number_integer();
		case Float:
			return value.is_number_float() || value.is_number_integer();
		case String:
			return value.is_string();
		default:
			return false;
		}
	}

	bool IsSameSetting(const SceneSettingsManager::SettingEntry& entry, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return entry.featureShortName == featureShortName &&
		       entry.settingPath == settingPath &&
		       entry.settingKey == settingKey;
	}

	std::string GetSettingLogName(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return JoinDisplayParts(settingPath, std::format("{}.{}", featureShortName, settingKey));
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path, bool create)
	{
		json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object()) {
				return nullptr;
			}

			auto it = node->find(segment);
			if (it == node->end()) {
				if (!create)
					return nullptr;
				it = node->emplace(segment, json::object()).first;
			}
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	bool RemoveObjectValueAtPath(json& data, const std::vector<std::string>& path,
		size_t pathIndex, const std::string& settingKey)
	{
		if (!data.is_object())
			return false;
		if (pathIndex == path.size())
			return data.erase(settingKey) == 1;

		auto childIt = data.find(path[pathIndex]);
		if (childIt == data.end() || !childIt->is_object() ||
			!RemoveObjectValueAtPath(*childIt, path, pathIndex + 1, settingKey))
			return false;
		if (childIt->empty())
			data.erase(childIt);
		return true;
	}

	const json* GetObjectAtPath(const json& data, const std::vector<std::string>& path)
	{
		const json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object())
				return nullptr;
			auto it = node->find(segment);
			if (it == node->end())
				return nullptr;
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path)
	{
		return const_cast<json*>(GetObjectAtPath(std::as_const(data), path));
	}

	bool ParseCatalogArrayIndex(std::string_view value, size_t& index)
	{
		const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
		return result.ec == std::errc{} && result.ptr == value.data() + value.size();
	}

	template <class Json>
	Json* GetCatalogNodeAtPath(Json& data, const std::vector<std::string>& path)
	{
		auto* node = &data;
		for (const auto& segment : path) {
			if (node->is_object()) {
				auto it = node->find(segment);
				if (it == node->end())
					return nullptr;
				node = &*it;
				continue;
			}

			size_t index = 0;
			if (!node->is_array() || !ParseCatalogArrayIndex(segment, index) || index >= node->size())
				return nullptr;
			node = &(*node)[index];
		}
		return node;
	}

	template <class Json>
	Json* GetCatalogSerializedValue(Json& data, const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto* parent = GetCatalogNodeAtPath(data, SplitCatalogPath(setting.serializedPath));
		if (!parent)
			return nullptr;

		Json* value = nullptr;
		if (parent->is_object()) {
			auto valueIt = parent->find(setting.serializedKey);
			if (valueIt == parent->end())
				return nullptr;
			value = &*valueIt;
		} else {
			size_t index = 0;
			if (!parent->is_array() || !ParseCatalogArrayIndex(setting.serializedKey, index) ||
				index >= parent->size())
				return nullptr;
			value = &(*parent)[index];
		}

		if (setting.serializedComponent < 0)
			return value;
		const auto component = static_cast<size_t>(setting.serializedComponent);
		if (!value->is_array() || component >= value->size())
			return nullptr;
		return &(*value)[component];
	}

	void CollectOverwriteEntries(const json& data, const std::vector<std::string>& settingPath,
		const std::function<void(const std::vector<std::string>&, const std::string&, const json&)>& callback)
	{
		if (!data.is_object())
			return;

		for (const auto& [key, value] : data.items()) {
			if (IsSceneMetadataKey(key))
				continue;
			if (IsSceneSettingPrimitive(value)) {
				callback(settingPath, key, value);

				continue;
			}
			if (!value.is_object())
				continue;

			auto childPath = settingPath;
			childPath.push_back(key);
			CollectOverwriteEntries(value, childPath, callback);
		}
	}

	bool PolicyContainsFeature(const std::vector<SceneSettingsPolicy::SettingPolicyPath>& paths,
		std::string_view featureShortName)
	{
		return std::any_of(paths.begin(), paths.end(), [&](const auto& prefix) {
			return !prefix.empty() && SceneSettingAddressTokensEqual(prefix.front(), featureShortName);
		});
	}

	bool IsInteriorOnlyFeatureAllowed(std::string_view featureShortName)
	{
		return PolicyContainsFeature(SceneSettingsPolicy::kLocationFeatureWhitelist, featureShortName);
	}

	bool IsTimeOfDayFeatureAllowed(std::string_view featureShortName)
	{
		return PolicyContainsFeature(SceneSettingsPolicy::kTimeOfDayFeatureWhitelist, featureShortName);
	}

	bool IsSettingAllowedBySceneTypePolicy(SceneSettingsManager::SceneType type,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey)
	{
		const auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		switch (type) {
		case SceneSettingsManager::SceneType::InteriorOnly:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kLocationFeatureWhitelist);
		case SceneSettingsManager::SceneType::TimeOfDay:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kTimeOfDayFeatureWhitelist);
		case SceneSettingsManager::SceneType::Location:
			return MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kLocationFeatureWhitelist) ||
			       MatchesSceneSettingPolicy(address, SceneSettingsPolicy::kTimeOfDayFeatureWhitelist);
		default:
			return false;
		}
	}

	bool ComputeCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SceneSettingsCatalog::IsSceneControllable(setting) &&
		       !IsBlacklistedSceneSetting(
				   std::string(setting.featureShortName), SplitCatalogPath(setting.settingPath),
				   std::string(setting.settingKey));
	}

	bool IsCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto allowedSettings = [] {
			std::vector<uint8_t> allowed;
			allowed.reserve(SceneSettingsCatalog::GetSettings().size());
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				allowed.push_back(ComputeCatalogSettingAllowedByPolicy(candidate) ? 1 : 0);
			return allowed;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < allowedSettings.size());
		return index < allowedSettings.size() && allowedSettings[index] != 0;
	}

	bool IsCatalogSettingAllowedForSceneType(SceneSettingsManager::SceneType type,
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		constexpr size_t sceneTypeCount = 3;
		const auto typeIndex = static_cast<size_t>(type);
		if (typeIndex >= sceneTypeCount)
			return false;

		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto allowedSettings = [] {
			std::array<std::vector<uint8_t>, sceneTypeCount> allowedByType;
			for (size_t index = 0; index < sceneTypeCount; ++index) {
				const auto sceneType = static_cast<SceneSettingsManager::SceneType>(index);
				auto& allowed = allowedByType[index];
				allowed.reserve(SceneSettingsCatalog::GetSettings().size());
				for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
					const bool transitionable = sceneType != SceneSettingsManager::SceneType::TimeOfDay ||
					                            SceneSettingsCatalog::HasFlag(
													candidate.flags, SceneSettingsCatalog::SettingFlag::Transitionable);
					allowed.push_back(IsCatalogSettingAllowedByPolicy(candidate) && transitionable &&
											  IsSettingAllowedBySceneTypePolicy(sceneType,
												  std::string(candidate.featureShortName), SplitCatalogPath(candidate.settingPath),
												  std::string(candidate.settingKey)) ?
										  1 :
										  0);
				}
			}
			return allowedByType;
		}();
		const auto settingIndex = static_cast<size_t>(&setting - settings.data());
		assert(settingIndex < allowedSettings[typeIndex].size());
		return settingIndex < allowedSettings[typeIndex].size() &&
		       allowedSettings[typeIndex][settingIndex] != 0;
	}

	std::span<const SceneSettingsCatalog::SettingMetadata> GetCatalogFeatureSettings(
		std::string_view featureShortName)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto featureRanges = [] {
			std::map<std::string_view, std::pair<size_t, size_t>> ranges;
			const auto allSettings = SceneSettingsCatalog::GetSettings();
			for (size_t index = 0; index < allSettings.size();) {
				const auto name = allSettings[index].featureShortName;
				size_t end = index + 1;
				while (end < allSettings.size() && allSettings[end].featureShortName == name)
					++end;
				ranges.emplace(name, std::pair{ index, end });
				index = end;
			}
			return ranges;
		}();
		auto rangeIt = featureRanges.find(featureShortName);
		if (rangeIt == featureRanges.end())
			return {};
		const auto [begin, end] = rangeIt->second;
		return settings.subspan(begin, end - begin);
	}

	const SceneSettingsCatalog::SettingMetadata* FindAllowedCatalogSetting(
		std::string_view featureShortName, const std::vector<std::string>& settingPath,
		std::string_view settingKey, bool requireTransitionable = false)
	{
		auto* setting = SceneSettingsCatalog::FindSetting(
			featureShortName, ToCatalogPath(settingPath), settingKey);
		if (!setting || !IsCatalogSettingAllowedByPolicy(*setting))
			return nullptr;
		if (requireTransitionable &&
			!SceneSettingsCatalog::HasFlag(setting->flags, SceneSettingsCatalog::SettingFlag::Transitionable))
			return nullptr;
		return setting;
	}

	bool CatalogHasSceneSettings(
		std::string_view featureShortName, SceneSettingsManager::SceneType type)
	{
		for (const auto& setting : GetCatalogFeatureSettings(featureShortName)) {
			if (IsCatalogSettingAllowedForSceneType(type, setting))
				return true;
		}
		return false;
	}

	std::vector<std::string> GetLoadedCatalogFeatureNames(SceneSettingsManager::SceneType type)
	{
		auto names = Feature::GetLoadedFeatureNames();
		std::erase_if(names, [&](const auto& name) { return !CatalogHasSceneSettings(name, type); });
		return names;
	}
}

size_t SceneSettingsManager::GetCatalogUpdateSignature(std::string_view featureShortName,
	std::span<const CatalogSceneSettingUpdate> updates)
{
	size_t signature = std::hash<std::string_view>{}(featureShortName);
	for (const auto& update : updates) {
		for (const auto& segment : update.settingPath)
			CombineHash(signature, std::hash<std::string_view>{}(segment));
		CombineHash(signature, std::hash<std::string_view>{}(update.key));
	}
	return signature;
}

bool SceneSettingsManager::ApplyCatalogSceneSettings(
	Feature& feature, const std::vector<CatalogSceneSettingUpdate>& updates)
{
	if (updates.empty())
		return true;

	const auto featureShortName = feature.GetShortName();
	auto documentIt = featureApplyDocuments.find(featureShortName);
	if (documentIt == featureApplyDocuments.end()) {
		json settingsDocument;
		try {
			feature.SaveSettings(settingsDocument);
		} catch (const std::exception& e) {
			logger::warn("[SceneSettings] Failed to snapshot settings for {}: {}", featureShortName, e.what());
			return false;
		} catch (...) {
			logger::warn("[SceneSettings] Failed to snapshot settings for {}", featureShortName);
			return false;
		}
		if (!settingsDocument.is_object())
			return false;
		documentIt = featureApplyDocuments.emplace(featureShortName, std::move(settingsDocument)).first;
	}

	auto& settingsDocument = documentIt->second;
	std::vector<const SceneSettingsCatalog::SettingMetadata*> catalogSettings;
	std::vector<json> originalValues;
	catalogSettings.reserve(updates.size());
	originalValues.reserve(updates.size());
	try {
		for (const auto& update : updates) {
			auto* setting = FindAllowedCatalogSetting(
				featureShortName, update.settingPath, update.key);
			auto* currentValue = setting ? GetCatalogSerializedValue(settingsDocument, *setting) : nullptr;
			if (!currentValue || !IsSceneSettingPrimitive(*currentValue) ||
				!IsSceneSettingPrimitive(update.value) ||
				!IsCompatibleSceneSettingValue(*currentValue, update.value))
				return false;
			catalogSettings.push_back(setting);
			originalValues.push_back(*currentValue);
		}
		for (size_t index = 0; index < updates.size(); ++index)
			*GetCatalogSerializedValue(settingsDocument, *catalogSettings[index]) = updates[index].value;
	} catch (const std::exception& e) {
		featureApplyDocuments.erase(featureShortName);
		logger::warn("[SceneSettings] Failed to prepare settings for {}: {}", featureShortName, e.what());
		return false;
	} catch (...) {
		featureApplyDocuments.erase(featureShortName);
		logger::warn("[SceneSettings] Failed to prepare settings for {}", featureShortName);
		return false;
	}

	try {
		feature.LoadSettings(settingsDocument);
		return true;
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Failed to apply settings for {}: {}", featureShortName, e.what());
	} catch (...) {
		logger::warn("[SceneSettings] Failed to apply settings for {}", featureShortName);
	}

	try {
		for (size_t index = updates.size(); index-- > 0;)
			*GetCatalogSerializedValue(settingsDocument, *catalogSettings[index]) = std::move(originalValues[index]);
		feature.LoadSettings(settingsDocument);
	} catch (...) {
		featureApplyDocuments.erase(featureShortName);
		logger::error("[SceneSettings] Failed to restore {} after an apply error", featureShortName);
	}
	return false;
}

void SceneSettingsManager::ScheduleApplyVerification(std::string_view featureShortName,
	const std::vector<CatalogSceneSettingUpdate>& updates, size_t signature, bool transition)
{
	pendingApplyVerifications[std::string(featureShortName)] = {
		.appliedFrame = lastUpdateFrame,
		.updates = updates,
		.signature = signature,
		.transition = transition,
	};
}

void SceneSettingsManager::VerifyPendingApplies()
{
	for (auto verificationIt = pendingApplyVerifications.begin();
		verificationIt != pendingApplyVerifications.end();) {
		auto& [featureShortName, verification] = *verificationIt;
		if (verification.appliedFrame == lastUpdateFrame) {
			++verificationIt;
			continue;
		}

		bool verified = false;
		json actualSettings;
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		try {
			if (feature)
				feature->SaveSettings(actualSettings);
			if (feature && actualSettings.is_object()) {
				verified = std::all_of(verification.updates.begin(), verification.updates.end(),
					[&](const auto& update) {
						auto* setting = FindAllowedCatalogSetting(
							featureShortName, update.settingPath, update.key);
						const auto* actual = setting ? GetCatalogSerializedValue(actualSettings, *setting) : nullptr;
						return actual && ResolvedValuesEqual(*actual, update.value);
					});
			}
		} catch (...) {
			verified = false;
		}

		if (!verified) {
			logger::warn("[SceneSettings] {} did not retain settings after reporting a successful apply",
				featureShortName);
			featureApplyDocuments.erase(featureShortName);
			for (const auto& update : verification.updates)
				appliedSettings.erase({ featureShortName, update.settingPath, update.key });
			if (std::none_of(appliedSettings.begin(), appliedSettings.end(), [&](const auto& item) {
					return item.first.featureShortName == featureShortName;
				}))
				appliedFeatureNames.erase(featureShortName);
			auto& failureMap = verification.transition ? transitionApplyFailures : applyFailures;
			auto& failure = failureMap[featureShortName];
			failure.signature = verification.signature;
			failure.retryAfter = std::chrono::steady_clock::now() + kApplyRetryDelay;
			failure.warningLogged = true;
			resolverDirty = true;
		}
		verificationIt = pendingApplyVerifications.erase(verificationIt);
	}
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry);
static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey);

static bool HasOverwriteEntryForPeriod(const std::vector<SceneSettingsManager::SettingEntry>& entries,
	const SceneSettingsManager::SettingEntry& candidate)
{
	return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
		return entry.source == SceneSettingsManager::EntrySource::Overwrite &&
		       entry.period == candidate.period &&
		       IsSameSetting(entry, candidate.featureShortName, candidate.settingPath, candidate.settingKey);
	});
}

static bool AddOverwriteEntryIfUnique(std::vector<SceneSettingsManager::SettingEntry>& entries,
	SceneSettingsManager::SettingEntry&& entry, std::string_view context)
{
	// Files are scanned lexicographically. The first overwrite for an address and period wins.
	if (HasOverwriteEntryForPeriod(entries, entry)) {
		logger::warn("[SceneSettings] Duplicate {} overwrite for {}.{} ({}) skipped",
			context, entry.featureShortName, entry.settingKey, entry.sourceFilename);
		return false;
	}

	entries.push_back(std::move(entry));
	return true;
}

// --- Path Resolution ---

std::string SceneSettingsManager::GetSceneTypeName(SceneType type)
{
	switch (type) {
	case SceneType::InteriorOnly:
		return "InteriorOnly";
	case SceneType::TimeOfDay:
		return "TimeOfDay";
	case SceneType::Location:
		return "Location";
	default:
		return "Unknown";
	}
}

std::filesystem::path SceneSettingsManager::GetUserSettingsFilePath()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "SceneManager.json";
}

std::filesystem::path SceneSettingsManager::GetOverwritesPath(SceneType type)
{
	return Util::PathHelpers::GetSceneSettingsPath() / GetSceneTypeName(type);
}

std::filesystem::path SceneSettingsManager::GetWeatherOverwritesDir()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "Weather";
}

std::filesystem::path SceneSettingsManager::GetLocationOverwritesDir(LocationTargetType type)
{
	(void)type;
	return Util::PathHelpers::GetSceneSettingsPath() / "Locations";
}

// --- Time of Day Period Helpers ---

const char* SceneSettingsManager::GetPeriodName(TimeOfDayPeriod period)
{
	int idx = static_cast<int>(period);
	return (idx >= 0 && idx < kPeriodCount) ? kPeriodNames[idx] : "Unknown";
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetPeriodFromName(const std::string& name)
{
	for (int i = 0; i < kPeriodCount; ++i) {
		if (name == GetPeriodName(static_cast<TimeOfDayPeriod>(i)))
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Count;
}

float SceneSettingsManager::GetCurrentGameHour()
{
	// Prefer calendar (ground truth), which the Weather Editor slider writes to.
	// sky->currentGameHour may lag when timeScale is 0 (time paused).
	auto calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	float hour = 12.0f;
	if (calendar && calendar->gameHour)
		hour = calendar->gameHour->value;
	else if (auto sky = globals::game::sky)
		hour = sky->currentGameHour;
	if (!std::isfinite(hour))
		hour = 12.0f;

	// Normalize into [0, 24) so midnight is 0 and never 24.
	hour = std::clamp(hour, 0.0f, 24.0f);
	if (hour >= 24.0f)
		hour = 0.0f;
	return hour;
}

void SceneSettingsManager::GetTimeOfDayFactors(float outFactors[kPeriodCount])
{
	for (int i = 0; i < kPeriodCount; ++i)
		outFactors[i] = 0.0f;

	float hour = GetCurrentGameHour();

	// Normalize to [0, 24) - Night wraps, so also check hour + 24 for pre-dawn hours
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;

		if (h >= start && h < end) {
			// Inside this period - check if we're in the blend-out zone near the end.
			float distFromEnd = end - h;

			if (distFromEnd < kTransitionHours) {
				// Blending out to next period
				float t = distFromEnd / kTransitionHours;
				outFactors[i] = t;
				outFactors[(i + 1) % kPeriodCount] = 1.0f - t;
			} else {
				outFactors[i] = 1.0f;
			}
			return;
		}
	}

	// Fallback: noon = Day
	outFactors[static_cast<int>(TimeOfDayPeriod::Day)] = 1.0f;
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetCurrentPeriod()
{
	float hour = GetCurrentGameHour();
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;
		if (h >= start && h < end)
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Day;
}

// --- Feature Metadata ---

bool SceneSettingsManager::IsFeatureAllowedForType(SceneType type, const std::string& featureShortName)
{
	if (!Feature::FindFeatureByShortName(featureShortName))
		return false;

	switch (type) {
	case SceneType::InteriorOnly:
		return IsInteriorOnlyFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, type);
	case SceneType::TimeOfDay:
		return IsTimeOfDayFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, type);
	case SceneType::Location:
		return (IsInteriorOnlyFeatureAllowed(featureShortName) ||
				   IsTimeOfDayFeatureAllowed(featureShortName)) &&
		       CatalogHasSceneSettings(featureShortName, type);
	default:
		return false;
	}
}

bool SceneSettingsManager::IsSettingAllowedForType(SceneType type,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	return Feature::FindFeatureByShortName(featureShortName) && setting &&
	       IsCatalogSettingAllowedForSceneType(type, *setting);
}

bool SceneSettingsManager::IsSceneSettingAllowed(
	std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey)
{
	auto* setting = SceneSettingsCatalog::FindSetting(featureShortName, settingPath, settingKey);
	return setting && IsCatalogSettingAllowedByPolicy(*setting);
}

std::vector<std::string> SceneSettingsManager::GetInteriorRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::InteriorOnly);
}

std::vector<std::string> SceneSettingsManager::GetExteriorRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::TimeOfDay);
}

std::vector<std::string> SceneSettingsManager::GetLocationRelevantFeatureNames()
{
	return GetLoadedCatalogFeatureNames(SceneType::Location);
}

std::string SceneSettingsManager::GetFeatureDisplayName(const std::string& featureShortName)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	return feature ? feature->GetDisplayName() : featureShortName;
}

namespace
{
	std::string GetDescriptorLabel(const SceneSettingsManager::SettingControlInfo& info,
		std::string_view component = {})
	{
		std::string leaf = info.displayName;
		if (!component.empty())
			leaf += std::format(" ({})", component);
		if (info.displayPath.empty())
			return leaf;
		return std::format("{}: {}", JoinDisplayParts(info.displayPath, {}), leaf);
	}

	ManagerSettingDescriptor MakeScalarDescriptor(
		const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		auto info = MakeSettingControlInfo(setting);
		const auto physicalPath = SplitCatalogPath(setting.settingPath);
		const auto physicalKey = std::string(setting.settingKey);
		const auto component = info.controlType == SceneSettingControlType::Scalar ?
		                           std::string() :
		                           info.componentDisplayName;
		return {
			.settingPath = physicalPath,
			.key = physicalKey,
			.displayName = GetDescriptorLabel(info, component),
			.displayPath = GetCatalogSelectorPath(setting),
			.value = value,
			.controlType = SceneSettingControlType::Scalar,
			.aggregatePresentation = ManagerAggregatePresentation::Components,
			.unifiedEditMode = ManagerUnifiedEditMode::None,
			.members = { { physicalPath, physicalKey, info.componentDisplayName, value,
				setting.serializedComponent, info.aggregateAll } },
		};
	}

	using DescriptorGroupKey = std::tuple<std::string, std::string, std::int8_t, std::uint8_t, SceneSettingControlType>;

	std::vector<ManagerSettingDescriptor> CollectFeatureSceneSettings(
		const std::string& featureShortName, SceneSettingsManager::SceneType type,
		const json& featureSettings)
	{
		if (!featureSettings.is_object())
			return {};

		const bool transitionableOnly = type == SceneSettingsManager::SceneType::TimeOfDay;
		std::vector<ManagerSettingDescriptor> descriptors;
		std::map<DescriptorGroupKey, ManagerSettingDescriptor> groups;
		for (const auto& setting : GetCatalogFeatureSettings(featureShortName)) {
			if (!IsCatalogSettingAllowedForSceneType(type, setting))
				continue;
			auto settingPath = SplitCatalogPath(setting.settingPath);

			if (setting.settingKey.empty())
				continue;

			const auto* value = GetCatalogSerializedValue(featureSettings, setting);
			if (!value || !IsSceneSettingPrimitive(*value) ||
				!IsCatalogValueCompatible(setting, *value) ||
				(transitionableOnly && !IsNumericValue(*value)))
				continue;

			auto info = MakeSettingControlInfo(setting);
			if (info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2) {
				descriptors.push_back(MakeScalarDescriptor(setting, *value));
				continue;
			}

			DescriptorGroupKey key{
				std::string(setting.serializedPath), std::string(setting.serializedKey),
				info.componentStart, info.componentCount, info.controlType
			};
			auto [groupIt, inserted] = groups.try_emplace(key);
			auto& descriptor = groupIt->second;
			if (inserted) {
				descriptor.settingPath = settingPath;
				descriptor.key = std::string(setting.settingKey);
				descriptor.displayName = GetDescriptorLabel(info);
				descriptor.displayPath = GetCatalogSelectorPath(setting);
				descriptor.value = *value;
				descriptor.controlType = info.controlType;
				descriptor.aggregatePresentation = info.aggregatePresentation;
				descriptor.unifiedEditMode = info.unifiedEditMode;
			} else {
				if (descriptor.aggregatePresentation != info.aggregatePresentation)
					descriptor.aggregatePresentation = ManagerAggregatePresentation::Components;
				if (descriptor.unifiedEditMode != info.unifiedEditMode)
					descriptor.unifiedEditMode = ManagerUnifiedEditMode::None;
			}
			descriptor.members.push_back({ std::move(settingPath), std::string(setting.settingKey), info.componentDisplayName,
				*value, setting.serializedComponent, info.aggregateAll });
		}

		for (auto& [key, descriptor] : groups) {
			const auto expectedCount = std::get<3>(key);
			const auto expectedStart = std::get<2>(key);
			std::sort(descriptor.members.begin(), descriptor.members.end(), [](const auto& lhs, const auto& rhs) {
				return lhs.componentIndex < rhs.componentIndex;
			});
			bool complete = descriptor.members.size() == expectedCount;
			for (size_t index = 0; complete && index < descriptor.members.size(); ++index)
				complete = descriptor.members[index].componentIndex == expectedStart + index;
			if (complete) {
				descriptors.push_back(std::move(descriptor));
				continue;
			}
			for (const auto& member : descriptor.members) {
				auto* setting = FindAllowedCatalogSetting(
					featureShortName, member.settingPath, member.key, transitionableOnly);
				if (setting)
					descriptors.push_back(MakeScalarDescriptor(*setting, member.value));
			}
		}

		std::sort(descriptors.begin(), descriptors.end(), [](const auto& lhs, const auto& rhs) {
			return std::tie(lhs.displayPath, lhs.displayName, lhs.settingPath, lhs.key) <
			       std::tie(rhs.displayPath, rhs.displayName, rhs.settingPath, rhs.key);
		});
		return descriptors;
	}
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetFeatureSceneSettings(
	SceneType type, const std::string& featureShortName)
{
	auto* manager = GetSingleton();
	return manager ? manager->GetCachedFeatureSceneSettings(type, featureShortName) :
	                 std::vector<SettingDescriptor>{};
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetTransitionableSceneSettings(const std::string& featureShortName)
{
	auto* manager = GetSingleton();
	return manager ? manager->GetCachedFeatureSceneSettings(SceneType::TimeOfDay, featureShortName) :
	                 std::vector<SettingDescriptor>{};
}

const std::vector<SceneSettingsManager::SettingDescriptor>& SceneSettingsManager::GetCachedFeatureSceneSettings(
	SceneType type, const std::string& featureShortName)
{
	static const std::vector<SettingDescriptor> empty;
	if (type != SceneType::InteriorOnly && type != SceneType::TimeOfDay && type != SceneType::Location)
		return empty;
	auto cacheIt = featurePresentationCache.find(featureShortName);
	if (cacheIt == featurePresentationCache.end()) {
		const auto* snapshot = GetFeatureBaseSnapshot(featureShortName);
		if (!snapshot)
			return empty;
		CachedFeaturePresentation presentation;
		presentation.interiorSettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::InteriorOnly, *snapshot);
		presentation.timeOfDaySettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::TimeOfDay, *snapshot);
		presentation.locationSettings = CollectFeatureSceneSettings(
			featureShortName, SceneType::Location, *snapshot);
		cacheIt = featurePresentationCache.emplace(featureShortName, std::move(presentation)).first;
	}
	switch (type) {
	case SceneType::InteriorOnly:
		return cacheIt->second.interiorSettings;
	case SceneType::TimeOfDay:
		return cacheIt->second.timeOfDaySettings;
	case SceneType::Location:
		return cacheIt->second.locationSettings;
	default:
		return empty;
	}
}

void SceneSettingsManager::InvalidateFeatureSnapshot(std::string_view featureShortName)
{
	cachedLocationOverridesValid = false;
	locationOverridesDirty = true;
	if (featureShortName.empty()) {
		featureBaseSnapshots.clear();
		featurePresentationCache.clear();
		featureApplyDocuments.clear();
		pendingApplyVerifications.clear();
		std::erase_if(baselineSettings,
			[&](const auto& item) { return !appliedSettings.contains(item.first); });
		return;
	}
	const auto featureName = std::string(featureShortName);
	featureBaseSnapshots.erase(featureName);
	featurePresentationCache.erase(featureName);
	featureApplyDocuments.erase(featureName);
	pendingApplyVerifications.erase(featureName);
	std::erase_if(baselineSettings, [&](const auto& item) {
		return item.first.featureShortName == featureName && !appliedSettings.contains(item.first);
	});
}

bool SceneSettingsManager::GetSettingControlInfo(const SettingEntry& entry, SettingControlInfo& info)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting)
		return false;
	info = MakeSettingControlInfo(*setting);
	return true;
}

std::string SceneSettingsManager::GetSettingDisplayName(const std::string& settingKey)
{
	return StripImGuiId(Util::PrettifyIdentifier(settingKey));
}

static std::string GetSceneSettingDisplayName(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (setting) {
		auto info = MakeSettingControlInfo(*setting);
		auto displayName = info.displayName;
		if (info.controlType != SceneSettingControlType::Scalar && !info.componentDisplayName.empty())
			displayName += std::format(" ({})", info.componentDisplayName);
		return JoinDisplayParts(info.displayPath, displayName);
	}
	return SceneSettingsManager::GetSettingDisplayName(settingKey);
}

json SceneSettingsManager::GetFeatureSettingValue(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (!setting)
		return {};
	auto* manager = GetSingleton();
	if (!manager)
		return {};
	const auto* snapshot = manager->GetFeatureBaseSnapshot(featureShortName);
	if (!snapshot)
		return {};
	const auto* value = GetCatalogSerializedValue(*snapshot, *setting);
	return value && IsSceneSettingPrimitive(*value) ? *value : json{};
}

SceneSettingsManager::SettingType SceneSettingsManager::DetectSettingType(const json& value)
{
	if (value.is_boolean())
		return SettingType::Boolean;
	if (value.is_number_integer())
		return SettingType::Integer;
	if (value.is_number_float())
		return SettingType::Float;
	if (value.is_string())
		return SettingType::String;
	return SettingType::Unknown;
}

bool SceneSettingsManager::IsBooleanControlSetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && SceneSettingsCatalog::HasFlag(
						  setting->flags, SceneSettingsCatalog::SettingFlag::BooleanControl);
}

bool SceneSettingsManager::IsInvertedDisplaySetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->invertedDisplay;
}

bool SceneSettingsManager::GetNumericBounds(const SettingEntry& entry, double& minimum, double& maximum)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric ||
		!setting->hasNumericBounds || !std::isfinite(setting->minimumValue) ||
		!std::isfinite(setting->maximumValue) || setting->minimumValue > setting->maximumValue)
		return false;
	minimum = setting->minimumValue;
	maximum = setting->maximumValue;
	return true;
}

double SceneSettingsManager::GetNumericDisplayScale(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
		return 1.0;
	return GetCatalogNumericDisplayScale(*setting);
}

bool SceneSettingsManager::GetNumericDisplayValue(
	const SettingEntry& entry, double storedValue, double& displayValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericStoredToDisplay(*setting, storedValue, displayValue);
}

bool SceneSettingsManager::GetNumericStoredValue(
	const SettingEntry& entry, double displayValue, double& storedValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericDisplayToStored(*setting, displayValue, storedValue);
}

bool SceneSettingsManager::IsNumericInputClamped(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->clampNumericInput;
}

bool SceneSettingsManager::IsHDRColorSetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->hdrColor;
}

size_t SceneSettingsManager::GetSettingChoiceCount(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice ?
	           setting->choiceCount :
	           0;
}

bool SceneSettingsManager::GetSettingChoice(
	const SettingEntry& entry, size_t index, std::int64_t& value, std::string& displayName)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Choice ||
		index >= setting->choiceCount)
		return false;
	const auto& choice = setting->choices[index];
	value = choice.value;
	displayName = StripImGuiId(choice.displayName);
	if (!choice.displayNameKey.empty())
		displayName = StripImGuiId(T(choice.displayNameKey, displayName.c_str()));
	return true;
}

static bool GetFeatureSettingValueForValidation(const std::string& featureShortName,
	const SceneSettingsCatalog::SettingMetadata& setting, json& featureValue)
{
	featureValue = SceneSettingsManager::GetFeatureSettingValue(
		featureShortName, SplitCatalogPath(setting.settingPath), std::string(setting.settingKey));
	return IsSceneSettingPrimitive(featureValue);
}

static bool IsSceneSettingValueAllowed(const json& featureValue,
	const SceneSettingsCatalog::SettingMetadata& setting, const json& value, bool requireNumeric)
{
	if (!IsCatalogValueCompatible(setting, featureValue) || !IsCatalogValueCompatible(setting, value))
		return false;

	if (value.is_number() && !std::isfinite(value.get<double>()))
		return false;
	if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Numeric) {
		double featureDisplayValue = 0.0;
		double candidateDisplayValue = 0.0;
		if (!featureValue.is_number() || !value.is_number() ||
			!ConvertCatalogNumericStoredToDisplay(setting, featureValue.get<double>(), featureDisplayValue) ||
			!ConvertCatalogNumericStoredToDisplay(setting, value.get<double>(), candidateDisplayValue))
			return false;
		if (setting.clampNumericInput && setting.hasNumericBounds &&
			(candidateDisplayValue < setting.minimumValue || candidateDisplayValue > setting.maximumValue))
			return false;
	}

	if (SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::BooleanControl)) {
		if (setting.valueType == SceneSettingsCatalog::ValueType::Integer &&
			(!value.is_number_integer() || (value.get<std::int64_t>() != 0 && value.get<std::int64_t>() != 1)))
			return false;
		if (setting.valueType == SceneSettingsCatalog::ValueType::Boolean && !value.is_boolean())
			return false;
	}

	if (setting.choiceCount > 0) {
		if (!value.is_number_integer())
			return false;
		const auto choiceValue = value.get<std::int64_t>();
		if (std::none_of(setting.choices, setting.choices + setting.choiceCount,
				[&](const auto& choice) { return choice.value == choiceValue; }))
			return false;
	}

	if (requireNumeric && (!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable) ||
							  !IsNumericValue(featureValue) || !IsNumericValue(value) || !std::isfinite(value.get<float>())))
		return false;
	if (!requireNumeric && !IsSceneSettingPrimitive(value))
		return false;

	return IsCompatibleSceneSettingValue(featureValue, value);
}

static bool ValidateSceneSettingEntry(std::string_view context, SceneSettingsManager::SceneType type,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, const json& value, bool requireNumeric)
{
	if (!IsSettingAllowedBySceneTypePolicy(type, featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry {} is not whitelisted for this scene type",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	if (IsBlacklistedSceneSetting(featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry {} is blacklisted",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey, requireNumeric);
	if (!setting) {
		logger::warn("[SceneSettings] {} entry {} is not permitted by the compiled scene settings catalog",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature) {
		logger::warn("[SceneSettings] {} entry {} - feature '{}' not found/loaded",
			context, GetSettingLogName(featureShortName, settingPath, settingKey), featureShortName);
		return false;
	}

	json featureValue;
	if (!GetFeatureSettingValueForValidation(featureShortName, *setting, featureValue) ||
		!IsSceneSettingValueAllowed(featureValue, *setting, value, requireNumeric)) {
		logger::warn("[SceneSettings] {} entry {} is not a supported scene-manager setting",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	return true;
}

static bool ApplyEntryValueUpdates(std::string_view context, SceneSettingsManager::SceneType type,
	std::vector<SceneSettingsManager::SettingEntry>& entries,
	std::span<const SceneSettingsManager::EntryValueUpdate> updates,
	bool requireNumeric, bool& userEntriesChanged)
{
	if (updates.empty())
		return false;

	std::set<size_t> updatedIndices;
	for (const auto& update : updates) {
		if (update.index >= entries.size() || !updatedIndices.insert(update.index).second)
			return false;
		const auto& entry = entries[update.index];
		if (!IsSettingAllowedBySceneTypePolicy(
				type, entry.featureShortName, entry.settingPath, entry.settingKey))
			return false;
		auto* setting = FindAllowedCatalogSetting(
			entry.featureShortName, entry.settingPath, entry.settingKey, requireNumeric);
		if (!setting || !Feature::FindFeatureByShortName(entry.featureShortName) ||
			!IsSceneSettingValueAllowed(entry.value, *setting, update.value, requireNumeric)) {
			logger::warn("[SceneSettings] {} update {} is not a supported value",
				context, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
	}

	userEntriesChanged = false;
	for (const auto& update : updates) {
		auto& entry = entries[update.index];
		entry.value = update.value;
		userEntriesChanged |= entry.source == SceneSettingsManager::EntrySource::User;
	}
	return true;
}

// --- Generic Entry Management ---

std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntriesMut(SceneType type)
{
	assert(IsEntryListSceneType(type));
	return entries[type];
}

void SceneSettingsManager::BumpEntryPresentationRevision()
{
	++entryPresentationRevision;
	activeEntryCacheDirty = true;
	configuredFeatureNamesRevision = std::numeric_limits<std::uint64_t>::max();
}

const std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntries(SceneType type) const
{
	static const std::vector<SettingEntry> empty;
	if (!IsEntryListSceneType(type))
		return empty;
	auto it = entries.find(type);
	return (it != entries.end()) ? it->second : empty;
}

void SceneSettingsManager::MarkEntryListUserSettingsModified(SceneType type)
{
	assert(IsEntryListSceneType(type));
	if (type == SceneType::InteriorOnly)
		interiorUserSettingsModified = true;
	else
		timeOfDayUserSettingsModified = true;
}

bool SceneSettingsManager::IsEntryActive(const SettingEntry& entry) const
{
	return !entry.paused && !IsFeaturePaused(entry.featureShortName);
}

bool SceneSettingsManager::HasEntryFromSource(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const
{
	for (const auto& entry : GetEntries(type)) {
		if (entry.source == source && IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasEntryForPeriod(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey,
	TimeOfDayPeriod period, EntrySource source) const
{
	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (entry.source == source && entry.period == period &&
			IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasDuplicateEntry(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source, TimeOfDayPeriod period) const
{
	if (!IsEntryListSceneType(type))
		return false;
	if (type == SceneType::TimeOfDay)
		return HasEntryForPeriod(featureShortName, settingPath, settingKey, period, source);
	return HasEntryFromSource(type, featureShortName, settingPath, settingKey, source);
}

bool SceneSettingsManager::AddSetting(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
	TimeOfDayPeriod period, bool deferCommit)
{
	if (!IsEntryListSceneType(type) ||
		!IsSettingAllowedForType(type, featureShortName, settingPath, settingKey))
		return false;

	const bool requireNumeric = type == SceneType::TimeOfDay;
	if (requireNumeric) {
		// Reject invalid period values (Count is the sentinel, not a real period)
		if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount) {
			logger::warn("[SceneSettings] Rejecting TOD setting with invalid period: {}", GetSettingLogName(featureShortName, settingPath, settingKey));
			return false;
		}
	}
	if (!ValidateSceneSettingEntry(
			GetSceneTypeName(type), type, featureShortName, settingPath, settingKey, value, requireNumeric))
		return false;

	if (HasDuplicateEntry(type, featureShortName, settingPath, settingKey, EntrySource::User, period))
		return false;

	auto& vec = GetEntriesMut(type);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = value;
	entry.originalValue = entry.value;
	entry.source = EntrySource::User;
	entry.period = period;
	vec.push_back(std::move(entry));
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	if (deferCommit)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveSetting(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;

	const auto entry = vec[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetSceneOverwritePath(type, entry), entry.settingPath, entry.settingKey))
		return;

	logger::info("[SceneSettings] Removed {} entry: {} (source={})", GetSceneTypeName(type),
		GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey),
		entry.source == EntrySource::Overwrite ? "overwrite" : "user");

	vec.erase(vec.begin() + static_cast<ptrdiff_t>(index));
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseEntry(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index < vec.size()) {
		vec[index].paused = !vec[index].paused;
		if (type == SceneType::TimeOfDay)
			++sceneValueRevision;
		BumpEntryPresentationRevision();
		if (vec[index].source == EntrySource::User) {
			MarkEntryListUserSettingsModified(type);
			SaveAllUserSettings();
		}
		ReapplyIfActive();
	}
}

void SceneSettingsManager::RevertEntryToDefault(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;
	auto& entry = vec[index];
	if (entry.originalValue.is_null() ||
		!ValidateSceneSettingEntry(GetSceneTypeName(type), type, entry.featureShortName,
			entry.settingPath, entry.settingKey, entry.originalValue, type == SceneType::TimeOfDay))
		return;

	entry.value = entry.originalValue;
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::SetAllOverwritesPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::Overwrite && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (changed)
		BumpEntryPresentationRevision();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllOverwritesPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::Overwrite)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);

	std::vector<bool> shouldErase(vec.size(), false);
	std::map<std::filesystem::path, bool> deleteResults;
	for (size_t i = 0; i < vec.size(); ++i) {
		const auto& entry = vec[i];
		if (entry.source != EntrySource::Overwrite)
			continue;
		if (entry.sourceFilename.empty()) {
			shouldErase[i] = true;
			continue;
		}
		auto filepath = GetSceneOverwritePath(type, entry);
		auto [resultIt, inserted] = deleteResults.try_emplace(filepath, false);
		if (inserted) {
			std::error_code ec;
			auto removed = std::filesystem::remove(filepath, ec);
			resultIt->second = removed || !ec;
			if (!resultIt->second)
				logger::error("[SceneSettings] Failed to delete overwrite file: {} ({}) - keeping entry", filepath.string(), ec.message());
		}

		if (resultIt->second)
			shouldErase[i] = true;
	}
	// Erase only entries whose backing files were successfully cleaned up
	// (iterate in reverse to preserve index validity)
	bool changed = false;
	for (size_t i = vec.size(); i-- > 0;) {
		if (shouldErase[i]) {
			vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
			changed = true;
		}
	}
	if (changed && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (changed)
		BumpEntryPresentationRevision();

	ReapplyIfActive();
}

void SceneSettingsManager::SetAllUserPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::User && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (changed)
		BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllUserPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::User)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllUserSettings(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const auto removed = std::erase_if(vec, [](const SettingEntry& e) {
		return e.source == EntrySource::User;
	});
	if (removed != 0 && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (removed != 0)
		BumpEntryPresentationRevision();
	unresolvedUserEntries[type].clear();

	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

static std::string GetSceneOverwriteTypeDescription(SceneSettingsManager::SceneType type, SceneSettingsManager::TimeOfDayPeriod period)
{
	if (type == SceneSettingsManager::SceneType::InteriorOnly)
		return "Interior Only";
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Time of Day - {}", SceneSettingsManager::GetPeriodName(period));
	return "Time of Day";
}

static std::string GetWeatherOverwriteTypeDescription(SceneSettingsManager::TimeOfDayPeriod period)
{
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Weather - {}", SceneSettingsManager::GetPeriodName(period));
	return "Weather";
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetOverwritesPath(type);
	if (type == SceneSettingsManager::SceneType::TimeOfDay && entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static std::filesystem::path GetWeatherOverwritePath(RE::FormID weatherId, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static std::filesystem::path GetLocationOverwritePath(SceneSettingsManager::LocationTargetType type,
	std::string_view formKey, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;
	return SceneSettingsManager::GetLocationOverwritesDir(type) / formKey / entry.sourceFilename;
}

static bool WriteGroupedOverwriteFile(const std::filesystem::path& path, const std::string& featureShortName,
	const std::string& overwriteType, const std::vector<const SceneSettingsManager::SettingEntry*>& entries,
	const json& extraMetadata = json::object())
{
	std::error_code ec;
	const auto pathExists = std::filesystem::exists(path, ec);
	if (ec) {
		logger::error("[SceneSettings] WriteGroupedOverwriteFile: could not inspect '{}': {}", path.string(), ec.message());
		return false;
	}

	json data = json::object();
	if (pathExists && !ReadBoundedSceneJson(path, data)) {
		logger::error("[SceneSettings] Refusing to replace invalid overwrite file '{}'", path.string());
		return false;
	}

	if (auto featureIt = data.find(kFeatureKey); featureIt != data.end() &&
												 (!featureIt->is_string() || featureIt->get<std::string>() != featureShortName)) {
		logger::error("[SceneSettings] Refusing to relabel overwrite file '{}' from another feature", path.string());
		return false;
	}
	data[kFeatureKey] = featureShortName;
	auto& metadata = data[kMetadataKey];
	if (!metadata.is_null() && !metadata.is_object()) {
		logger::error("[SceneSettings] Refusing to replace invalid metadata in overwrite file '{}'", path.string());
		return false;
	}
	if (metadata.is_null())
		metadata = json::object();
	metadata[kMetadataDescriptionKey] = std::format("{} scene settings overwrite ({})",
		SceneSettingsManager::GetFeatureDisplayName(featureShortName), overwriteType);
	if (extraMetadata.is_object())
		for (const auto& [key, value] : extraMetadata.items())
			metadata[key] = value;
	for (const auto* entry : entries) {
		auto* node = GetObjectAtPath(data, entry->settingPath, true);
		if (!node) {
			logger::error("[SceneSettings] Refusing to replace a non-object path in overwrite file '{}'",
				path.string());
			return false;
		}
		(*node)[entry->settingKey] = entry->value;
	}

	return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
}

static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	if (path.empty())
		return true;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return !ec;

	std::ifstream in(path);
	if (!in.is_open()) {
		logger::error("[SceneSettings] Could not open overwrite file '{}' for editing", path.string());
		return false;
	}

	auto data = json::parse(in, nullptr, false);
	if (!data.is_object()) {
		logger::error("[SceneSettings] Could not parse overwrite file '{}' for editing", path.string());
		return false;
	}

	if (!RemoveObjectValueAtPath(data, settingPath, 0, settingKey)) {
		logger::error("[SceneSettings] Overwrite setting '{}' was not found in '{}'",
			settingKey, path.string());
		return false;
	}
	if (!HasSceneOverwriteContent(data)) {
		auto removed = std::filesystem::remove(path, ec);
		if (removed || !ec)
			return true;
		logger::error("[SceneSettings] Failed to delete overwrite file '{}': {}", path.string(), ec.message());
		return false;
	}

	return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
}

void SceneSettingsManager::ExportUserSettingsToOverwrites(SceneType type, const std::vector<size_t>& indices, const std::string& modName)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	auto basePath = GetOverwritesPath(type);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (type == SceneType::TimeOfDay && e.period != TimeOfDayPeriod::Count) ? basePath / GetPeriodName(e.period) : basePath;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetSceneOverwriteTypeDescription(type, grouped.front()->period);
		WriteGroupedOverwriteFile(dir / std::format("{}_{}.json", safeModName, featureShortName), featureShortName, typeDescription, grouped);
	}
}

void SceneSettingsManager::ExportWeatherUserSettingsToOverwrites(RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto& vec = GetWeatherConfigMut(weatherId).entries;
	auto baseDir = GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (e.period != TimeOfDayPeriod::Count) ? baseDir / GetPeriodName(e.period) : baseDir;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetWeatherOverwriteTypeDescription(grouped.front()->period);
		WriteGroupedOverwriteFile(dir / std::format("{}_{}.json", safeModName, featureShortName), featureShortName, typeDescription, grouped);
	}
}

void SceneSettingsManager::UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateEntryValues(type, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateEntryValues(
	SceneType type, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const bool requireNumeric = type == SceneType::TimeOfDay;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			GetSceneTypeName(type), type, vec, updates, requireNumeric, userEntriesChanged))
		return;
	if (type == SceneType::TimeOfDay)
		++sceneValueRevision;

	if (userEntriesChanged) {
		MarkEntryListUserSettingsModified(type);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::CommitSceneSettingChanges()
{
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::MarkDeferredSceneChanges()
{
	deferredSceneChangesPending = true;
	deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveDelay;
}

void SceneSettingsManager::FlushDeferredSceneChanges()
{
	if (!deferredSceneChangesPending || std::chrono::steady_clock::now() < deferredSceneChangesDeadline)
		return;

	SaveAllUserSettings();
	ReapplyIfActive();
}

// --- Event Handler ---

RE::BSEventNotifyControl SceneSettingsManager::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening) {
		GetSingleton()->queuedLoadingTransition.store(true, std::memory_order_relaxed);
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Scene Application ---

void SceneSettingsManager::Update()
{
	if (globals::state) {
		const auto frame = globals::state->frameCount;
		if (lastUpdateFrame == frame)
			return;
		lastUpdateFrame = frame;
	}
	VerifyPendingApplies();
	FlushDeferredSceneChanges();

	if (queuedLoadingTransition.exchange(false, std::memory_order_relaxed))
		OnLoadingTransition();
	else
		ResolveAndApply();
}

void SceneSettingsManager::OnLoadingTransition()
{
	locationTargetsCached = false;
	resolverDirty = true;
	ResolveAndApply(true, false);
}

void SceneSettingsManager::ReapplyIfActive(bool activeSetMayHaveChanged)
{
	if (activeSetMayHaveChanged)
		activeEntryCacheDirty = true;
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

bool SceneSettingsManager::HasActiveSettingsForFeature(const std::string& featureShortName) const
{
	return appliedFeatureNames.contains(featureShortName);
}

bool SceneSettingsManager::HasAnySceneEntriesForFeature(const std::string& featureShortName) const
{
	if (configuredFeatureNamesRevision != entryPresentationRevision) {
		configuredFeatureNamesCache.clear();
		const auto collect = [&](const auto& sourceEntries) {
			for (const auto& entry : sourceEntries)
				configuredFeatureNamesCache.insert(entry.featureShortName);
		};
		for (const auto& [_, sourceEntries] : entries)
			collect(sourceEntries);
		for (const auto& [_, config] : weatherSceneConfigs)
			collect(config.entries);
		for (const auto& [_, config] : locationSceneConfigs)
			collect(config.entries);
		configuredFeatureNamesRevision = entryPresentationRevision;
	}
	return configuredFeatureNamesCache.contains(featureShortName);
}

bool SceneSettingsManager::IsActiveSceneSetting(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return IsActiveSceneSetting(std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey));
}

bool SceneSettingsManager::IsActiveSceneSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	return appliedSettings.contains({ featureShortName, settingPath, settingKey });
}

void SceneSettingsManager::CaptureExternalFeatureChanges(Feature* feature)
{
	if (!feature)
		return;
	if (appliedSettings.empty()) {
		InvalidateFeatureSnapshot(feature->GetShortName());
		return;
	}

	json featureSettings;
	try {
		feature->SaveSettings(featureSettings);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}: {}",
			feature->GetShortName(), e.what());
		return;
	} catch (...) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}",
			feature->GetShortName());
		return;
	}
	if (!featureSettings.is_object())
		return;

	std::vector<std::pair<SettingAddress, json>> changedSettings;
	const auto featureShortName = feature->GetShortName();
	InvalidateFeatureSnapshot(featureShortName);
	for (auto appliedIt = appliedSettings.lower_bound({ featureShortName, {}, {} });
		appliedIt != appliedSettings.end() && appliedIt->first.featureShortName == featureShortName;
		++appliedIt) {
		const auto& [address, appliedValue] = *appliedIt;
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		if (!setting)
			continue;
		const auto* value = GetCatalogSerializedValue(featureSettings, *setting);
		if (!value || !IsSceneSettingPrimitive(*value) ||
			!IsCompatibleSceneSettingValue(appliedValue, *value) || appliedValue == *value)
			continue;
		changedSettings.emplace_back(address, *value);
	}

	if (changedSettings.empty())
		return;
	for (const auto& [address, value] : changedSettings) {
		baselineSettings[address] = value;
		appliedSettings[address] = value;
	}
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

SceneSettingsManager::SceneLayerGuard::SceneLayerGuard(SceneSettingsManager& manager) :
	manager(manager)
{
	manager.SuspendSceneLayer();
}

SceneSettingsManager::SceneLayerGuard::~SceneLayerGuard()
{
	manager.ResumeSceneLayer();
}

bool SceneSettingsManager::IsFeaturePaused(const std::string& featureShortName) const
{
	auto it = featurePauseStates.find(featureShortName);
	return it != featurePauseStates.end() && it->second;
}

void SceneSettingsManager::SetFeaturePaused(const std::string& featureShortName, bool paused)
{
	featurePauseStates[featureShortName] = paused;
	++sceneValueRevision;
	locationOverridesDirty = true;
	ReapplyIfActive();
}

void SceneSettingsManager::SuspendSceneLayer()
{
	if (++sceneLayerSuspendDepth > 1)
		return;

	resolverSuspended = true;
	RestoreAppliedSettings();
}

void SceneSettingsManager::ResumeSceneLayer()
{
	if (sceneLayerSuspendDepth <= 0) {
		logger::warn("[SceneSettings] ResumeSceneLayer called without a matching suspend");
		sceneLayerSuspendDepth = 0;
		return;
	}
	if (--sceneLayerSuspendDepth > 0)
		return;

	InvalidateFeatureSnapshot();
	resolverSuspended = false;
	resolverDirty = true;
	ResolveAndApply(true);
}

void SceneSettingsManager::ResolveAndApply(bool force, bool allowLocationTransitions)
{
	if (resolverSuspended || sceneLayerSuspendDepth > 0)
		return;
	if (!locationDataLoaded)
		TryEnsureLocationDataLoaded();
	if (!weatherDataLoaded)
		TryEnsureWeatherDataLoaded();
	if (!HasActiveSceneEntriesCached()) {
		applyFailures.clear();
		if (!appliedSettings.empty())
			RestoreAppliedSettings();
		else
			ClearLocationTransitions();
		resolverDirty = !appliedSettings.empty();
		return;
	}

	if (globals::state && (globals::state->isMainMenuOpen || globals::state->isLoadingMenuOpen)) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	const bool interior = Util::IsInterior();
	const auto hour = GetCurrentGameHour();
	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();
	const auto* worldspace = player->GetWorldspace();
	const auto worldspaceId = worldspace ? worldspace->GetFormID() : 0;
	const bool cellChanged = cellId != lastResolvedCellId;
	const bool locationContextChanged = locationId != lastResolvedLocationId || cellChanged;
	const bool walkedBetweenWorldspaceCells = allowLocationTransitions && cellChanged &&
	                                          lastResolvedCellId != 0 && !interior && !lastResolvedInterior &&
	                                          worldspaceId != 0 && worldspaceId == lastResolvedWorldspaceId;

	RE::FormID currentWeatherId = 0;
	RE::FormID previousWeatherId = 0;
	float weatherLerp = 0.0f;
	if (!interior) {
		TryEnsureWeatherDataLoaded();
		if (auto* sky = globals::game::sky) {
			currentWeatherId = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
			weatherLerp = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 0.0f;
			previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
		}
	}

	const bool contextChanged = interior != lastResolvedInterior ||
	                            locationContextChanged ||
	                            currentWeatherId != lastResolvedCurrentWeatherId ||
	                            previousWeatherId != lastResolvedPreviousWeatherId ||
	                            std::abs(weatherLerp - lastResolvedWeatherLerp) >= kBlendEpsilon ||
	                            (!interior && (lastResolvedHour < 0.0f ||
												  std::abs(hour - lastResolvedHour) >= kHourUpdateThreshold));
	const auto now = std::chrono::steady_clock::now();
	const bool applyRetryDue = std::any_of(applyFailures.begin(), applyFailures.end(),
		[&](const auto& item) { return now >= item.second.retryAfter; });
	const auto transitionTime = GetPauseAwareTime();
	if (!force && !resolverDirty && !contextChanged && !applyRetryDue) {
		if (!activeLocationTransitions.empty())
			AdvanceLocationTransitions(transitionTime);
		return;
	}

	resolverDirty = false;
	const bool reconcileLocationTransitions = locationContextChanged || locationOverridesDirty;
	auto& resolved = BuildResolvedSettings(reconcileLocationTransitions);
	if (reconcileLocationTransitions) {
		StartLocationTransitions(resolved, transitionTime, walkedBetweenWorldspaceCells);
		locationOverridesDirty = false;
	}
	if (!activeLocationTransitions.empty()) {
		for (const auto& [address, transition] : activeLocationTransitions) {
			const auto linear = transition.duration > 0.0f ?
			                        std::clamp((transitionTime - transition.startTime) /
												   transition.duration,
										0.0f, 1.0f) :
			                        1.0f;
			const auto smooth = linear * linear * (3.0f - 2.0f * linear);
			resolved[address] = transition.startValue +
			                    (transition.targetValue - transition.startValue) * smooth;
		}
	}
	ApplyResolvedSettings(resolved, force);
	std::set<std::string> restoredTransitionFeatures;
	for (auto transitionIt = activeLocationTransitions.begin();
		transitionIt != activeLocationTransitions.end();) {
		const auto& [address, transition] = *transitionIt;
		const bool finished = transition.duration <= 0.0f ||
		                      transitionTime - transition.startTime >= transition.duration;
		auto appliedIt = appliedSettings.find(address);
		if (!finished || appliedIt == appliedSettings.end() || !IsNumericValue(appliedIt->second) ||
			std::abs(appliedIt->second.get<float>() - transition.targetValue) >= kBlendEpsilon) {
			++transitionIt;
			continue;
		}
		if (transition.restoreAtEnd) {
			appliedSettings.erase(address);
			baselineSettings.erase(address);
			restoredTransitionFeatures.insert(address.featureShortName);
		}
		transitionIt = activeLocationTransitions.erase(transitionIt);
		locationTransitionBatchesDirty = true;
	}
	for (const auto& featureShortName : restoredTransitionFeatures) {
		if (std::none_of(appliedSettings.begin(), appliedSettings.end(), [&](const auto& item) {
				return item.first.featureShortName == featureShortName;
			}))
			appliedFeatureNames.erase(featureShortName);
	}

	lastResolvedInterior = interior;
	lastResolvedLocationId = locationId;
	lastResolvedCellId = cellId;
	lastResolvedWorldspaceId = worldspaceId;
	lastResolvedHour = hour;
	lastResolvedCurrentWeatherId = currentWeatherId;
	lastResolvedPreviousWeatherId = previousWeatherId;
	lastResolvedWeatherLerp = weatherLerp;
}

float SceneSettingsManager::GetPauseAwareTime() const
{
	return globals::state ? globals::state->timer : 0.0f;
}

void SceneSettingsManager::StartLocationTransitions(
	const ResolvedSettingMap& resolved, float now, bool animateChanges)
{
	ResolvedSettingMap nextOverrideValues;
	for (const auto& [address, _] : pendingLocationTransitionDurations) {
		if (auto resolvedIt = resolved.find(address);
			resolvedIt != resolved.end() && IsNumericValue(resolvedIt->second))
			nextOverrideValues.emplace(address, resolvedIt->second);
	}

	std::set<SettingAddress> changedAddresses;
	for (const auto& [address, _] : lastLocationOverrideValues)
		changedAddresses.insert(address);
	for (const auto& [address, _] : nextOverrideValues)
		changedAddresses.insert(address);
	for (const auto& [address, _] : activeLocationTransitions)
		changedAddresses.insert(address);

	for (const auto& address : changedAddresses) {
		auto previousIt = lastLocationOverrideValues.find(address);
		auto nextIt = nextOverrideValues.find(address);
		const bool membershipChanged = (previousIt == lastLocationOverrideValues.end()) !=
		                               (nextIt == nextOverrideValues.end());
		const bool valueChanged = previousIt != lastLocationOverrideValues.end() &&
		                          nextIt != nextOverrideValues.end() &&
		                          !ResolvedValuesEqual(previousIt->second, nextIt->second);
		auto previousDurationIt = lastLocationTransitionDurations.find(address);
		auto nextDurationIt = pendingLocationTransitionDurations.find(address);
		const float duration = nextDurationIt != pendingLocationTransitionDurations.end() ?
		                           nextDurationIt->second :
		                           locationTransitionSeconds;
		const bool durationChanged = previousDurationIt != lastLocationTransitionDurations.end() &&
		                             nextDurationIt != pendingLocationTransitionDurations.end() &&
		                             std::abs(previousDurationIt->second - nextDurationIt->second) >= kBlendEpsilon;
		if (!membershipChanged && !valueChanged &&
			!(durationChanged && activeLocationTransitions.contains(address)))
			continue;
		if (!animateChanges) {
			if (activeLocationTransitions.erase(address) != 0)
				locationTransitionBatchesDirty = true;
			continue;
		}

		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;

		float startValue = baselineIt->second.get<float>();
		if (auto activeIt = activeLocationTransitions.find(address);
			activeIt != activeLocationTransitions.end()) {
			const auto& transition = activeIt->second;
			const auto linear = transition.duration > 0.0f ?
			                        std::clamp((now - transition.startTime) / transition.duration, 0.0f, 1.0f) :
			                        1.0f;
			const auto smooth = linear * linear * (3.0f - 2.0f * linear);
			startValue = transition.startValue + (transition.targetValue - transition.startValue) * smooth;
		} else if (auto appliedIt = appliedSettings.find(address);
			appliedIt != appliedSettings.end() && IsNumericValue(appliedIt->second)) {
			startValue = appliedIt->second.get<float>();
		}

		const auto resolvedIt = resolved.find(address);
		const bool restoreAtEnd = nextIt == nextOverrideValues.end() && resolvedIt == resolved.end();
		const auto& targetJson = nextIt != nextOverrideValues.end() ? nextIt->second :
		                         resolvedIt != resolved.end()       ? resolvedIt->second :
		                                                              baselineIt->second;
		if (!IsNumericValue(targetJson))
			continue;
		const auto targetValue = targetJson.get<float>();
		if (!std::isfinite(startValue) || !std::isfinite(targetValue))
			continue;
		if (duration <= 0.0f || std::abs(targetValue - startValue) < kBlendEpsilon) {
			if (activeLocationTransitions.erase(address) != 0)
				locationTransitionBatchesDirty = true;
			continue;
		}
		auto [transitionIt, inserted] = activeLocationTransitions.insert_or_assign(address, LocationTransition{
																								.startValue = startValue,
																								.targetValue = targetValue,
																								.startTime = now,
																								.duration = duration,
																								.restoreAtEnd = restoreAtEnd,
																							});
		(void)transitionIt;
		(void)inserted;
		locationTransitionBatchesDirty = true;
	}
	lastLocationOverrideValues = std::move(nextOverrideValues);
	lastLocationTransitionDurations = pendingLocationTransitionDurations;
}

bool SceneSettingsManager::AdvanceLocationTransitions(float now)
{
	if (activeLocationTransitions.empty())
		return false;
	if (locationTransitionBatchesDirty)
		RebuildLocationTransitionBatches();

	bool appliedAny = false;
	const auto retryNow = std::chrono::steady_clock::now();
	for (auto& [featureShortName, batch] : locationTransitionBatches) {
		for (size_t index = 0; index < batch.transitions.size(); ++index) {
			auto& transition = *batch.transitions[index];
			const auto linear = transition.duration > 0.0f ?
			                        std::clamp((now - transition.startTime) / transition.duration, 0.0f, 1.0f) :
			                        1.0f;
			const auto smooth = linear * linear * (3.0f - 2.0f * linear);
			batch.updates[index].value = transition.startValue +
			                             (transition.targetValue - transition.startValue) * smooth;
		}

		auto transitionFailureIt = transitionApplyFailures.find(featureShortName);
		if (transitionFailureIt != transitionApplyFailures.end() &&
			transitionFailureIt->second.signature != batch.signature) {
			transitionApplyFailures.erase(transitionFailureIt);
			transitionFailureIt = transitionApplyFailures.end();
		}
		if (transitionFailureIt != transitionApplyFailures.end() &&
			retryNow < transitionFailureIt->second.retryAfter)
			continue;

		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			auto& failure = transitionApplyFailures[featureShortName];
			failure.signature = batch.signature;
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Cannot apply location transition, feature {} is not loaded",
					featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = retryNow + kApplyRetryDelay;
			continue;
		}
		if (!ApplyCatalogSceneSettings(*feature, batch.updates)) {
			auto& failure = transitionApplyFailures[featureShortName];
			failure.signature = batch.signature;
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Failed to apply location transition for {}", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = retryNow + kApplyRetryDelay;
			continue;
		}
		transitionApplyFailures.erase(featureShortName);
		ScheduleApplyVerification(featureShortName, batch.updates, batch.signature, true);
		appliedAny = true;

		bool restoredSetting = false;
		bool retainedSetting = false;
		for (size_t index = 0; index < batch.transitions.size(); ++index) {
			const auto& address = batch.addresses[index];
			const auto& transition = *batch.transitions[index];
			const bool finished = transition.duration <= 0.0f ||
			                      now - transition.startTime >= transition.duration;
			if (finished && transition.restoreAtEnd) {
				appliedSettings.erase(address);
				baselineSettings.erase(address);
				restoredSetting = true;
			} else {
				appliedSettings[address] = batch.updates[index].value;
				retainedSetting = true;
			}
			if (finished) {
				activeLocationTransitions.erase(address);
				locationTransitionBatchesDirty = true;
			}
		}
		if (retainedSetting)
			appliedFeatureNames.insert(featureShortName);
		else if (restoredSetting && std::none_of(appliedSettings.begin(), appliedSettings.end(),
										[&](const auto& item) { return item.first.featureShortName == featureShortName; }))
			appliedFeatureNames.erase(featureShortName);
	}
	if (activeLocationTransitions.empty()) {
		locationTransitionBatches.clear();
		transitionApplyFailures.clear();
		locationTransitionBatchesDirty = false;
	}
	return appliedAny;
}

void SceneSettingsManager::RebuildLocationTransitionBatches()
{
	locationTransitionBatches.clear();
	for (auto& [address, transition] : activeLocationTransitions) {
		auto& batch = locationTransitionBatches[address.featureShortName];
		if (batch.addresses.empty())
			batch.signature = std::hash<std::string_view>{}(address.featureShortName);
		batch.addresses.push_back(address);
		batch.transitions.push_back(&transition);
		batch.updates.push_back({ address.settingPath, address.settingKey, transition.startValue });
		for (const auto& segment : address.settingPath)
			CombineHash(batch.signature, std::hash<std::string_view>{}(segment));
		CombineHash(batch.signature, std::hash<std::string_view>{}(address.settingKey));
		HashSceneSettingValue(batch.signature, transition.startValue);
		HashSceneSettingValue(batch.signature, transition.targetValue);
		HashSceneSettingValue(batch.signature, transition.duration);
		CombineHash(batch.signature, static_cast<size_t>(transition.restoreAtEnd));
	}
	std::erase_if(transitionApplyFailures,
		[&](const auto& item) { return !locationTransitionBatches.contains(item.first); });
	locationTransitionBatchesDirty = false;
}

void SceneSettingsManager::ClearLocationTransitions()
{
	activeLocationTransitions.clear();
	locationTransitionBatches.clear();
	transitionApplyFailures.clear();
	lastLocationOverrideValues.clear();
	lastLocationTransitionDurations.clear();
	pendingLocationTransitionDurations.clear();
	cachedLocationOverrides.clear();
	cachedLocationOverridesValid = false;
	locationTransitionBatchesDirty = false;
	locationOverridesDirty = true;
}

bool SceneSettingsManager::HasActiveSceneEntriesCached()
{
	if (!activeEntryCacheDirty)
		return hasActiveSceneEntries;

	const auto hasActiveEntry = [&](const auto& sourceEntries, SceneType type) {
		const bool floatsOnly = type == SceneType::TimeOfDay;
		return std::any_of(sourceEntries.begin(), sourceEntries.end(), [&](const auto& entry) {
			return IsEntryActive(entry) &&
			       IsSettingAllowedForType(
					   type, entry.featureShortName, entry.settingPath, entry.settingKey) &&
			       (!floatsOnly || IsNumericValue(entry.value));
		});
	};

	hasActiveSceneEntries = false;
	for (const auto& [type, sourceEntries] : entries) {
		if (hasActiveEntry(sourceEntries, type)) {
			hasActiveSceneEntries = true;
			break;
		}
	}
	if (!hasActiveSceneEntries)
		for (const auto& [_, config] : weatherSceneConfigs)
			if (hasActiveEntry(config.entries, SceneType::TimeOfDay)) {
				hasActiveSceneEntries = true;
				break;
			}
	if (!hasActiveSceneEntries)
		for (const auto& [_, config] : locationSceneConfigs)
			if (hasActiveEntry(config.entries, SceneType::Location)) {
				hasActiveSceneEntries = true;
				break;
			}

	activeEntryCacheDirty = false;
	return hasActiveSceneEntries;
}

SceneSettingsManager::ResolvedSettingMap& SceneSettingsManager::BuildResolvedSettings(
	bool collectLocationTransitionDurations)
{
	auto& resolved = resolvedSettingsScratch;
	for (auto& [_, value] : resolved)
		value = nullptr;
	const bool interior = Util::IsInterior();
	std::vector<SettingAddress> requiredBaselines;
	const PeriodSettingMap* timeOfDayValues = nullptr;
	const auto collectBaselines = [&](const std::vector<SettingEntry>& sourceEntries, SceneType type) {
		const bool floatsOnly = type == SceneType::TimeOfDay;
		for (const auto& entry : sourceEntries) {
			if (!IsEntryActive(entry) || (floatsOnly && !IsNumericValue(entry.value)) ||
				!IsSettingAllowedForType(
					type, entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
			if (!baselineSettings.contains(address))
				requiredBaselines.push_back(std::move(address));
		}
	};
	const auto collectGroupedBaselines = [&](const PeriodSettingMap& values) {
		for (const auto& [address, _] : values)
			if (!baselineSettings.contains(address))
				requiredBaselines.push_back(address);
	};

	if (interior) {
		collectBaselines(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly);
	} else {
		timeOfDayValues = &BuildTimeOfDayValueGroups();
		collectGroupedBaselines(*timeOfDayValues);
		if (auto* sky = globals::game::sky) {
			const auto weatherLerp = std::isfinite(sky->currentWeatherPct) ?
			                             std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
			                             0.0f;
			const auto previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
			for (auto weatherId : { sky->currentWeather ? sky->currentWeather->GetFormID() : 0,
					 previousWeatherId })
				collectGroupedBaselines(BuildWeatherValueGroups(weatherId));
		}
	}

	const auto& locationTargets = GetCurrentLocationTargets();
	const bool rebuildLocationOverrides =
		collectLocationTransitionDurations || !cachedLocationOverridesValid;
	if (rebuildLocationOverrides) {
		for (const auto& target : locationTargets) {
			auto it = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
			if (it != locationSceneConfigs.end())
				collectBaselines(it->second.entries, SceneType::Location);
		}
	}
	std::sort(requiredBaselines.begin(), requiredBaselines.end());
	requiredBaselines.erase(std::unique(requiredBaselines.begin(), requiredBaselines.end()), requiredBaselines.end());
	EnsureBaselines(requiredBaselines);

	if (interior) {
		ResolveInteriorSettings(resolved);
	} else {
		std::array<float, kPeriodCount> factors{};
		GetTimeOfDayFactors(factors.data());
		ResolveTimeOfDaySettings(resolved, *timeOfDayValues, factors);
		ResolveWeatherSettings(resolved, *timeOfDayValues, factors);
	}
	if (rebuildLocationOverrides) {
		pendingLocationTransitionDurations.clear();
		cachedLocationOverrides.clear();
		ResolveLocationSettings(cachedLocationOverrides, locationTargets, true);
		cachedLocationOverridesValid = true;
	}
	for (const auto& [address, value] : cachedLocationOverrides)
		resolved[address] = value;
	std::erase_if(resolved, [](const auto& item) { return item.second.is_null(); });
	return resolved;
}

void SceneSettingsManager::ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry)
{
	struct PendingUpdate
	{
		const SettingAddress* address = nullptr;
		const json* value = nullptr;
		bool restore = false;
	};

	std::map<std::string, std::vector<PendingUpdate>> pendingByFeature;
	for (const auto& [address, _] : appliedSettings) {
		if (resolved.contains(address))
			continue;
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt != baselineSettings.end())
			pendingByFeature[address.featureShortName].push_back({ &address, &baselineIt->second, true });
	}

	for (const auto& [address, value] : resolved) {
		auto appliedIt = appliedSettings.find(address);
		if (appliedIt != appliedSettings.end() && ResolvedValuesEqual(appliedIt->second, value))
			continue;
		pendingByFeature[address.featureShortName].push_back({ &address, &value, false });
	}
	std::erase_if(applyFailures, [&](const auto& item) { return !pendingByFeature.contains(item.first); });

	for (const auto& [featureShortName, pending] : pendingByFeature) {
		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& update : pending)
			updates.push_back({ update.address->settingPath, update.address->settingKey, *update.value });
		std::optional<size_t> signature;
		const auto getSignature = [&]() {
			if (!signature) {
				signature = GetCatalogUpdateSignature(featureShortName, updates);
				for (const auto& update : pending)
					CombineHash(*signature, static_cast<size_t>(update.restore));
			}
			return *signature;
		};
		auto failureIt = applyFailures.find(featureShortName);
		if (failureIt != applyFailures.end()) {
			if (failureIt->second.signature != getSignature()) {
				applyFailures.erase(failureIt);
				failureIt = applyFailures.end();
			}
		}
		const auto now = std::chrono::steady_clock::now();
		if (!forceRetry && failureIt != applyFailures.end() && now < failureIt->second.retryAfter)
			continue;

		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			auto& failure = applyFailures[featureShortName];
			failure.signature = getSignature();
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Cannot apply resolved settings, feature {} is not loaded", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = now + kApplyRetryDelay;
			continue;
		}

		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			auto& failure = applyFailures[featureShortName];
			failure.signature = getSignature();
			if (!failure.warningLogged) {
				logger::warn("[SceneSettings] Failed to apply resolved settings for {}", featureShortName);
				failure.warningLogged = true;
			}
			failure.retryAfter = now + kApplyRetryDelay;
			continue;
		}
		applyFailures.erase(featureShortName);
		ScheduleApplyVerification(featureShortName, updates, getSignature(), false);
		restoreFailureWarnings.erase(featureShortName);

		bool restoredSetting = false;
		bool appliedSetting = false;
		for (const auto& update : pending) {
			if (update.restore) {
				baselineSettings.erase(*update.address);
				appliedSettings.erase(*update.address);
				restoredSetting = true;
			} else {
				appliedSettings[*update.address] = *update.value;
				appliedSetting = true;
			}
		}
		if (appliedSetting)
			appliedFeatureNames.insert(featureShortName);
		else if (restoredSetting && std::none_of(appliedSettings.begin(), appliedSettings.end(),
										[&](const auto& item) { return item.first.featureShortName == featureShortName; }))
			appliedFeatureNames.erase(featureShortName);
	}
}

void SceneSettingsManager::RestoreAppliedSettings()
{
	ClearLocationTransitions();
	struct PendingRestore
	{
		SettingAddress address;
		CatalogSceneSettingUpdate update;
	};

	std::map<std::string, std::vector<PendingRestore>> updatesByFeature;
	for (const auto& [address, _] : appliedSettings) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt != baselineSettings.end())
			updatesByFeature[address.featureShortName].push_back({ address, { address.settingPath, address.settingKey, baselineIt->second } });
	}

	for (const auto& [featureShortName, pending] : updatesByFeature) {
		const auto now = std::chrono::steady_clock::now();
		if (auto retryIt = restoreRetryAfter.find(featureShortName);
			retryIt != restoreRetryAfter.end() && now < retryIt->second)
			continue;
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Cannot restore {}, feature is not loaded", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}

		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& item : pending)
			updates.push_back(item.update);
		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Failed to restore base settings for {}", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}
		restoreFailureWarnings.erase(featureShortName);
		restoreRetryAfter.erase(featureShortName);
		pendingApplyVerifications.erase(featureShortName);

		for (const auto& item : pending) {
			appliedSettings.erase(item.address);
			baselineSettings.erase(item.address);
		}
		appliedFeatureNames.erase(featureShortName);
	}

	if (appliedSettings.empty()) {
		baselineSettings.clear();
		appliedFeatureNames.clear();
		restoreFailureWarnings.clear();
		restoreRetryAfter.clear();
	} else {
		resolverDirty = true;
	}
}

void SceneSettingsManager::ResolveInteriorSettings(ResolvedSettingMap& resolved) const
{
	OverlayEntries(resolved, GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly, EntrySource::User);
	OverlayEntries(resolved, GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly, EntrySource::Overwrite);
}

const SceneSettingsManager::PeriodSettingMap& SceneSettingsManager::BuildTimeOfDayValueGroups() const
{
	if (timeOfDayValueGroups.revision == sceneValueRevision)
		return timeOfDayValueGroups.values;
	auto& values = timeOfDayValueGroups.values;
	values.clear();
	for (auto source : { EntrySource::User, EntrySource::Overwrite }) {
		for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
			const auto periodIndex = static_cast<int>(entry.period);
			if (entry.source != source || !IsEntryActive(entry) || !IsNumericValue(entry.value) ||
				periodIndex < 0 || periodIndex >= kPeriodCount ||
				!IsSettingAllowedForType(SceneType::TimeOfDay,
					entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			const auto value = entry.value.get<float>();
			if (std::isfinite(value))
				values[{ entry.featureShortName, entry.settingPath, entry.settingKey }][periodIndex] = value;
		}
	}
	timeOfDayValueGroups.revision = sceneValueRevision;
	return values;
}

const SceneSettingsManager::PeriodSettingMap& SceneSettingsManager::BuildWeatherValueGroups(RE::FormID weatherId) const
{
	auto& cached = weatherValueGroups[weatherId];
	if (cached.revision == sceneValueRevision)
		return cached.values;
	auto& values = cached.values;
	values.clear();
	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end()) {
		cached.revision = sceneValueRevision;
		return values;
	}
	for (auto source : { EntrySource::User, EntrySource::Overwrite }) {
		for (const auto& entry : configIt->second.entries) {
			const auto periodIndex = static_cast<int>(entry.period);
			if (entry.source != source || !IsEntryActive(entry) || !IsNumericValue(entry.value) ||
				periodIndex < 0 || periodIndex >= kPeriodCount ||
				!IsSettingAllowedForType(SceneType::TimeOfDay,
					entry.featureShortName, entry.settingPath, entry.settingKey))
				continue;
			const auto value = entry.value.get<float>();
			if (std::isfinite(value))
				values[{ entry.featureShortName, entry.settingPath, entry.settingKey }][periodIndex] = value;
		}
	}
	cached.revision = sceneValueRevision;
	return values;
}

void SceneSettingsManager::ResolveTimeOfDaySettings(ResolvedSettingMap& resolved,
	const PeriodSettingMap& values, const std::array<float, kPeriodCount>& factors) const
{
	for (const auto& [address, periodValues] : values) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		const auto baseline = baselineIt->second.get<float>();
		float result = 0.0f;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex)
			result += factors[periodIndex] * periodValues[periodIndex].value_or(baseline);
		resolved[address] = result;
	}
}

void SceneSettingsManager::ResolveWeatherSettings(ResolvedSettingMap& resolved,
	const PeriodSettingMap& timeOfDayValues, const std::array<float, kPeriodCount>& factors) const
{
	auto* sky = globals::game::sky;
	if (!sky || !sky->currentWeather)
		return;
	const auto weatherLerp = std::isfinite(sky->currentWeatherPct) ?
	                             std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
	                             0.0f;
	const auto previousWeatherId = GetEffectivePreviousWeatherId(sky, weatherLerp);
	const auto& currentValues = BuildWeatherValueGroups(sky->currentWeather->GetFormID());
	const auto& previousValues = BuildWeatherValueGroups(previousWeatherId);

	const auto resolveWeather = [&](const SettingAddress& address,
									const PeriodSettingMap& weatherValues) -> std::optional<float> {
		auto weatherIt = weatherValues.find(address);
		if (weatherIt == weatherValues.end())
			return std::nullopt;
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			return std::nullopt;
		const auto baseline = baselineIt->second.get<float>();
		auto timeOfDayIt = timeOfDayValues.find(address);
		float result = 0.0f;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
			const auto lower = timeOfDayIt != timeOfDayValues.end() ?
			                       timeOfDayIt->second[periodIndex].value_or(baseline) :
			                       baseline;
			result += factors[periodIndex] * weatherIt->second[periodIndex].value_or(lower);
		}
		return result;
	};

	auto currentIt = currentValues.begin();
	auto previousIt = previousValues.begin();
	while (currentIt != currentValues.end() || previousIt != previousValues.end()) {
		const SettingAddress* address = nullptr;
		if (previousIt == previousValues.end() ||
			(currentIt != currentValues.end() && currentIt->first < previousIt->first)) {
			address = &currentIt->first;
			++currentIt;
		} else if (currentIt == currentValues.end() || previousIt->first < currentIt->first) {
			address = &previousIt->first;
			++previousIt;
		} else {
			address = &currentIt->first;
			++currentIt;
			++previousIt;
		}
		auto baselineIt = baselineSettings.find(*address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		float lowerValue = baselineIt->second.get<float>();
		if (auto resolvedIt = resolved.find(*address);
			resolvedIt != resolved.end() && IsNumericValue(resolvedIt->second))
			lowerValue = resolvedIt->second.get<float>();
		const auto currentValue = resolveWeather(*address, currentValues);
		const auto previousValue = resolveWeather(*address, previousValues);
		if (!currentValue && !previousValue)
			continue;
		const auto from = previousValue.value_or(lowerValue);
		const auto to = currentValue.value_or(lowerValue);
		resolved[*address] = from + (to - from) * weatherLerp;
	}
}

void SceneSettingsManager::ResolveLocationSettings(
	ResolvedSettingMap& resolved, const std::vector<LocationTarget>& locationTargets,
	bool collectTransitionDurations)
{
	auto* transitionDurations = collectTransitionDurations ?
	                                &pendingLocationTransitionDurations :
	                                nullptr;
	for (const auto& target : locationTargets) {
		auto it = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (it == locationSceneConfigs.end())
			continue;
		OverlayEntries(resolved, it->second.entries, SceneType::Location, EntrySource::User,
			transitionDurations);
		OverlayEntries(resolved, it->second.entries, SceneType::Location, EntrySource::Overwrite,
			transitionDurations);
	}
}

void SceneSettingsManager::OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
	SceneType type, std::optional<EntrySource> source,
	std::map<SettingAddress, float>* transitionDurations) const
{
	for (const auto& entry : sourceEntries) {
		if (!IsEntryActive(entry) || (source && entry.source != *source) ||
			!IsSettingAllowedForType(type, entry.featureShortName, entry.settingPath, entry.settingKey))
			continue;
		SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
		if (!baselineSettings.contains(address))
			continue;
		resolved[address] = entry.value;
		if (transitionDurations && IsNumericValue(entry.value)) {
			const auto duration = entry.transitionSeconds.value_or(locationTransitionSeconds);
			(*transitionDurations)[address] = std::clamp(
				std::isfinite(duration) ? duration : locationTransitionSeconds,
				0.0f, kMaxLocationTransitionSeconds);
		}
	}
}

const json* SceneSettingsManager::GetFeatureBaseSnapshot(const std::string& featureShortName)
{
	if (auto snapshotIt = featureBaseSnapshots.find(featureShortName);
		snapshotIt != featureBaseSnapshots.end())
		return &snapshotIt->second;

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature)
		return nullptr;

	json snapshot;
	try {
		feature->SaveSettings(snapshot);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not snapshot {}: {}", featureShortName, e.what());
		return nullptr;
	} catch (...) {
		logger::warn("[SceneSettings] Could not snapshot {}", featureShortName);
		return nullptr;
	}
	if (!snapshot.is_object())
		return nullptr;

	// SaveSettings contains the live scene layer. Replace only applied addresses in memory.
	for (auto appliedIt = appliedSettings.lower_bound({ featureShortName, {}, {} });
		appliedIt != appliedSettings.end() && appliedIt->first.featureShortName == featureShortName;
		++appliedIt) {
		const auto& address = appliedIt->first;
		auto baselineIt = baselineSettings.find(address);
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		auto* value = setting ? GetCatalogSerializedValue(snapshot, *setting) : nullptr;
		if (baselineIt != baselineSettings.end() && value &&
			IsCompatibleSceneSettingValue(*value, baselineIt->second))
			*value = baselineIt->second;
	}

	auto [snapshotIt, _] = featureBaseSnapshots.emplace(featureShortName, std::move(snapshot));
	return &snapshotIt->second;
}

void SceneSettingsManager::EnsureBaselines(std::span<const SettingAddress> addresses)
{
	std::map<std::string, std::vector<const SettingAddress*>> missingByFeature;
	for (const auto& address : addresses)
		if (!baselineSettings.contains(address))
			missingByFeature[address.featureShortName].push_back(&address);

	for (const auto& [featureShortName, missing] : missingByFeature) {
		const auto* snapshot = GetFeatureBaseSnapshot(featureShortName);
		if (!snapshot)
			continue;
		for (const auto* address : missing) {
			auto* setting = FindAllowedCatalogSetting(
				address->featureShortName, address->settingPath, address->settingKey);
			const auto* value = setting ? GetCatalogSerializedValue(*snapshot, *setting) : nullptr;
			if (value && IsSceneSettingPrimitive(*value))
				baselineSettings.try_emplace(*address, *value);
		}
	}
}

json SceneSettingsManager::GetBaselineValue(const SettingAddress& address)
{
	if (!FindAllowedCatalogSetting(address.featureShortName, address.settingPath, address.settingKey))
		return {};
	if (auto it = baselineSettings.find(address); it != baselineSettings.end())
		return it->second;
	EnsureBaselines(std::span{ &address, 1 });
	if (auto it = baselineSettings.find(address); it != baselineSettings.end())
		return it->second;
	return {};
}

bool SceneSettingsManager::ResolvedValuesEqual(const json& lhs, const json& rhs)
{
	if (lhs.is_number() && rhs.is_number())
		return std::abs(lhs.get<double>() - rhs.get<double>()) < kBlendEpsilon;
	return lhs == rhs;
}

// --- Unified Persistence ---

static json EntryToJson(const SceneSettingsManager::SettingEntry& entry)
{
	json item = entry.serializedTemplate.is_object() ? entry.serializedTemplate : json::object();
	item["feature"] = entry.featureShortName;
	if (!entry.settingPath.empty())
		item["path"] = entry.settingPath;
	else
		item.erase("path");
	item["setting"] = entry.settingKey;
	item["value"] = entry.value;
	item["originalValue"] = entry.originalValue;
	item["paused"] = entry.paused;
	if (entry.transitionSeconds)
		item["transitionSeconds"] = *entry.transitionSeconds;
	else
		item.erase("transitionSeconds");
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		item["period"] = SceneSettingsManager::GetPeriodName(entry.period);
	else
		item.erase("period");
	return item;
}

static json UserEntriesToArray(const std::vector<SceneSettingsManager::SettingEntry>& entries, bool transitionOnly = false)
{
	json arr = json::array();
	for (const auto& entry : entries)
		if (entry.source == SceneSettingsManager::EntrySource::User &&
			(!transitionOnly || IsNumericValue(entry.value)))
			arr.push_back(EntryToJson(entry));
	return arr;
}

static void AppendRawEntries(json& arr, const std::vector<json>& rawEntries)
{
	if (!arr.is_array())
		arr = json::array();
	for (const auto& raw : rawEntries)
		arr.push_back(raw);
}

static bool ShouldSerializeUserSection(const json& data, std::string_view key, bool expectObject, bool modified)
{
	auto it = data.find(std::string(key));
	return modified || it == data.end() || (expectObject ? it->is_object() : it->is_array());
}

void SceneSettingsManager::SaveAllUserSettings()
{
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();
	const bool weatherLoaded = TryEnsureWeatherDataLoaded();
	const bool locationLoaded = TryEnsureLocationDataLoaded();
	if (!userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object()) {
		if (!userSettingsWriteBlockedWarning) {
			logger::error("[SceneSettings] Refusing to overwrite SceneManager.json because its existing document is invalid");
			userSettingsWriteBlockedWarning = true;
		}
		deferredSceneChangesPending = true;
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
		return;
	}

	auto path = GetUserSettingsFilePath();
	json data = preservedUserSettingsRoot;
	if (ShouldSerializeUserSection(data, "interiorOnly", false, interiorUserSettingsModified)) {
		data["interiorOnly"] = UserEntriesToArray(GetEntries(SceneType::InteriorOnly));
		AppendRawEntries(data["interiorOnly"], unresolvedUserEntries[SceneType::InteriorOnly]);
	}
	if (ShouldSerializeUserSection(data, "timeOfDay", false, timeOfDayUserSettingsModified)) {
		data["timeOfDay"] = UserEntriesToArray(GetEntries(SceneType::TimeOfDay), true);
		AppendRawEntries(data["timeOfDay"], unresolvedUserEntries[SceneType::TimeOfDay]);
	}

	// Weather entries (keyed by SPID)
	if (weatherLoaded && ShouldSerializeUserSection(data, "weather", true, weatherUserSettingsModified)) {
		json weatherObj = unresolvedWeatherUserSettings.is_object() ?
		                      unresolvedWeatherUserSettings :
		                      json::object();
		std::set<RE::FormID> weatherIds;
		for (const auto& [weatherId, _] : weatherSceneConfigs)
			weatherIds.insert(weatherId);
		for (const auto& [weatherId, _] : weatherShowTimeOfDay_)
			weatherIds.insert(weatherId);

		for (auto weatherId : weatherIds) {
			if (weatherId == 0)
				continue;
			const auto spid = Util::FormIdToSpid(weatherId);
			auto configIt = weatherSceneConfigs.find(weatherId);
			auto userEntries = configIt != weatherSceneConfigs.end() ?
			                       UserEntriesToArray(configIt->second.entries, true) :
			                       json::array();
			auto showIt = weatherShowTimeOfDay_.find(weatherId);
			const bool hasShowPreference = showIt != weatherShowTimeOfDay_.end();

			auto rawIt = weatherObj.find(spid);
			const bool hasRaw = rawIt != weatherObj.end();
			if (userEntries.empty() && !hasShowPreference && !hasRaw)
				continue;
			if (hasRaw && !rawIt->is_object()) {
				if (userEntries.empty() && !hasShowPreference)
					continue;
				*rawIt = json::object();
			}

			json weatherEntry = hasRaw ? *rawIt : json::object();
			if (!userEntries.empty()) {
				if (auto entriesIt = weatherEntry.find("entries");
					entriesIt != weatherEntry.end() && entriesIt->is_array())
					for (const auto& rawEntry : *entriesIt)
						userEntries.push_back(rawEntry);
				weatherEntry["entries"] = std::move(userEntries);
			}
			if (hasShowPreference)
				weatherEntry["showTimeOfDay"] = showIt->second;
			weatherObj[spid] = std::move(weatherEntry);
		}
		data["weather"] = std::move(weatherObj);
	}

	if (locationLoaded && ShouldSerializeUserSection(data, "location", true, locationUserSettingsModified)) {
		json locationObj = unresolvedLocationUserSettings.is_object() ?
		                       unresolvedLocationUserSettings :
		                       json::object();
		if (locationTransitionModified)
			locationObj["transitionSeconds"] = locationTransitionSeconds;
		for (const auto& [_, config] : locationSceneConfigs) {
			auto userEntries = UserEntriesToArray(config.entries);
			if (userEntries.empty())
				continue;
			const auto* sectionName = GetLocationSectionName(config.type);
			auto& section = locationObj[sectionName];
			if (!section.is_object())
				section = json::object();
			auto& rawConfig = section[config.formKey];
			json locationEntry = rawConfig.is_object() ? rawConfig : json::object();
			if (auto entriesIt = locationEntry.find("entries");
				entriesIt != locationEntry.end() && entriesIt->is_array())
				for (const auto& rawEntry : *entriesIt)
					userEntries.push_back(rawEntry);
			locationEntry["type"] = GetLocationTargetTypeName(config.type);
			locationEntry["name"] = config.name;
			locationEntry["coc"] = config.cocCode;
			locationEntry["entries"] = std::move(userEntries);
			rawConfig = std::move(locationEntry);
		}
		data["location"] = std::move(locationObj);
	}

	const bool saved = WriteJsonAtomically(path, data, kOverwriteJsonIndent, "SceneManager.json");
	if (saved) {
		preservedUserSettingsRoot = data;
		if (locationLoaded && locationTransitionModified) {
			if (!unresolvedLocationUserSettings.is_object())
				unresolvedLocationUserSettings = json::object();
			unresolvedLocationUserSettings["transitionSeconds"] = locationTransitionSeconds;
		}
		interiorUserSettingsModified = false;
		timeOfDayUserSettingsModified = false;
		weatherUserSettingsModified = false;
		locationUserSettingsModified = false;
		locationTransitionModified = false;
		userSettingsWriteBlockedWarning = false;
		logger::info("[SceneSettings] Saved SceneManager.json");
	}

	deferredSceneChangesPending = !saved;
	if (!saved)
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
}

static bool LoadEntryFromJson(const nlohmann::json& item, SceneSettingsManager::SettingEntry& entry,
	bool requirePeriod, const char* typeName,
	std::optional<SceneSettingsManager::SceneType> allowedSceneType = std::nullopt,
	bool requireNumericValue = false)
{
	using SSM = SceneSettingsManager;

	if (!item.contains("feature") || !item.contains("setting") || !item.contains("value")) {
		logger::warn("[SceneSettings] {} entry missing feature/setting/value fields", typeName);
		return false;
	}
	if (!item["feature"].is_string() || !item["setting"].is_string()) {
		logger::warn("[SceneSettings] {} entry feature/setting not strings", typeName);
		return false;
	}

	entry.featureShortName = item["feature"].get<std::string>();
	entry.settingPath.clear();
	if (auto it = item.find("path"); it != item.end()) {
		if (!it->is_array()) {
			logger::warn("[SceneSettings] {} entry path is not an array", typeName);
			return false;
		}
		for (const auto& part : *it) {
			if (!part.is_string()) {
				logger::warn("[SceneSettings] {} entry path contains a non-string component", typeName);
				return false;
			}
			entry.settingPath.push_back(part.get<std::string>());
		}
	}
	entry.settingKey = item["setting"].get<std::string>();
	entry.value = item["value"];
	entry.originalValue = item.value("originalValue", entry.value);
	entry.serializedTemplate = item.is_object() ? item : json::object();
	if (auto pausedIt = item.find("paused"); pausedIt != item.end() && !pausedIt->is_boolean()) {
		logger::warn("[SceneSettings] {} entry paused field is not boolean", typeName);
		return false;
	}
	entry.paused = item.value("paused", false);
	entry.source = SSM::EntrySource::User;

	auto sceneType = allowedSceneType.value_or(requirePeriod ? SSM::SceneType::TimeOfDay : SSM::SceneType::InteriorOnly);
	if (auto transitionIt = item.find("transitionSeconds"); transitionIt != item.end()) {
		if (sceneType != SSM::SceneType::Location || !transitionIt->is_number()) {
			logger::warn("[SceneSettings] {} entry transitionSeconds is not valid for this scene type", typeName);
			return false;
		}
		const auto seconds = transitionIt->get<float>();
		if (!std::isfinite(seconds) || seconds < 0.0f || seconds > SSM::kMaxLocationTransitionSeconds) {
			logger::warn("[SceneSettings] {} entry transitionSeconds is outside 0..{}",
				typeName, SSM::kMaxLocationTransitionSeconds);
			return false;
		}
		entry.transitionSeconds = seconds;
	}
	if (!SSM::IsFeatureAllowedForType(sceneType, entry.featureShortName)) {
		logger::warn("[SceneSettings] {} entry feature '{}' is not allowed for this scene type", typeName, entry.featureShortName);
		return false;
	}

	if (requirePeriod) {
		if (!item.contains("period") || !item["period"].is_string()) {
			logger::warn("[SceneSettings] {} entry {}.{} missing period - skipping", typeName, entry.featureShortName, entry.settingKey);
			return false;
		}
		entry.period = SSM::GetPeriodFromName(item["period"].get<std::string>());
		if (entry.period == SSM::TimeOfDayPeriod::Count) {
			logger::warn("[SceneSettings] {} entry {}.{} has invalid period '{}' - skipping", typeName, entry.featureShortName, entry.settingKey, item["period"].get<std::string>());
			return false;
		}
		if (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue)) {
			logger::warn("[SceneSettings] {} entry {} is not a float setting - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
		if (!std::isfinite(entry.value.get<float>())) {
			logger::warn("[SceneSettings] {} entry {} has non-finite value - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
	}
	if (requireNumericValue && (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue) ||
								   !std::isfinite(entry.value.get<float>()))) {
		logger::warn("[SceneSettings] {} entry {} is not a finite float setting - skipping",
			typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
		return false;
	}

	const bool requireNumeric = requirePeriod || requireNumericValue;
	if (!ValidateSceneSettingEntry(typeName, sceneType, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.value, requireNumeric) ||
		!ValidateSceneSettingEntry(typeName, sceneType, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.originalValue, requireNumeric))
		return false;
	if (entry.transitionSeconds &&
		(!IsNumericValue(entry.value) || !FindAllowedCatalogSetting(
											 entry.featureShortName, entry.settingPath, entry.settingKey, true))) {
		logger::warn("[SceneSettings] {} entry {} has a transition on a discrete setting; preserving it without loading",
			typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
		return false;
	}

	entry.displayName = GetSceneSettingDisplayName(entry.featureShortName, entry.settingPath, entry.settingKey);
	return true;
}

void SceneSettingsManager::LoadAllUserSettings()
{
	auto path = GetUserSettingsFilePath();
	logger::info("[SceneSettings] Loading user settings from: {}", path.string());
	for (auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay })
		std::erase_if(entries[type], [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	unresolvedUserEntries[SceneType::InteriorOnly].clear();
	unresolvedUserEntries[SceneType::TimeOfDay].clear();
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	interiorUserSettingsModified = false;
	timeOfDayUserSettingsModified = false;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = !ec;
		preservedUserSettingsRoot = json::object();
		if (ec)
			logger::error("[SceneSettings] Could not inspect SceneManager.json: {}", ec.message());
		else
			logger::info("[SceneSettings] SceneManager.json not found at {}", path.string());
		return;
	}

	try {
		std::ifstream file(path);
		if (!file.is_open()) {
			userSettingsDocumentLoaded = true;
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] Could not open SceneManager.json for reading");
			return;
		}

		json data = json::parse(file, nullptr, false);
		userSettingsDocumentLoaded = true;
		preservedUserSettingsRoot = data;
		if (!data.is_object()) {
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] SceneManager.json must contain a valid JSON object; automatic saves are blocked");
			return;
		}
		userSettingsDocumentWritable = true;
		// Interior Only
		if (data.contains("interiorOnly") && data["interiorOnly"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::InteriorOnly);
			int loaded = 0;
			for (const auto& item : data["interiorOnly"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "InteriorOnly")) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::InteriorOnly, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} InteriorOnly user settings", loaded);
		} else if (data.contains("interiorOnly"))
			logger::warn("[SceneSettings] Preserving non-array interiorOnly section");

		// Time of Day
		if (data.contains("timeOfDay") && data["timeOfDay"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::TimeOfDay);
			int loaded = 0;
			for (const auto& item : data["timeOfDay"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "TimeOfDay")) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::TimeOfDay, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} TimeOfDay user settings", loaded);
		} else if (data.contains("timeOfDay"))
			logger::warn("[SceneSettings] Preserving non-array timeOfDay section");

		// Weather and location are loaded lazily once game data is available.

		logger::info("[SceneSettings] Loaded SceneManager.json (non-weather)");
	} catch (const std::exception& e) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = false;
		logger::error("[SceneSettings] Failed to load SceneManager.json: {}", e.what());
	}
}

void SceneSettingsManager::LoadLocationUserSettings(const json& data)
{
	for (auto& [_, config] : locationSceneConfigs)
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	unresolvedLocationUserSettings = json::object();
	locationUserSettingsModified = false;
	locationTransitionSeconds = kDefaultLocationTransitionSeconds;
	locationTransitionModified = false;
	auto locationIt = data.find("location");
	if (locationIt == data.end())
		return;
	if (!locationIt->is_object()) {
		logger::warn("[SceneSettings] Preserving non-object location section");
		return;
	}
	unresolvedLocationUserSettings = *locationIt;
	if (auto transitionIt = locationIt->find("transitionSeconds"); transitionIt != locationIt->end()) {
		if (transitionIt->is_number()) {
			const auto seconds = transitionIt->get<float>();
			if (std::isfinite(seconds) && seconds >= 0.0f && seconds <= kMaxLocationTransitionSeconds)
				locationTransitionSeconds = seconds;
			else
				logger::warn("[SceneSettings] Location transitionSeconds is outside 0..{}; preserving it",
					kMaxLocationTransitionSeconds);
		} else {
			logger::warn("[SceneSettings] Location transitionSeconds must be numeric; preserving it");
		}
	}
	const auto loadSection = [&](const char* sectionName, LocationTargetType type) {
		auto sectionIt = locationIt->find(sectionName);
		if (sectionIt == locationIt->end() || !sectionIt->is_object())
			return;
		json preservedSection = json::object();

		for (const auto& [formKey, rawConfig] : sectionIt->items()) {
			if (formKey.empty()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
			if (!rawConfig.is_object()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto configContext = std::format("Location config '{}'", formKey);
			std::string name;
			std::string persistedType;
			std::string cocCode;
			const auto expectedType = GetLocationTargetTypeName(type);
			persistedType = expectedType;
			if (!ReadOptionalStringField(rawConfig, "name", name, configContext) ||
				!ReadOptionalStringField(rawConfig, "type", persistedType, configContext) ||
				!ReadOptionalStringField(rawConfig, "coc", cocCode, configContext)) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (persistedType != expectedType) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			auto& config = GetLocationConfigMut(type, canonicalFormKey, name);
			if (!cocCode.empty())
				config.cocCode = cocCode;
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt == rawConfig.end()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (!entriesIt->is_array()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			auto preservedConfig = rawConfig;
			preservedConfig["entries"] = json::array();
			bool hasValidEntry = false;

			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "Location", SceneType::Location)) {
					preservedConfig["entries"].push_back(item);
					continue;
				}
				hasValidEntry = true;
				if (HasLocationEntry(type, canonicalFormKey, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User)) {
					preservedConfig["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
			}
			if (hasValidEntry && formKey != canonicalFormKey) {
				preservedConfig.erase("type");
				preservedConfig.erase("name");
				preservedConfig.erase("coc");
			}
			preservedSection[formKey] = std::move(preservedConfig);
		}
		unresolvedLocationUserSettings[sectionName] = std::move(preservedSection);
	};

	loadSection("regions", LocationTargetType::Region);
	loadSection("locations", LocationTargetType::Location);
	loadSection("cells", LocationTargetType::Cell);
}

void SceneSettingsManager::LoadWeatherUserSettings()
{
	for (auto& [_, config] : weatherSceneConfigs)
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	weatherShowTimeOfDay_.clear();
	unresolvedWeatherUserSettings = json::object();
	weatherUserSettingsModified = false;
	if (!userSettingsDocumentLoaded || !userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object())
		return;

	try {
		auto weatherIt = preservedUserSettingsRoot.find("weather");
		if (weatherIt == preservedUserSettingsRoot.end())
			return;
		if (!weatherIt->is_object()) {
			logger::warn("[SceneSettings] Preserving non-object weather section");
			return;
		}

		logger::info("[SceneSettings] Weather section found with {} entries", weatherIt->size());
		for (const auto& [spidKey, weatherData] : weatherIt->items()) {
			logger::info("[SceneSettings] Processing weather SPID '{}'", spidKey);
			RE::FormID weatherId = Util::SpidToFormId(spidKey);
			if (weatherId == 0) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather SPID '{}' could not be resolved - skipping", spidKey);
				continue;
			}
			const auto canonicalSpid = Util::FormIdToSpid(weatherId);
			logger::info("[SceneSettings] Resolved SPID '{}' to FormID 0x{:X}", spidKey, weatherId);
			if (!weatherData.is_object()) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather config '{}' is not an object - preserving", spidKey);
				continue;
			}
			auto preservedWeather = weatherData;

			// Load showTimeOfDay preference
			if (auto showIt = weatherData.find("showTimeOfDay"); showIt != weatherData.end()) {
				if (!showIt->is_boolean()) {
					logger::warn("[SceneSettings] Weather config '{}' showTimeOfDay is not boolean - preserving", spidKey);
				} else {
					weatherShowTimeOfDay_[weatherId] = showIt->get<bool>();
					preservedWeather.erase("showTimeOfDay");
				}
			}

			auto entriesIt = weatherData.find("entries");
			if (entriesIt == weatherData.end()) {
				unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
				continue;
			}
			if (!entriesIt->is_array()) {
				unresolvedWeatherUserSettings[spidKey] = preservedWeather;
				logger::warn("[SceneSettings] Weather config '{}' entries is not an array - preserving", spidKey);
				continue;
			}
			preservedWeather["entries"] = json::array();

			auto& config = GetWeatherConfigMut(weatherId);
			int loaded = 0;
			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "Weather")) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				if (HasWeatherEntryForPeriod(weatherId, entry.featureShortName, entry.settingPath,
						entry.settingKey, entry.period, EntrySource::User)) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} weather entries for {}", loaded, spidKey);
			unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
		}

		logger::info("[SceneSettings] Loaded weather user settings");
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load weather user settings: {}", e.what());
	}
}

void SceneSettingsManager::DiscoverOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	const auto previousEntryCount = GetEntries(type).size();
	// TimeOfDay has period subfolders; delegate to a shared loader
	if (type == SceneType::TimeOfDay) {
		auto basePath = GetOverwritesPath(type);
		for (int i = 0; i < kPeriodCount; ++i) {
			auto period = static_cast<TimeOfDayPeriod>(i);
			auto periodPath = basePath / GetPeriodName(period);
			DiscoverOverwritesInDir(type, periodPath, period);
		}
	} else {
		DiscoverOverwritesInDir(type, GetOverwritesPath(type));
	}

	if (GetEntries(type).size() != previousEntryCount && type == SceneType::TimeOfDay)
		++sceneValueRevision;
	if (GetEntries(type).size() != previousEntryCount)
		BumpEntryPresentationRevision();
}

static bool ParseOverwriteFileEntries(const std::filesystem::path& filePath,
	SceneSettingsManager::SceneType allowedType, bool requireNumeric,
	std::vector<SceneSettingsManager::SettingEntry>& outEntries)
{
	using SSM = SceneSettingsManager;

	json data;
	if (!ReadBoundedSceneJson(filePath, data))
		return false;

	std::string featureShortName = data.value(kFeatureKey, "");
	if (featureShortName.empty()) {
		auto stem = filePath.stem().string();
		auto lastUnderscore = stem.rfind('_');
		if (lastUnderscore != std::string::npos)
			featureShortName = stem.substr(lastUnderscore + 1);
	}

	auto* featurePtr = Feature::FindFeatureByShortName(featureShortName);
	if (!featurePtr || !SSM::IsFeatureAllowedForType(allowedType, featureShortName))
		return false;

	bool foundAny = false;
	CollectOverwriteEntries(data, {}, [&](const auto& settingPath, const auto& key, const auto& value) {
		if (!ValidateSceneSettingEntry(
				"Overwrite", allowedType, featureShortName, settingPath, key, value, requireNumeric))
			return;

		SSM::SettingEntry entry;
		entry.featureShortName = featureShortName;
		entry.settingPath = settingPath;
		entry.settingKey = key;
		entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, key);
		entry.value = value;
		entry.originalValue = entry.value;
		entry.source = SSM::EntrySource::Overwrite;
		entry.sourceFilename = filePath.filename().string();

		entry.sourcePath = filePath;
		outEntries.push_back(std::move(entry));
		foundAny = true;
	});
	return foundAny;
}

void SceneSettingsManager::DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir, TimeOfDayPeriod period)
{
	auto typeName = GetSceneTypeName(type);

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec))
		return;

	logger::info("[SceneSettings] Discovering {} overwrites in: {}", typeName, dir.string());

	bool requireNumeric = (type == SceneType::TimeOfDay);
	auto& vec = GetEntriesMut(type);
	int filesFound = 0, overwritesLoaded = 0;
	for (const auto& filePath : GetSortedJsonFiles(dir, std::format("{} overwrite files", typeName))) {
		filesFound++;
		try {
			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(filePath, type, requireNumeric, parsedEntries))
				continue;
			for (auto& entry : parsedEntries) {
				entry.period = period;
				if (AddOverwriteEntryIfUnique(vec, std::move(entry), typeName))
					overwritesLoaded++;
			}
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load {} overwrite '{}': {}", typeName, filePath.filename().string(), e.what());
		}
	}

	if (filesFound > 0)
		logger::info("[SceneSettings] {} overwrite scan: {} files, {} loaded", typeName, filesFound, overwritesLoaded);
}

void SceneSettingsManager::LoadAll()
{
	if (!dataLoaded) {
		dataLoaded = true;
		DiscoverOverwrites(SceneType::InteriorOnly);
		DiscoverOverwrites(SceneType::TimeOfDay);
		LoadAllUserSettings();
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
	}
	TryEnsureLocationDataLoaded();
}

void SceneSettingsManager::OnDataLoaded()
{
	gameDataReady = true;
	if (dataLoaded)
		TryEnsureLocationDataLoaded();
}

bool SceneSettingsManager::TryEnsureLocationDataLoaded()
{
	if (locationDataLoaded)
		return true;
	if (!gameDataReady || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	try {
		DiscoverLocationOverwrites();
		if (userSettingsDocumentLoaded && userSettingsDocumentWritable && preservedUserSettingsRoot.is_object())
			LoadLocationUserSettings(preservedUserSettingsRoot);
		locationDataLoaded = true;
		locationTargetsCached = false;
		locationManagementTargetsCached = false;
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
		return true;
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load location settings: {}", e.what());
		return false;
	}
}

bool SceneSettingsManager::TryEnsureWeatherDataLoaded()
{
	if (weatherDataLoaded)
		return true;
	if (!globals::game::sky || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	weatherDataLoaded = true;
	LoadWeatherData();
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	activeEntryCacheDirty = true;
	resolverDirty = true;
	return true;
}

void SceneSettingsManager::LoadWeatherData()
{
	DiscoverWeatherOverwrites();
	LoadWeatherUserSettings();
}

RE::FormID SceneSettingsManager::GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const
{
	if (!sky)
		return 0;
	if (weatherLerp >= 1.0f) {
		if (sky->currentWeather)
			cachedPreviousWeatherId = sky->currentWeather->GetFormID();
		return 0;
	}
	if (sky->lastWeather)
		cachedPreviousWeatherId = sky->lastWeather->GetFormID();
	return cachedPreviousWeatherId;
}

// --- Per-Weather Scene Settings ---

const SceneSettingsManager::WeatherSceneConfig SceneSettingsManager::kEmptyWeatherConfig{};

const SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return kEmptyWeatherConfig;

	auto it = weatherSceneConfigs.find(weatherId);
	return (it != weatherSceneConfigs.end()) ? it->second : kEmptyWeatherConfig;
}

SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfigMut(RE::FormID weatherId)
{
	return weatherSceneConfigs[weatherId];
}

bool SceneSettingsManager::HasWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	return it != weatherSceneConfigs.end() && std::any_of(it->second.entries.begin(), it->second.entries.end(),
												  [](const auto& entry) { return IsNumericValue(entry.value); });
}

void SceneSettingsManager::PrepareWeatherUserSettingsMutation(RE::FormID weatherId, bool replaceMalformedEntries)
{
	weatherUserSettingsModified = true;
	if (!unresolvedWeatherUserSettings.is_object())
		unresolvedWeatherUserSettings = json::object();
	const auto canonicalSpid = Util::FormIdToSpid(weatherId);
	const auto normalizedSpid = NormalizeLocationFormKey(canonicalSpid);
	if (replaceMalformedEntries) {
		for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items()) {
			if (!rawWeather.is_object() || NormalizeLocationFormKey(rawSpid) != normalizedSpid)
				continue;
			auto entriesIt = rawWeather.find("entries");
			if (entriesIt != rawWeather.end() && !entriesIt->is_array())
				*entriesIt = json::array();
		}
	}

	auto& rawWeather = unresolvedWeatherUserSettings[canonicalSpid];
	if (!rawWeather.is_object())
		rawWeather = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawWeather.find("entries");
		if (entriesIt != rawWeather.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

std::optional<float> SceneSettingsManager::ResolveWeatherLowerValue(RE::FormID weatherId,
	const SettingAddress& address, TimeOfDayPeriod period, EntrySource selectedSource)
{
	const auto periodIndex = static_cast<int>(period);
	if (periodIndex < 0 || periodIndex >= kPeriodCount)
		return std::nullopt;
	auto baseline = GetBaselineValue(address);
	if (!IsNumericValue(baseline))
		return std::nullopt;
	const auto baselineValue = baseline.get<float>();
	if (!std::isfinite(baselineValue))
		return std::nullopt;

	float lowerValue = baselineValue;
	const auto& timeOfDayValues = BuildTimeOfDayValueGroups();
	if (auto valueIt = timeOfDayValues.find(address); valueIt != timeOfDayValues.end())
		lowerValue = valueIt->second[periodIndex].value_or(baselineValue);
	if (selectedSource != EntrySource::Overwrite)
		return lowerValue;

	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end())
		return lowerValue;
	for (const auto& entry : configIt->second.entries) {
		if (entry.source != EntrySource::User || entry.period != period || !IsEntryActive(entry) ||
			!IsNumericValue(entry.value) ||
			!IsSameSetting(entry, address.featureShortName, address.settingPath, address.settingKey))
			continue;
		const auto value = entry.value.get<float>();
		if (std::isfinite(value))
			lowerValue = value;
	}
	return lowerValue;
}

bool SceneSettingsManager::AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
	bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;
	if (!IsSettingAllowedForType(
			SceneType::TimeOfDay, featureShortName, settingPath, settingKey))
		return false;

	// All weather entries are per-period
	if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount)
		return false;
	if (HasWeatherEntryForPeriod(weatherId, featureShortName, settingPath, settingKey, period, EntrySource::User))
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, period, EntrySource::User);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Weather", SceneType::TimeOfDay, featureShortName, settingPath, settingKey, *lowerValue, true))
		return false;

	auto& config = GetWeatherConfigMut(weatherId);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	entry.source = EntrySource::User;
	entry.period = period;
	config.entries.push_back(std::move(entry));
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	PrepareWeatherUserSettingsMutation(weatherId, true);
	if (deferSave) {
		MarkDeferredSceneChanges();
	} else {
		CommitSceneSettingChanges();
	}
	return true;
}

void SceneSettingsManager::RemoveWeatherSetting(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	const auto previousSize = it->second.entries.size();
	const auto entry = it->second.entries[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty()) {
		const auto backingPath = GetWeatherOverwritePath(weatherId, entry);
		if (!RemoveSettingFromOverwriteFile(backingPath, entry.settingPath, entry.settingKey))
			return;
		std::erase_if(it->second.entries, [&](const auto& candidate) {
			return candidate.source == EntrySource::Overwrite &&
			       GetWeatherOverwritePath(weatherId, candidate) == backingPath &&
			       IsSameSetting(candidate, entry.featureShortName, entry.settingPath, entry.settingKey);
		});
	} else {
		it->second.entries.erase(it->second.entries.begin() + static_cast<ptrdiff_t>(index));
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	if (it->second.entries.size() != previousSize)
		++sceneValueRevision;
	if (it->second.entries.size() != previousSize)
		BumpEntryPresentationRevision();
	ReapplyIfActive();
}

void SceneSettingsManager::DeleteAllWeatherUserSettings(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return;
	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt != weatherSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			++sceneValueRevision;
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareWeatherUserSettingsMutation(weatherId, false);
	const auto normalizedSpid = NormalizeLocationFormKey(Util::FormIdToSpid(weatherId));
	for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items())
		if (rawWeather.is_object() && NormalizeLocationFormKey(rawSpid) == normalizedSpid)
			rawWeather.erase("entries");
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseWeatherEntry(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::SetWeatherEntriesPaused(
	RE::FormID weatherId, std::span<const size_t> indices, bool paused)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end())
		return;
	bool changed = false;
	bool userEntriesChanged = false;
	for (const auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		auto& entry = configIt->second.entries[index];
		if (entry.paused == paused)
			continue;
		entry.paused = paused;
		changed = true;
		userEntriesChanged |= entry.source == EntrySource::User;
	}
	if (!changed)
		return;

	++sceneValueRevision;
	BumpEntryPresentationRevision();
	if (userEntriesChanged) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateWeatherEntryValues(weatherId, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateWeatherEntryValues(
	RE::FormID weatherId, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Weather", SceneType::TimeOfDay, it->second.entries, updates, true, userEntriesChanged))
		return;
	++sceneValueRevision;
	if (userEntriesChanged) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, entry.period, entry.source);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Weather", SceneType::TimeOfDay, entry.featureShortName,
						   entry.settingPath, entry.settingKey, *lowerValue, true))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	++sceneValueRevision;
	if (entry.source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

bool SceneSettingsManager::HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period, std::optional<EntrySource> source)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return false;
	for (const auto& e : it->second.entries)
		if (IsSameSetting(e, featureShortName, settingPath, settingKey) && e.period == period &&
			(!source || e.source == *source))
			return true;
	return false;
}

// --- Per-Weather Persistence ---

bool SceneSettingsManager::IsWeatherShowTimeOfDay(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherShowTimeOfDay_.find(weatherId);
	return it != weatherShowTimeOfDay_.end() && it->second;
}

void SceneSettingsManager::SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	weatherShowTimeOfDay_[weatherId] = show;
	PrepareWeatherUserSettingsMutation(weatherId, false);
	SaveAllUserSettings();
}

namespace
{
	using CopyGroupKey = std::tuple<std::string, std::vector<std::string>, std::string,
		std::int8_t, std::uint8_t, SceneSettingsManager::SettingControlType>;

	CopyGroupKey GetCopyGroupKey(const SceneSettingsManager::SettingIdentity& identity)
	{
		SceneSettingsManager::SettingEntry entry{
			.featureShortName = identity.featureShortName,
			.settingPath = identity.settingPath,
			.settingKey = identity.settingKey,
		};
		SceneSettingsManager::SettingControlInfo info;
		const bool aggregate = SceneSettingsManager::GetSettingControlInfo(entry, info) &&
		                       info.controlType != SceneSettingsManager::SettingControlType::Scalar;
		return { identity.featureShortName,
			aggregate ? info.settingPath : identity.settingPath,
			aggregate ? info.settingKey : identity.settingKey,
			aggregate ? info.componentStart : -1,
			aggregate ? info.componentCount : 0,
			aggregate ? info.controlType : SceneSettingsManager::SettingControlType::Scalar };
	}

	bool IsValidCopyScope(SceneSettingsManager::CopyScope scope)
	{
		return scope == SceneSettingsManager::CopyScope::EntireContext ||
		       scope == SceneSettingsManager::CopyScope::Setting;
	}

	bool IsValidCopyConflictPolicy(SceneSettingsManager::CopyConflictPolicy policy)
	{
		return policy == SceneSettingsManager::CopyConflictPolicy::SkipExisting ||
		       policy == SceneSettingsManager::CopyConflictPolicy::OverwriteExisting ||
		       policy == SceneSettingsManager::CopyConflictPolicy::Cancel;
	}

	const char* GetCopyPeriodName(SceneSettingsManager::TimeOfDayPeriod period)
	{
		switch (period) {
		case SceneSettingsManager::TimeOfDayPeriod::Dawn:
			return T("feature.scene_manager.period.dawn", "Dawn");
		case SceneSettingsManager::TimeOfDayPeriod::Sunrise:
			return T("feature.scene_manager.period.sunrise", "Sunrise");
		case SceneSettingsManager::TimeOfDayPeriod::Day:
			return T("feature.scene_manager.period.day", "Day");
		case SceneSettingsManager::TimeOfDayPeriod::Sunset:
			return T("feature.scene_manager.period.sunset", "Sunset");
		case SceneSettingsManager::TimeOfDayPeriod::Dusk:
			return T("feature.scene_manager.period.dusk", "Dusk");
		case SceneSettingsManager::TimeOfDayPeriod::Night:
			return T("feature.scene_manager.period.night", "Night");
		default:
			return "";
		}
	}

	const char* GetCopyLocationTypeName(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Region:
			return T("feature.scene_manager.location.target_region", "Region");
		case SceneSettingsManager::LocationTargetType::Location:
			return T("feature.scene_manager.location.target_location", "Location");
		case SceneSettingsManager::LocationTargetType::Cell:
			return T("feature.scene_manager.location.target_cell", "Cell");
		default:
			return "";
		}
	}

	bool EntryBelongsToContext(const SceneSettingsManager::SettingEntry& entry,
		const SceneSettingsManager::SceneContextId& context)
	{
		return context.type == SceneSettingsManager::SceneContextType::Location ||
		       entry.period == context.period;
	}

	template <class Form>
	std::string GetLocationTargetDisplayName(const Form* form)
	{
		if (const char* fullName = form->GetFullName(); fullName && fullName[0] != '\0')
			return std::string(fullName);
		return Util::GetFormDisplayName(form->GetFormID());
	}

	std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(
		RE::BGSLocation* location, RE::TESObjectCELL* cell)
	{
		const auto cocCode = cell ? Util::GetFormEditorID(cell) : std::string{};
		std::vector<RE::BGSLocation*> locationChain;
		std::set<RE::FormID> visited;
		for (auto* current = location;
			current && visited.insert(current->GetFormID()).second; current = current->parentLoc)
			locationChain.push_back(current);
		std::reverse(locationChain.begin(), locationChain.end());

		std::vector<SceneSettingsManager::LocationTarget> targets;
		RE::TESRegion* region = nullptr;
		if (cell && cell->IsExteriorCell()) {
			if (auto* player = globals::game::player;
				player && player->GetParentCell() == cell && globals::game::sky)
				region = globals::game::sky->region;
			if (!region) {
				if (auto* regions = cell->GetRegionList(false)) {
					for (auto* candidate : *regions) {
						if (!candidate)
							continue;
						region = candidate;
						break;
					}
				}
			}
		}
		if (region) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Region,
				.formKey = Util::GetFormFileKey(region),
				.name = Util::GetFormDisplayName(region->GetFormID()),
				.cocCode = cocCode,
				.formId = region->GetFormID(),
			});
		}

		for (auto* current : locationChain) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Location,
				.formKey = Util::GetFormFileKey(current),
				.name = GetLocationTargetDisplayName(current),
				.cocCode = cocCode,
				.formId = current->GetFormID(),
			});
		}
		if (cell) {
			targets.push_back({
				.type = SceneSettingsManager::LocationTargetType::Cell,
				.formKey = Util::GetFormFileKey(cell),
				.name = GetLocationTargetDisplayName(cell),
				.cocCode = cocCode,
				.formId = cell->GetFormID(),
			});
		}
		return targets;
	}

	RE::TESForm* ResolveLocationTargetForm(std::string_view formKey)
	{
		const auto parsed = Util::ParseSpid(std::string(formKey));
		if (parsed.localFormId == 0)
			return nullptr;
		const auto formId = parsed.pluginName.empty() ? parsed.localFormId :
		                                                Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
	}

	std::vector<SceneSettingsManager::LocationTarget> ResolveLocationTargetChain(
		SceneSettingsManager::LocationTargetType type, std::string_view formKey)
	{
		if (auto* manager = SceneSettingsManager::GetSingleton()) {
			const auto& currentTargets = manager->GetCurrentLocationTargets();
			const auto normalizedKey = NormalizeLocationFormKey(formKey);
			if (std::any_of(currentTargets.begin(), currentTargets.end(), [&](const auto& target) {
					return target.type == type && NormalizeLocationFormKey(target.formKey) == normalizedKey;
				}))
				return currentTargets;
		}
		auto* form = ResolveLocationTargetForm(formKey);
		if (!form)
			return {};
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Region:
			{
				auto* region = form->As<RE::TESRegion>();
				return region ? std::vector<SceneSettingsManager::LocationTarget>{ {
									.type = SceneSettingsManager::LocationTargetType::Region,
									.formKey = Util::GetFormFileKey(region),
									.name = Util::GetFormDisplayName(region->GetFormID()),
									.formId = region->GetFormID(),
								} } :
				                std::vector<SceneSettingsManager::LocationTarget>{};
			}
		case SceneSettingsManager::LocationTargetType::Location:
			return BuildLocationTargetChain(form->As<RE::BGSLocation>(), nullptr);
		case SceneSettingsManager::LocationTargetType::Cell:
			{
				auto* cell = form->As<RE::TESObjectCELL>();
				return cell ? BuildLocationTargetChain(cell->GetLocation(), cell) :
				              std::vector<SceneSettingsManager::LocationTarget>{};
			}
		default:
			return {};
		}
	}

}

bool SceneSettingsManager::IsValidSceneContext(const SceneContextId& context)
{
	const auto periodIndex = static_cast<int>(context.period);
	switch (context.type) {
	case SceneContextType::TimeOfDay:
		return periodIndex >= 0 && periodIndex < kPeriodCount && context.weatherId == 0 &&
		       context.locationFormKey.empty() &&
		       context.locationType == LocationTargetType::Location;
	case SceneContextType::Weather:
		return context.weatherId != 0 && periodIndex >= 0 && periodIndex < kPeriodCount &&
		       context.locationFormKey.empty() &&
		       context.locationType == LocationTargetType::Location;
	case SceneContextType::Location:
		return context.period == TimeOfDayPeriod::Count && context.weatherId == 0 &&
		       IsValidLocationTargetType(context.locationType) &&
		       !context.locationFormKey.empty();
	default:
		return false;
	}
}

const std::vector<SceneSettingsManager::SettingEntry>* SceneSettingsManager::GetCopyContextEntries(
	const SceneContextId& context) const
{
	if (!IsValidSceneContext(context))
		return nullptr;
	switch (context.type) {
	case SceneContextType::TimeOfDay:
		return &GetEntries(SceneType::TimeOfDay);
	case SceneContextType::Weather:
		if (auto configIt = weatherSceneConfigs.find(context.weatherId);
			configIt != weatherSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	case SceneContextType::Location:
		if (auto configIt = locationSceneConfigs.find(
				GetLocationConfigKey(context.locationType, context.locationFormKey));
			configIt != locationSceneConfigs.end())
			return &configIt->second.entries;
		return nullptr;
	default:
		return nullptr;
	}
}

std::vector<SceneSettingsManager::CopyCandidate> SceneSettingsManager::BuildCopyCandidates(
	const SceneContextId& source, const SceneContextId& destination, CopyScope scope,
	const std::optional<SettingIdentity>& selectedSetting) const
{
	std::vector<CopyCandidate> candidates;
	if (!IsValidSceneContext(source) || !IsValidSceneContext(destination) || !IsValidCopyScope(scope) ||
		(scope == CopyScope::Setting && !selectedSetting))
		return candidates;
	if (destination.type == SceneContextType::Location &&
		ResolveLocationTargetChain(destination.locationType, destination.locationFormKey).empty())
		return candidates;
	const auto sameContext = [&] {
		if (source.type != destination.type)
			return false;
		switch (source.type) {
		case SceneContextType::TimeOfDay:
			return source.period == destination.period;
		case SceneContextType::Weather:
			return source.weatherId == destination.weatherId && source.period == destination.period;
		case SceneContextType::Location:
			return source.locationType == destination.locationType &&
			       NormalizeLocationFormKey(source.locationFormKey) ==
			           NormalizeLocationFormKey(destination.locationFormKey);
		default:
			return false;
		}
	}();
	if (sameContext)
		return candidates;
	const auto* sourceEntries = GetCopyContextEntries(source);
	if (!sourceEntries)
		return candidates;
	const auto* destinationEntries = GetCopyContextEntries(destination);
	static const std::vector<SettingEntry> empty;
	if (!destinationEntries)
		destinationEntries = &empty;

	std::map<SettingIdentity, const SettingEntry*> effectiveEntries;
	for (auto entrySource : { EntrySource::User, EntrySource::Overwrite })
		for (const auto& entry : *sourceEntries)
			if (entry.source == entrySource && !entry.paused && EntryBelongsToContext(entry, source))
				effectiveEntries[{ entry.featureShortName, entry.settingPath, entry.settingKey }] = &entry;
	std::set<SettingIdentity> destinationUserSettings;
	std::set<SettingIdentity> destinationOverwriteSettings;
	for (const auto& entry : *destinationEntries)
		if (entry.source == EntrySource::User && EntryBelongsToContext(entry, destination))
			destinationUserSettings.insert({ entry.featureShortName, entry.settingPath, entry.settingKey });
		else if (entry.source == EntrySource::Overwrite && !entry.paused &&
				 EntryBelongsToContext(entry, destination))
			destinationOverwriteSettings.insert({ entry.featureShortName, entry.settingPath, entry.settingKey });

	const auto selectedGroup = selectedSetting ?
	                               std::optional{ GetCopyGroupKey(*selectedSetting) } :
	                               std::nullopt;
	const bool selectedIsAggregate = selectedGroup &&
	                                 std::get<5>(*selectedGroup) != SettingControlType::Scalar;

	const bool requireNumeric = destination.type != SceneContextType::Location;
	const auto destinationSceneType = requireNumeric ? SceneType::TimeOfDay : SceneType::Location;
	for (const auto& [identity, entry] : effectiveEntries) {
		if (scope == CopyScope::Setting) {
			bool selected = identity == *selectedSetting;
			if (!selected && selectedIsAggregate)
				selected = GetCopyGroupKey(identity) == *selectedGroup;
			if (!selected)
				continue;
		}

		auto* setting = FindAllowedCatalogSetting(
			identity.featureShortName, identity.settingPath, identity.settingKey, requireNumeric);
		const bool compatible = setting &&
		                        IsSettingAllowedForType(destinationSceneType,
									identity.featureShortName, identity.settingPath, identity.settingKey) &&
		                        IsSceneSettingValueAllowed(entry->value, *setting, entry->value, requireNumeric) &&
		                        !destinationOverwriteSettings.contains(identity);
		const bool conflicts = compatible && destinationUserSettings.contains(identity);
		candidates.push_back({
			.setting = identity,
			.displayName = entry->displayName.empty() ?
		                       GetSceneSettingDisplayName(identity.featureShortName, identity.settingPath, identity.settingKey) :
		                       entry->displayName,
			.value = entry->value,
			.compatible = compatible,
			.conflicts = conflicts,
		});
	}
	std::map<CopyGroupKey, std::vector<size_t>> candidateGroups;
	for (size_t index = 0; index < candidates.size(); ++index)
		candidateGroups[GetCopyGroupKey(candidates[index].setting)].push_back(index);
	for (const auto& [_, indices] : candidateGroups) {
		const bool groupCompatible = std::all_of(indices.begin(), indices.end(),
			[&](size_t index) { return candidates[index].compatible; });
		if (groupCompatible)
			continue;
		for (const auto index : indices) {
			candidates[index].compatible = false;
			candidates[index].conflicts = false;
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.displayName, lhs.setting) < std::tie(rhs.displayName, rhs.setting);
	});
	return candidates;
}

std::vector<SceneSettingsManager::CopyCandidate> SceneSettingsManager::GetCopyCandidates(
	const SceneContextId& source, const SceneContextId& destination, CopyScope scope,
	const std::optional<SettingIdentity>& setting) const
{
	return BuildCopyCandidates(source, destination, scope, setting);
}

std::vector<SceneSettingsManager::CopySource> SceneSettingsManager::GetCopySources(
	const SceneContextId& destination, CopyScope scope,
	const std::optional<SettingIdentity>& setting) const
{
	if (!IsValidSceneContext(destination) || !IsValidCopyScope(scope) ||
		(scope == CopyScope::Setting && !setting))
		return {};
	if (destination.type == SceneContextType::Location &&
		ResolveLocationTargetChain(destination.locationType, destination.locationFormKey).empty())
		return {};
	const auto* destinationEntries = GetCopyContextEntries(destination);
	std::set<SettingIdentity> destinationOverwrites;
	if (destinationEntries)
		for (const auto& entry : *destinationEntries)
			if (entry.source == EntrySource::Overwrite && !entry.paused &&
				EntryBelongsToContext(entry, destination))
				destinationOverwrites.insert({ entry.featureShortName, entry.settingPath, entry.settingKey });

	const auto selectedGroup = setting ? std::optional{ GetCopyGroupKey(*setting) } : std::nullopt;
	const bool selectedIsAggregate = selectedGroup &&
	                                 std::get<5>(*selectedGroup) != SettingControlType::Scalar;
	const auto isSelected = [&](const SettingIdentity& identity) {
		if (scope == CopyScope::EntireContext)
			return true;
		if (identity == *setting)
			return true;
		return selectedIsAggregate && GetCopyGroupKey(identity) == *selectedGroup;
	};
	const bool requireNumeric = destination.type != SceneContextType::Location;
	const auto destinationSceneType = requireNumeric ? SceneType::TimeOfDay : SceneType::Location;
	const auto countCompatible = [&](const std::map<SettingIdentity, const SettingEntry*>& effectiveEntries) {
		struct GroupCount
		{
			size_t members = 0;
			size_t compatible = 0;
		};
		std::map<CopyGroupKey, GroupCount> groups;
		for (const auto& [identity, entry] : effectiveEntries) {
			if (!isSelected(identity))
				continue;
			auto& group = groups[GetCopyGroupKey(identity)];
			++group.members;
			auto* metadata = FindAllowedCatalogSetting(
				identity.featureShortName, identity.settingPath, identity.settingKey, requireNumeric);
			if (!destinationOverwrites.contains(identity) && metadata &&
				IsSettingAllowedForType(destinationSceneType,
					identity.featureShortName, identity.settingPath, identity.settingKey) &&
				IsSceneSettingValueAllowed(entry->value, *metadata, entry->value, requireNumeric))
				++group.compatible;
		}
		size_t count = 0;
		for (const auto& [_, group] : groups)
			if (group.members == group.compatible)
				count += group.members;
		return count;
	};
	const auto sameContext = [&](const SceneContextId& source) {
		if (source.type != destination.type)
			return false;
		if (source.type == SceneContextType::TimeOfDay)
			return source.period == destination.period;
		if (source.type == SceneContextType::Weather)
			return source.weatherId == destination.weatherId && source.period == destination.period;
		return source.locationType == destination.locationType &&
		       NormalizeLocationFormKey(source.locationFormKey) ==
		           NormalizeLocationFormKey(destination.locationFormKey);
	};
	std::vector<CopySource> sources;
	const auto addSource = [&](const SceneContextId& context,
							   const std::map<SettingIdentity, const SettingEntry*>& effectiveEntries, std::string displayName) {
		if (sameContext(context))
			return;
		const auto settingCount = countCompatible(effectiveEntries);
		if (settingCount != 0)
			sources.push_back({ context, std::move(displayName), settingCount });
	};
	const auto buildPeriodMaps = [](const std::vector<SettingEntry>& sourceEntries) {
		std::array<std::map<SettingIdentity, const SettingEntry*>, kPeriodCount> periods;
		for (auto entrySource : { EntrySource::User, EntrySource::Overwrite })
			for (const auto& entry : sourceEntries) {
				const auto periodIndex = static_cast<int>(entry.period);
				if (entry.source == entrySource && !entry.paused &&
					periodIndex >= 0 && periodIndex < kPeriodCount)
					periods[periodIndex][{ entry.featureShortName, entry.settingPath, entry.settingKey }] = &entry;
			}
		return periods;
	};

	const auto timeOfDayPeriods = buildPeriodMaps(GetEntries(SceneType::TimeOfDay));
	for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
		const auto period = static_cast<TimeOfDayPeriod>(periodIndex);
		addSource({ .type = SceneContextType::TimeOfDay, .period = period },
			timeOfDayPeriods[periodIndex], GetCopyPeriodName(period));
	}
	for (const auto& [weatherId, config] : weatherSceneConfigs) {
		const auto weatherPeriods = buildPeriodMaps(config.entries);
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
			const auto period = static_cast<TimeOfDayPeriod>(periodIndex);
			addSource({ .type = SceneContextType::Weather, .period = period, .weatherId = weatherId },
				weatherPeriods[periodIndex], std::format("{} / {}", Util::GetFormDisplayName(weatherId), GetCopyPeriodName(period)));
		}
	}
	for (const auto& [_, config] : locationSceneConfigs) {
		std::map<SettingIdentity, const SettingEntry*> effectiveEntries;
		for (auto entrySource : { EntrySource::User, EntrySource::Overwrite })
			for (const auto& entry : config.entries)
				if (entry.source == entrySource && !entry.paused)
					effectiveEntries[{ entry.featureShortName, entry.settingPath, entry.settingKey }] = &entry;
		SceneContextId context{
			.type = SceneContextType::Location,
			.locationType = config.type,
			.locationFormKey = config.formKey,
		};
		std::string displayName;
		displayName = std::format("{} / {}", GetCopyLocationTypeName(context.locationType),
			config.name.empty() ? context.locationFormKey : config.name);
		addSource(context, effectiveEntries, std::move(displayName));
	}
	std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.context.type, lhs.displayName, lhs.context) <
		       std::tie(rhs.context.type, rhs.displayName, rhs.context);
	});
	return sources;
}

SceneSettingsManager::CopyResult SceneSettingsManager::CopySettings(const SceneContextId& source,
	const SceneContextId& destination, CopyConflictPolicy conflictPolicy, CopyScope scope,
	const std::optional<SettingIdentity>& setting)
{
	CopyResult result;
	if (!IsValidSceneContext(source) || !IsValidSceneContext(destination) ||
		!IsValidCopyScope(scope) || !IsValidCopyConflictPolicy(conflictPolicy) ||
		(scope == CopyScope::Setting && !setting))
		return result;
	if ((source.type == SceneContextType::Weather || destination.type == SceneContextType::Weather) &&
		!TryEnsureWeatherDataLoaded())
		return result;
	if ((source.type == SceneContextType::Location || destination.type == SceneContextType::Location) &&
		!TryEnsureLocationDataLoaded())
		return result;

	auto candidates = BuildCopyCandidates(source, destination, scope, setting);
	if (candidates.empty())
		return result;
	std::map<CopyGroupKey, std::vector<CopyCandidate>> groups;
	for (const auto& candidate : candidates)
		groups[GetCopyGroupKey(candidate.setting)].push_back(candidate);

	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; })) {
			result.incompatible += group.size();
			continue;
		}
		result.hadConflicts |= std::any_of(group.begin(), group.end(),
			[](const auto& candidate) { return candidate.conflicts; });
	}
	if (conflictPolicy == CopyConflictPolicy::Cancel && result.hadConflicts) {
		result.cancelled = true;
		return result;
	}

	std::vector<LocationTarget> destinationLocationTargets;
	std::optional<LocationTarget> destinationLocationTarget;
	if (destination.type == SceneContextType::Location) {
		destinationLocationTargets = ResolveLocationTargetChain(
			destination.locationType, destination.locationFormKey);
		const auto destinationKey = GetLocationConfigKey(
			destination.locationType, destination.locationFormKey);
		auto targetIt = std::find_if(destinationLocationTargets.begin(), destinationLocationTargets.end(),
			[&](const auto& target) {
				return GetLocationConfigKey(target.type, target.formKey) == destinationKey;
			});
		if (targetIt == destinationLocationTargets.end())
			return result;
		destinationLocationTarget = *targetIt;
	}

	std::vector<SettingEntry> emptyDestinationEntries;
	std::vector<SettingEntry>* destinationEntries = nullptr;
	bool destinationNeedsMaterialization = false;
	switch (destination.type) {
	case SceneContextType::TimeOfDay:
		destinationEntries = &GetEntriesMut(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		{
			auto configIt = weatherSceneConfigs.find(destination.weatherId);
			destinationEntries = configIt != weatherSceneConfigs.end() ?
			                         &configIt->second.entries :
			                         &emptyDestinationEntries;
			destinationNeedsMaterialization = configIt == weatherSceneConfigs.end();
			break;
		}
	case SceneContextType::Location:
		{
			auto configIt = locationSceneConfigs.find(GetLocationConfigKey(
				destination.locationType, destination.locationFormKey));
			destinationEntries = configIt != locationSceneConfigs.end() ?
			                         &configIt->second.entries :
			                         &emptyDestinationEntries;
			destinationNeedsMaterialization = configIt == locationSceneConfigs.end();
			break;
		}
	}
	if (!destinationEntries)
		return result;

	std::map<SettingIdentity, size_t> destinationUserIndices;
	for (size_t index = 0; index < destinationEntries->size(); ++index) {
		const auto& entry = (*destinationEntries)[index];
		if (entry.source == EntrySource::User && EntryBelongsToContext(entry, destination))
			destinationUserIndices[{ entry.featureShortName, entry.settingPath, entry.settingKey }] = index;
	}
	std::vector<SettingAddress> candidateAddresses;
	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; }) ||
			(conflictPolicy == CopyConflictPolicy::SkipExisting &&
				std::any_of(group.begin(), group.end(), [](const auto& candidate) { return candidate.conflicts; })))
			continue;
		for (const auto& candidate : group)
			if (!destinationUserIndices.contains(candidate.setting))
				candidateAddresses.push_back({ candidate.setting.featureShortName,
					candidate.setting.settingPath, candidate.setting.settingKey });
	}
	std::sort(candidateAddresses.begin(), candidateAddresses.end());
	candidateAddresses.erase(std::unique(candidateAddresses.begin(), candidateAddresses.end()),
		candidateAddresses.end());
	EnsureBaselines(candidateAddresses);

	ResolvedSettingMap lowerLayers;
	if (destination.type == SceneContextType::Location) {
		auto resolvedLowerLayers = BuildLocationLowerLayers(
			destination.locationType, destination.locationFormKey);
		if (!resolvedLowerLayers)
			return result;
		lowerLayers = std::move(*resolvedLowerLayers);
	}
	PeriodSettingMap timeOfDayValues;
	if (destination.type == SceneContextType::Weather)
		timeOfDayValues = BuildTimeOfDayValueGroups();

	const auto* sourceEntries = GetCopyContextEntries(source);
	std::map<SettingIdentity, std::optional<float>> sourceTransitions;
	if (sourceEntries)
		for (auto entrySource : { EntrySource::User, EntrySource::Overwrite })
			for (const auto& sourceEntry : *sourceEntries)
				if (sourceEntry.source == entrySource && !sourceEntry.paused &&
					EntryBelongsToContext(sourceEntry, source))
					sourceTransitions[{ sourceEntry.featureShortName, sourceEntry.settingPath,
						sourceEntry.settingKey }] = sourceEntry.transitionSeconds;
	struct PendingCopy
	{
		CopyCandidate candidate;
		std::optional<size_t> destinationIndex;
		json originalValue;
		std::optional<float> transitionSeconds;
	};
	std::vector<PendingCopy> pending;
	for (const auto& [_, group] : groups) {
		if (std::any_of(group.begin(), group.end(), [](const auto& candidate) { return !candidate.compatible; }))
			continue;
		const bool hasConflict = std::any_of(group.begin(), group.end(),
			[](const auto& candidate) { return candidate.conflicts; });
		if (hasConflict && conflictPolicy == CopyConflictPolicy::SkipExisting) {
			result.skipped += group.size();
			continue;
		}

		std::vector<PendingCopy> groupPending;
		bool groupValid = true;
		std::optional<float> groupTransitionSeconds;
		bool groupTransitionSelected = false;
		if (destination.type == SceneContextType::Location) {
			for (const auto& candidate : group) {
				if (auto indexIt = destinationUserIndices.find(candidate.setting);
					indexIt != destinationUserIndices.end()) {
					groupTransitionSeconds = (*destinationEntries)[indexIt->second].transitionSeconds;
					groupTransitionSelected = true;
					break;
				}
			}
			if (!groupTransitionSelected) {
				for (const auto& candidate : group) {
					if (auto transitionIt = sourceTransitions.find(candidate.setting);
						transitionIt != sourceTransitions.end()) {
						groupTransitionSeconds = transitionIt->second;
						break;
					}
				}
			}
		}
		for (const auto& candidate : group) {
			std::optional<size_t> destinationIndex;
			if (auto indexIt = destinationUserIndices.find(candidate.setting);
				indexIt != destinationUserIndices.end())
				destinationIndex = indexIt->second;

			SettingAddress address{ candidate.setting.featureShortName,
				candidate.setting.settingPath, candidate.setting.settingKey };
			json originalValue;
			if (destinationIndex) {
				originalValue = (*destinationEntries)[*destinationIndex].originalValue;
			} else if (destination.type == SceneContextType::Weather) {
				auto baselineIt = baselineSettings.find(address);
				if (baselineIt != baselineSettings.end() && IsNumericValue(baselineIt->second)) {
					originalValue = baselineIt->second;
					if (auto valueIt = timeOfDayValues.find(address); valueIt != timeOfDayValues.end())
						originalValue = valueIt->second[static_cast<int>(destination.period)]
						                    .value_or(baselineIt->second.get<float>());
				}
			} else if (destination.type == SceneContextType::Location) {
				if (auto lowerIt = lowerLayers.find(address); lowerIt != lowerLayers.end())
					originalValue = lowerIt->second;
				else if (auto baselineIt = baselineSettings.find(address); baselineIt != baselineSettings.end())
					originalValue = baselineIt->second;
			} else if (auto baselineIt = baselineSettings.find(address); baselineIt != baselineSettings.end()) {
				originalValue = baselineIt->second;
			}
			if (!destinationIndex && !IsSceneSettingPrimitive(originalValue)) {
				groupValid = false;
				break;
			}

			groupPending.push_back({ candidate, destinationIndex, std::move(originalValue),
				groupTransitionSeconds });
		}
		if (!groupValid) {
			result.incompatible += group.size();
			continue;
		}
		pending.insert(pending.end(), std::make_move_iterator(groupPending.begin()),
			std::make_move_iterator(groupPending.end()));
	}

	if (pending.empty())
		return result;
	if (destinationNeedsMaterialization) {
		if (destination.type == SceneContextType::Weather) {
			destinationEntries = &GetWeatherConfigMut(destination.weatherId).entries;
		} else if (destination.type == SceneContextType::Location) {
			auto& config = GetLocationConfigMut(destination.locationType,
				destination.locationFormKey, destinationLocationTarget->name);
			config.cocCode = destinationLocationTarget->cocCode;
			destinationEntries = &config.entries;
		}
	}

	for (auto& copy : pending) {
		if (copy.destinationIndex) {
			auto& destinationEntry = (*destinationEntries)[*copy.destinationIndex];
			destinationEntry.value = copy.candidate.value;
			if (destination.type == SceneContextType::Location)
				destinationEntry.transitionSeconds = copy.transitionSeconds;
			++result.overwritten;
			continue;
		}
		destinationEntries->push_back({
			.featureShortName = copy.candidate.setting.featureShortName,
			.settingPath = copy.candidate.setting.settingPath,
			.settingKey = copy.candidate.setting.settingKey,
			.displayName = copy.candidate.displayName,
			.value = copy.candidate.value,
			.originalValue = std::move(copy.originalValue),
			.paused = false,
			.source = EntrySource::User,
			.period = destination.type == SceneContextType::Location ? TimeOfDayPeriod::Count : destination.period,
			.transitionSeconds = copy.transitionSeconds,
		});
		++result.copied;
	}
	if (!result.Changed())
		return result;

	if (destination.type != SceneContextType::Location)
		++sceneValueRevision;
	BumpEntryPresentationRevision();
	switch (destination.type) {
	case SceneContextType::TimeOfDay:
		MarkEntryListUserSettingsModified(SceneType::TimeOfDay);
		break;
	case SceneContextType::Weather:
		PrepareWeatherUserSettingsMutation(destination.weatherId, true);
		break;
	case SceneContextType::Location:
		PrepareLocationUserSettingsMutation(
			destination.locationType, destination.locationFormKey, true);
		break;
	}
	CommitSceneSettingChanges();
	return result;
}

// --- Per-Location Scene Settings ---

const SceneSettingsManager::LocationSceneConfig SceneSettingsManager::kEmptyLocationConfig{};

std::string SceneSettingsManager::GetLocationConfigKey(LocationTargetType type, std::string_view formKey)
{
	return std::format("{}:{}", GetLocationTargetTypeName(type), NormalizeLocationFormKey(formKey));
}

const char* SceneSettingsManager::GetLocationSectionName(LocationTargetType type)
{
	switch (type) {
	case LocationTargetType::Region:
		return "regions";
	case LocationTargetType::Location:
		return "locations";
	case LocationTargetType::Cell:
		return "cells";
	default:
		return "invalid";
	}
}

const char* SceneSettingsManager::GetLocationTargetTypeName(LocationTargetType type)
{
	switch (type) {
	case LocationTargetType::Region:
		return "Region";
	case LocationTargetType::Location:
		return "Location";
	case LocationTargetType::Cell:
		return "Cell";
	default:
		return "Invalid";
	}
}

const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetCurrentLocationTargets() const
{
	auto* player = globals::game::player;
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		cachedTargetLocationId = 0;
		cachedTargetCellId = 0;
		cachedTargetRegionId = 0;
		locationTargetsCached = false;
		cachedLocationTargets.clear();
		return cachedLocationTargets;
	}

	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();
	const auto regionId = cell->IsExteriorCell() && globals::game::sky && globals::game::sky->region ?
	                          globals::game::sky->region->GetFormID() :
	                          0;
	if (locationTargetsCached && cachedTargetLocationId == locationId &&
		cachedTargetCellId == cellId && cachedTargetRegionId == regionId)
		return cachedLocationTargets;

	cachedTargetLocationId = locationId;
	cachedTargetCellId = cellId;
	cachedTargetRegionId = regionId;
	locationTargetsCached = true;
	cachedLocationTargets = BuildLocationTargetChain(location, cell);
	locationManagementTargetsCached = false;
	return cachedLocationTargets;
}

const std::vector<SceneSettingsManager::LocationTarget>& SceneSettingsManager::GetLocationManagementTargets() const
{
	if (locationManagementTargetsCached)
		return cachedLocationManagementTargets;

	cachedLocationManagementTargets.clear();
	std::map<std::string, LocationTarget> targets;
	const auto addTarget = [&](LocationTarget target) {
		if (target.formKey.empty())
			return;
		targets[GetLocationConfigKey(target.type, target.formKey)] = std::move(target);
	};
	const auto addForm = [&](LocationTargetType type, RE::TESForm* form) {
		if (!form || form->GetFormID() == 0)
			return;
		std::string name;
		std::string cocCode;
		if (type == LocationTargetType::Region) {
			name = Util::GetFormDisplayName(form->GetFormID());
		} else if (type == LocationTargetType::Location) {
			auto* location = form->As<RE::BGSLocation>();
			if (!location)
				return;
			name = GetLocationTargetDisplayName(location);
		} else {
			auto* cell = form->As<RE::TESObjectCELL>();
			if (!cell)
				return;
			cocCode = Util::GetFormEditorID(cell);
			const char* fullName = cell->GetFullName();
			if (cocCode.empty() && (!fullName || fullName[0] == '\0'))
				return;
			name = GetLocationTargetDisplayName(cell);
		}
		addTarget({
			.type = type,
			.formKey = Util::GetFormFileKey(form),
			.name = std::move(name),
			.cocCode = std::move(cocCode),
			.formId = form->GetFormID(),
		});
	};

	if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
		for (auto* region : dataHandler->GetFormArray<RE::TESRegion>())
			if (region && region->worldSpace)
				addForm(LocationTargetType::Region, region);
		for (auto* location : dataHandler->GetFormArray<RE::BGSLocation>())
			addForm(LocationTargetType::Location, location);
		for (auto* cell : dataHandler->GetFormArray<RE::TESObjectCELL>())
			addForm(LocationTargetType::Cell, cell);
	}
	for (const auto& target : GetCurrentLocationTargets())
		addTarget(target);

	for (const auto& [_, config] : locationSceneConfigs) {
		auto key = GetLocationConfigKey(config.type, config.formKey);
		if (auto targetIt = targets.find(key); targetIt != targets.end()) {
			if (!config.name.empty())
				targetIt->second.name = config.name;
			if (!config.cocCode.empty())
				targetIt->second.cocCode = config.cocCode;
			continue;
		}
		RE::FormID formId = 0;
		if (auto* form = ResolveLocationTargetForm(config.formKey))
			formId = form->GetFormID();
		addTarget({
			.type = config.type,
			.formKey = config.formKey,
			.name = config.name.empty() ? config.formKey : config.name,
			.cocCode = config.cocCode,
			.formId = formId,
		});
	}

	cachedLocationManagementTargets.reserve(targets.size());
	for (auto& [_, target] : targets)
		cachedLocationManagementTargets.push_back(std::move(target));
	std::ranges::sort(cachedLocationManagementTargets, [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.type, lhs.name, lhs.formKey) < std::tie(rhs.type, rhs.name, rhs.formKey);
	});
	locationManagementTargetsCached = true;
	return cachedLocationManagementTargets;
}

SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfigMut(
	LocationTargetType type, const std::string& formKey, const std::string& name)
{
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	auto& config = locationSceneConfigs[GetLocationConfigKey(type, canonicalFormKey)];
	config.type = type;
	config.formKey = canonicalFormKey;
	if (!name.empty())
		config.name = name;
	return config;
}

const SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfig(
	LocationTargetType type, std::string_view formKey) const
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	return it != locationSceneConfigs.end() ? it->second : kEmptyLocationConfig;
}

bool SceneSettingsManager::HasLocationConfig(LocationTargetType type, std::string_view formKey) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return IsEntryActive(entry);
	});
}

void SceneSettingsManager::SetLocationTransitionSeconds(float seconds, bool deferSave)
{
	if (!std::isfinite(seconds) || !TryEnsureLocationDataLoaded())
		return;
	seconds = std::clamp(seconds, 0.0f, kMaxLocationTransitionSeconds);
	if (std::abs(locationTransitionSeconds - seconds) < kBlendEpsilon)
		return;
	locationTransitionSeconds = seconds;
	locationTransitionModified = true;
	locationUserSettingsModified = true;
	locationOverridesDirty = true;
	if (deferSave)
		MarkDeferredSceneChanges();
	else {
		SaveAllUserSettings();
		ReapplyIfActive(false);
	}
}

std::optional<float> SceneSettingsManager::GetLocationEntryTransitionSeconds(
	LocationTargetType type, std::string_view formKey, size_t index) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return index < config.entries.size() ? config.entries[index].transitionSeconds : std::nullopt;
}

void SceneSettingsManager::SetLocationEntryTransitionSeconds(LocationTargetType type,
	const std::string& formKey, std::span<const size_t> indices, std::optional<float> seconds,
	bool deferSave)
{
	if (indices.empty() || (seconds && (!std::isfinite(*seconds) || *seconds < 0.0f ||
										   *seconds > kMaxLocationTransitionSeconds)))
		return;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;
	auto& config = configIt->second;
	std::set<size_t> expandedIndices;
	for (const auto index : indices) {
		if (index >= config.entries.size())
			return;
		expandedIndices.insert(index);
		const auto& selectedEntry = config.entries[index];
		const auto selectedGroup = GetCopyGroupKey({ selectedEntry.featureShortName,
			selectedEntry.settingPath, selectedEntry.settingKey });
		if (std::get<5>(selectedGroup) == SettingControlType::Scalar)
			continue;
		for (size_t candidateIndex = 0; candidateIndex < config.entries.size(); ++candidateIndex) {
			const auto& candidate = config.entries[candidateIndex];
			if (candidate.source == selectedEntry.source &&
				GetCopyGroupKey({ candidate.featureShortName,
					candidate.settingPath, candidate.settingKey }) == selectedGroup)
				expandedIndices.insert(candidateIndex);
		}
	}
	for (const auto index : expandedIndices) {
		const auto& entry = config.entries[index];
		if (entry.source != EntrySource::User || !IsNumericValue(entry.value) ||
			!FindAllowedCatalogSetting(entry.featureShortName, entry.settingPath, entry.settingKey, true))
			return;
	}

	bool changed = false;
	for (const auto index : expandedIndices) {
		auto& entry = config.entries[index];
		if (entry.transitionSeconds != seconds) {
			entry.transitionSeconds = seconds;
			changed = true;
		}
	}
	if (!changed)
		return;
	PrepareLocationUserSettingsMutation(type, formKey, false);
	if (deferSave)
		MarkDeferredSceneChanges();
	else {
		SaveAllUserSettings();
		ReapplyIfActive(false);
	}
}

std::optional<json> SceneSettingsManager::ResolveLocationLowerValue(LocationTargetType type,
	std::string_view formKey, const SettingAddress& address, EntrySource selectedSource)
{
	auto baseline = GetBaselineValue(address);
	if (!IsSceneSettingPrimitive(baseline))
		return std::nullopt;
	auto lowerLayers = BuildLocationLowerLayers(type, formKey, selectedSource);
	if (!lowerLayers)
		return std::nullopt;
	if (auto valueIt = lowerLayers->find(address); valueIt != lowerLayers->end() &&
												   IsSceneSettingPrimitive(valueIt->second))
		return valueIt->second;
	return baseline;
}

std::optional<SceneSettingsManager::ResolvedSettingMap> SceneSettingsManager::BuildLocationLowerLayers(
	LocationTargetType type, std::string_view formKey, std::optional<EntrySource> selectedSource)
{
	ResolvedSettingMap lowerLayers;
	if (Util::IsInterior()) {
		ResolveInteriorSettings(lowerLayers);
	} else {
		std::array<float, kPeriodCount> factors{};
		GetTimeOfDayFactors(factors.data());
		const auto& timeOfDayValues = BuildTimeOfDayValueGroups();
		ResolveTimeOfDaySettings(lowerLayers, timeOfDayValues, factors);
		ResolveWeatherSettings(lowerLayers, timeOfDayValues, factors);
	}

	bool targetFound = false;
	const auto selectedTargetKey = GetLocationConfigKey(type, formKey);
	for (const auto& target : ResolveLocationTargetChain(type, formKey)) {
		if (GetLocationConfigKey(target.type, target.formKey) == selectedTargetKey) {
			targetFound = true;
			if (selectedSource == EntrySource::Overwrite) {
				auto configIt = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
				if (configIt != locationSceneConfigs.end())
					OverlayEntries(
						lowerLayers, configIt->second.entries, SceneType::Location, EntrySource::User);
			}
			break;
		}
		auto configIt = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (configIt == locationSceneConfigs.end())
			continue;
		OverlayEntries(lowerLayers, configIt->second.entries, SceneType::Location, EntrySource::User);
		OverlayEntries(lowerLayers, configIt->second.entries, SceneType::Location, EntrySource::Overwrite);
	}
	if (!targetFound)
		return std::nullopt;
	return lowerLayers;
}

void SceneSettingsManager::PrepareLocationUserSettingsMutation(LocationTargetType type,
	std::string_view formKey, bool replaceMalformedEntries)
{
	locationOverridesDirty = true;
	locationUserSettingsModified = true;
	if (!unresolvedLocationUserSettings.is_object())
		unresolvedLocationUserSettings = json::object();
	const auto* sectionName = GetLocationSectionName(type);
	auto& section = unresolvedLocationUserSettings[sectionName];
	if (!section.is_object())
		section = json::object();
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	const auto targetKey = GetLocationConfigKey(type, canonicalFormKey);
	if (replaceMalformedEntries) {
		for (auto& [rawFormKey, rawConfig] : section.items()) {
			if (!rawConfig.is_object() ||
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) != targetKey)
				continue;
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt != rawConfig.end() && !entriesIt->is_array())
				*entriesIt = json::array();
			if (rawFormKey != canonicalFormKey) {
				rawConfig.erase("type");
				rawConfig.erase("name");
				rawConfig.erase("coc");
			}
		}
	}

	auto& rawConfig = section[canonicalFormKey];
	if (!rawConfig.is_object())
		rawConfig = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawConfig.find("entries");
		if (entriesIt != rawConfig.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

bool SceneSettingsManager::AddLocationSetting(LocationTargetType type, const std::string& formKey,
	const std::string& name, const std::string& cocCode, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, bool deferSave)
{
	if (!TryEnsureLocationDataLoaded() || formKey.empty() ||
		!IsSettingAllowedForType(SceneType::Location, featureShortName, settingPath, settingKey) ||
		HasLocationEntry(type, formKey, featureShortName, settingPath, settingKey, EntrySource::User))
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, EntrySource::User);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Location", SceneType::Location, featureShortName, settingPath, settingKey, *lowerValue, false))
		return false;

	auto& config = GetLocationConfigMut(type, formKey, name);
	if (!cocCode.empty())
		config.cocCode = cocCode;
	config.entries.push_back({
		.featureShortName = featureShortName,
		.settingPath = settingPath,
		.settingKey = settingKey,
		.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey),
		.value = *lowerValue,
		.originalValue = *lowerValue,
		.source = EntrySource::User,
	});
	BumpEntryPresentationRevision();
	PrepareLocationUserSettingsMutation(type, formKey, true);
	if (deferSave)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveLocationSetting(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;

	const auto entry = it->second.entries[index];
	const bool userEntry = entry.source == EntrySource::User;
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetLocationOverwritePath(type, formKey, entry), entry.settingPath, entry.settingKey))
		return;
	it->second.entries.erase(it->second.entries.begin() + static_cast<ptrdiff_t>(index));
	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (userEntry) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey)
{
	if (!TryEnsureLocationDataLoaded())
		return;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt != locationSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareLocationUserSettingsMutation(type, formKey, false);
	const auto* sectionName = GetLocationSectionName(type);
	auto sectionIt = unresolvedLocationUserSettings.find(sectionName);
	if (sectionIt != unresolvedLocationUserSettings.end() && sectionIt->is_object()) {
		const auto targetKey = GetLocationConfigKey(type, formKey);
		for (auto& [rawFormKey, rawConfig] : sectionIt->items())
			if (rawConfig.is_object() &&
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) == targetKey)
				rawConfig.erase("entries");
	}
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseLocationEntry(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::SetLocationEntriesPaused(LocationTargetType type, const std::string& formKey,
	std::span<const size_t> indices, bool paused)
{
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;
	bool changed = false;
	bool userEntriesChanged = false;
	for (const auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		auto& entry = configIt->second.entries[index];
		if (entry.paused == paused)
			continue;
		entry.paused = paused;
		changed = true;
		userEntriesChanged |= entry.source == EntrySource::User;
	}
	if (!changed)
		return;

	locationOverridesDirty = true;
	BumpEntryPresentationRevision();
	if (userEntriesChanged) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateLocationEntryValue(LocationTargetType type, const std::string& formKey,
	size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateLocationEntryValues(type, formKey, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateLocationEntryValues(LocationTargetType type, const std::string& formKey,
	std::span<const EntryValueUpdate> updates, bool deferSave)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Location", SceneType::Location, it->second.entries, updates, false, userEntriesChanged))
		return;
	locationOverridesDirty = true;
	if (userEntriesChanged) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

void SceneSettingsManager::RevertLocationEntryToDefault(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, entry.source);
	if (!lowerValue || !ValidateSceneSettingEntry(
						   "Location", SceneType::Location, entry.featureShortName,
						   entry.settingPath, entry.settingKey, *lowerValue, false))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	locationOverridesDirty = true;
	if (entry.source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive(false);
}

bool SceneSettingsManager::HasLocationEntry(LocationTargetType type, std::string_view formKey,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, std::optional<EntrySource> source) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return (!source || entry.source == *source) &&
		       IsSameSetting(entry, featureShortName, settingPath, settingKey);
	});
}

void SceneSettingsManager::ExportLocationUserSettingsToOverwrites(LocationTargetType type,
	const std::string& formKey, const std::vector<size_t>& indices, const std::string& modName)
{
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::string, std::vector<const SettingEntry*>> groupedEntries;
	for (auto index : indices) {
		if (index >= configIt->second.entries.size())
			continue;
		const auto& entry = configIt->second.entries[index];
		if (entry.source == EntrySource::User)
			groupedEntries[entry.featureShortName].push_back(&entry);
	}

	const auto targetDescription = GetLocationTargetTypeName(type);
	const json metadata = {
		{ "targetType", targetDescription },
		{ "targetName", configIt->second.name },
		{ "coc", configIt->second.cocCode },
	};
	const auto directory = GetLocationOverwritesDir(type) / configIt->second.formKey;
	for (const auto& [featureShortName, grouped] : groupedEntries) {
		WriteGroupedOverwriteFile(directory / std::format("{}_{}.json", safeModName, featureShortName),
			featureShortName, targetDescription, grouped, metadata);
	}
}

void SceneSettingsManager::DiscoverLocationOverwrites()
{
	const auto root = GetLocationOverwritesDir(LocationTargetType::Location);
	std::error_code ec;
	if (!std::filesystem::exists(root, ec))
		return;
	for (const auto& directory : GetSortedDirectoryPaths(root, true, "location overwrite directories"))
		DiscoverLocationOverwritesForTarget(LocationTargetType::Location, directory);
}

void SceneSettingsManager::DiscoverLocationOverwritesForTarget(LocationTargetType type,
	const std::filesystem::path& targetDir)
{
	(void)type;
	const auto formKey = targetDir.filename().string();
	if (formKey.empty())
		return;

	std::optional<LocationTargetType> resolvedType;
	std::string resolvedName;
	std::string resolvedCocCode;
	std::string canonicalFormKey = formKey;
	if (const auto formId = Util::SpidToFormId(formKey); formId != 0) {
		canonicalFormKey = Util::FormIdToSpid(formId);
		if (auto* form = RE::TESForm::LookupByID(formId)) {
			if (form->GetFormType() == RE::FormType::Region) {
				resolvedType = LocationTargetType::Region;
			} else if (form->GetFormType() == RE::FormType::Location)
				resolvedType = LocationTargetType::Location;
			else if (form->GetFormType() == RE::FormType::Cell)
				resolvedType = LocationTargetType::Cell;
			else {
				logger::warn("[SceneSettings] Location overwrite target '{}' is not a region, location, or cell", formKey);
				return;
			}
			resolvedName = Util::GetFormDisplayName(formId);
			if (*resolvedType == LocationTargetType::Cell)
				resolvedCocCode = Util::GetFormEditorID(form);
		}
	}

	for (const auto& filePath : GetSortedJsonFiles(targetDir, "location overwrite files")) {
		try {
			json data;
			if (!ReadBoundedSceneJson(filePath, data)) {
				logger::warn("[SceneSettings] Location overwrite '{}' is invalid or exceeds {} bytes",
					filePath.string(), kMaxSceneOverwriteFileSize);
				continue;
			}

			std::optional<LocationTargetType> metadataType;
			std::string metadataName;
			std::string metadataCocCode;
			if (auto metadataIt = data.find(kMetadataKey); metadataIt != data.end()) {
				if (!metadataIt->is_object()) {
					logger::warn("[SceneSettings] Location overwrite '{}' metadata must be an object",
						filePath.string());
					continue;
				}
				const auto metadataContext = std::format("Location overwrite '{}' metadata", filePath.string());
				std::string targetType;
				if (!ReadOptionalStringField(*metadataIt, "targetType", targetType, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "targetName", metadataName, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "coc", metadataCocCode, metadataContext))
					continue;
				if (targetType == "Region")
					metadataType = LocationTargetType::Region;
				else if (targetType == "Location")
					metadataType = LocationTargetType::Location;
				else if (targetType == "Cell")
					metadataType = LocationTargetType::Cell;
				else if (!targetType.empty()) {
					logger::warn("[SceneSettings] {} has invalid targetType '{}'", metadataContext, targetType);
					continue;
				}
			}
			if (resolvedType && metadataType && *resolvedType != *metadataType) {
				logger::warn("[SceneSettings] Location overwrite '{}' targetType does not match resolved form '{}'",
					filePath.string(), formKey);
				continue;
			}
			const auto targetType = resolvedType ? resolvedType : metadataType;
			if (!targetType) {
				logger::warn("[SceneSettings] Location overwrite '{}' has no resolvable target type",
					filePath.string());
				continue;
			}

			auto& config = GetLocationConfigMut(*targetType, canonicalFormKey,
				!metadataName.empty() ? metadataName : resolvedName);
			if (!metadataCocCode.empty())
				config.cocCode = metadataCocCode;
			else if (!resolvedCocCode.empty())
				config.cocCode = resolvedCocCode;

			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(filePath, SceneType::Location, false, parsedEntries))
				continue;
			for (auto& entry : parsedEntries)
				AddOverwriteEntryIfUnique(config.entries, std::move(entry), "location");
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load location overwrite '{}': {}",
				filePath.filename().string(), e.what());
		}
	}
}

void SceneSettingsManager::DiscoverWeatherOverwrites()
{
	const auto previousEntryCount = std::accumulate(weatherSceneConfigs.begin(), weatherSceneConfigs.end(), size_t{ 0 },
		[](size_t total, const auto& config) { return total + config.second.entries.size(); });
	auto baseDir = GetWeatherOverwritesDir();
	std::error_code ec;
	if (!std::filesystem::exists(baseDir, ec))
		return;

	logger::info("[SceneSettings] Discovering weather overwrites in: {}", baseDir.string());

	for (const auto& weatherDirectory : GetSortedDirectoryPaths(baseDir, true, "weather overwrite directories")) {
		auto folderName = weatherDirectory.filename().string();
		RE::FormID weatherId = Util::SpidToFormId(folderName);
		if (weatherId == 0) {
			logger::warn("[SceneSettings] Weather overwrite folder '{}' could not be resolved - skipping", folderName);
			continue;
		}

		DiscoverWeatherOverwritesForSpid(weatherId, weatherDirectory);
	}

	const auto entryCount = std::accumulate(weatherSceneConfigs.begin(), weatherSceneConfigs.end(), size_t{ 0 },
		[](size_t total, const auto& config) { return total + config.second.entries.size(); });
	if (entryCount != previousEntryCount)
		++sceneValueRevision;
	if (entryCount != previousEntryCount)
		BumpEntryPresentationRevision();
}

void SceneSettingsManager::DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir)
{
	auto& config = GetWeatherConfigMut(weatherId);
	// Scan period subfolders (TOD entries)
	for (int i = 0; i < kPeriodCount; ++i) {
		auto period = static_cast<TimeOfDayPeriod>(i);
		auto periodDir = weatherDir / GetPeriodName(period);
		std::error_code ec;
		if (!std::filesystem::exists(periodDir, ec))
			continue;

		for (const auto& filePath : GetSortedJsonFiles(periodDir, "weather period overwrite files")) {
			try {
				std::vector<SettingEntry> parsedEntries;
				if (!ParseOverwriteFileEntries(filePath, SceneType::TimeOfDay, true, parsedEntries))
					continue;
				for (auto& entry : parsedEntries) {
					entry.period = period;
					AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
				}
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", filePath.filename().string(), e.what());
			}
		}
	}

	// Flat weather files are copied to every period after period-specific files are loaded.
	{
		for (const auto& filePath : GetSortedJsonFiles(weatherDir, "flat weather overwrite files")) {
			try {
				std::vector<SettingEntry> parsedEntries;
				if (!ParseOverwriteFileEntries(filePath, SceneType::TimeOfDay, true, parsedEntries))
					continue;
				for (auto& parsed : parsedEntries) {
					for (int p = 0; p < kPeriodCount; ++p) {
						SettingEntry entry = parsed;
						entry.period = static_cast<TimeOfDayPeriod>(p);
						AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
					}
				}
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", filePath.filename().string(), e.what());
			}
		}
	}
}
