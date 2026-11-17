#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/threading.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((float)((d) * (M_PI / 180.0)))
#define MAX_TECHNIQUES 32
#define MAX_TECH_LEN   64

enum transition_mode {
	MODE_CUBE_LEFT = 0,
	MODE_CUBE_RIGHT,
	MODE_CUBE_UP,
	MODE_CUBE_DOWN,
	MODE_ARENA_CRANE,
	MODE_BLAST_DOORS,
	MODE_STADIUM_LOUVERS,
	MODE_RIBBON_WAVE,
	MODE_TILES_FLIP,
	MODE_CARD_FLIP_Y,
	MODE_CARD_FLIP_X,
	MODE_CAROUSEL_SWING,
	MODE_PORTAL_ZOOM,
};

enum ease_mode {
	EASE_INOUT_CUBIC = 0,
	EASE_INOUT_QUINT,
	EASE_OUT_BACK,
	EASE_LINEAR,
};

struct t3d_data {
	obs_source_t *source;
	pthread_mutex_t lock;
	gs_effect_t *effect;

	gs_eparam_t *ep_image;
	gs_eparam_t *ep_canvas_size;
	gs_eparam_t *ep_rot_rad;
	gs_eparam_t *ep_pos_offset;
	gs_eparam_t *ep_pivot;
	gs_eparam_t *ep_fov_deg;
	gs_eparam_t *ep_shade;
	gs_eparam_t *ep_alpha;
	gs_eparam_t *ep_progress;
	gs_eparam_t *ep_time_sec;

	char techniques[MAX_TECHNIQUES][MAX_TECH_LEN];
	int num_techniques;
	char active_technique[MAX_TECH_LEN];

	int mode;
	int ease;
	float fov;
	float depth_pull;
	float shading;
	int tile_cols;
	int tile_rows;
	float current_raw_t;
};

