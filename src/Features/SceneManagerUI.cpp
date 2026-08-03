#include "SceneManagerUI.h"

#include "CSEditor/SceneSettingsUI.h"
#include "I18n/I18n.h"

namespace SceneManagerUI
{
	void Draw()
	{
		if (!ImGui::BeginTabBar("##SceneManagerTabs"))
			return;

		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.interior", "Interior"))) {
			SceneSettingsUI::DrawInteriorPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.time_of_day", "Time of Day"))) {
			SceneSettingsUI::DrawTimeOfDayPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T("feature.scene_manager.tab.locations", "Locations"))) {
			SceneSettingsUI::DrawLocationPanel();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}
