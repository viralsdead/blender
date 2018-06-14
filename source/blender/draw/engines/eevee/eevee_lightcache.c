/*
 * Copyright 2016, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Institute
 *
 */

/** \file eevee_lightcache.c
 *  \ingroup draw_engine
 *
 * Eevee's indirect lighting cache.
 */

#include "DRW_render.h"

#include "BKE_global.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "DNA_lightprobe_types.h"
#include "DNA_group_types.h"

#include "PIL_time.h"

#include "eevee_lightcache.h"
#include "eevee_private.h"

#include "../../../intern/gawain/gawain/gwn_context.h"

#include "WM_api.h"

/* Rounded to nearest PowerOfTwo */
#if defined(IRRADIANCE_SH_L2)
#define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#define IRRADIANCE_SAMPLE_SIZE_Y 4 /* 3 in reality */
#elif defined(IRRADIANCE_CUBEMAP)
#define IRRADIANCE_SAMPLE_SIZE_X 8
#define IRRADIANCE_SAMPLE_SIZE_Y 8
#elif defined(IRRADIANCE_HL2)
#define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#define IRRADIANCE_SAMPLE_SIZE_Y 2
#endif

#define IRRADIANCE_MAX_POOL_LAYER 256 /* OpenGL 3.3 core requirement, can be extended but it's already very big */
#define IRRADIANCE_MAX_POOL_SIZE 1024
#define MAX_IRRADIANCE_SAMPLES \
        (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_X) * \
        (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_Y)

/* TODO should be replace by a more elegant alternative. */
extern void DRW_opengl_context_enable(void);
extern void DRW_opengl_context_disable(void);

extern void DRW_opengl_render_context_enable(void *re_gl_context);
extern void DRW_opengl_render_context_disable(void *re_gl_context);
extern void DRW_gawain_render_context_enable(void *re_gwn_context);
extern void DRW_gawain_render_context_disable(void *re_gwn_context);

typedef struct EEVEE_LightBake {
	Depsgraph *depsgraph;
	ViewLayer *view_layer;
	Scene *scene;
	struct Main *bmain;

	Object *probe;                   /* Current probe being rendered. */
	GPUTexture *rt_color;            /* Target cube color texture. */
	GPUTexture *rt_depth;            /* Target cube depth texture. */
	GPUFrameBuffer *rt_fb[6];        /* Target cube framebuffers. */
	GPUFrameBuffer *store_fb;        /* Storage framebuffer. */
	int rt_res;                      /* Cube render target resolution. */

	/* Shared */
	int layer;                       /* Target layer to store the data to. */
	float samples_ct, invsamples_ct; /* Sample count for the convolution. */
	float lod_factor;                /* Sampling bias during convolution step. */
	float lod_max;                   /* Max cubemap LOD to sample when convolving. */
	int cube_count, grid_count;      /* Number of probes to render + world probe. */

	/* Irradiance grid */
	int irr_cube_res;                /* Target cubemap at MIP 0. */
	int total_irr_samples;           /* Total for all grids */
	int bounce_curr, bounce_count;   /* The current light bounce being evaluated. */
	float vis_range, vis_blur;       /* Sample Visibility compression and bluring. */
	float vis_res;                   /* Resolution of the Visibility shadowmap. */
	GPUTexture *grid_prev;           /* Result of previous light bounce. */
	LightProbe **grid_prb;           /* Pointer to the id.data of the probe object. */

	/* Reflection probe */
	int ref_cube_res;                /* Target cubemap at MIP 0. */
	float probemat[6][4][4];         /* ViewProjection matrix for each cube face. */
	float texel_size, padding_size;  /* Texel and padding size for the final octahedral map. */
	float roughness;                 /* Roughness level of the current mipmap. */
	LightProbe **cube_prb;           /* Pointer to the id.data of the probe object. */

	/* Dummy Textures */
	struct GPUTexture *dummy_color, *dummy_depth;
	struct GPUTexture *dummy_layer_color;

	void *gl_context, *gwn_context;  /* If running in parallel (in a separate thread), use this context. */
} EEVEE_LightBake;

void *EEVEE_lightcache_job_data_alloc(struct Main *bmain, struct ViewLayer *view_layer, struct Scene *scene, bool run_as_job)
{
	EEVEE_LightBake *lbake = MEM_callocN(sizeof(EEVEE_LightBake), "EEVEE_LightBake");

	lbake->depsgraph = DEG_graph_new(scene, view_layer, DAG_EVAL_RENDER);
	lbake->scene = scene;
	lbake->bmain = bmain;

	if (run_as_job) {
		lbake->gl_context = WM_opengl_context_create();
	}

    DEG_graph_relations_update(lbake->depsgraph, bmain, scene, view_layer);

	return lbake;
}

