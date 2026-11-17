#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <cmath>
#include <vector>

#define SETTING_DIRECTION   "direction"
#define SETTING_LOGO_PATH   "logo_path"
#define SETTING_PULLBACK    "pullback"

enum fly_direction {
    DIR_LEFT_TO_RIGHT = 0,
    DIR_RIGHT_TO_LEFT = 1,
    DIR_ALTERNATE     = 2,
};

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct ape_transition {
    obs_source_t *source;
    gs_texrender_t *rt;
    uint32_t rt_cx, rt_cy;

    gs_effect_t *effect;
    gs_eparam_t *param_image;
    gs_eparam_t *param_time;
    gs_eparam_t *param_sun_dir;
    gs_eparam_t *param_base_color;

    gs_vertbuffer_t *quad_vb;
    gs_vertbuffer_t *box_vb;
    uint32_t box_vert_count;

    gs_image_file_t logo_img;
    bool has_logo;
    char *logo_path;

    int direction;
    bool flip_next;
    bool was_active;
    float pullback;
    uint64_t start_ns;
};

static gs_vertbuffer_t *create_vb_from_verts(const std::vector<Vertex> &verts)
{
    struct gs_vb_data *data = gs_vbdata_create();
    data->num = verts.size();
    data->points  = (struct vec3 *)bmalloc(sizeof(struct vec3) * verts.size());
    data->normals = (struct vec3 *)bmalloc(sizeof(struct vec3) * verts.size());
    data->num_tex = 1;
    data->tvarray = (struct gs_tvertarray *)bzalloc(sizeof(struct gs_tvertarray));
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bmalloc(sizeof(struct vec2) * verts.size());

    struct vec2 *uvs = (struct vec2 *)data->tvarray[0].array;
    for (size_t i = 0; i < verts.size(); i++) {
        vec3_set(&data->points[i], verts[i].x, verts[i].y, verts[i].z);
        vec3_set(&data->normals[i], verts[i].nx, verts[i].ny, verts[i].nz);
        uvs[i].x = verts[i].u;
        uvs[i].y = verts[i].v;
    }
    return gs_vertexbuffer_create(data, 0);
}

/* Unit quad from (-0.5, -0.5) to (+0.5, +0.5) facing +Z */
static gs_vertbuffer_t *build_unit_quad()
{
    std::vector<Vertex> v = {
        {-0.5f,  0.5f, 0.0f,  0,0,1,  0.0f, 0.0f},
        { 0.5f,  0.5f, 0.0f,  0,0,1,  1.0f, 0.0f},
        {-0.5f, -0.5f, 0.0f,  0,0,1,  0.0f, 1.0f},

        { 0.5f,  0.5f, 0.0f,  0,0,1,  1.0f, 0.0f},
        { 0.5f, -0.5f, 0.0f,  0,0,1,  1.0f, 1.0f},
        {-0.5f, -0.5f, 0.0f,  0,0,1,  0.0f, 1.0f},
    };
    return create_vb_from_verts(v);
}

