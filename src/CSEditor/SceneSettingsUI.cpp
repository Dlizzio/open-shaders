#include "SceneSettingsUI.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "../Globals.h"
#include "../I18n/I18n.h"
#include "../Menu.h"
#include "../Menu/ThemeManager.h"
#include "../Utils/FileSystem.h"
#include "../SceneSettingsManager.h"
#include "../Utils/Format.h"

namespace SceneSettingsUI
{
	using C = ThemeManager::Constants;
	constexpr int kNoSubFeatureSelection = -1;
	constexpr int kPeriodlessEntrySlot = 0;
	constexpr float kSingleValueColumnScale = 1.25f;
	constexpr float kLabelOverflowTolerance = 0.5f;
	constexpr float kSceneFloatDragSpeed = 0.01f;
	constexpr float kSceneIntDragSpeed = 1.0f;
	constexpr float kActionsColumnMinWidthEm = 4.5f;
	constexpr float kActionControlSpacingCount = 2.0f;
	constexpr float kCompactToggleWidthScale = 1.6f;
	constexpr float kCompactToggleHeightScale = 0.8f;
	constexpr float kSceneFlyoutRounding = 4.0f;
	constexpr float kSceneFlyoutPaddingX = 6.0f;
	constexpr float kSceneFlyoutPaddingY = 2.0f;
	constexpr float kSceneFlyoutBackgroundAlpha = 0.95f;
	constexpr float kChannelSelectorWidthEm = 4.0f;
	constexpr const char* kEllipsis = "...";
	constexpr std::string_view kDisplaySeparator = " / ";
	static int s_addDialogFrame = -1;
	using SettingEntry = SceneSettingsManager::SettingEntry;

	static size_t PreviousUtf8CodepointBoundary(std::string_view text, size_t offset)
	{
		if (offset == 0)
			return 0;

		--offset;
		while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
			--offset;
		return offset;
	}

	static std::string_view GetVisibleLabel(std::string_view label)
	{
		return label.substr(0, label.find("##"));
	}

	static void ContinueButtonRowIfFits(const char* nextLabel)
	{
		const float nextWidth = ImGui::CalcTextSize(nextLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextWidth <= contentRight)
			ImGui::SameLine();
	}

	// --- Shared helpers ---
	static ImVec2 GetCompactFeatureToggleSize()
	{
		const float frameHeight = ImGui::GetFrameHeight();
		return ImVec2(
			frameHeight * kCompactToggleWidthScale * C::FLYOUT_TOGGLE_SCALE,
			frameHeight * kCompactToggleHeightScale * C::FLYOUT_TOGGLE_SCALE);
	}

	static float GetSceneActionButtonSize()
	{
		return ImGui::GetFrameHeight() * C::FLYOUT_BUTTON_SCALE;
	}

	static Util::FlyoutStyle GetSceneFlyoutStyle()
	{
		const float scale = Util::GetUIScale();
		return {
			{ kSceneFlyoutPaddingX * scale, kSceneFlyoutPaddingY * scale },
			kSceneFlyoutRounding * scale,
			kSceneFlyoutBackgroundAlpha,
			1.0f,
			true,
			true
		};
	}