static void parse_techniques(struct t3d_data *s, const char *path)
{
	s->num_techniques = 0;
	char *file_data = os_quick_read_utf8_file(path);
	if (!file_data) {
		blog(LOG_WARNING, "[3D Engine] Could not read %s", path);
		return;
	}

	const char *cur = file_data;
	while ((cur = strstr(cur, "technique")) != NULL &&
	       s->num_techniques < MAX_TECHNIQUES) {
		bool valid_start = (cur == file_data) ||
		                   isspace((unsigned char)cur[-1]);
		cur += 9;

		if (!valid_start ||
		    !(*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n'))
			continue;

		const char *line_start = cur - 9;
		while (line_start > file_data && line_start[-1] != '\n')
			line_start--;
		const char *ls = line_start;
		while (*ls == ' ' || *ls == '\t')
			ls++;
		if (ls[0] == '/' && ls[1] == '/')
			continue;

		while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n')
			cur++;

		int idx = 0;
		char name[MAX_TECH_LEN];
		while (*cur && (isalnum((unsigned char)*cur) || *cur == '_') &&
		       idx < MAX_TECH_LEN - 1) {
			name[idx++] = *cur++;
		}
		name[idx] = '\0';

		if (idx > 0 && gs_effect_get_technique(s->effect, name)) {
			snprintf(s->techniques[s->num_techniques],
			         MAX_TECH_LEN, "%s", name);
			s->num_techniques++;
		}
	}

	blog(LOG_INFO, "[3D Engine] Parsed %d techniques from %s",
	     s->num_techniques, path);
	bfree(file_data);
}

static void cache_params(struct t3d_data *s)
{
	s->ep_image       = gs_effect_get_param_by_name(s->effect, "image");
	s->ep_canvas_size = gs_effect_get_param_by_name(s->effect, "canvas_size");
	s->ep_rot_rad     = gs_effect_get_param_by_name(s->effect, "rot_rad");
	s->ep_pos_offset  = gs_effect_get_param_by_name(s->effect, "pos_offset");
	s->ep_pivot       = gs_effect_get_param_by_name(s->effect, "pivot");
	s->ep_fov_deg     = gs_effect_get_param_by_name(s->effect, "fov_deg");
	s->ep_shade       = gs_effect_get_param_by_name(s->effect, "shade");
	s->ep_alpha       = gs_effect_get_param_by_name(s->effect, "alpha");
	s->ep_progress    = gs_effect_get_param_by_name(s->effect, "progress");
	s->ep_time_sec    = gs_effect_get_param_by_name(s->effect, "time_sec");
}

static bool reload_effect(struct t3d_data *s)
{
	char *path = obs_module_file("effects/transition_3d.effect");
	if (!path)
		return false;

	obs_enter_graphics();
	char *err = NULL;
	gs_effect_t *new_eff = gs_effect_create_from_file(path, &err);
	if (!new_eff) {
		blog(LOG_ERROR, "[3D Engine] Shader compile error: %s",
		     err ? err : "unknown error");
		bfree(err);
		obs_leave_graphics();
		bfree(path);
		return false;
	}

	pthread_mutex_lock(&s->lock);
	if (s->effect)
		gs_effect_destroy(s->effect);
	s->effect = new_eff;
	cache_params(s);
	parse_techniques(s, path);
	pthread_mutex_unlock(&s->lock);

	obs_leave_graphics();
	bfree(path);
	return true;
}

static float apply_ease(float t, int ease)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;

	switch (ease) {
	case EASE_INOUT_CUBIC:
		return (t < 0.5f)
			? 4.0f * t * t * t
			: 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
	case EASE_INOUT_QUINT:
		return (t < 0.5f)
			? 16.0f * t * t * t * t * t
			: 1.0f - powf(-2.0f * t + 2.0f, 5.0f) * 0.5f;
	case EASE_OUT_BACK: {
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	case EASE_LINEAR:
	default:
		return t;
	}
}

static const char *t3d_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Transition3DEngine");
}

static void t3d_update(void *data, obs_data_t *settings)
{
	struct t3d_data *s = data;
	s->mode       = (int)obs_data_get_int(settings, "mode");
	s->ease       = (int)obs_data_get_int(settings, "ease");
	s->fov        = (float)obs_data_get_double(settings, "fov");
	s->depth_pull = (float)obs_data_get_double(settings, "depth_pull");
	s->shading    = (float)obs_data_get_double(settings, "shading");
	s->tile_cols  = (int)obs_data_get_int(settings, "tile_cols");
	s->tile_rows  = (int)obs_data_get_int(settings, "tile_rows");

	const char *tech = obs_data_get_string(settings, "technique");
	if (tech && *tech)
		snprintf(s->active_technique, MAX_TECH_LEN, "%s", tech);
}

static void *t3d_create(obs_data_t *settings, obs_source_t *source)
{
	struct t3d_data *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->lock, NULL);

	if (!reload_effect(s)) {
		pthread_mutex_destroy(&s->lock);
		bfree(s);
		return NULL;
	}

	if (s->num_techniques > 0)
		snprintf(s->active_technique, MAX_TECH_LEN, "%s", s->techniques[0]);
	else
		snprintf(s->active_technique, MAX_TECH_LEN, "Draw");

	t3d_update(s, settings);
	return s;
}