/* Unit cube (-0.5..+0.5) with face normals for white architectural blocks */
static gs_vertbuffer_t *build_unit_box(uint32_t *out_count)
{
    auto add_face = [](std::vector<Vertex> &out,
                       vec3 p0, vec3 p1, vec3 p2, vec3 p3, vec3 n) {
        out.push_back({p0.x,p0.y,p0.z, n.x,n.y,n.z, 0,0});
        out.push_back({p1.x,p1.y,p1.z, n.x,n.y,n.z, 1,0});
        out.push_back({p2.x,p2.y,p2.z, n.x,n.y,n.z, 0,1});
        out.push_back({p1.x,p1.y,p1.z, n.x,n.y,n.z, 1,0});
        out.push_back({p3.x,p3.y,p3.z, n.x,n.y,n.z, 1,1});
        out.push_back({p2.x,p2.y,p2.z, n.x,n.y,n.z, 0,1});
    };
    std::vector<Vertex> v;
    // Front (+Z), Back (-Z), Left (-X), Right (+X), Top (+Y), Bottom (-Y)
    add_face(v, {-0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, {0,0,1});
    add_face(v, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f,-0.5f}, {0,0,-1});
    add_face(v, {-0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f, 0.5f}, {-1,0,0});
    add_face(v, { 0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f,-0.5f}, { 0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f,-0.5f}, {1,0,0});
    add_face(v, {-0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {0,1,0});
    add_face(v, {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, {0,-1,0});

    *out_count = (uint32_t)v.size();
    return create_vb_from_verts(v);
}

static void draw_box(struct ape_transition *s, float x, float y, float z,
                     float sx, float sy, float sz,
                     float r, float g, float b)
{
    struct vec4 col = {r, g, b, 1.0f};
    gs_effect_set_vec4(s->param_base_color, &col);

    gs_matrix_push();
    gs_matrix_translate3f(x, y, z);
    gs_matrix_scale3f(sx, sy, sz);

    while (gs_effect_loop(s->effect, "WhiteSet")) {
        gs_load_vertexbuffer(s->box_vb);
        gs_draw(GS_TRIS, 0, s->box_vert_count);
    }
    gs_matrix_pop();
}

static void draw_textured_quad(struct ape_transition *s, gs_texture_t *tex,
                               float x, float y, float z,
                               float w, float h, float yaw_deg,
                               const char *tech)
{
    if (tex && s->param_image)
        gs_effect_set_texture(s->param_image, tex);

    gs_matrix_push();
    gs_matrix_translate3f(x, y, z);
    if (yaw_deg != 0.0f)
        gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, RAD(yaw_deg));
    gs_matrix_scale3f(w, h, 1.0f);

    while (gs_effect_loop(s->effect, tech)) {
        gs_load_vertexbuffer(s->quad_vb);
        gs_draw(GS_TRIS, 0, 6);
    }
    gs_matrix_pop();
}

static float smoothstep(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

static void ape_transition_callback(void *data, gs_texture_t *a, gs_texture_t *b,
                                    float t, uint32_t cx, uint32_t cy)
{
    struct ape_transition *s = (struct ape_transition *)data;
    if (!s->effect || !cx || !cy) return;

    // Track Auto-Alternate direction on each new transition
    if (t < 0.01f && !s->was_active) {
        s->was_active = true;
    } else if (t > 0.99f && s->was_active) {
        s->was_active = false;
        if (s->direction == DIR_ALTERNATE)
            s->flip_next = !s->flip_next;
    }

    bool right_to_left = (s->direction == DIR_RIGHT_TO_LEFT) ||
                         (s->direction == DIR_ALTERNATE && s->flip_next);

    if (!s->rt || s->rt_cx != cx || s->rt_cy != cy) {
        gs_texrender_destroy(s->rt);
        s->rt = gs_texrender_create(GS_RGBA, GS_Z24_S8);
        s->rt_cx = cx;
        s->rt_cy = cy;
    }

    float aspect = (float)cx / (float)cy;
    float fovy   = 50.0f;
    float screen_h = 2.0f;
    float screen_w = screen_h * aspect;

    // Exact camera distance so screen quad fills 100% of canvas at t=0 and t=1
    float lock_dist = (screen_h * 0.5f) / tanf(RAD(fovy * 0.5f));

    // Positions of Left & Right white studio stages
    float left_x  = -3.2f;
    float right_x =  3.2f;
    float stage_y =  1.2f;

    float start_x = right_to_left ? right_x : left_x;
    float end_x   = right_to_left ? left_x  : right_x;

    // Smooth camera flight: pull back in the middle (sin arc), fly across
    float ease = smoothstep(t);
    float arc  = sinf(ease * (float)M_PI);

    float cam_x = start_x + (end_x - start_x) * ease;
    float cam_y = stage_y + 0.35f * arc;
    float cam_z = lock_dist + s->pullback * arc;

    float time_sec = (float)((os_gettime_ns() - s->start_ns) / 1000000000.0);
    struct vec4 sun_dir = {0.25f, 0.35f, -0.90f, 0.0f};

    gs_texrender_reset(s->rt);
    if (gs_texrender_begin(s->rt, cx, cy)) {
        struct vec4 clear_col = {0.0f, 0.0f, 0.0f, 1.0f};
        gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_col, 1.0f, 0);

        gs_enable_depth_test(true);
        gs_set_cull_mode(GS_BACK);

        gs_effect_set_float(s->param_time, time_sec);
        gs_effect_set_vec4(s->param_sun_dir, &sun_dir);

        gs_projection_push();
        gs_perspective(fovy, aspect, 0.1f, 200.0f);

        gs_matrix_push();
        gs_matrix_identity();
        gs_matrix_translate3f(-cam_x, -cam_y, -cam_z);

        // 1. Sunset Sky & Shimmering Ocean Backdrop
        draw_textured_quad(s, nullptr, 0.0f, 4.5f, -24.0f, 70.0f, 36.0f, 0.0f, "SunsetOcean");

        // 2. White Studio Beach Deck & Stepped Podiums
        draw_box(s, 0.0f, -0.35f, -2.5f, 14.0f, 0.7f, 7.0f,  0.97f, 0.96f, 0.95f); // Main white deck
        draw_box(s, 0.0f, -0.75f, -3.0f, 26.0f, 0.2f, 12.0f, 0.92f, 0.85f, 0.76f); // Warm sand apron

        // Left & Right White Stage Bezels + Backing Walls
        draw_box(s, left_x,  stage_y, -0.15f, screen_w + 0.28f, screen_h + 0.28f, 0.24f, 0.99f, 0.99f, 0.99f);
        draw_box(s, right_x, stage_y, -0.15f, screen_w + 0.28f, screen_h + 0.28f, 0.24f, 0.99f, 0.99f, 0.99f);

        // Sleek White Overhead Pergola / Architectural Arch Pillars
        draw_box(s, -6.2f, 2.2f, -0.8f, 0.35f, 4.4f, 0.5f, 0.98f, 0.97f, 0.96f);
        draw_box(s,  6.2f, 2.2f, -0.8f, 0.35f, 4.4f, 0.5f, 0.98f, 0.97f, 0.96f);
        draw_box(s,  0.0f, 4.3f, -0.8f, 12.8f, 0.32f, 0.6f, 0.99f, 0.98f, 0.97f);

        // Center White Plinth for Ape Squad Logo
        draw_box(s, 0.0f, 1.15f, -0.4f, 1.9f, 2.2f, 0.35f, 0.98f, 0.98f, 0.98f);

        // 3. Live OBS Scene Screens (Screen A & Screen B)
        gs_texture_t *left_tex  = right_to_left ? b : a;
        gs_texture_t *right_tex = right_to_left ? a : b;
        draw_textured_quad(s, left_tex,  left_x,  stage_y, 0.0f, screen_w, screen_h, 0.0f, "Textured");
        draw_textured_quad(s, right_tex, right_x, stage_y, 0.0f, screen_w, screen_h, 0.0f, "Textured");

        // 4. Ape Squad Logo on Center Plinth
        if (s->has_logo && s->logo_img.texture) {
            gs_enable_blending(true);
            gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
            draw_textured_quad(s, s->logo_img.texture, 0.0f, 1.35f, -0.20f, 1.4f, 1.4f, 0.0f, "Textured");
        }

        gs_matrix_pop();
        gs_projection_pop();
        gs_enable_depth_test(false);
        gs_texrender_end(s->rt);
    }

    // Draw final rendered 3D frame onto OBS transition output
    gs_texture_t *out_tex = gs_texrender_get_texture(s->rt);
    if (out_tex) {
        gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
        gs_effect_set_texture(img, out_tex);
        while (gs_effect_loop(def, "Draw"))
            gs_draw_sprite(out_tex, 0, cx, cy);
    }
}

static void ape_video_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    struct ape_transition *s = (struct ape_transition *)data;
    obs_transition_video_render(s->source, ape_transition_callback);
}

