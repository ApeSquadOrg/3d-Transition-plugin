#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info transition_3d_engine;

bool obs_module_load(void)
{
	obs_register_source(&transition_3d_engine);
	obs_log(LOG_INFO, "3D Broadcast Engine loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "3D Broadcast Engine unloaded");
}