static void t3d_destroy(void *data)
{
	struct t3d_data *s = data;
	if (!s) return;

	if (s->effect) {
		obs_enter_graphics();
		gs_effect_destroy(s->effect);
		obs_leave_graphics();
	}
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

static void draw_face(struct t3d_data *s, gs_texture_t *tex,
                      uint32_t cx, uint32_t cy,
                      struct vec3 rot, struct vec3 pos, struct vec3 pivot,
                      float shade, float alpha,
                      float u0, float v0, float u1, float v1)
{
	if (!tex || !s->effect || alpha <= 0.001f)
		return;

	const char *tech = s->active_technique;
	if (!gs_effect_get_technique(s->effect, tech))
		tech = (s->num_techniques > 0) ? s->techniques[0] : "Draw";
	if (!gs_effect_get_technique(s->effect, tech))
		return;

	struct vec2 sz = { (float)cx, (float)cy };
	float t_sec = (float)fmod((double)os_gettime_ns() / 1000000000.0, 3600.0);

	if (s->ep_image)       gs_effect_set_texture_srgb(s->ep_image, tex);
	if (s->ep_canvas_size) gs_effect_set_vec2(s->ep_canvas_size, &sz);
	if (s->ep_rot_rad)     gs_effect_set_vec3(s->ep_rot_rad, &rot);
	if (s->ep_pos_offset)  gs_effect_set_vec3(s->ep_pos_offset, &pos);
	if (s->ep_pivot)       gs_effect_set_vec3(s->ep_pivot, &pivot);
	if (s->ep_fov_deg)     gs_effect_set_float(s->ep_fov_deg, s->fov);
	if (s->ep_shade)       gs_effect_set_float(s->ep_shade, shade);
	if (s->ep_alpha)       gs_effect_set_float(s->ep_alpha, alpha);
	if (s->ep_progress)    gs_effect_set_float(s->ep_progress, s->current_raw_t);
	if (s->ep_time_sec)    gs_effect_set_float(s->ep_time_sec, t_sec);

	while (gs_effect_loop(s->effect, tech)) {
		gs_render_start(true);
		gs_vertex3f(u0, v0, 0.0f); gs_texcoord(u0, v0, 0);
		gs_vertex3f(u1, v0, 0.0f); gs_texcoord(u1, v0, 0);
		gs_vertex3f(u0, v1, 0.0f); gs_texcoord(u0, v1, 0);

		gs_vertex3f(u1, v0, 0.0f); gs_texcoord(u1, v0, 0);
		gs_vertex3f(u1, v1, 0.0f); gs_texcoord(u1, v1, 0);
		gs_vertex3f(u0, v1, 0.0f); gs_texcoord(u0, v1, 0);
		gs_render_stop(GS_TRIS);
	}
}

static void t3d_callback(void *data, gs_texture_t *a, gs_texture_t *b,
                         float raw_t, uint32_t cx, uint32_t cy)
{
	if (!a && !b)
		return;

	struct t3d_data *s = data;
	s->current_raw_t = raw_t;

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	const enum gs_cull_mode prev_cull = gs_get_cull_mode();
	gs_enable_framebuffer_srgb(true);
	gs_set_cull_mode(GS_NEITHER);

	const float aspect = (float)cx / (float)(cy ? cy : 1);
	const float t      = apply_ease(raw_t, s->ease);
	const float pull   = -sinf((float)M_PI * raw_t) * s->depth_pull;
	struct vec3 zero   = { 0.0f, 0.0f, 0.0f };

	switch (s->mode) {
	case MODE_CUBE_LEFT:
	case MODE_CUBE_RIGHT: {
		float dir = (s->mode == MODE_CUBE_LEFT) ? -1.0f : 1.0f;
		struct vec3 pivot = { 0.0f, 0.0f, -aspect };
		struct vec3 pos   = { 0.0f, 0.0f, pull };
		struct vec3 rot_a = { 0.0f, dir * t * DEG2RAD(90.0f), 0.0f };
		struct vec3 rot_b = { 0.0f, dir * (t - 1.0f) * DEG2RAD(90.0f), 0.0f };
		float shade_a = 1.0f - s->shading * t;
		float shade_b = 1.0f - s->shading * (1.0f - t);

		if (t < 0.5f) {
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
		} else {
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
		}
		break;
	}

	case MODE_CUBE_UP:
	case MODE_CUBE_DOWN: {
		float dir = (s->mode == MODE_CUBE_UP) ? 1.0f : -1.0f;
		struct vec3 pivot = { 0.0f, 0.0f, -1.0f };
		struct vec3 pos   = { 0.0f, 0.0f, pull };
		struct vec3 rot_a = { dir * t * DEG2RAD(90.0f), 0.0f, 0.0f };
		struct vec3 rot_b = { dir * (t - 1.0f) * DEG2RAD(90.0f), 0.0f, 0.0f };
		float shade_a = 1.0f - s->shading * t;
		float shade_b = 1.0f - s->shading * (1.0f - t);

		if (t < 0.5f) {
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
		} else {
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
		}
		break;
	}

	case MODE_ARENA_CRANE: {
		float bank = sinf((float)M_PI * raw_t) * DEG2RAD(5.0f);
		struct vec3 pivot = { 0.0f, 0.0f, -aspect * 1.45f };
		struct vec3 pos   = { 0.0f, 0.0f, pull * 1.15f };
		struct vec3 rot_a = { -bank * 0.5f, -t * DEG2RAD(95.0f), bank };
		struct vec3 rot_b = { -bank * 0.5f, -(t - 1.0f) * DEG2RAD(95.0f), bank };
		float shade_a = 1.0f - s->shading * t;
		float shade_b = 1.0f - s->shading * (1.0f - t);

		if (t < 0.5f) {
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
		} else {
			draw_face(s, a, cx, cy, rot_a, pos, pivot, shade_a, 1.0f, 0, 0, 1, 1);
			draw_face(s, b, cx, cy, rot_b, pos, pivot, shade_b, 1.0f, 0, 0, 1, 1);
		}
		break;
	}

	case MODE_BLAST_DOORS: {
		struct vec3 pos_b = { 0.0f, 0.0f, -(1.0f - t) * (2.2f + s->depth_pull) };
		float alpha_b = (t * 1.35f > 1.0f) ? 1.0f : (t * 1.35f);
		draw_face(s, b, cx, cy, zero, pos_b, zero, 1.0f, alpha_b, 0, 0, 1, 1);

		struct vec3 piv_l = { -aspect, 0.0f, 0.0f };
		struct vec3 piv_r = {  aspect, 0.0f, 0.0f };
		struct vec3 rot_l = { 0.0f,  t * DEG2RAD(102.0f), 0.0f };
		struct vec3 rot_r = { 0.0f, -t * DEG2RAD(102.0f), 0.0f };
		float door_shade  = 1.0f - s->shading * t;

		if (t < 0.96f) {
			draw_face(s, a, cx, cy, rot_l, zero, piv_l, door_shade, 1.0f, 0.0f, 0.0f, 0.5f, 1.0f);
			draw_face(s, a, cx, cy, rot_r, zero, piv_r, door_shade, 1.0f, 0.5f, 0.0f, 1.0f, 1.0f);
		}
		break;
	}

	case MODE_STADIUM_LOUVERS: {
		int cols = s->tile_cols > 0 ? s->tile_cols : 12;
		float wave = 0.42f;
		for (int c = 0; c < cols; c++) {
			float u0 = (float)c / (float)cols;
			float u1 = (float)(c + 1) / (float)cols;
			float center_dist = fabsf((u0 + u1) * 0.5f - 0.5f) * 2.0f;
			float lt = apply_ease((raw_t - center_dist * wave) / (1.0f - wave), s->ease);

			struct vec3 piv = { (((u0 + u1) * 0.5f) * 2.0f - 1.0f) * aspect, 0.0f, 0.0f };
			struct vec3 pos = { 0.0f, 0.0f, -sinf((float)M_PI * lt) * s->depth_pull * 0.6f };
			if (lt < 0.5f) {
				struct vec3 rot = { 0.0f, lt * DEG2RAD(180.0f), 0.0f };
				draw_face(s, a, cx, cy, rot, pos, piv, 1.0f - s->shading * (lt * 2.0f), 1.0f, u0, 0.0f, u1, 1.0f);
			} else {
				struct vec3 rot = { 0.0f, (lt - 1.0f) * DEG2RAD(180.0f), 0.0f };
				draw_face(s, b, cx, cy, rot, pos, piv, 1.0f - s->shading * ((1.0f - lt) * 2.0f), 1.0f, u0, 0.0f, u1, 1.0f);
			}
		}
		break;
	}

	case MODE_RIBBON_WAVE: {
		int rows = s->tile_rows > 0 ? s->tile_rows : 7;
		float wave = 0.45f;
		for (int r = 0; r < rows; r++) {
			float v0 = (float)r / (float)rows;
			float v1 = (float)(r + 1) / (float)rows;
			float lt = apply_ease((raw_t - ((float)r / (float)rows) * wave) / (1.0f - wave), s->ease);

			struct vec3 piv = { 0.0f, 1.0f - ((v0 + v1) * 0.5f) * 2.0f, 0.0f };
			struct vec3 pos = { 0.0f, 0.0f, -sinf((float)M_PI * lt) * s->depth_pull * 0.6f };
			if (lt < 0.5f) {
				struct vec3 rot = { lt * DEG2RAD(180.0f), 0.0f, 0.0f };
				draw_face(s, a, cx, cy, rot, pos, piv, 1.0f - s->shading * (lt * 2.0f), 1.0f, 0.0f, v0, 1.0f, v1);
			} else {
				struct vec3 rot = { (lt - 1.0f) * DEG2RAD(180.0f), 0.0f, 0.0f };
				draw_face(s, b, cx, cy, rot, pos, piv, 1.0f - s->shading * ((1.0f - lt) * 2.0f), 1.0f, 0.0f, v0, 1.0f, v1);
			}
		}
		break;
	}

	case MODE_TILES_FLIP: {
		int cols = s->tile_cols > 0 ? s->tile_cols : 10;
		int rows = s->tile_rows > 0 ? s->tile_rows : 6;
		float wave_spread = 0.45f;

		for (int r = 0; r < rows; r++) {
			for (int c = 0; c < cols; c++) {
				float u0 = (float)c / (float)cols;
				float u1 = (float)(c + 1) / (float)cols;
				float v0 = (float)r / (float)rows;
				float v1 = (float)(r + 1) / (float)rows;

				float diag = ((float)c / (float)cols + (float)r / (float)rows) * 0.5f;
				float lt = apply_ease((raw_t - diag * wave_spread) / (1.0f - wave_spread), s->ease);

				struct vec3 tile_pivot = {
					(((u0 + u1) * 0.5f) * 2.0f - 1.0f) * aspect,
					1.0f - ((v0 + v1) * 0.5f) * 2.0f,
					0.0f
				};
				struct vec3 pos = { 0.0f, 0.0f, -sinf((float)M_PI * lt) * s->depth_pull * 0.5f };

				if (lt < 0.5f) {
					struct vec3 rot = { 0.0f, lt * DEG2RAD(180.0f), 0.0f };
					draw_face(s, a, cx, cy, rot, pos, tile_pivot,
					          1.0f - s->shading * (lt * 2.0f), 1.0f, u0, v0, u1, v1);
				} else {
					struct vec3 rot = { 0.0f, (lt - 1.0f) * DEG2RAD(180.0f), 0.0f };
					draw_face(s, b, cx, cy, rot, pos, tile_pivot,
					          1.0f - s->shading * ((1.0f - lt) * 2.0f), 1.0f, u0, v0, u1, v1);
				}
			}
		}
		break;
	}

	case MODE_CARD_FLIP_Y:
	case MODE_CARD_FLIP_X: {
		struct vec3 pos = { 0.0f, 0.0f, pull };
		if (t < 0.5f) {
			float angle = t * DEG2RAD(180.0f);
			struct vec3 rot = (s->mode == MODE_CARD_FLIP_Y)
				? (struct vec3){ 0.0f, angle, 0.0f }
				: (struct vec3){ angle, 0.0f, 0.0f };
			draw_face(s, a, cx, cy, rot, pos, zero, 1.0f - s->shading * (t * 2.0f), 1.0f, 0, 0, 1, 1);
		} else {
			float angle = (t - 1.0f) * DEG2RAD(180.0f);
			struct vec3 rot = (s->mode == MODE_CARD_FLIP_Y)
				? (struct vec3){ 0.0f, angle, 0.0f }
				: (struct vec3){ angle, 0.0f, 0.0f };
			draw_face(s, b, cx, cy, rot, pos, zero, 1.0f - s->shading * ((1.0f - t) * 2.0f), 1.0f, 0, 0, 1, 1);
		}
		break;
	}

	case MODE_CAROUSEL_SWING: {
		struct vec3 pos_a = { -t * aspect * 2.2f, 0.0f, pull - t * 1.2f };
		struct vec3 pos_b = { (1.0f - t) * aspect * 2.2f, 0.0f, pull - (1.0f - t) * 1.2f };
		struct vec3 rot_a = { 0.0f,  t * DEG2RAD(55.0f), 0.0f };
		struct vec3 rot_b = { 0.0f, -(1.0f - t) * DEG2RAD(55.0f), 0.0f };

		if (t < 0.5f) {
			draw_face(s, b, cx, cy, rot_b, pos_b, zero, 1.0f - s->shading * (1.0f - t), 1.0f, 0, 0, 1, 1);
			draw_face(s, a, cx, cy, rot_a, pos_a, zero, 1.0f - s->shading * t, 1.0f, 0, 0, 1, 1);
		} else {
			draw_face(s, a, cx, cy, rot_a, pos_a, zero, 1.0f - s->shading * t, 1.0f, 0, 0, 1, 1);
			draw_face(s, b, cx, cy, rot_b, pos_b, zero, 1.0f - s->shading * (1.0f - t), 1.0f, 0, 0, 1, 1);
		}
		break;
	}

	case MODE_PORTAL_ZOOM: {
		struct vec3 pos_b = { 0.0f, 0.0f, -(1.0f - t) * (3.0f + s->depth_pull) };
		struct vec3 rot_b = { 0.0f, 0.0f, (1.0f - t) * DEG2RAD(12.0f) };
		draw_face(s, b, cx, cy, rot_b, pos_b, zero, 1.0f, t, 0, 0, 1, 1);

		struct vec3 pos_a = { 0.0f, 0.0f, t * 2.1f };
		struct vec3 rot_a = { 0.0f, 0.0f, -t * DEG2RAD(10.0f) };
		draw_face(s, a, cx, cy, rot_a, pos_a, zero, 1.0f, 1.0f - t, 0, 0, 1, 1);
		break;
	}
	}

	gs_set_cull_mode(prev_cull);
	gs_enable_framebuffer_srgb(prev_srgb);
}

static void t3d_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct t3d_data *s = data;
	pthread_mutex_lock(&s->lock);
	const bool prev = gs_set_linear_srgb(true);
	obs_transition_video_render2(s->source, t3d_callback, NULL);
	gs_set_linear_srgb(prev);
	pthread_mutex_unlock(&s->lock);
}