static float mix_a(void *data, float t) { UNUSED_PARAMETER(data); return 1.0f - t; }
static float mix_b(void *data, float t) { UNUSED_PARAMETER(data); return t; }

static bool ape_audio_render(void *data, uint64_t *ts_out,
                             struct obs_source_audio_mix *audio, uint32_t mixers,
                             size_t channels, size_t sample_rate)
{
    struct ape_transition *s = (struct ape_transition *)data;
    return obs_transition_audio_render(s->source, ts_out, audio, mixers,
                                       channels, sample_rate, mix_a, mix_b);
}

static enum gs_color_space ape_get_color_space(void *data, size_t count,
                                               const enum gs_color_space *preferred_spaces)
{
    UNUSED_PARAMETER(count);
    UNUSED_PARAMETER(preferred_spaces);
    struct ape_transition *s = (struct ape_transition *)data;
    return obs_transition_video_get_color_space(s->source);
}

static void load_logo(struct ape_transition *s, const char *path)
{
    obs_enter_graphics();
    if (s->has_logo) {
        gs_image_file_free(&s->logo_img);
        s->has_logo = false;
    }
    if (path && *path) {
        gs_image_file_init(&s->logo_img, path);
        gs_image_file_init_texture(&s->logo_img);
        s->has_logo = s->logo_img.loaded;
    }
    obs_leave_graphics();
}

