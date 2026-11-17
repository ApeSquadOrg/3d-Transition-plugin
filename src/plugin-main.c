extern struct obs_source_info transition_3d_engine;

bool obs_module_load(void)
{
    obs_register_source(&transition_3d_engine);
    return true;
}