	static bool DrawSceneIconButton(const char* id, void* texture, const ImVec2& size, float padding)
	{
		if (!texture)
			return false;
		const ImVec2 imageSize(
			std::max(1.0f, size.x - padding * 2.0f),
			std::max(1.0f, size.y - padding * 2.0f));
		auto& colors = ImGui::GetStyle().Colors;
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_FrameBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_FrameBgHovered]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_FrameBgActive]);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
		const bool clicked = ImGui::ImageButton(
			id, texture, imageSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), Util::GetIconTint());
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		return clicked;
	}

	static bool DrawSceneDeleteButton(const char* label, float size)
	{
		auto& colors = ImGui::GetStyle().Colors;
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_FrameBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_FrameBgHovered]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_FrameBgActive]);
		const float paddingY = std::max(0.0f, (size - ImGui::GetFontSize()) * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size * 0.3f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(paddingY, paddingY));
		const bool clicked = ImGui::Button(label, ImVec2(size, size));
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
		return clicked;
	}

	static bool IsTransitionEntry(const SettingEntry& entry)
	{
		return entry.value.is_number_float();
	}

	static bool HasTransitionEntries(const std::vector<SettingEntry>& entries)
	{
		return std::any_of(entries.begin(), entries.end(), IsTransitionEntry);
	}

	static const char* GetPeriodDisplayName(Period period)
	{
		switch (period) {
		case Period::Dawn:
			return T("feature.scene_manager.period.dawn", "Dawn");
		case Period::Sunrise:
			return T("feature.scene_manager.period.sunrise", "Sunrise");
		case Period::Day:
			return T("feature.scene_manager.period.day", "Day");
		case Period::Sunset:
			return T("feature.scene_manager.period.sunset", "Sunset");
		case Period::Dusk:
			return T("feature.scene_manager.period.dusk", "Dusk");
		case Period::Night:
			return T("feature.scene_manager.period.night", "Night");
		default:
			return "";
		}
	}

	static std::string GetAddPeriodLabel(Period period)
	{
		const auto* periodName = GetPeriodDisplayName(period);
		return std::vformat(T("feature.scene_manager.action.add_period", "Add {0}"),
			std::make_format_args(periodName));
	}

	static std::string GetEntryDisplayName(const SettingEntry& entry)
	{
		return entry.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(entry.settingKey) : entry.displayName;
	}

	static std::vector<std::string> SplitDisplayName(std::string_view displayName)
	{
		std::vector<std::string> parts;
		if (displayName.empty())
			return parts;
		for (size_t start = 0; start <= displayName.size();) {
			size_t end = displayName.find(kDisplaySeparator, start);
			if (end == std::string_view::npos) {
				parts.emplace_back(displayName.substr(start));
				break;
			}
			parts.emplace_back(displayName.substr(start, end - start));
			start = end + kDisplaySeparator.size();
		}
		return parts;
	}

	static std::string JoinDisplayParts(const std::vector<std::string>& parts)
	{
		std::string result;
		for (const auto& part : parts) {
			if (!result.empty())
				result += kDisplaySeparator;
			result += part;
		}
		return result;
	}

	static SettingId MakeSettingId(const SettingEntry& entry, const std::string& rootCategoryName)
	{
		auto displayParts = SplitDisplayName(GetEntryDisplayName(entry));
		if (displayParts.empty())
			displayParts.push_back(SceneSettingsManager::GetSettingDisplayName(entry.settingKey));

		auto settingName = displayParts.back();
		displayParts.pop_back();

		std::string categoryName = displayParts.empty() ? rootCategoryName : displayParts.front();
		std::vector<std::string> parentPath;
		if (displayParts.size() > 1)
			parentPath.assign(std::next(displayParts.begin()), displayParts.end());

		return { entry.featureShortName, entry.settingPath, entry.settingKey, settingName, categoryName, parentPath, -1 };
	}

	static SettingId MakeControlSettingId(const SettingEntry& entry,
		const SceneSettingsManager::SettingControlInfo& info, const std::string& rootCategoryName)
	{
		std::string categoryName = info.displayPath.empty() ? rootCategoryName : info.displayPath.front();
		std::vector<std::string> parentPath;
		if (info.displayPath.size() > 1)
			parentPath.assign(std::next(info.displayPath.begin()), info.displayPath.end());
		return {
			entry.featureShortName, info.settingPath, info.settingKey, info.displayName,
			std::move(categoryName), std::move(parentPath), info.componentStart
		};
	}

	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource, bool transitionOnly)
	{
		SourceGroup group;
		std::map<std::string, std::string> featureDisplayNames;
		auto getFeatureDisplayName = [&](const std::string& feature) -> const std::string& {
			auto it = featureDisplayNames.find(feature);
			if (it == featureDisplayNames.end())
				it = featureDisplayNames.emplace(feature, SceneSettingsManager::GetFeatureDisplayName(feature)).first;
			return it->second;
		};

		using AggregateKey = std::tuple<
			std::string, std::vector<std::string>, std::string, std::int8_t, std::uint8_t,
			SceneSettingControlType, EntrySource, int>;
		struct AggregateCandidate
		{
			SceneSettingsManager::SettingControlInfo info;
			std::map<std::int8_t, std::vector<size_t>> components;
			bool valid = true;
		};
		std::map<AggregateKey, AggregateCandidate> candidates;
		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& entry = entries[idx];
			if (filterBySource && entry.source != sourceFilter)
				continue;
			if (transitionOnly && !IsTransitionEntry(entry))
				continue;
			int period = static_cast<int>(entry.period);
			if (period < 0)
				continue;
			if (period >= kPeriodCount)
				period = kPeriodlessEntrySlot;
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entry, info) ||
				info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2)
				continue;
			auto [candidateIt, inserted] = candidates.try_emplace(AggregateKey{
				entry.featureShortName, info.settingPath, info.settingKey,
				info.componentStart, info.componentCount, info.controlType, entry.source, period
			});
			auto& candidate = candidateIt->second;
			if (inserted) {
				candidate.info = info;
			} else if (candidate.info.displayName != info.displayName ||
				candidate.info.displayPath != info.displayPath) {
				candidate.valid = false;
			}
			candidate.components[info.componentIndex].push_back(idx);
		}

		std::map<size_t, SettingId> aggregateSettings;
		for (const auto& [candidateKey, candidate] : candidates) {
			(void)candidateKey;
			bool complete = candidate.valid && candidate.components.size() == candidate.info.componentCount;
			for (size_t component = 0; complete && component < candidate.info.componentCount; ++component)
				complete = candidate.components.contains(
					static_cast<std::int8_t>(candidate.info.componentStart + component));
			std::optional<bool> paused;
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices) {
					if (!paused)
						paused = entries[index].paused;
					else if (*paused != entries[index].paused)
						complete = false;
				}
			}
			if (!complete)
				continue;
			const auto firstIndex = candidate.components.begin()->second.front();
			const auto setting = MakeControlSettingId(
				entries[firstIndex], candidate.info, getFeatureDisplayName(entries[firstIndex].featureShortName));
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices)
					aggregateSettings.emplace(index, setting);
			}
		}

		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& e = entries[idx];
			if (filterBySource && e.source != sourceFilter)
				continue;
			if (transitionOnly && !IsTransitionEntry(e))
				continue;
			int p = static_cast<int>(e.period);
			if (p < 0)
				continue;
			if (p >= kPeriodCount)
				p = kPeriodlessEntrySlot;
			auto aggregate = aggregateSettings.find(idx);
			SettingId setting = aggregate != aggregateSettings.end() ?
			                    aggregate->second :
			                    MakeSettingId(e, getFeatureDisplayName(e.featureShortName));
			auto [it, inserted] = group.map.try_emplace(setting);
			if (inserted) {
				it->second.fill(SIZE_MAX);
				group.order.push_back(setting);
			}
			if (it->second[p] == SIZE_MAX)
				it->second[p] = idx;
			group.members[setting][p].push_back(idx);
		}
		std::sort(group.order.begin(), group.order.end());
		return group;
	}

	void SplitBySource(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		std::vector<size_t>& overwriteOut, std::vector<size_t>& userOut, bool transitionOnly)
	{
		for (size_t i = 0; i < entries.size(); ++i) {
			if (transitionOnly && !IsTransitionEntry(entries[i]))
				continue;
			(entries[i].source == EntrySource::Overwrite ? overwriteOut : userOut).push_back(i);
		}
	}

	void RemoveIndicesReversed(const std::vector<size_t>& indices, std::function<void(size_t)> removeFn)
	{
		auto sorted = indices;
		std::sort(sorted.begin(), sorted.end(), std::greater<>());
		for (auto idx : sorted)
			removeFn(idx);
	}

	using OverrideKey = std::tuple<std::string, std::vector<std::string>, std::string, int>;

	static OverrideKey MakeOverrideKey(const SettingEntry& entry)
	{
		return { entry.featureShortName, entry.settingPath, entry.settingKey, static_cast<int>(entry.period) };
	}

	static std::set<OverrideKey> BuildActiveOverrideSet(const std::vector<SettingEntry>& entries)
	{
		std::set<OverrideKey> overrides;
		for (const auto& entry : entries)
			if (entry.source == EntrySource::Overwrite && !entry.paused)
				overrides.insert(MakeOverrideKey(entry));
		return overrides;
	}

	static bool IsOverridden(const std::set<OverrideKey>& overrides, const SettingEntry& entry)
	{
		return !overrides.empty() && overrides.contains(MakeOverrideKey(entry));
	}

	static bool HasOverriddenUserEntries(const std::vector<SettingEntry>& entries)
	{
		auto overrides = BuildActiveOverrideSet(entries);
		return std::any_of(entries.begin(), entries.end(),
			[&](const auto& entry) { return entry.source == EntrySource::User && IsOverridden(overrides, entry); });
	}

	static bool AreAllPaused(const std::vector<size_t>& indices, const std::vector<SettingEntry>& entries)
	{
		return std::all_of(indices.begin(), indices.end(),
			[&](size_t idx) { return idx < entries.size() && entries[idx].paused; });
	}

	/// Request a confirmation popup for deleting overwrite entries by indices.
	static void RequestOverwriteRowDelete(PopupState& popups,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const std::vector<size_t>& indices)
	{
		std::set<std::string> filenames;
		for (auto idx : indices)
			if (idx < entries.size())
				filenames.insert(entries[idx].sourceFilename);
		std::string fileList;
		for (const auto& f : filenames) {
			if (!fileList.empty())
				fileList += ", ";
			fileList += "'" + f + "'";
		}
		popups.pendingDeleteRow = indices;
		popups.deleteRowOverwrite.message = std::vformat(
			T("feature.scene_manager.confirm.delete_overwrite_entries",
				"Delete overwrite entries from {0}?\nEmpty overwrite files will be removed from disk."),
			std::make_format_args(fileList));
		popups.deleteRowOverwrite.Request();
	}

	struct FlyoutSource
	{
		ImVec2 cursor;
		ImGuiID id;
		bool pressed;
	};

	static FlyoutSource SubmitFlyoutSource(const char* id, const ImVec2& minimum, const ImVec2& maximum)
	{
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(minimum);
		const bool pressed = ImGui::InvisibleButton(
			id, ImVec2(std::max(1.0f, maximum.x - minimum.x), std::max(1.0f, maximum.y - minimum.y)));
		return { cursor, ImGui::GetItemID(), pressed };
	}

	static void RestoreFlyoutSourceCursor(const FlyoutSource& source)
	{
		ImGui::SetCursorScreenPos(source.cursor);
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	}

	static void ApplyGroupControlResult(const FlyoutResult& result, const std::vector<size_t>& indices,
		bool allPaused, bool isOverwrite, PopupState* popups, const std::vector<SettingEntry>& entries,
		const TableCallbacks& cb, Util::FlyoutState* flyout)
	{
		if (result.toggled)
			for (auto idx : indices)
				if (idx < entries.size() && entries[idx].paused == allPaused)
					cb.togglePause(idx);
		if (result.reverted)
			for (auto idx : indices)
				cb.revert(idx);
		if (!result.deleted)
			return;

		if (popups && isOverwrite) {
			RequestOverwriteRowDelete(*popups, entries, indices);
			if (flyout)
				Util::RequestCloseFlyout(*flyout);
		} else {
			RemoveIndicesReversed(indices, cb.remove);
			if (flyout)
				Util::CloseFlyout(*flyout);
		}
	}

	// --- Feature name resolution by scene type ---

	static std::vector<std::string> GetFeatureNamesForType(SceneType type)
	{
		switch (type) {
		case SceneType::InteriorOnly:
			return SceneSettingsManager::GetInteriorRelevantFeatureNames();
		case SceneType::Location:
			return SceneSettingsManager::GetLocationRelevantFeatureNames();
		default:
			return SceneSettingsManager::GetExteriorRelevantFeatureNames();
		}
	}

	static std::string GetDescriptorDisplayName(const SceneSettingDescriptor& descriptor)
	{
		return descriptor.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(descriptor.key) : descriptor.displayName;
	}

	static void RebuildSettingTree(AddSettingState& state)
	{
		state.settingTree = {};
		state.selectedSubFeaturePath.clear();

		for (size_t i = 0; i < state.cachedSettings.size(); ++i) {
			auto* node = &state.settingTree;
			for (const auto& part : state.cachedSettings[i].displayPath)
				node = &node->children[part];
			node->settings.push_back(i);
		}
	}

	static int GetStoredAllMemberIndex(const SceneSettingDescriptor& descriptor)
	{
		for (size_t index = 0; index < descriptor.members.size(); ++index)
			if (descriptor.members[index].aggregateAll)
				return static_cast<int>(index);
		return -1;
	}

	static void CacheSettings(AddSettingState& state, std::vector<SceneSettingDescriptor> settings)
	{
		state.cachedSettings = std::move(settings);
		state.selectedSettings.assign(state.cachedSettings.size(), false);
		state.selectedMembers.assign(state.cachedSettings.size(), -1);
		for (size_t index = 0; index < state.cachedSettings.size(); ++index) {
			const auto storedAll = GetStoredAllMemberIndex(state.cachedSettings[index]);
			if (storedAll >= 0)
				state.selectedMembers[index] = storedAll;
		}
		RebuildSettingTree(state);
	}

	static std::string GetDescriptorMemberName(const SceneSettingDescriptor& descriptor, size_t memberIndex)
	{
		if (memberIndex >= descriptor.members.size())
			return {};
		const auto& name = descriptor.members[memberIndex].componentDisplayName;
		return name.empty() ? std::to_string(memberIndex + 1) : name;
	}

	template <class Callback>
	static void ForEachSelectedDescriptorMember(
		const SceneSettingDescriptor& descriptor, int selectedMember, Callback&& callback)
	{
		if (selectedMember >= 0 && selectedMember < static_cast<int>(descriptor.members.size())) {
			callback(descriptor.members[selectedMember]);
			return;
		}
		for (const auto& member : descriptor.members)
			callback(member);
	}

	static bool IsValidChildIndex(const AddSettingNode& node, int index)
	{
		return index >= 0 && index < static_cast<int>(node.children.size());
	}

	static const AddSettingNode* GetChildByIndex(const AddSettingNode& node, int index)
	{
		if (!IsValidChildIndex(node, index))
			return nullptr;
		auto it = node.children.begin();
		std::advance(it, index);
		return &it->second;
	}

	static bool DrawSubFeatureBackButton(size_t level)
	{
		auto* menu = globals::menu;
		float buttonSize = ImGui::GetFrameHeight();
		float iconPadding = buttonSize * C::FLYOUT_REVERT_PAD_SCALE;
		ImGui::PushID(static_cast<int>(level));
		bool clicked = menu && menu->uiIcons.undo.texture ?
		                   DrawSceneIconButton("##SubFeatureBack", menu->uiIcons.undo.texture,
							   ImVec2(buttonSize, buttonSize), iconPadding) :
		                   ImGui::ArrowButton("##SubFeatureBack", ImGuiDir_Left);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.scene_manager.action.show_parent_settings", "Show parent settings"));
		ImGui::PopID();
		return clicked;
	}

	static const AddSettingNode* DrawSubFeatureSelectors(AddSettingState& state)
	{
		const auto* node = &state.settingTree;
		for (size_t level = 0; !node->children.empty(); ++level) {
			int selectedIdx = level < state.selectedSubFeaturePath.size() ?
			                      state.selectedSubFeaturePath[level] :
			                      kNoSubFeatureSelection;
			auto selectedIt = node->children.begin();
			bool hasSelection = IsValidChildIndex(*node, selectedIdx);
			if (hasSelection)
				std::advance(selectedIt, selectedIdx);
			auto label = hasSelection ? selectedIt->first :
				std::string(T("feature.scene_manager.select_subfeature", "Select Sub Feature..."));

			ImGui::Spacing();
			bool canGoBack = hasSelection;
			float width = ImGui::GetContentRegionAvail().x;
			if (canGoBack)
				width = std::max(ImGui::GetFrameHeight(), width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(width);
			ImGui::PushID(static_cast<int>(level));
			if (ImGui::BeginCombo("##SubFeatureSelect", label.c_str())) {
				int i = 0;
				for (const auto& [name, _] : node->children) {
					if (ImGui::Selectable(name.c_str(), i == selectedIdx)) {
						if (state.selectedSubFeaturePath.size() <= level)
							state.selectedSubFeaturePath.resize(level + 1, kNoSubFeatureSelection);
						state.selectedSubFeaturePath[level] = i;
						state.selectedSubFeaturePath.resize(level + 1);
						selectedIdx = i;
						hasSelection = true;
					}
					if (i == selectedIdx)
						ImGui::SetItemDefaultFocus();
					++i;
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();

			if (canGoBack) {
				ImGui::SameLine();
				if (DrawSubFeatureBackButton(level)) {
					state.selectedSubFeaturePath.resize(level);
					return node;
				}
			}

			if (!hasSelection)
				return node;

			node = GetChildByIndex(*node, selectedIdx);
			if (!node)
				return nullptr;
		}
		return node;
	}

	template <class Callback>
	static void ForEachSettingIndex(const AddSettingNode& node, Callback&& callback)
	{
		for (auto idx : node.settings)
			callback(idx);
	}

	// --- Duplicate checking by scene type ---

	static bool IsAlreadyAdded(SceneType type, const std::string& feature,
		const std::vector<std::string>& path, const std::string& key, Period period)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		return (type == SceneType::TimeOfDay) ? manager->HasEntryForPeriod(feature, path, key, period, EntrySource::User) : manager->HasEntryFromSource(type, feature, path, key, EntrySource::User);
	}

	template <class IsAddedFn>
	static bool IsAddedForTargetPeriods(const std::string& feature,
		const std::vector<std::string>& path, const std::string& key, Period period, bool addToAllPeriods,
		IsAddedFn&& isAddedFn)
	{
		if (!addToAllPeriods)
			return isAddedFn(feature, path, key, period);

		for (int p = 0; p < kPeriodCount; ++p)
			if (!isAddedFn(feature, path, key, static_cast<Period>(p)))
				return false;
		return true;
	}

	// --- Shared Drawing ---

	void OpenAddDialog(SceneType type, AddSettingState& state)
	{
		state.Reset();
		state.dialogOpen = true;
		state.cachedFeatureNames = GetFeatureNamesForType(type);
	}

	void OpenWeatherAddDialog(RE::FormID /*weatherId*/, AddSettingState& state)
	{
		state.Reset();
		state.dialogOpen = true;
		state.cachedFeatureNames = SceneSettingsManager::GetExteriorRelevantFeatureNames();
	}

	// Core add-setting dialog: renders UI and delegates data ops to callbacks.
	static void DrawAddDialogCore(AddSettingState& state, Period period, bool addToAllPeriods,
		bool selectAggregateMember,
		std::function<std::vector<SceneSettingDescriptor>(const std::string&)> settingsFn,
		std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&, Period)> isAddedFn,
		std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&, const json&, Period)> addFn,
		std::function<void()> commitFn)
	{
		if (!state.dialogOpen)
			return;
		s_addDialogFrame = ImGui::GetFrameCount();

		ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
		ImGui::SetNextWindowSize(ImVec2(C::Em(C::SCENE_ADD_DIALOG_WIDTH_EM), 0));
		auto windowTitle = std::format("{}##{:x}",
			T("feature.scene_manager.add_dialog.title", "Add Feature Settings"),
			reinterpret_cast<uintptr_t>(&state));

		if (!Util::BeginWithRoundedClose(windowTitle.c_str(), &state.dialogOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::End();
			return;
		}
		if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

		auto displayName = (state.selectedFeatureIdx >= 0 &&
							   state.selectedFeatureIdx < static_cast<int>(state.cachedFeatureNames.size())) ?
		                       SceneSettingsManager::GetFeatureDisplayName(state.cachedFeatureNames[state.selectedFeatureIdx]) :
		                       std::string(T("feature.scene_manager.select_feature", "Select Feature..."));

		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##FeatureSelect", displayName.c_str())) {
			for (int i = 0; i < static_cast<int>(state.cachedFeatureNames.size()); ++i) {
				auto itemLabel = SceneSettingsManager::GetFeatureDisplayName(state.cachedFeatureNames[i]);
				if (ImGui::Selectable(itemLabel.c_str(), i == state.selectedFeatureIdx)) {
					state.selectedFeatureIdx = i;
					CacheSettings(state, settingsFn(state.cachedFeatureNames[i]));
				}
				if (i == state.selectedFeatureIdx)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		const AddSettingNode* visibleNode = nullptr;
		if (!state.cachedSettings.empty())
			visibleNode = !state.settingTree.children.empty() ? DrawSubFeatureSelectors(state) : &state.settingTree;

		bool hasVisibleSettings = state.selectedFeatureIdx >= 0 && visibleNode && !visibleNode->settings.empty();
		if (hasVisibleSettings) {
			ImGui::Spacing();
			ImGui::Separator();

			if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
				ForEachSettingIndex(*visibleNode, [&](size_t i) { state.selectedSettings[i] = true; });
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
				ForEachSettingIndex(*visibleNode, [&](size_t i) { state.selectedSettings[i] = false; });

			ImGui::Spacing();

			auto& featureName = state.cachedFeatureNames[state.selectedFeatureIdx];
			if (ImGui::BeginChild("##SettingList", ImVec2(-FLT_MIN, C::Em(C::SCENE_ADD_LIST_HEIGHT_EM)), ImGuiChildFlags_Borders)) {
				ForEachSettingIndex(*visibleNode, [&](size_t i) {
					const auto& descriptor = state.cachedSettings[i];
					const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
					bool alreadyAdded = true;
					ForEachSelectedDescriptorMember(descriptor, selectedMember, [&](const auto& member) {
						alreadyAdded &= IsAddedForTargetPeriods(featureName, member.settingPath, member.key,
							period, addToAllPeriods, isAddedFn);
					});

					auto prettyKey = GetDescriptorDisplayName(descriptor);
					ImGui::PushID(static_cast<int>(i));
					if (alreadyAdded) {
						state.selectedSettings[i] = false;
						auto _ = Util::DisableGuard(true);
						bool checked = true;
						ImGui::Checkbox(prettyKey.c_str(), &checked);
					} else {
						bool sel = state.selectedSettings[i];
						if (ImGui::Checkbox(prettyKey.c_str(), &sel))
							state.selectedSettings[i] = sel;
					}
					if (selectAggregateMember && descriptor.controlType != SceneSettingControlType::Scalar &&
						!descriptor.members.empty()) {
						const bool hasStoredAll = GetStoredAllMemberIndex(descriptor) >= 0;
						ImGui::SameLine();
						ImGui::SetNextItemWidth(C::Em(kChannelSelectorWidthEm));
						const auto preview = state.selectedMembers[i] < 0 ?
						                         std::string(T("feature.scene_manager.channel.all", "All")) :
						                         GetDescriptorMemberName(descriptor, state.selectedMembers[i]);
						if (ImGui::BeginCombo("##Channel", preview.c_str())) {
							const bool allSelected = state.selectedMembers[i] < 0;
							if (!hasStoredAll) {
								if (ImGui::Selectable(T("feature.scene_manager.channel.all", "All"), allSelected))
									state.selectedMembers[i] = -1;
								if (allSelected)
									ImGui::SetItemDefaultFocus();
							}
							for (size_t member = 0; member < descriptor.members.size(); ++member) {
								auto memberName = GetDescriptorMemberName(descriptor, member);
								const bool selected = state.selectedMembers[i] == static_cast<int>(member);
								if (ImGui::Selectable(memberName.c_str(), selected))
									state.selectedMembers[i] = static_cast<int>(member);
								if (selected)
									ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
					}
					ImGui::PopID();
				});
			}
			ImGui::EndChild();

			ImGui::Spacing();

			int selectedCount = 0;
			for (size_t i = 0; i < state.selectedSettings.size(); ++i)
				if (state.selectedSettings[i])
					++selectedCount;

			{
				auto _ = Util::DisableGuard(selectedCount == 0);
				auto label = std::vformat(T("feature.scene_manager.action.add_count", "Add ({0})"),
					std::make_format_args(selectedCount));
				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) {
					bool added = false;
					for (size_t i = 0; i < state.cachedSettings.size(); ++i) {
						if (!state.selectedSettings[i])
							continue;
						const auto& descriptor = state.cachedSettings[i];
						const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
						ForEachSelectedDescriptorMember(descriptor, selectedMember, [&](const auto& member) {
							auto currentValue = SceneSettingsManager::GetFeatureSettingValue(
								featureName, member.settingPath, member.key);
							if (currentValue.is_null())
								currentValue = member.value;
							if (addToAllPeriods) {
								for (int p = 0; p < kPeriodCount; ++p)
									if (!isAddedFn(featureName, member.settingPath, member.key, static_cast<Period>(p)))
										added |= addFn(featureName, member.settingPath, member.key, currentValue, static_cast<Period>(p));
							} else if (!isAddedFn(featureName, member.settingPath, member.key, period)) {
								added |= addFn(featureName, member.settingPath, member.key, currentValue, period);
							}
						});
					}
					if (added)
						commitFn();
					state.dialogOpen = false;
				}
			}
		}

		ImGui::End();
	}

	void DrawAddSettingDialog(SceneType type, AddSettingState& state, Period period, bool addToAllPeriods)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawAddDialogCore(state, period, addToAllPeriods,
			type == SceneType::TimeOfDay,
			[type](const std::string& feat) { return (type == SceneType::TimeOfDay) ? SceneSettingsManager::GetTransitionableSceneSettings(feat) : SceneSettingsManager::GetFeatureSceneSettings(feat); },
			[type](const std::string& feat, const std::vector<std::string>& path, const std::string& key, Period p) { return IsAlreadyAdded(type, feat, path, key, p); },
			[=](const std::string& feat, const std::vector<std::string>& path, const std::string& key, const json& val, Period p) { return manager->AddSetting(type, feat, path, key, val, p, true); },
			[=]() { manager->CommitSceneSettingChanges(); });
	}

	void DrawWeatherAddDialog(RE::FormID weatherId, AddSettingState& state, Period period, bool addToAllPeriods)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawAddDialogCore(state, period, addToAllPeriods,
			true,
			[](const std::string& feat) { return SceneSettingsManager::GetTransitionableSceneSettings(feat); },
			[=](const std::string& feat, const std::vector<std::string>& path, const std::string& key, Period p) { return manager->HasWeatherEntryForPeriod(weatherId, feat, path, key, p, EntrySource::User); },
			[=](const std::string& feat, const std::vector<std::string>& path, const std::string& key, const json&, Period p) { return manager->AddWeatherSetting(weatherId, feat, path, key, p, true); },
			[=]() { manager->SaveAllUserSettings(); });
	}

	FlyoutResult DrawFlyoutControls(bool paused, bool isGroup, bool isOverwrite)
	{
		FlyoutResult result;
		float frameH = ImGui::GetFrameHeight();
		float buttonH = frameH * C::FLYOUT_BUTTON_SCALE;
		float toggleH = buttonH * C::FLYOUT_TOGGLE_SCALE;
		float toggleOffsetY = (buttonH - toggleH) * 0.5f;

		ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + toggleOffsetY));
		bool active = !paused;
		if (Util::FeatureToggle(isGroup ? "##groupActive" : "##active", &active, GetCompactFeatureToggleSize()))
			result.toggled = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(paused ?
				(isGroup ? T("feature.scene_manager.action.unpause_all", "Unpause All") : T("feature.scene_manager.status.paused", "Paused")) :
				(isGroup ? T("feature.scene_manager.action.pause_all", "Pause All") : T("feature.scene_manager.status.active", "Active")));

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		auto* menu = globals::menu;
		float iconH = frameH * C::FLYOUT_BUTTON_SCALE;
		float revertPad = iconH * C::FLYOUT_REVERT_PAD_SCALE;
		if (isOverwrite)
			ImGui::BeginDisabled();
		if (menu && DrawSceneIconButton(isGroup ? "##revertAll" : "##revert", menu->uiIcons.undo.texture, ImVec2(iconH, iconH), revertPad))
			result.reverted = true;
		if (!isOverwrite) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(isGroup ?
					T("feature.scene_manager.action.revert_all", "Revert all to default") :
					T("feature.scene_manager.action.revert", "Revert to default"));
		}
		if (isOverwrite)
			ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		if (DrawSceneDeleteButton("X", iconH))
			result.deleted = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(isGroup ?
				T("feature.scene_manager.action.remove_all", "Remove all") :
				T("feature.scene_manager.action.remove", "Remove"));

		return result;
	}

	// Core value editor: renders the catalog-approved widget and calls updateFn on change.
	static void DrawValueEditorCore(const SettingEntry& entry, float inputWidth, bool readOnly,
		std::function<void(const json&)> updateFn, std::function<void()> commitFn)
	{
		const auto& value = entry.value;
		const auto choiceCount = SceneSettingsManager::GetSettingChoiceCount(entry);
		const bool booleanControl = SceneSettingsManager::IsBooleanControlSetting(entry);
		auto settingType = booleanControl ? SceneSettingsManager::SettingType::Boolean :
		                                      SceneSettingsManager::DetectSettingType(value);
		int readOnlyStyleColors = 0;

		if (readOnly) {
			// Save alpha before/after BeginDisabled to compute our contribution.
			// Nested BeginDisabled (e.g. when paused) won't change alpha, so boost = 1.0 (no counteraction).
			float alphaBefore = ImGui::GetStyle().Alpha;
			ImGui::BeginDisabled();
			float alphaAfter = ImGui::GetStyle().Alpha;
			float boost = (alphaAfter > 0.0f) ? alphaBefore / alphaAfter : 1.0f;

			if (settingType == SceneSettingsManager::SettingType::Boolean) {
				// Boost checkmark alpha to counteract only our disabled dimming
				ImVec4 cm = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
				cm.w *= boost;
				ImGui::PushStyleColor(ImGuiCol_CheckMark, cm);
				readOnlyStyleColors = 1;
			} else {
				// Transparent frame so overwrite values look like plain text
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
				// Boost text alpha to counteract only our disabled dimming
				ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
				tc.w *= boost;
				ImGui::PushStyleColor(ImGuiCol_Text, tc);
				readOnlyStyleColors = 5;
			}
		}

		if (choiceCount > 0) {
			const auto currentValue = value.get<std::int64_t>();
			std::string preview;
			for (size_t choiceIndex = 0; choiceIndex < choiceCount; ++choiceIndex) {
				std::int64_t choiceValue = 0;
				std::string choiceName;
				if (SceneSettingsManager::GetSettingChoice(entry, choiceIndex, choiceValue, choiceName) &&
					choiceValue == currentValue) {
					preview = std::move(choiceName);
					break;
				}
			}
			ImGui::SetNextItemWidth(inputWidth);
			const bool comboOpen = ImGui::BeginCombo("##val", preview.c_str());
			const ImGuiLastItemData comboItem = GImGui->LastItemData;
			if (comboOpen) {
				for (size_t choiceIndex = 0; choiceIndex < choiceCount; ++choiceIndex) {
					std::int64_t choiceValue = 0;
					std::string choiceName;
					if (!SceneSettingsManager::GetSettingChoice(entry, choiceIndex, choiceValue, choiceName))
						continue;
					const bool selected = choiceValue == currentValue;
					if (ImGui::Selectable(choiceName.c_str(), selected) && !readOnly) {
						updateFn(json(choiceValue));
						commitFn();
					}
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
				GImGui->LastItemData = comboItem;
			}
		} else switch (settingType) {
		case SceneSettingsManager::SettingType::Boolean:
			{
				const bool invertedDisplay = SceneSettingsManager::IsInvertedDisplaySetting(entry);
				const bool storedValue = value.is_boolean() ? value.get<bool>() : value.get<int64_t>() != 0;
				bool val = invertedDisplay ? !storedValue : storedValue;
				if (ImGui::Checkbox("##val", &val) && !readOnly) {
					const bool updatedValue = invertedDisplay ? !val : val;
					updateFn(value.is_boolean() ? json(updatedValue) : json(updatedValue ? 1 : 0));
					commitFn();
				}
			}
			break;
		case SceneSettingsManager::SettingType::Float:
			{
				double minimum = 0.0;
				double maximum = 0.0;
				bool hasBounds = SceneSettingsManager::GetNumericBounds(entry, minimum, maximum);
				double minimumDisplayValue = 0.0;
				double maximumDisplayValue = 0.0;
				hasBounds = hasBounds &&
				            SceneSettingsManager::GetNumericDisplayValue(entry, minimum, minimumDisplayValue) &&
				            SceneSettingsManager::GetNumericDisplayValue(entry, maximum, maximumDisplayValue) &&
				            minimumDisplayValue <= maximumDisplayValue;
				double displayValue = 0.0;
				if (!value.is_number() ||
					!SceneSettingsManager::GetNumericDisplayValue(entry, value.get<double>(), displayValue))
					displayValue = hasBounds ? minimumDisplayValue : 0.0;
				float val = static_cast<float>(displayValue);
				if (!std::isfinite(val))
					val = hasBounds ? static_cast<float>(minimumDisplayValue) : 0.0f;
				const double displayScale = SceneSettingsManager::GetNumericDisplayScale(entry);
				const float minimumValue = hasBounds ? static_cast<float>(minimumDisplayValue) : 0.0f;
				const float maximumValue = hasBounds ? static_cast<float>(maximumDisplayValue) : 0.0f;
				const float dragSpeed = static_cast<float>(kSceneFloatDragSpeed * displayScale);
				ImGui::SetNextItemWidth(inputWidth);
				if (ImGui::DragFloat("##val", &val, dragSpeed, minimumValue, maximumValue,
						displayScale == 1.0 ? "%.3f" : "%.1f") && !readOnly) {
					double storedValue = 0.0;
					if (SceneSettingsManager::GetNumericStoredValue(entry, val, storedValue))
						updateFn(json(storedValue));
				}
				if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
					commitFn();
			}
			break;
		case SceneSettingsManager::SettingType::Integer:
			{
				double minimum = 0.0;
				double maximum = 0.0;
				const bool hasBounds = SceneSettingsManager::GetNumericBounds(entry, minimum, maximum);
				std::int64_t val = value.get<std::int64_t>();
				const auto minimumValue = static_cast<std::int64_t>(minimum);
				const auto maximumValue = static_cast<std::int64_t>(maximum);
				ImGui::SetNextItemWidth(inputWidth);
				if (ImGui::DragScalar("##val", ImGuiDataType_S64, &val, kSceneIntDragSpeed,
						hasBounds ? &minimumValue : nullptr,
						hasBounds ? &maximumValue : nullptr, "%lld") && !readOnly)
					updateFn(json(val));
				if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
					commitFn();
			}
			break;
		case SceneSettingsManager::SettingType::String:
			{
				std::string val = value.is_string() ? value.get<std::string>() : std::string();
				ImGui::SetNextItemWidth(inputWidth);
				if (ImGui::InputText("##val", &val) && !readOnly)
					updateFn(json(val));
				if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
					commitFn();
			}
			break;
		default:
			ImGui::TextDisabled("%s", T("feature.scene_manager.unsupported_type", "(unsupported type)"));
			break;
		}

		if (readOnly) {
			ImGui::PopStyleColor(readOnlyStyleColors);
			ImGui::EndDisabled();
		}
	}

	template <class Update, class Commit>
	static bool DrawAggregateValueEditorCore(const std::vector<SettingEntry>& entries,
		const std::vector<size_t>& indices, float inputWidth, bool readOnly, Update&& update, Commit&& commit)
	{
		if (indices.size() < 2)
			return false;

		struct Component
		{
			SceneSettingsManager::SettingControlInfo info;
			std::vector<size_t> entryIndices;
		};
		std::map<std::int8_t, Component> components;
		std::optional<bool> paused;
		for (const auto index : indices) {
			if (index >= entries.size() || !entries[index].value.is_number())
				return false;
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entries[index], info) ||
				info.controlType == SceneSettingControlType::Scalar)
				return false;
			if (!paused)
				paused = entries[index].paused;
			else if (*paused != entries[index].paused)
				return false;
			auto [componentIt, inserted] = components.try_emplace(info.componentIndex);
			if (inserted)
				componentIt->second.info = std::move(info);
			else {
				const auto& existing = componentIt->second.info;
				if (info.controlType != existing.controlType || info.settingPath != existing.settingPath ||
					info.settingKey != existing.settingKey || info.componentStart != existing.componentStart ||
					info.componentCount != existing.componentCount)
					return false;
			}
			if (std::find(componentIt->second.entryIndices.begin(), componentIt->second.entryIndices.end(), index) ==
				componentIt->second.entryIndices.end())
				componentIt->second.entryIndices.push_back(index);
		}
		if (components.size() < 2 || components.size() > 4)
			return false;

		const auto& first = components.begin()->second.info;
		if (components.size() != first.componentCount)
			return false;
		if (first.controlType == SceneSettingControlType::Color &&
			components.size() != 3 && components.size() != 4)
			return false;
		std::array<const Component*, 4> orderedComponents{};
		for (size_t component = 0; component < components.size(); ++component) {
			auto componentIt = components.find(static_cast<std::int8_t>(first.componentStart + component));
			if (componentIt == components.end())
				return false;
			const auto& info = componentIt->second.info;
			if (info.controlType != first.controlType || info.settingPath != first.settingPath ||
				info.settingKey != first.settingKey || info.componentStart != first.componentStart ||
				info.componentCount != first.componentCount ||
				info.componentIndex != first.componentStart + component)
				return false;
			orderedComponents[component] = &componentIt->second;
		}

		std::array<float, 4> values{};
		for (size_t component = 0; component < components.size(); ++component)
			values[component] = entries[orderedComponents[component]->entryIndices.back()].value.get<float>();

		if (readOnly)
			ImGui::BeginDisabled();
		ImGui::BeginGroup();
		ImGui::SetNextItemWidth(inputWidth);
		bool changed = false;
		bool editDeactivated = false;
		if (first.controlType == SceneSettingControlType::Color) {
			constexpr auto flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
			changed = components.size() == 3 ?
			              ImGui::ColorEdit3("##val", values.data(), flags) :
			              ImGui::ColorEdit4("##val", values.data(), flags);
			editDeactivated = ImGui::IsItemDeactivatedAfterEdit();
		} else {
			std::array<double, 4> minimums{};
			std::array<double, 4> maximums{};
			std::array<double, 4> displayScales{};
			std::array<bool, 4> hasBounds{};
			for (size_t component = 0; component < components.size(); ++component) {
				const auto entryIndex = orderedComponents[component]->entryIndices.back();
				const auto& entry = entries[entryIndex];
				hasBounds[component] = SceneSettingsManager::GetNumericBounds(
					entry, minimums[component], maximums[component]);
				displayScales[component] = SceneSettingsManager::GetNumericDisplayScale(entry);
				double minimumDisplayValue = 0.0;
				double maximumDisplayValue = 0.0;
				hasBounds[component] = hasBounds[component] &&
				                       SceneSettingsManager::GetNumericDisplayValue(
						                       entry, minimums[component], minimumDisplayValue) &&
				                       SceneSettingsManager::GetNumericDisplayValue(
						                       entry, maximums[component], maximumDisplayValue) &&
				                       minimumDisplayValue <= maximumDisplayValue;
				minimums[component] = minimumDisplayValue;
				maximums[component] = maximumDisplayValue;
				double displayValue = 0.0;
				if (!SceneSettingsManager::GetNumericDisplayValue(entry, values[component], displayValue))
					displayValue = hasBounds[component] ? minimumDisplayValue : 0.0;
				values[component] = static_cast<float>(displayValue);
			}
			const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			const float componentWidth = std::max(
				(inputWidth - spacing * static_cast<float>(components.size() - 1)) /
					static_cast<float>(components.size()),
				1.0f);
			for (size_t component = 0; component < components.size(); ++component) {
				const auto displayScale = displayScales[component];
				const float minimumValue = hasBounds[component] ?
				                               static_cast<float>(minimums[component]) : 0.0f;
				const float maximumValue = hasBounds[component] ?
				                               static_cast<float>(maximums[component]) : 0.0f;
				const float speed = static_cast<float>(kSceneFloatDragSpeed * displayScale);
				const char* valueFormat = displayScale == 1.0 ? "%.3f" : "%.1f";
				if (component > 0)
					ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(static_cast<int>(component));
				ImGui::SetNextItemWidth(componentWidth);
				std::string componentName;
				for (const char ch : orderedComponents[component]->info.componentDisplayName) {
					componentName.push_back(ch);
					if (ch == '%')
						componentName.push_back('%');
				}
				const auto format = std::format("{} {}", valueFormat, componentName);
				changed |= ImGui::DragFloat(
					"##val", &values[component], speed, minimumValue, maximumValue, format.c_str());
				editDeactivated |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::PopID();
			}
		}
		ImGui::EndGroup();

		if (changed && !readOnly) {
			std::vector<SceneSettingsManager::EntryValueUpdate> updates;
			updates.reserve(indices.size());
			for (size_t component = 0; component < components.size(); ++component) {
				const auto& componentEntries = orderedComponents[component]->entryIndices;
				const auto& entry = entries[componentEntries.back()];
				double storedValue = values[component];
				const bool validValue = first.controlType == SceneSettingControlType::Color ?
				                            std::isfinite(storedValue) :
				                            SceneSettingsManager::GetNumericStoredValue(
					                            entry, values[component], storedValue);
				if (validValue)
					for (const auto entryIndex : orderedComponents[component]->entryIndices)
						updates.push_back({ entryIndex, json(storedValue) });
			}
			if (updates.size() == indices.size())
				update(updates);
		}
		if (!readOnly && editDeactivated)
			commit();
		if (readOnly)
			ImGui::EndDisabled();
		return true;
	}

	void DrawValueEditor(SceneType type, size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = manager->GetEntries(type)[index];
		DrawValueEditorCore(entry, inputWidth, readOnly,
			[=](const json& v) { manager->UpdateEntryValue(type, index, v, true); },
			[=]() { manager->SaveAllUserSettings(); });
	}

	static void DrawValueEditor(
		SceneType type, const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(type);
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, readOnly,
				[=](const auto& updates) { manager->UpdateEntryValues(type, updates, true); },
				[=] { manager->CommitSceneSettingChanges(); }))
			return;
		DrawValueEditor(type, indices.back(), inputWidth, readOnly);
	}

	void DrawWeatherValueEditor(RE::FormID weatherId, size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = manager->GetWeatherConfig(weatherId).entries[index];
		DrawValueEditorCore(entry, inputWidth, readOnly,
			[=](const json& v) { manager->UpdateWeatherEntryValue(weatherId, index, v, true); },
			[=]() { manager->SaveAllUserSettings(); });
	}

	void DrawWeatherValueEditor(RE::FormID weatherId, const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetWeatherConfig(weatherId).entries;
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, readOnly,
				[=](const auto& updates) { manager->UpdateWeatherEntryValues(weatherId, updates, true); },
				[=] { manager->SaveAllUserSettings(); }))
			return;
		const auto& entry = entries[indices.back()];
		DrawValueEditorCore(entry, inputWidth, readOnly,
			[=](const json& value) {
				std::vector<SceneSettingsManager::EntryValueUpdate> updates;
				updates.reserve(indices.size());
				for (const auto index : indices)
					updates.push_back({ index, value });
				manager->UpdateWeatherEntryValues(weatherId, updates, true);
			},
			[=]() { manager->SaveAllUserSettings(); });
	}

	void DrawPopups(SceneType type, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();

		if (popups.deleteAllOverwrites.Draw())
			manager->DeleteAllOverwrites(type);

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetEntries(type).size())
				manager->RemoveSetting(type, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			RemoveIndicesReversed(popups.pendingDeleteRow, [&](size_t idx) {
				if (idx < manager->GetEntries(type).size())
					manager->RemoveSetting(type, idx);
			});
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			manager->DeleteAllUserSettings(type);
	}

	static void ConfigurePopups(PopupState& popups, const char* overwriteMessage, const char* userMessage)
	{
		const auto* deleteLabel = T("feature.scene_manager.action.delete", "Delete");
		const auto* deleteAllLabel = T("feature.scene_manager.action.delete_all", "Delete All");
		const auto* cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");

		popups.deleteAllOverwrites.title = T("feature.scene_manager.confirm.delete_all_overwrites_title", "Delete All Overwrites?");
		popups.deleteAllOverwrites.message = overwriteMessage;
		popups.deleteAllOverwrites.confirmLabel = deleteAllLabel;
		popups.deleteAllOverwrites.cancelLabel = cancelLabel;

		popups.deleteSingleOverwrite.title = T("feature.scene_manager.confirm.delete_overwrite_file_title", "Delete Overwrite File?");
		popups.deleteSingleOverwrite.confirmLabel = deleteLabel;
		popups.deleteSingleOverwrite.cancelLabel = cancelLabel;

		popups.deleteRowOverwrite.title = T("feature.scene_manager.confirm.delete_overwrite_row_title", "Delete Overwrite Row?");
		popups.deleteRowOverwrite.confirmLabel = deleteLabel;
		popups.deleteRowOverwrite.cancelLabel = cancelLabel;

		popups.deleteAllUser.title = T("feature.scene_manager.confirm.delete_all_user_title", "Delete All User Settings?");
		popups.deleteAllUser.message = userMessage;
		popups.deleteAllUser.confirmLabel = deleteAllLabel;
		popups.deleteAllUser.cancelLabel = cancelLabel;
	}

	static int GetSettingLabelMaxLines()
	{
		return static_cast<int>(C::SCENE_SETTING_MAX_LINES);
	}

	static std::string GetSettingLabel(const SettingId& setting)
	{
		return setting.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(setting.key) : setting.displayName;
	}

	static float GetSettingLabelVisualHeight()
	{
		return ImGui::GetTextLineHeight() * C::SCENE_SETTING_MAX_LINES;
	}

	static std::string TruncateTextToFitWidth(std::string text, float width)
	{
		if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		size_t visibleLen = text.size();
		while (visibleLen > 0 &&
		       ImGui::CalcTextSize((text.substr(0, visibleLen) + kEllipsis).c_str()).x > width)
			visibleLen = PreviousUtf8CodepointBoundary(text, visibleLen);
		return text.substr(0, visibleLen) + kEllipsis;
	}

	static std::string TruncateWrappedTextToLines(std::string text, float wrapWidth, int maxLines)
	{
		assert(maxLines > 0);
		if (text.empty() || wrapWidth <= 0.0f)
			return text;

		const float fixedH = ImGui::GetTextLineHeight() * maxLines;
		if (ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth).y <= fixedH + kLabelOverflowTolerance)
			return text;

		auto* font = ImGui::GetFont();
		const char* textBegin = text.c_str();
		const char* textEnd = textBegin + text.size();
		const char* lineStart = textBegin;
		for (int line = 0; line < maxLines && lineStart < textEnd; ++line) {
			const char* nextLine = font->CalcWordWrapPositionA(1.0f, lineStart, textEnd, wrapWidth);
			if (nextLine <= lineStart)
				break;
			lineStart = nextLine;
		}

		size_t visibleLen = static_cast<size_t>(lineStart - textBegin);
		while (visibleLen > 0 &&
		       ImGui::CalcTextSize((text.substr(0, visibleLen) + kEllipsis).c_str(), nullptr, false, wrapWidth).y > fixedH + kLabelOverflowTolerance)
			visibleLen = PreviousUtf8CodepointBoundary(text, visibleLen);
		return text.substr(0, visibleLen) + kEllipsis;
	}

	static void DrawSettingLabel(const SettingId& setting)
	{
		const float wrapWidth = ImGui::GetContentRegionAvail().x;
		const float fixedH = GetSettingLabelVisualHeight();

		if (setting.parentPath.empty()) {
			auto text = TruncateWrappedTextToLines(GetSettingLabel(setting), wrapWidth, GetSettingLabelMaxLines());
			ImGui::TextWrapped("%s", text.c_str());
		} else {
			auto parent = TruncateTextToFitWidth(JoinDisplayParts(setting.parentPath), wrapWidth);
			auto leaf = TruncateTextToFitWidth(GetSettingLabel(setting), wrapWidth);
			auto text = std::format("{}\n{}", parent, leaf);
			ImGui::TextUnformatted(text.c_str());
		}

		const float usedH = ImGui::GetItemRectSize().y;
		if (usedH < fixedH) {
			const float pad = fixedH - usedH - ImGui::GetStyle().ItemSpacing.y;
			if (pad > 0.0f)
				ImGui::Dummy(ImVec2(0, pad));
		}
	}

	static bool HasInlineActionColumn(int numValueColumns)
	{
		return numValueColumns == 1;
	}

	static float GetActionsColumnWidth()
	{
		const float controlWidth = GetCompactFeatureToggleSize().x +
		                           GetSceneActionButtonSize() * 2.0f +
		                           ImGui::GetStyle().ItemSpacing.x * kActionControlSpacingCount;
		return std::max(C::Em(kActionsColumnMinWidthEm), controlWidth);
	}

	static void CenterCursorY(float rowContentHeight, float itemHeight)
	{
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowContentHeight - itemHeight) * 0.5f));
	}

	static void DrawInlineControls(const std::vector<size_t>& indices, bool isOverwrite, PopupState* popups,
		const std::vector<SettingEntry>& entries, const TableCallbacks& cb)
	{
		if (indices.empty())
			return;

		const bool allPaused = AreAllPaused(indices, entries);
		auto result = DrawFlyoutControls(allPaused, indices.size() > 1, isOverwrite);
		ApplyGroupControlResult(result, indices, allPaused, isOverwrite, popups, entries, cb, nullptr);
	}

	void DrawSourceTable(
		const SourceGroup& group,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const char* tableId,
		EntrySource source,
		int numValueColumns,
		PopupState* popups,
		TableFlyoutState& flyout,
		const TableCallbacks& cb)
	{
		bool isOverwrite = source == EntrySource::Overwrite;
		bool multiColumn = numValueColumns > 1;
		bool inlineActions = HasInlineActionColumn(numValueColumns);
		const bool suppressFlyouts = s_addDialogFrame == ImGui::GetFrameCount();
		if (suppressFlyouts) {
			Util::CloseFlyout(flyout.cell);
			Util::CloseFlyout(flyout.row);
			Util::CloseFlyout(flyout.col);
		}
		constexpr int kSettingColumn = 0;
		constexpr int kFirstValueColumn = 1;
		const int actionsColumn = kFirstValueColumn + numValueColumns;
		int totalCols = actionsColumn + (inlineActions ? 1 : 0);
		struct PendingAddPeriod
		{
			std::string feature;
			std::vector<std::string> path;
			std::string key;
			int period;
		};
		std::vector<size_t> pendingRemoveIndices;
		std::vector<PendingAddPeriod> pendingAddPeriods;
		auto deferredCallbacks = cb;
		deferredCallbacks.remove = [&](size_t index) {
			if (std::find(pendingRemoveIndices.begin(), pendingRemoveIndices.end(), index) == pendingRemoveIndices.end())
				pendingRemoveIndices.push_back(index);
		};
		if (cb.onAddPeriod) {
			deferredCallbacks.onAddPeriod = [&](const std::string& feature, const std::vector<std::string>& path,
				const std::string& key, int period) {
				pendingAddPeriods.push_back({ feature, path, key, period });
			};
		}

		// Pre-collect per-column indices for header controls (multi-column only)
		std::array<std::vector<size_t>, kPeriodCount> perColumn{};
		if (multiColumn) {
			for (const auto& [setting, perKey] : group.map) {
				const auto members = group.members.find(setting);
				for (int p = 0; p < numValueColumns; ++p) {
					if (members != group.members.end() && !members->second[p].empty())
						perColumn[p].insert(
							perColumn[p].end(), members->second[p].begin(), members->second[p].end());
					else if (perKey[p] != SIZE_MAX)
						perColumn[p].push_back(perKey[p]);
				}
			}
		}

		const ImGuiTableFlags tableFlags =
			ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
		if (!ImGui::BeginTable(tableId, totalCols, tableFlags))
			return;

		// Column setup
		ImGui::TableSetupColumn(T("feature.scene_manager.column.setting", "Setting"),
			ImGuiTableColumnFlags_WidthFixed, C::Em(C::SCENE_TOD_PARAM_COL_EM));
		if (multiColumn) {
			for (int i = 0; i < numValueColumns; ++i)
				ImGui::TableSetupColumn(GetPeriodDisplayName(static_cast<Period>(i)),
					ImGuiTableColumnFlags_WidthFixed, C::Em(C::SCENE_TOD_PERIOD_COL_EM));
		} else {
			ImGui::TableSetupColumn(T("feature.scene_manager.column.value", "Value"), ImGuiTableColumnFlags_WidthFixed,
				C::Em(C::SCENE_TOD_PERIOD_COL_EM) * kSingleValueColumnScale);
		}
		if (inlineActions)
			ImGui::TableSetupColumn(T("feature.scene_manager.column.actions", "Actions"),
				ImGuiTableColumnFlags_WidthFixed, GetActionsColumnWidth());

		if (multiColumn) {
			// Header row
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImGui::TableHeader(T("feature.scene_manager.column.setting", "Setting"));

			for (int i = 0; i < numValueColumns; ++i) {
				ImGui::TableSetColumnIndex(kFirstValueColumn + i);
				ImVec2 cellMin = ImGui::GetCursorScreenPos();
				float colW = ImGui::GetContentRegionAvail().x;
				ImGui::TextUnformatted(GetPeriodDisplayName(static_cast<Period>(i)));
				ImVec2 cellMax(cellMin.x + colW, ImGui::GetItemRectMax().y);

				const auto& indices = perColumn[i];
				if (!indices.empty() && !suppressFlyouts) {
					ImGui::PushID(i);
					const auto flyoutSource = SubmitFlyoutSource("##colFlyout", cellMin, cellMax);
					{
						Util::FlyoutScope flyoutScope(
							flyout.col, flyoutSource.id, flyoutSource.pressed, GetSceneFlyoutStyle());
						if (flyoutScope) {
							bool allPaused = AreAllPaused(indices, entries);
							auto result = DrawFlyoutControls(allPaused, true, isOverwrite);
							ApplyGroupControlResult(result, indices, allPaused, isOverwrite,
								popups, entries, deferredCallbacks, &flyout.col);
						}
					}
					RestoreFlyoutSourceCursor(flyoutSource);
					ImGui::PopID();
				}
			}
		}

		// Data rows
		auto& theme = globals::menu->GetSettings().Theme;
		std::string lastCategoryFeature;
		std::string lastCategoryName;

		auto overrideSet = (source == EntrySource::User) ? BuildActiveOverrideSet(entries) : std::set<OverrideKey>{};

		for (const auto& sid : group.order) {
			// Category header
			if (sid.feature != lastCategoryFeature || sid.categoryName != lastCategoryName) {
				lastCategoryFeature = sid.feature;
				lastCategoryName = sid.categoryName;
				ImGui::TableNextRow();
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
				ImGui::TableSetColumnIndex(kSettingColumn);
				ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);
				const auto visibleCategory = GetVisibleLabel(sid.categoryName);
				ImGui::TextColored(theme.FeatureHeading.ColorDefault, "%.*s:",
					static_cast<int>(visibleCategory.size()), visibleCategory.data());
				ImGui::SetWindowFontScale(1.0f);
			}

			auto keyIt = group.map.find(sid);
			if (keyIt == group.map.end())
				continue;
			const auto& perKey = keyIt->second;
			auto membersIt = group.members.find(sid);
			std::vector<size_t> addPeriodSourceIndices;
			if (membersIt != group.members.end()) {
				for (const auto& periodMembers : membersIt->second) {
					if (!periodMembers.empty()) {
						addPeriodSourceIndices = periodMembers;
						break;
					}
				}
			}

			// Collect valid indices for this row
			// In single-column mode, collect from ALL period slots (entries may span multiple periods)
			std::vector<size_t> rowIndices;
			rowIndices.reserve(kPeriodCount * 4);
			int scanCols = multiColumn ? numValueColumns : kPeriodCount;
			for (int p = 0; p < scanCols; ++p) {
				if (membersIt != group.members.end() && !membersIt->second[p].empty())
					rowIndices.insert(rowIndices.end(), membersIt->second[p].begin(), membersIt->second[p].end());
				else if (perKey[p] != SIZE_MAX)
					rowIndices.push_back(perKey[p]);
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImVec2 cellStart = ImGui::GetCursorScreenPos();
			float cellWidth = ImGui::GetContentRegionAvail().x;
			const float labelRowStartY = ImGui::GetCursorPosY();

			ImGui::PushID(sid.key.c_str());
			ImGui::PushID(static_cast<int>(sid.componentStart));
			for (const auto& part : sid.path)
				ImGui::PushID(part.c_str());
			ImGui::PushID(sid.feature.c_str());

			ImGui::Indent(C::Em(C::SCENE_ENTRY_INDENT_EM));
			ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);

			DrawSettingLabel(sid);
			const float labelVisualH = GetSettingLabelVisualHeight();
			ImGui::SetWindowFontScale(1.0f);
			const float labelContentH = ImGui::GetCursorPosY() - labelRowStartY;

			// Row-level flyout (only for multi-column to avoid duplicate controls)
			if (multiColumn && !suppressFlyouts) {
				// Hover area covers full row for detection
				ImVec2 hoverMin = cellStart;
				float rowH = std::max(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetItemRectMax().y - cellStart.y);
				ImVec2 hoverMax(cellStart.x + cellWidth, cellStart.y + rowH);
				const auto flyoutSource = SubmitFlyoutSource("##rowFlyout", hoverMin, hoverMax);
				{
					Util::FlyoutScope flyoutScope(
						flyout.row, flyoutSource.id, flyoutSource.pressed, GetSceneFlyoutStyle());
					if (flyoutScope) {
						bool allPaused = AreAllPaused(rowIndices, entries);
						auto result = DrawFlyoutControls(allPaused, true, isOverwrite);
						ApplyGroupControlResult(result, rowIndices, allPaused, isOverwrite,
							popups, entries, deferredCallbacks, &flyout.row);
					}
				}
				RestoreFlyoutSourceCursor(flyoutSource);
			}

			ImGui::Unindent(C::Em(C::SCENE_ENTRY_INDENT_EM));

			// Value columns
			if (multiColumn) {
				for (int p = 0; p < numValueColumns; ++p) {
					ImGui::TableSetColumnIndex(kFirstValueColumn + p);
					size_t entryIndex = perKey[p];
					std::vector<size_t> cellIndices;
					if (membersIt != group.members.end() && !membersIt->second[p].empty())
						cellIndices = membersIt->second[p];
					else if (entryIndex != SIZE_MAX)
						cellIndices.push_back(entryIndex);

					if (cellIndices.empty()) {
						if (source == EntrySource::User && deferredCallbacks.onAddPeriod) {
							ImGui::PushID(p);
							const float btnSz = C::Em(C::SCENE_ADD_PERIOD_BTN_EM);
							const float cellW = ImGui::GetContentRegionAvail().x;
							const float visualH = labelContentH - ImGui::GetStyle().ItemSpacing.y;
							ImGui::SetCursorPos(ImVec2(
								ImGui::GetCursorPosX() + std::max(0.f, (cellW - btnSz) * 0.5f),
								ImGui::GetCursorPosY() + std::max(0.f, (visualH - btnSz) * 0.5f)));
							ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
							if (ImGui::Button("+", ImVec2(btnSz, btnSz))) {
								for (const auto sourceIndex : addPeriodSourceIndices) {
									if (sourceIndex >= entries.size())
										continue;
									const auto& sourceEntry = entries[sourceIndex];
									deferredCallbacks.onAddPeriod(
										sourceEntry.featureShortName, sourceEntry.settingPath,
										sourceEntry.settingKey, p);
								}
							}
							ImGui::PopStyleVar();
							ImGui::PopID();
						} else {
							ImGui::TextDisabled("--");
						}
						continue;
					}

					entryIndex = cellIndices.front();
					const auto& entry = entries[entryIndex];
					ImGui::PushID(static_cast<int>(entryIndex));
					const bool allPaused = AreAllPaused(cellIndices, entries);

					if (allPaused)
						ImGui::BeginDisabled();

					const bool isOverridden = std::any_of(
						cellIndices.begin(), cellIndices.end(),
						[&](const size_t index) { return IsOverridden(overrideSet, entries[index]); });
					if (isOverridden) {
						const auto& ec = theme.StatusPalette.Error;
						ImGui::PushStyleColor(ImGuiCol_Text, ec);
						ImGui::PushStyleColor(ImGuiCol_CheckMark, ec);
					}

					if (cellIndices.size() > 1 && deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(
							cellIndices, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);
					else
						deferredCallbacks.drawEditor(
							entryIndex, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);

					if (isOverridden)
						ImGui::PopStyleColor(2);

					if (allPaused)
						ImGui::EndDisabled();

					// Cell flyout
					const bool sourcePressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
					const ImGuiID cellId = ImGui::GetID("##cellFlyout");
					if (!suppressFlyouts) {
						Util::FlyoutScope flyoutScope(
							flyout.cell, cellId, sourcePressed, GetSceneFlyoutStyle());
						if (flyoutScope) {
							auto result = DrawFlyoutControls(allPaused, cellIndices.size() > 1, isOverwrite);

							if (result.toggled) {
								for (const auto index : cellIndices)
									if (entries[index].paused == allPaused)
										deferredCallbacks.togglePause(index);
							}
							if (result.reverted)
								for (const auto index : cellIndices)
									deferredCallbacks.revert(index);
							if (result.deleted) {
								if (popups && entry.source == EntrySource::Overwrite) {
									if (cellIndices.size() > 1) {
										RequestOverwriteRowDelete(*popups, entries, cellIndices);
									} else {
										popups->pendingDeleteIndex = entryIndex;
										popups->deleteSingleOverwrite.message = std::vformat(
											T("feature.scene_manager.confirm.delete_overwrite_entry",
												"Delete overwrite entry from '{0}'?\nThe file will be removed if no settings remain."),
											std::make_format_args(entry.sourceFilename));
										popups->deleteSingleOverwrite.Request();
									}
									Util::RequestCloseFlyout(flyout.cell);
								} else {
									for (const auto index : cellIndices)
										deferredCallbacks.remove(index);
									Util::CloseFlyout(flyout.cell);
								}
							}
						}
					}

					ImGui::PopID();
				}
			} else {
				// Single-column: collapsed view of all entries for this key
				ImGui::TableSetColumnIndex(kFirstValueColumn);
				CenterCursorY(labelVisualH, ImGui::GetFrameHeight());

				if (rowIndices.empty()) {
					ImGui::TextDisabled("--");
					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(labelVisualH, ImGui::GetFrameHeight());
						ImGui::TextDisabled("--");
					}
				} else {
					size_t displayIndex = rowIndices[0];
					bool anyPaused = std::any_of(rowIndices.begin(), rowIndices.end(),
						[&](size_t i) { return i < entries.size() && entries[i].paused; });

					ImGui::PushID(static_cast<int>(displayIndex));

					bool isOverridden = std::any_of(rowIndices.begin(), rowIndices.end(),
						[&](size_t i) { return i < entries.size() && IsOverridden(overrideSet, entries[i]); });

					if (anyPaused)
						ImGui::BeginDisabled();

					if (isOverridden) {
						const auto& ec = theme.StatusPalette.Error;
						ImGui::PushStyleColor(ImGuiCol_Text, ec);
						ImGui::PushStyleColor(ImGuiCol_CheckMark, ec);
					}

					if (deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(rowIndices, ImGui::GetContentRegionAvail().x, isOverwrite);
					else
						deferredCallbacks.drawEditor(displayIndex, ImGui::GetContentRegionAvail().x, isOverwrite);

					if (isOverridden)
						ImGui::PopStyleColor(2);

					if (anyPaused)
						ImGui::EndDisabled();

					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(labelVisualH, ImGui::GetFrameHeight());
						DrawInlineControls(rowIndices, isOverwrite, popups, entries, deferredCallbacks);
					}

					ImGui::PopID();
				}
			}

			// Suppress row flyout when a cell flyout is active to prevent accidental whole-row deletion
			if (multiColumn && flyout.cell.isOpen && !flyout.cell.closing && flyout.row.isOpen && !flyout.row.flyoutHovered)
				flyout.row.closing = true;

			ImGui::PopID();
			for (size_t i = 0; i < sid.path.size(); ++i)
				ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		}

		ImGui::EndTable();
		RemoveIndicesReversed(pendingRemoveIndices, cb.remove);
		for (const auto& pending : pendingAddPeriods)
			cb.onAddPeriod(pending.feature, pending.path, pending.key, pending.period);
	}

	float GetSectionWidth(int numValueColumns)
	{
		auto& style = ImGui::GetStyle();
		bool multiColumn = numValueColumns > 1;
		bool inlineActions = HasInlineActionColumn(numValueColumns);
		int totalCols = 1 + numValueColumns + (inlineActions ? 1 : 0);
		float colSum = C::Em(C::SCENE_TOD_PARAM_COL_EM);
		colSum += multiColumn ? numValueColumns * C::Em(C::SCENE_TOD_PERIOD_COL_EM)
		                      : C::Em(C::SCENE_TOD_PERIOD_COL_EM) * kSingleValueColumnScale;
		if (inlineActions)
			colSum += GetActionsColumnWidth();
		float tableWidth = colSum + totalCols * style.CellPadding.x * 2.0f + (totalCols + 1) * 1.0f;
		// With fewer value columns, extend the header to the target width.
		float extraCols = C::SCENE_SECTION_HEADER_TARGET_COLS - numValueColumns;
		if (extraCols > 0.0f)
			tableWidth += extraCols * (C::Em(C::SCENE_TOD_PERIOD_COL_EM) + style.CellPadding.x * 2.0f + 1.0f);
		return tableWidth;
	}

	bool DrawSectionHeader(const char* label, const char* idSuffix,
		bool allPaused, std::function<void()> onTogglePause, std::function<void()> onDeleteAll,
		int numValueColumns, std::function<void()> onExportAll, bool hasActiveOverrides)
	{
		ImGui::Spacing();
		float w = GetSectionWidth(numValueColumns);
		ImGui::BeginChild(std::format("##sec{}", idSuffix).c_str(), ImVec2(w, 0), ImGuiChildFlags_AutoResizeY);
		auto headerLabel = std::format("{}{}", label, idSuffix);
		bool open = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

		if (open) {
			if (hasActiveOverrides) {
				Util::Text::WrappedError(T("feature.scene_manager.overridden_warning",
					"Feature values are being overridden. Pause overwrites to see changes."));
			}
			if (onExportAll) {
				if (ImGui::SmallButton(std::format("{}{}",
						T("feature.scene_manager.action.export_all", "Export All"), idSuffix).c_str()))
					onExportAll();
				ImGui::SameLine();
			}
			if (ImGui::SmallButton(std::format("{}{}", allPaused ?
						T("feature.scene_manager.action.unpause_all", "Unpause All") :
						T("feature.scene_manager.action.pause_all", "Pause All"), idSuffix).c_str()))
				onTogglePause();
			ImGui::SameLine();
			if (ImGui::SmallButton(std::format("{}{}",
						T("feature.scene_manager.action.delete_all", "Delete All"), idSuffix).c_str()))
				onDeleteAll();
		}
		return open;
	}

	static bool DrawSelectedCheckbox(const std::string& label, uint8_t& selected)
	{
		bool checked = selected != 0;
		if (!ImGui::Checkbox(label.c_str(), &checked))
			return false;
		selected = checked ? 1 : 0;
		return true;
	}

	void EndSection()
	{
		ImGui::EndChild();
	}

	// Core export popup: list user entries with checkboxes (all on by default), then export on confirm.
	static void DrawExportPopupCore(
		const char* popupId,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		ExportAllPopupState& state,
		std::function<void(const std::string&, const std::vector<size_t>&)> exportFn,
		bool showPeriod = true)
	{
		if (!state.dialogOpen)
			return;

		ImGui::OpenPopup(popupId);

		auto popup = Util::CenteredPopupModal(popupId, &state.dialogOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
		if (!popup)
			return;

		ImGui::InputText(T("feature.scene_manager.export.mod_name", "Mod Name"), state.modName, IM_ARRAYSIZE(state.modName));
		auto modName = Util::FileHelpers::SanitizeFileName(state.modName);
		if (modName.empty())
			Util::Text::WrappedDisabled(T("feature.scene_manager.export.enter_mod_name", "Enter a mod name to export."));
		ImGui::Spacing();

		ImGui::TextUnformatted(T("feature.scene_manager.export.select_settings", "Select settings to export as overwrite files:"));
		ImGui::Spacing();

		if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
			std::fill(state.selected.begin(), state.selected.end(), uint8_t(1));
		ImGui::SameLine();
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
			std::fill(state.selected.begin(), state.selected.end(), uint8_t(0));

		ImGui::Spacing();
		if (ImGui::BeginChild("##ExportList", ImVec2(-FLT_MIN, C::Em(C::SCENE_ADD_LIST_HEIGHT_EM)), ImGuiChildFlags_Borders)) {
			if (showPeriod) {
				for (size_t i = 0; i < state.userIndices.size(); ++i) {
					auto idx = state.userIndices[i];
					if (idx >= entries.size())
						continue;
					const auto& e = entries[idx];
					auto label = e.period != SceneSettingsManager::TimeOfDayPeriod::Count
						? std::format("{} - {} ({})", SceneSettingsManager::GetFeatureDisplayName(e.featureShortName),
							GetEntryDisplayName(e), GetPeriodDisplayName(e.period))
						: std::format("{} - {}", SceneSettingsManager::GetFeatureDisplayName(e.featureShortName),
							GetEntryDisplayName(e));
					DrawSelectedCheckbox(std::format("{}##exp{}", label, i), state.selected[i]);
				}
			} else {
				using GroupKey = std::tuple<std::string, std::vector<std::string>, std::string>;
				std::map<GroupKey, std::vector<size_t>> groups;
				for (size_t i = 0; i < state.userIndices.size(); ++i) {
					auto idx = state.userIndices[i];
					if (idx < entries.size())
						groups[{ entries[idx].featureShortName, entries[idx].settingPath, entries[idx].settingKey }].push_back(i);
				}
				for (auto& [gk, stateIs] : groups) {
					bool checked = std::all_of(stateIs.begin(), stateIs.end(), [&](size_t i) { return state.selected[i]; });
					const auto& [feature, path, key] = gk;
					auto entryIndex = state.userIndices[stateIs.front()];
					auto labelId = key;
					for (const auto& part : path)
						labelId += part;
					auto label = std::format("{} - {}##expg{}{}",
						SceneSettingsManager::GetFeatureDisplayName(feature), GetEntryDisplayName(entries[entryIndex]),
						feature, labelId);
					if (ImGui::Checkbox(label.c_str(), &checked))
						for (auto i : stateIs)
							state.selected[i] = checked ? 1 : 0;
				}
			}
		}
		ImGui::EndChild();

		ImGui::Spacing();

		int count = static_cast<int>(std::count_if(state.selected.begin(), state.selected.end(), [](uint8_t v) { return v != 0; }));
		{
			auto _ = Util::DisableGuard(count == 0 || modName.empty());
			auto exportLabel = std::vformat(T("feature.scene_manager.action.export_count", "Export ({0})"),
				std::make_format_args(count));
			if (ImGui::Button(exportLabel.c_str(), ImVec2(-FLT_MIN, 0))) {
				std::vector<size_t> toExport;
				for (size_t i = 0; i < state.userIndices.size(); ++i)
					if (state.selected[i])
						toExport.push_back(state.userIndices[i]);
				exportFn(modName, toExport);
				state.dialogOpen = false;
				ImGui::CloseCurrentPopup();
			}
		}
	}

	void DrawExportAllPopup(SceneType type, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state)
	{
		auto popupId = std::format("{}##scene", T("feature.scene_manager.export.title", "Export User Settings"));
		DrawExportPopupCore(popupId.c_str(), entries, state,
			[type](const std::string& modName, const std::vector<size_t>& indices) {
				SceneSettingsManager::GetSingleton()->ExportUserSettingsToOverwrites(type, indices, modName);
			});
	}

	void DrawWeatherExportAllPopup(RE::FormID weatherId, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state, bool showTod)
	{
		auto popupId = std::format("{}##wx{:08X}", T("feature.scene_manager.export.title", "Export User Settings"), weatherId);
		DrawExportPopupCore(popupId.c_str(), entries, state,
			[weatherId](const std::string& modName, const std::vector<size_t>& indices) {
				SceneSettingsManager::GetSingleton()->ExportWeatherUserSettingsToOverwrites(weatherId, indices, modName);
			}, showTod);
	}

	// =========================================================================
	// Consolidated Panel Implementations
	// =========================================================================

	// --- Interior Panel ---

	static AddSettingState s_interiorAddState;
	static PopupState s_interiorPopups;
	static TableFlyoutState s_interiorTableFlyout;
	static ExportAllPopupState s_interiorExportState;

	void DrawInteriorPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(SceneType::InteriorOnly);
		auto& theme = globals::menu->GetSettings().Theme;
		ConfigurePopups(s_interiorPopups,
			T("feature.scene_manager.confirm.delete_all_interior_overwrites",
				"Are you sure you want to delete all interior overwrite files?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_interior_user",
				"Are you sure you want to remove all user-added interior settings?"));

		ImGui::TextUnformatted(T("feature.scene_manager.interior.title", "Interior Settings"));
		ImGui::Separator();

		if (ImGui::SmallButton(T("feature.scene_manager.action.add_setting", "Add Setting")))
			OpenAddDialog(SceneType::InteriorOnly, s_interiorAddState);

		DrawPopups(SceneType::InteriorOnly, s_interiorPopups);
		DrawAddSettingDialog(SceneType::InteriorOnly, s_interiorAddState);

		if (entries.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.interior.empty", "No interior settings configured."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.empty_add_hint", "Use the Add Setting button above to add overrides."));
			ImGui::Spacing();
			ImGui::TextWrapped("%s", T("feature.scene_manager.interior.description",
				"Settings added here override feature values in interior cells and revert automatically outside."));
			return;
		}

		ImGui::Spacing();

		TableCallbacks cb{
			[](size_t idx, float w, bool ro) { DrawValueEditor(SceneType::InteriorOnly, idx, w, ro); },
			[](const std::vector<size_t>& indices, float w, bool ro) {
				DrawValueEditor(SceneType::InteriorOnly, indices, w, ro);
			},
			[](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseEntry(SceneType::InteriorOnly, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RevertEntryToDefault(SceneType::InteriorOnly, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveSetting(SceneType::InteriorOnly, idx); }
		};

		auto overwriteGroup = BuildSourceGroup(entries, EntrySource::Overwrite);
		auto userGroup = BuildSourceGroup(entries, EntrySource::User);

		if (!overwriteGroup.order.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##iow", manager->AreAllOverwritesPaused(SceneType::InteriorOnly), [&] { manager->SetAllOverwritesPaused(SceneType::InteriorOnly, !manager->AreAllOverwritesPaused(SceneType::InteriorOnly)); }, [&] { s_interiorPopups.deleteAllOverwrites.Request(); }, 1))
				DrawSourceTable(overwriteGroup, entries, "##InteriorOW", EntrySource::Overwrite, 1, &s_interiorPopups, s_interiorTableFlyout, cb);
			EndSection();
		}

		if (!userGroup.order.empty()) {
			std::vector<size_t> owTmp, userIndices;
			SplitBySource(entries, owTmp, userIndices);
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##iusr", manager->AreAllUserPaused(SceneType::InteriorOnly),
					[&] { manager->SetAllUserPaused(SceneType::InteriorOnly, !manager->AreAllUserPaused(SceneType::InteriorOnly)); },
					[&] { s_interiorPopups.deleteAllUser.Request(); }, 1,
					[&] { s_interiorExportState.Open(userIndices); }, HasOverriddenUserEntries(entries)))
				DrawSourceTable(userGroup, entries, "##InteriorUsr", EntrySource::User, 1, &s_interiorPopups, s_interiorTableFlyout, cb);
			EndSection();
		}
		DrawExportAllPopup(SceneType::InteriorOnly, entries, s_interiorExportState);
	}

	// --- Time of Day Panel ---

	static AddSettingState s_todPeriodAddState[kPeriodCount];
	static AddSettingState s_todAllPeriodsAddState;
	static PopupState s_todPopups;
	static TableFlyoutState s_todTableFlyout;
	static ExportAllPopupState s_todExportState;

	void DrawTimeOfDayPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(SceneType::TimeOfDay);
		auto& theme = globals::menu->GetSettings().Theme;
		ConfigurePopups(s_todPopups,
			T("feature.scene_manager.confirm.delete_all_tod_overwrites",
				"Are you sure you want to delete all time-of-day overwrite files?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_tod_user",
				"Are you sure you want to remove all user-added time-of-day settings?"));

		ImGui::TextUnformatted(T("feature.scene_manager.time_of_day.title", "Time of Day Settings"));
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T("feature.scene_manager.time_of_day.exterior_only", "(Exterior Only)"));

		auto currentPeriod = SceneSettingsManager::GetCurrentPeriod();
		const auto* currentPeriodName = GetPeriodDisplayName(currentPeriod);
		const float currentGameHour = SceneSettingsManager::GetCurrentGameHour();
		ImGui::SameLine();
		auto currentTimeLabel = std::vformat(
			T("feature.scene_manager.time_of_day.current_time", "[{0} {1:.1f} h]"),
			std::make_format_args(currentPeriodName, currentGameHour));
		ImGui::TextColored(theme.StatusPalette.InfoColor, "%s", currentTimeLabel.c_str());

		ImGui::Separator();

		for (int i = 0; i < kPeriodCount; ++i) {
			auto label = GetAddPeriodLabel(static_cast<Period>(i));
			if (i > 0)
				ContinueButtonRowIfFits(label.c_str());
			ImGui::PushID(i);
			if (ImGui::SmallButton(label.c_str()))
				OpenAddDialog(SceneType::TimeOfDay, s_todPeriodAddState[i]);
			ImGui::PopID();
		}
		const auto* addAllLabel = T("feature.scene_manager.action.add_all", "Add All");
		ContinueButtonRowIfFits(addAllLabel);
		if (ImGui::SmallButton(addAllLabel))
			OpenAddDialog(SceneType::TimeOfDay, s_todAllPeriodsAddState);

		for (int i = 0; i < kPeriodCount; ++i)
			DrawAddSettingDialog(SceneType::TimeOfDay, s_todPeriodAddState[i], static_cast<Period>(i));
		DrawAddSettingDialog(SceneType::TimeOfDay, s_todAllPeriodsAddState, Period::Count, true);

		ImGui::Separator();

		DrawPopups(SceneType::TimeOfDay, s_todPopups);

		if (!HasTransitionEntries(entries)) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.time_of_day.empty", "No time-of-day settings configured."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.time_of_day.empty_hint", "Use the Add buttons above to add overrides for each period."));
			return;
		}

		ImGui::Spacing();

		TableCallbacks cb{
			[](size_t idx, float w, bool ro) { DrawValueEditor(SceneType::TimeOfDay, idx, w, ro); },
			[](const std::vector<size_t>& indices, float w, bool ro) {
				DrawValueEditor(SceneType::TimeOfDay, indices, w, ro);
			},
			[](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseEntry(SceneType::TimeOfDay, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RevertEntryToDefault(SceneType::TimeOfDay, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveSetting(SceneType::TimeOfDay, idx); },
			[](const std::string& feat, const std::vector<std::string>& path, const std::string& key, int p) {
				SceneSettingsManager::GetSingleton()->AddSetting(SceneType::TimeOfDay, feat, path, key,
					SceneSettingsManager::GetFeatureSettingValue(feat, path, key), static_cast<Period>(p));
			}
		};

		auto overwriteGroup = BuildSourceGroup(entries, EntrySource::Overwrite, true, true);
		auto userGroup = BuildSourceGroup(entries, EntrySource::User, true, true);

		if (!overwriteGroup.order.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##tow", manager->AreAllOverwritesPaused(SceneType::TimeOfDay), [&] { manager->SetAllOverwritesPaused(SceneType::TimeOfDay, !manager->AreAllOverwritesPaused(SceneType::TimeOfDay)); }, [&] { s_todPopups.deleteAllOverwrites.Request(); }, kPeriodCount))
				DrawSourceTable(overwriteGroup, entries, "##TODOverwrite", EntrySource::Overwrite, kPeriodCount, &s_todPopups, s_todTableFlyout, cb);
			EndSection();
		}

		if (!userGroup.order.empty()) {
			std::vector<size_t> owTmp, userIndices;
			SplitBySource(entries, owTmp, userIndices, true);
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##tusr", manager->AreAllUserPaused(SceneType::TimeOfDay),
					[&] { manager->SetAllUserPaused(SceneType::TimeOfDay, !manager->AreAllUserPaused(SceneType::TimeOfDay)); },
					[&] { s_todPopups.deleteAllUser.Request(); }, kPeriodCount,
					[&] { s_todExportState.Open(userIndices); }, HasOverriddenUserEntries(entries)))
				DrawSourceTable(userGroup, entries, "##TODUser", EntrySource::User, kPeriodCount, &s_todPopups, s_todTableFlyout, cb);
			EndSection();
		}
		DrawExportAllPopup(SceneType::TimeOfDay, entries, s_todExportState);
	}

	// --- Location Panel ---

	using LocationTarget = SceneSettingsManager::LocationTarget;
	using LocationTargetType = SceneSettingsManager::LocationTargetType;

	struct LocationPanelState
	{
		LocationTargetType selectedType = LocationTargetType::Location;
		std::string selectedFormKey;
		LocationTarget targetSnapshot;
		bool hasTargetSnapshot = false;
		AddSettingState addState;
		PopupState popups;
		TableFlyoutState tableFlyout;
		ExportAllPopupState exportState;
	};

	static LocationPanelState s_locationState;

	static const char* GetLocationTargetTypeName(LocationTargetType type)
	{
		return type == LocationTargetType::Cell ?
			T("feature.scene_manager.location.target_cell", "Cell") :
			T("feature.scene_manager.location.target_location", "Location");
	}

	static std::string GetLocationTargetLabel(const LocationTarget& target)
	{
		const auto* targetTypeName = GetLocationTargetTypeName(target.type);
		return std::vformat(T("feature.scene_manager.location.target_label", "{0} ({1})"),
			std::make_format_args(target.name.empty() ? target.formKey : target.name,
				targetTypeName));
	}

	static const LocationTarget* ResolveSelectedLocationTarget(
		const std::vector<LocationTarget>& targets, LocationPanelState& state)
	{
		auto selected = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
			return target.type == state.selectedType && target.formKey == state.selectedFormKey;
		});
		if (selected != targets.end())
			return &*selected;

		selected = std::find_if(targets.begin(), targets.end(), [](const auto& target) {
			return target.type == LocationTargetType::Cell;
		});
		if (selected == targets.end()) {
			for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
				if (it->type == LocationTargetType::Location) {
					selected = std::prev(it.base());
					break;
				}
			}
		}
		if (selected == targets.end())
			return nullptr;

		state.selectedType = selected->type;
		state.selectedFormKey = selected->formKey;
		return &*selected;
	}

	static bool IsLocationPopupOpen(const PopupState& popups)
	{
		return popups.deleteAllOverwrites.IsOpen() || popups.deleteSingleOverwrite.IsOpen() ||
		       popups.deleteRowOverwrite.IsOpen() || popups.deleteAllUser.IsOpen();
	}

	static void DrawLocationAddDialog(const LocationTarget& target, AddSettingState& state)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawAddDialogCore(state, Period::Count, false,
			false,
			[](const std::string& feature) { return SceneSettingsManager::GetFeatureSceneSettings(feature); },
			[=](const std::string& feature, const std::vector<std::string>& path, const std::string& key, Period) {
				return manager->HasLocationEntry(target.type, target.formKey, feature, path, key, EntrySource::User);
			},
			[=](const std::string& feature, const std::vector<std::string>& path, const std::string& key,
				const json&, Period) {
				return manager->AddLocationSetting(target.type, target.formKey, target.name, target.cocCode,
					feature, path, key, true);
			},
			[=] { manager->CommitSceneSettingChanges(); });
	}

	static void DrawLocationValueEditor(const LocationTarget& target, size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = manager->GetLocationConfig(target.type, target.formKey).entries[index];
		DrawValueEditorCore(entry, inputWidth, readOnly,
			[=](const json& value) { manager->UpdateLocationEntryValue(target.type, target.formKey, index, value, true); },
			[=] { manager->CommitSceneSettingChanges(); });
	}

	static void DrawLocationValueEditor(const LocationTarget& target,
		const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, readOnly,
				[=](const auto& updates) {
					manager->UpdateLocationEntryValues(target.type, target.formKey, updates, true);
				},
				[=] { manager->CommitSceneSettingChanges(); }))
			return;
		DrawLocationValueEditor(target, indices.back(), inputWidth, readOnly);
	}

	static void DrawLocationPopups(const LocationTarget& target, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto removeIndices = [&](const std::vector<size_t>& indices) {
			RemoveIndicesReversed(indices, [&](size_t index) {
				if (index < manager->GetLocationConfig(target.type, target.formKey).entries.size())
					manager->RemoveLocationSetting(target.type, target.formKey, index);
			});
		};
		auto getSourceIndices = [&](EntrySource source) {
			std::vector<size_t> indices;
			const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
			for (size_t i = 0; i < entries.size(); ++i)
				if (entries[i].source == source)
					indices.push_back(i);
			return indices;
		};

		if (popups.deleteAllOverwrites.Draw())
			removeIndices(getSourceIndices(EntrySource::Overwrite));

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetLocationConfig(target.type, target.formKey).entries.size())
				manager->RemoveLocationSetting(target.type, target.formKey, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			removeIndices(popups.pendingDeleteRow);
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			manager->DeleteAllLocationUserSettings(target.type, target.formKey);
	}

	static void DrawLocationExportPopup(const LocationTarget& target,
		const std::vector<SettingEntry>& entries, ExportAllPopupState& state)
	{
		auto popupId = std::format("{}##location{}:{}",
			T("feature.scene_manager.export.title", "Export User Settings"),
			static_cast<int>(target.type), target.formKey);
		DrawExportPopupCore(popupId.c_str(), entries, state,
			[=](const std::string& modName, const std::vector<size_t>& indices) {
				SceneSettingsManager::GetSingleton()->ExportLocationUserSettingsToOverwrites(
					target.type, target.formKey, indices, modName);
			}, false);
	}

	void DrawLocationPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto targets = manager->GetCurrentLocationTargets();
		auto& theme = globals::menu->GetSettings().Theme;

		ImGui::TextUnformatted(T("feature.scene_manager.location.title", "Location Settings"));
		ImGui::Separator();
		ConfigurePopups(s_locationState.popups,
			T("feature.scene_manager.confirm.delete_all_location_overwrites",
				"Are you sure you want to delete all overwrite files for this target?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_location_user",
				"Are you sure you want to remove all user-added settings for this target?"));

		bool lockTargetSelection = s_locationState.addState.dialogOpen || s_locationState.exportState.dialogOpen ||
		                           IsLocationPopupOpen(s_locationState.popups);
		LocationTarget target;
		if (lockTargetSelection && s_locationState.hasTargetSnapshot) {
			target = s_locationState.targetSnapshot;
		} else if (targets.empty()) {
			s_locationState.addState.Reset();
			s_locationState.exportState.dialogOpen = false;
			s_locationState.hasTargetSnapshot = false;
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.location.no_target", "No current location or cell is available."));
			ImGui::TextWrapped("%s", T("feature.scene_manager.location.no_target_hint",
				"Load into the game world to configure settings for the current location hierarchy or cell."));
			return;
		} else {
			const auto* selectedTarget = ResolveSelectedLocationTarget(targets, s_locationState);
			if (!selectedTarget)
				return;
			target = *selectedTarget;
			s_locationState.targetSnapshot = target;
			s_locationState.hasTargetSnapshot = true;
		}

		ImGui::BeginDisabled(lockTargetSelection);
		ImGui::SetNextItemWidth(-FLT_MIN);
		auto preview = GetLocationTargetLabel(target);
		if (ImGui::BeginCombo(T("feature.scene_manager.location.target", "Target"), preview.c_str())) {
			for (const auto& candidate : targets) {
				bool selected = candidate.type == target.type && candidate.formKey == target.formKey;
				auto label = std::format("{}##{}:{}", GetLocationTargetLabel(candidate),
					static_cast<int>(candidate.type), candidate.formKey);
				if (ImGui::Selectable(label.c_str(), selected)) {
					s_locationState.selectedType = candidate.type;
					s_locationState.selectedFormKey = candidate.formKey;
					s_locationState.addState.Reset();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();

		if (!lockTargetSelection) {
			const auto* selectedTarget = ResolveSelectedLocationTarget(targets, s_locationState);
			if (!selectedTarget)
				return;
			target = *selectedTarget;
			s_locationState.targetSnapshot = target;
		}

		ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.spid_key", "SPID key"), target.formKey.c_str());
		if (!target.cocCode.empty())
			ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.coc_code", "COC code"), target.cocCode.c_str());

		if (ImGui::SmallButton(T("feature.scene_manager.action.add_setting", "Add Setting")))
			OpenAddDialog(SceneType::Location, s_locationState.addState);
		DrawLocationAddDialog(target, s_locationState.addState);
		DrawLocationPopups(target, s_locationState.popups);

		const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
		if (entries.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.location.empty", "No settings are configured for this target."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.empty_add_hint", "Use the Add Setting button above to add overrides."));
			return;
		}

		TableCallbacks callbacks{
			[target](size_t index, float width, bool readOnly) { DrawLocationValueEditor(target, index, width, readOnly); },
			[target](const std::vector<size_t>& indices, float width, bool readOnly) {
				DrawLocationValueEditor(target, indices, width, readOnly);
			},
			[target](size_t index) { SceneSettingsManager::GetSingleton()->TogglePauseLocationEntry(target.type, target.formKey, index); },
			[target](size_t index) { SceneSettingsManager::GetSingleton()->RevertLocationEntryToDefault(target.type, target.formKey, index); },
			[target](size_t index) { SceneSettingsManager::GetSingleton()->RemoveLocationSetting(target.type, target.formKey, index); }
		};

		auto overwriteGroup = BuildSourceGroup(entries, EntrySource::Overwrite);
		auto userGroup = BuildSourceGroup(entries, EntrySource::User);
		std::vector<size_t> overwriteIndices;
		std::vector<size_t> userIndices;
		SplitBySource(entries, overwriteIndices, userIndices);

		if (!overwriteIndices.empty()) {
			bool allPaused = AreAllPaused(overwriteIndices, entries);
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##low",
					allPaused,
					[&] { for (auto index : overwriteIndices) if (entries[index].paused == allPaused) manager->TogglePauseLocationEntry(target.type, target.formKey, index); },
					[&] { s_locationState.popups.deleteAllOverwrites.Request(); }, 1))
				DrawSourceTable(overwriteGroup, entries, "##LocationOverwrite", EntrySource::Overwrite, 1,
					&s_locationState.popups, s_locationState.tableFlyout, callbacks);
			EndSection();
		}

		if (!userIndices.empty()) {
			bool allPaused = AreAllPaused(userIndices, entries);
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##lusr",
					allPaused,
					[&] { for (auto index : userIndices) if (entries[index].paused == allPaused) manager->TogglePauseLocationEntry(target.type, target.formKey, index); },
					[&] { s_locationState.popups.deleteAllUser.Request(); }, 1,
					[&] { s_locationState.exportState.Open(userIndices); }, HasOverriddenUserEntries(entries)))
				DrawSourceTable(userGroup, entries, "##LocationUser", EntrySource::User, 1,
					&s_locationState.popups, s_locationState.tableFlyout, callbacks);
			EndSection();
		}

		DrawLocationExportPopup(target, entries, s_locationState.exportState);
	}

	// --- Weather Scene Panel ---

	struct WeatherPanelState
	{
		AddSettingState periodAddStates[kPeriodCount];
		AddSettingState allPeriodsAddState;
		PopupState popups;
		TableFlyoutState tableFlyout;
		ExportAllPopupState exportState;
	};
	static std::map<RE::FormID, WeatherPanelState> s_weatherPanelStates;

	static WeatherPanelState& GetWeatherState(RE::FormID id) { return s_weatherPanelStates[id]; }

	static void DrawWeatherPopups(RE::FormID weatherId, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto removeIndices = [&](const std::vector<size_t>& indices) {
			std::vector<SettingEntry> pendingEntries;
			const auto& entries = manager->GetWeatherConfig(weatherId).entries;
			pendingEntries.reserve(indices.size());
			for (auto index : indices)
				if (index < entries.size())
					pendingEntries.push_back(entries[index]);

			for (const auto& pending : pendingEntries) {
				const auto& currentEntries = manager->GetWeatherConfig(weatherId).entries;
				auto current = std::ranges::find_if(currentEntries, [&](const auto& candidate) {
					return candidate.featureShortName == pending.featureShortName &&
					       candidate.settingPath == pending.settingPath &&
					       candidate.settingKey == pending.settingKey &&
					       candidate.source == pending.source && candidate.sourcePath == pending.sourcePath &&
					       candidate.sourceFilename == pending.sourceFilename && candidate.period == pending.period;
				});
				if (current != currentEntries.end())
					manager->RemoveWeatherSetting(weatherId,
						static_cast<size_t>(std::distance(currentEntries.begin(), current)));
			}
		};
		auto getSourceIndices = [&](EntrySource source) {
			std::vector<size_t> indices;
			const auto& entries = manager->GetWeatherConfig(weatherId).entries;
			for (size_t i = 0; i < entries.size(); ++i)
				if (entries[i].source == source)
					indices.push_back(i);
			return indices;
		};

		if (popups.deleteAllOverwrites.Draw())
			removeIndices(getSourceIndices(EntrySource::Overwrite));

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetWeatherConfig(weatherId).entries.size())
				manager->RemoveWeatherSetting(weatherId, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			removeIndices(popups.pendingDeleteRow);
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			manager->DeleteAllWeatherUserSettings(weatherId);
	}

	static TableCallbacks MakeWeatherCallbacks(RE::FormID weatherId)
	{
		return {
			[weatherId](size_t idx, float w, bool ro) { DrawWeatherValueEditor(weatherId, idx, w, ro); },
			[weatherId](const std::vector<size_t>& indices, float w, bool ro) {
				DrawWeatherValueEditor(weatherId, indices, w, ro);
			},
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseWeatherEntry(weatherId, idx); },
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->RevertWeatherEntryToDefault(weatherId, idx); },
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveWeatherSetting(weatherId, idx); },
			[weatherId](const std::string& feat, const std::vector<std::string>& path, const std::string& key, int p) {
				SceneSettingsManager::GetSingleton()->AddWeatherSetting(
					weatherId, feat, path, key, static_cast<Period>(p));
			}
		};
	}

	static void DrawWeatherSections(RE::FormID weatherId, WeatherPanelState& state, int numValueColumns)
	{
		bool showTod = numValueColumns > 1;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetWeatherConfig(weatherId).entries;
		auto cb = MakeWeatherCallbacks(weatherId);

		std::vector<size_t> overwriteIndices, userIndices;
		SplitBySource(entries, overwriteIndices, userIndices, true);

		if (!overwriteIndices.empty()) {
			auto group = BuildSourceGroup(entries, EntrySource::Overwrite, true, true);
			bool allPaused = std::all_of(overwriteIndices.begin(), overwriteIndices.end(),
				[&](size_t i) { return entries[i].paused; });
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##wow",
					allPaused,
					[&] { for (auto idx : overwriteIndices) if (entries[idx].paused == allPaused) manager->TogglePauseWeatherEntry(weatherId, idx); },
					[&] { state.popups.deleteAllOverwrites.Request(); },
					numValueColumns))
				DrawSourceTable(group, entries, "##WxOverwrite", EntrySource::Overwrite, numValueColumns, &state.popups, state.tableFlyout, cb);
			EndSection();
		}

		if (!userIndices.empty()) {
			auto group = BuildSourceGroup(entries, EntrySource::User, true, true);
			bool allPaused = std::all_of(userIndices.begin(), userIndices.end(),
				[&](size_t i) { return entries[i].paused; });
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##wusr",
					allPaused,
					[&] { for (auto idx : userIndices) if (entries[idx].paused == allPaused) manager->TogglePauseWeatherEntry(weatherId, idx); },
					[&] { state.popups.deleteAllUser.Request(); },
					numValueColumns,
					[&] { state.exportState.Open(userIndices); }, HasOverriddenUserEntries(entries)))
				DrawSourceTable(group, entries, "##WxUser", EntrySource::User, numValueColumns, nullptr, state.tableFlyout, cb);
			EndSection();
		}
		DrawWeatherExportAllPopup(weatherId, entries, state.exportState, showTod);
	}

	void DrawWeatherScenePanel(RE::FormID weatherId)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto& state = GetWeatherState(weatherId);
		auto& theme = globals::menu->GetSettings().Theme;
		bool showTod = manager->IsWeatherShowTimeOfDay(weatherId);
		ConfigurePopups(state.popups,
			T("feature.scene_manager.confirm.delete_all_weather_overwrites",
				"Are you sure you want to delete all overwrite files for this weather?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_weather_user",
				"Are you sure you want to remove all user-added settings for this weather?"));
		DrawWeatherPopups(weatherId, state.popups);
		const auto& config = manager->GetWeatherConfig(weatherId);

		ImGui::TextUnformatted(T("feature.scene_manager.weather.title", "Scene Settings"));
		ImGui::Separator();

		{
			bool toggled = showTod;
			if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &toggled))
				manager->SetWeatherShowTimeOfDay(weatherId, toggled);
			showTod = toggled;
		}

		if (showTod) {
			for (int i = 0; i < kPeriodCount; ++i) {
				auto label = GetAddPeriodLabel(static_cast<Period>(i));
				if (i > 0)
					ContinueButtonRowIfFits(label.c_str());
				ImGui::PushID(i);
				if (ImGui::SmallButton(label.c_str()))
					OpenWeatherAddDialog(weatherId, state.periodAddStates[i]);
				ImGui::PopID();
			}
		}

		const auto* addLabel = showTod ?
		                         T("feature.scene_manager.action.add_all", "Add All") :
		                         T("feature.scene_manager.action.add_setting", "Add Setting");
		if (showTod)
			ContinueButtonRowIfFits(addLabel);
		if (ImGui::SmallButton(addLabel))
			OpenWeatherAddDialog(weatherId, state.allPeriodsAddState);

		DrawWeatherAddDialog(weatherId, state.allPeriodsAddState, Period::Count, true);
		if (showTod)
			for (int p = 0; p < kPeriodCount; ++p)
				DrawWeatherAddDialog(weatherId, state.periodAddStates[p], static_cast<Period>(p));

		if (!HasTransitionEntries(config.entries)) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.weather.empty", "No scene settings for this weather."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.weather.empty_hint", "Use the Add buttons above to add overrides."));
			return;
		}

		DrawWeatherSections(weatherId, state, showTod ? kPeriodCount : 1);
	}
}