static void ape_update(void *data, obs_data_t *settings)
{
    struct ape_transition *s = (struct ape_transition *)data;
    s->direction = (int)obs_data_get_int(settings, SETTING_DIRECTION);
    s->pullback  = (float)obs_data_get_double(settings, SETTING_PULLBACK);

    const char *path = obs_data_get_string(settings, SETTING_LOGO_PATH);
    if (!s->logo_path || strcmp(s->logo_path, path) != 0) {
        bfree(s->logo_path);
        s->logo_path = bstrdup(path);
        load_logo(s, s->logo_path);
    }
}

static void *ape_create(obs_data_t *settings, obs_source_t *source)
{
    struct ape_transition *s = (struct ape_transition *)bzalloc(sizeof(*s));
    s->source   = source;
    s->start_ns = os_gettime_ns();

    char *effect_path = obs_module_file("sunset_stage.effect");
    obs_enter_graphics();
    s->effect = gs_effect_create_from_file(effect_path, nullptr);
    if (s->effect) {
        s->param_image      = gs_effect_get_param_by_name(s->effect, "image");
        s->param_time       = gs_effect_get_param_by_name(s->effect, "time_sec");
        s->param_sun_dir    = gs_effect_get_param_by_name(s->effect, "sun_dir");
        s->param_base_color = gs_effect_get_param_by_name(s->effect, "base_color");
    }
    s->quad_vb = build_unit_quad();
    s->box_vb  = build_unit_box(&s->box_vert_count);
    obs_leave_graphics();
    bfree(effect_path);

    ape_update(s, settings);
    return s;
}

static void ape_destroy(void *data)
{
    struct ape_transition *s = (struct ape_transition *)data;
    obs_enter_graphics();
    if (s->has_logo) gs_image_file_free(&s->logo_img);
    gs_vertexbuffer_destroy(s->quad_vb);
    gs_vertexbuffer_destroy(s->box_vb);
    gs_texrender_destroy(s->rt);
    gs_effect_destroy(s->effect);
    obs_leave_graphics();
    bfree(s->logo_path);
    bfree(s);
}

static obs_properties_t *ape_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_property_t *dir = obs_properties_add_list(
        props, SETTING_DIRECTION, "Camera Direction",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(dir, "Left Stage -> Right Stage", DIR_LEFT_TO_RIGHT);
    obs_property_list_add_int(dir, "Right Stage -> Left Stage", DIR_RIGHT_TO_LEFT);
    obs_property_list_add_int(dir, "Auto-Alternate Each Switch", DIR_ALTERNATE);

    obs_properties_add_float_slider(props, SETTING_PULLBACK, "Stage Pullback Distance", 1.5, 10.0, 0.25);
    obs_properties_add_path(props, SETTING_LOGO_PATH, "Ape Squad Logo (PNG)",
                            OBS_PATH_FILE, "Images (*.png *.jpg *.webp)", nullptr);
    return props;
}

static void ape_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, SETTING_DIRECTION, DIR_ALTERNATE);
    obs_data_set_default_double(settings, SETTING_PULLBACK, 5.5);
}

static const char *ape_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Ape Squad 3D Sunset Stage";
}

struct obs_source_info ape_squad_transition_info = {
    .id                   = "ape_squad_sunset_transition",
    .type                 = OBS_SOURCE_TYPE_TRANSITION,
    .get_name             = ape_get_name,
    .create               = ape_create,
    .destroy              = ape_destroy,
    .get_defaults         = ape_defaults,
    .get_properties       = ape_properties,
    .update               = ape_update,
    .video_render         = ape_video_render,
    .audio_render         = ape_audio_render,
    .video_get_color_space = ape_get_color_space,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("ape-squad-transition", "en-US")

bool obs_module_load(void)
{
    obs_register_source(&ape_squad_transition_info);
    return true;
}