static float mix_a(void *data, float t) { UNUSED_PARAMETER(data); return 1.0f - t; }
static float mix_b(void *data, float t) { UNUSED_PARAMETER(data); return t; }

static bool t3d_audio_render(void *data, uint64_t *ts_out,
                             struct obs_source_audio_mix *audio,
                             uint32_t mixers, size_t channels, size_t sample_rate)
{
	struct t3d_data *s = data;
	return obs_transition_audio_render(s->source, ts_out, audio,
	                                   mixers, channels, sample_rate,
	                                   mix_a, mix_b);
}

static enum gs_color_space t3d_get_color_space(void *data, size_t count,
                                               const enum gs_color_space *preferred)
{
	struct t3d_data *s = data;
	const enum gs_color_space ts = obs_transition_video_get_color_space(s->source);
	enum gs_color_space space = ts;
	for (size_t i = 0; i < count; ++i) {
		space = preferred[i];
		if (space == ts) break;
	}
	return space;
}

static void populate_technique_list(struct t3d_data *s, obs_property_t *tech_prop)
{
	obs_property_list_clear(tech_prop);
	for (int i = 0; i < s->num_techniques; i++) {
		obs_property_list_add_string(tech_prop,
		                             s->techniques[i],
		                             s->techniques[i]);
	}
}

