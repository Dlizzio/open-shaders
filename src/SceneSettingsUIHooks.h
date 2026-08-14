#pragma once

struct Feature;

namespace SceneSettingsUIHooks
{
	class FeatureDrawGuard
	{
	public:
		FeatureDrawGuard(Feature* feature, bool sceneControlled);
		~FeatureDrawGuard();

		FeatureDrawGuard(const FeatureDrawGuard&) = delete;
		FeatureDrawGuard& operator=(const FeatureDrawGuard&) = delete;

	private:
		Feature* previousFeature = nullptr;
		bool previousSceneControlled = false;
	};

	void Install();
}