void EEVEE_lightcache_job_data_free(void *custom_data)
{
	EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;

	DEG_graph_free(lbake->depsgraph);

	MEM_SAFE_FREE(lbake->cube_prb);
	MEM_SAFE_FREE(lbake->grid_prb);

	MEM_freeN(lbake);
}

static void eevee_irradiance_pool_size_get(int visibility_size, int total_samples, int r_size[3])
{
	/* Compute how many irradiance samples we can store per visibility sample. */
	int irr_per_vis = (visibility_size / IRRADIANCE_SAMPLE_SIZE_X) *
	                  (visibility_size / IRRADIANCE_SAMPLE_SIZE_Y);

	/* The irradiance itself take one layer, hence the +1 */
	int layer_ct = MIN2(irr_per_vis + 1, IRRADIANCE_MAX_POOL_LAYER);

	int texel_ct = (int)ceilf((float)total_samples / (float)(layer_ct - 1));
	r_size[0] = visibility_size * max_ii(1, min_ii(texel_ct, (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
	r_size[1] = visibility_size * max_ii(1, (texel_ct / (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
	r_size[2] = layer_ct;
}

static void eevee_lightcache_create_resources(const SceneEEVEE *eevee, EEVEE_LightBake *lbake, EEVEE_LightCache *lcache)
{
	lbake->bounce_count = eevee->gi_diffuse_bounces;
	lbake->vis_res      = eevee->gi_visibility_resolution;
	lbake->rt_res       = eevee->gi_cubemap_resolution;

	/* TODO: Remove when multiple drawmanager/context are supported.
	 * Currently this locks the viewport without any reason
	 * (resource creation can be done from another context). */
	if (lbake->gl_context) {
		DRW_opengl_render_context_enable(lbake->gl_context);
		lbake->gwn_context = GWN_context_create();
		DRW_gawain_render_context_enable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_enable();
	}

	//lbake->ref_cube_res = octahedral_from_cubemap();
	lbake->ref_cube_res = lbake->rt_res;

	/* Only one render target for now. */
	lbake->rt_depth = DRW_texture_create_cube(lbake->rt_res, GPU_DEPTH_COMPONENT24, 0, NULL);
	lbake->rt_color = DRW_texture_create_cube(lbake->rt_res, GPU_RGBA16F, DRW_TEX_FILTER | DRW_TEX_MIPMAP, NULL);

	for (int i = 0; i < 6; ++i) {
		GPU_framebuffer_ensure_config(&lbake->rt_fb[i], {
			GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_depth, i),
			GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_color, i)
		});
	}

	int irr_size[3];
	eevee_irradiance_pool_size_get(lbake->vis_res, lbake->total_irr_samples, irr_size);

#ifdef IRRADIANCE_SH_L2
	/* we need a signed format for Spherical Harmonics */
	int irradiance_format = GPU_RGBA16F;
#else
	int irradiance_format = GPU_RGBA8;
#endif
	lbake->grid_prev = DRW_texture_create_2D_array(irr_size[0], irr_size[1], irr_size[2], irradiance_format, DRW_TEX_FILTER, NULL);
	// lcache->grid_tx = DRW_texture_create_2D_array(irr_size[0], irr_size[1], irr_size[2], irradiance_format, DRW_TEX_FILTER, NULL);
	// lcache->cube_tx = DRW_texture_create_2D_array(lbake->ref_cube_res, lbake->ref_cube_res, lcache->cube_count, GPU_RGBA8, DRW_TEX_FILTER, NULL);
	/* XXX */
	float rgba[4] = {1.5f, 0.5f, 0.5f, 0.5f};
	lcache->grid_tx = DRW_texture_create_2D_array(1, 1, 1, irradiance_format, DRW_TEX_FILTER, rgba);
	lcache->cube_tx = DRW_texture_create_2D_array(1, 1, 1, GPU_RGBA8, DRW_TEX_FILTER, rgba);
	DRW_TEXTURE_FREE_SAFE(lcache->cube_tx);
	DRW_TEXTURE_FREE_SAFE(lcache->grid_tx);

	if (lbake->gl_context) {
		DRW_opengl_render_context_disable(lbake->gl_context);
		DRW_gawain_render_context_disable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_disable();
	}
}

static void eevee_lightcache_delete_resources(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_enable(lbake->gl_context);
		DRW_gawain_render_context_enable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_enable();
	}

	DRW_TEXTURE_FREE_SAFE(lbake->rt_depth);
	DRW_TEXTURE_FREE_SAFE(lbake->rt_color);
	DRW_TEXTURE_FREE_SAFE(lbake->grid_prev);
	for (int i = 0; i < 6; ++i) {
		GPU_FRAMEBUFFER_FREE_SAFE(lbake->rt_fb[i]);
	}

	if (lbake->gl_context) {
		/* Delete the baking context. */
		DRW_gawain_render_context_disable(lbake->gwn_context);
		DRW_gawain_render_context_enable(lbake->gwn_context);
		GWN_context_discard(lbake->gwn_context);
		DRW_opengl_render_context_disable(lbake->gl_context);
		WM_opengl_context_dispose(lbake->gl_context);
		lbake->gwn_context = NULL;
		lbake->gl_context = NULL;
	}
	else {
		DRW_opengl_context_disable();
	}
}

static void eevee_lightcache_context_enable(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_enable(lbake->gl_context);
		DRW_gawain_render_context_enable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_enable();
	}
}

static void eevee_lightcache_context_disable(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_disable(lbake->gl_context);
		DRW_gawain_render_context_disable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_disable();
	}
}

static void eevee_lightcache_render_world(void *ved, void *UNUSED(user_data))
{
	EEVEE_Data *vedata = (EEVEE_Data *)ved;
	EEVEE_StorageList *stl = vedata->stl;
	EEVEE_FramebufferList *fbl = vedata->fbl;
	// EEVEE_LightBake *lbake = user_data;
	EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();

	EEVEE_materials_init(sldata, stl, fbl);
	EEVEE_lights_init(sldata);
	EEVEE_lightprobes_init(sldata, vedata);

	EEVEE_lightprobes_cache_init(sldata, vedata);
	EEVEE_lightprobes_refresh_world(sldata, vedata);
}

static void eevee_lightcache_render_probe(void *ved, void *UNUSED(user_data))
{
	EEVEE_Data *vedata = (EEVEE_Data *)ved;
	EEVEE_StorageList *stl = vedata->stl;
	EEVEE_FramebufferList *fbl = vedata->fbl;
	// EEVEE_LightBake *lbake = user_data;
	EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();

	EEVEE_materials_init(sldata, stl, fbl);
	EEVEE_lights_init(sldata);
	EEVEE_lightprobes_init(sldata, vedata);

	EEVEE_lightprobes_cache_init(sldata, vedata);
	EEVEE_lights_cache_init(sldata, vedata);
	EEVEE_materials_cache_init(sldata, vedata);

	/* Disable specular lighting when rendering probes to avoid feedback loops (looks bad).
	 * Disable AO until we find a way to hide really bad discontinuities between cubefaces. */
	// common_data->spec_toggle = false;
	// common_data->ssr_toggle = false;
	// common_data->sss_toggle = false;
	// common_data->ao_settings = 0.0f;
	// common_data->ao_dist = 0.0f;

	EEVEE_lightprobes_refresh_world(sldata, vedata);
}

static void eevee_lightcache_gather_probes(EEVEE_LightBake *lbake, EEVEE_LightCache *lcache)
{
	Depsgraph *depsgraph = lbake->depsgraph;

	/* First convert all lightprobes to tight UBO data from all lightprobes in the scene.
	 * This allows a large number of probe to be precomputed. */
	if (lcache->cube_data == NULL) {
		lcache->cube_data = MEM_callocN(1, "EEVEE Cube Data Cache");
		lcache->grid_data = MEM_callocN(1, "EEVEE Grid Data Cache");
	}
	lbake->cube_prb = MEM_callocN(sizeof(LightProbe *), "EEVEE Cube visgroup ptr");
	lbake->grid_prb = MEM_callocN(sizeof(LightProbe *), "EEVEE Grid visgroup ptr");

	lcache->grid_count = lbake->grid_count = 1;
	lcache->cube_count = lbake->cube_count = 1;
	lbake->total_irr_samples = 1; /* At least one for the world */

	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_BEGIN(depsgraph, ob)
	{
		if (ob->type == OB_LIGHTPROBE) {
			LightProbe *prb = (LightProbe *)ob->data;

			if (prb->type == LIGHTPROBE_TYPE_GRID) {
				lbake->grid_count += 1;
				lcache->grid_data = MEM_reallocN(lcache->grid_data, sizeof(EEVEE_LightGrid) * lbake->grid_count);
				lbake->grid_prb = MEM_reallocN(lbake->grid_prb, sizeof(LightProbe *) * lbake->cube_count);

				EEVEE_LightGrid *egrid = &lcache->grid_data[lbake->grid_count - 1];
				EEVEE_lightprobes_grid_data_from_object(ob, egrid, &lbake->total_irr_samples);

				lbake->grid_prb[lbake->grid_count - 1] = prb;
			}
			else if (prb->type == LIGHTPROBE_TYPE_CUBE) {
				lbake->cube_count += 1;
				lcache->cube_data = MEM_reallocN(lcache->cube_data, sizeof(EEVEE_LightProbe) * lbake->cube_count);
				lbake->cube_prb = MEM_reallocN(lbake->cube_prb, sizeof(LightProbe *) * lbake->cube_count);

				EEVEE_LightProbe *eprobe = &lcache->cube_data[lbake->cube_count - 1];
				EEVEE_lightprobes_cube_data_from_object(ob, eprobe);

				lbake->cube_prb[lbake->cube_count - 1] = prb;
			}
		}
	}
	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;
}

void EEVEE_lightcache_bake_job(void *custom_data, short *UNUSED(stop), short *UNUSED(do_update), float *UNUSED(progress))
{
	EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;
	Depsgraph *depsgraph = lbake->depsgraph;

	int frame = 0; /* TODO make it user param. */
	DEG_evaluate_on_framechange(lbake->bmain, depsgraph, frame);

	ViewLayer *view_layer_ori = DEG_get_input_view_layer(depsgraph);
	const Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);

	EEVEE_ViewLayerData *sldata_ori = EEVEE_view_layer_data_ensure_ex(view_layer_ori);
	EEVEE_LightCache *lcache = EEVEE_lightcache_ensure(sldata_ori);

	/* Share light cache with the evaluated layer and the original layer. */
	lbake->view_layer = DEG_get_evaluated_view_layer(depsgraph);
	EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure_ex(lbake->view_layer);
	EEVEE_lightcache_add_reference(sldata, lcache);

	/* Gather all probes data */
	eevee_lightcache_gather_probes(lbake, lcache);
	eevee_lightcache_create_resources(&scene_eval->eevee, lbake, lcache);

	/* Render world irradiance and reflection first */
	if (lcache->flag & LIGHTCACHE_UPDATE_WORLD) {
		eevee_lightcache_context_enable(lbake);
		DRW_custom_pipeline(&draw_engine_eevee_type, depsgraph, eevee_lightcache_render_world, lbake);
		DEG_id_tag_update(&lbake->scene->id, DEG_TAG_COPY_ON_WRITE);
		eevee_lightcache_context_disable(lbake);
	}

#if 0
	/* Render irradiance grids */
	lbake->bounce_curr = 0;
	while (lbake->bounce_curr < lbake->bounce_count) {
		/* Bypass world, start at 1. */
		for (int p = 1; p < lbake->grid_count; ++p) {
			/* TODO: make DRW manager instanciable (and only lock on drawing) */
			DRW_opengl_context_enable();

			/* Create passes */
			/* Iter through objects */
			DRW_opengl_context_disable();

			/* Render one cubemap/irradiance sample. */
			if (*stop != 0) {
				return;
			}
		}
		lbake->bounce_curr += 1;
	}

	/* Render reflections */
	for (prb in cube_probes) {
		/* Ask for lower importance draw manager lock. */

		/* Create passes */
		/* Iter through objects */
		/* Render one cubemap/irradiance sample. */
		if (*stop != 0) {
			return;
		}
	}
#endif

	eevee_lightcache_delete_resources(lbake);
}


EEVEE_LightCache *EEVEE_lightcache_ensure(EEVEE_ViewLayerData *sldata)
{
	if (sldata->light_cache == NULL) {
		sldata->light_cache = MEM_callocN(sizeof(EEVEE_LightCache), "EEVEE_LightCache");
		sldata->light_cache->refcount++;
	}

	return sldata->light_cache;
}

void EEVEE_lightcache_add_reference(EEVEE_ViewLayerData *sldata, EEVEE_LightCache *lcache)
{
	lcache->refcount++;

	if (sldata->light_cache != NULL) {
		EEVEE_lightcache_free(lcache);
	}

	sldata->light_cache = lcache;
}

void EEVEE_lightcache_free(EEVEE_LightCache *lcache)
{
	lcache->refcount--;

	if (lcache->refcount == 0) {
		DRW_TEXTURE_FREE_SAFE(lcache->cube_tx);
		DRW_TEXTURE_FREE_SAFE(lcache->grid_tx);

		MEM_SAFE_FREE(lcache->cube_data);
		MEM_SAFE_FREE(lcache->grid_data);
		MEM_freeN(lcache);
	}
}