static bool t3d_reload_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	struct t3d_data *s = data;
	if (!reload_effect(s))
		return false;

	obs_property_t *tech_prop = obs_properties_get(props, "technique");
	if (tech_prop)
		populate_technique_list(s, tech_prop);
	return true;
}

static obs_properties_t *t3d_properties(void *data)
{
	struct t3d_data *s = data;
	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *tech = obs_properties_add_list(
		ppts, "technique", obs_module_text("Technique"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (s)
		populate_technique_list(s, tech);

	obs_properties_add_button(ppts, "reload_effect",
	                          obs_module_text("ReloadEffect"),
	                          t3d_reload_clicked);

	obs_property_t *mode = obs_properties_add_list(
		ppts, "mode", obs_module_text("Mode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "3D Arena Stage Left",   MODE_CUBE_LEFT);
	obs_property_list_add_int(mode, "3D Arena Stage Right",  MODE_CUBE_RIGHT);
	obs_property_list_add_int(mode, "3D Arena Stage Up",     MODE_CUBE_UP);
	obs_property_list_add_int(mode, "3D Arena Stage Down",   MODE_CUBE_DOWN);
	obs_property_list_add_int(mode, "3D Arena Crane Sweep",  MODE_ARENA_CRANE);
	obs_property_list_add_int(mode, "3D Blast Doors Split",  MODE_BLAST_DOORS);
	obs_property_list_add_int(mode, "3D Stadium Louvers",    MODE_STADIUM_LOUVERS);
	obs_property_list_add_int(mode, "3D Horizontal Wave",    MODE_RIBBON_WAVE);
	obs_property_list_add_int(mode, "3D Cascading Tiles",    MODE_TILES_FLIP);
	obs_property_list_add_int(mode, "3D Card Flip (Y)",      MODE_CARD_FLIP_Y);
	obs_property_list_add_int(mode, "3D Card Flip (X)",      MODE_CARD_FLIP_X);
	obs_property_list_add_int(mode, "3D Carousel Swing",     MODE_CAROUSEL_SWING);
	obs_property_list_add_int(mode, "3D Portal Depth Zoom",  MODE_PORTAL_ZOOM);

	obs_property_t *ease = obs_properties_add_list(
		ppts, "ease", obs_module_text("Easing"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ease, "Ease In-Out Quint", EASE_INOUT_QUINT);
	obs_property_list_add_int(ease, "Ease In-Out Cubic", EASE_INOUT_CUBIC);
	obs_property_list_add_int(ease, "Bounce Overshoot",  EASE_OUT_BACK);
	obs_property_list_add_int(ease, "Linear",            EASE_LINEAR);

	obs_properties_add_float_slider(ppts, "fov",        "FOV (Degrees)",     15.0, 100.0, 1.0);
	obs_properties_add_float_slider(ppts, "depth_pull", "3D Depth Pullback", 0.0,  3.0,   0.05);
	obs_properties_add_float_slider(ppts, "shading",    "3D Edge Shading",   0.0,  0.8,   0.05);
	obs_properties_add_int_slider(ppts, "tile_cols",    "Grid / Louver Cols",2,    24,    1);
	obs_properties_add_int_slider(ppts, "tile_rows",    "Grid / Wave Rows",  2,    16,    1);

	return ppts;
}

static void t3d_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "technique", "CyberArena");
	obs_data_set_default_int(settings, "mode", MODE_CUBE_LEFT);
	obs_data_set_default_int(settings, "ease", EASE_INOUT_QUINT);
	obs_data_set_default_double(settings, "fov", 55.0);
	obs_data_set_default_double(settings, "depth_pull", 0.75);
	obs_data_set_default_double(settings, "shading", 0.40);
	obs_data_set_default_int(settings, "tile_cols", 12);
	obs_data_set_default_int(settings, "tile_rows", 7);
}

struct obs_source_info transition_3d_engine = {
	.id                    = "transition_3d_engine",
	.type                  = OBS_SOURCE_TYPE_TRANSITION,
	.get_name              = t3d_get_name,
	.create                = t3d_create,
	.destroy               = t3d_destroy,
	.update                = t3d_update,
	.video_render          = t3d_video_render,
	.audio_render          = t3d_audio_render,
	.get_properties        = t3d_properties,
	.get_defaults          = t3d_defaults,
	.video_get_color_space = t3d_get_color_space,
};
