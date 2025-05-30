/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "log.h"
#include "shaders.h"
#include "dispatch.h"
#include "gpu.h"
#include "pl_thread.h"

// Maximum number of passes to keep around at once. If full, passes older than
// MIN_AGE are evicted to make room. (Failing that, the passes array doubles)
#define MAX_PASSES 100
#define MIN_AGE 10

enum {
    TMP_PRELUDE,   // GLSL version, global definitions, etc.
    TMP_MAIN,      // main GLSL shader body
    TMP_VERT_HEAD, // vertex shader inputs/outputs
    TMP_VERT_BODY, // vertex shader body
    TMP_COUNT,
};

struct pl_dispatch_t {
    pl_mutex lock;
    pl_log log;
    pl_gpu gpu;
    uint8_t current_ident;
    uint8_t current_index;
    bool dynamic_constants;
    int max_passes;

    void (*info_callback)(void *, const struct pl_dispatch_info *);
    void *info_priv;

    PL_ARRAY(pl_shader) shaders;                // to avoid re-allocations
    PL_ARRAY(struct pass *) passes;             // compiled passes

    // temporary buffers to help avoid re_allocations during pass creation
    PL_ARRAY(const struct pl_buffer_var *) buf_tmp;
    pl_str_builder tmp[TMP_COUNT];
    uint8_t *ubo_tmp;
};

enum pass_var_type {
    PASS_VAR_NONE = 0,
    PASS_VAR_GLOBAL, // regular/global uniforms
    PASS_VAR_UBO,    // uniform buffers
    PASS_VAR_PUSHC   // push constants
};

// Cached metadata about a variable's effective placement / update method
struct pass_var {
    int index; // for pl_var_update
    enum pass_var_type type;
    struct pl_var_layout layout;
    void *cached_data;
};

struct pass {
    uint64_t signature;
    pl_pass pass;
    int last_index;

    // contains cached data and update metadata, same order as pl_shader
    struct pass_var *vars;
    int num_var_locs;

    // for uniform buffer updates
    struct pl_shader_desc ubo_desc; // temporary
    int ubo_index;
    pl_buf ubo;

    // Cached pl_pass_run_params. This will also contain mutable allocations
    // for the push constants, descriptor bindings (including the binding for
    // the UBO pre-filled), vertex array and variable updates
    struct pl_pass_run_params run_params;

    // for pl_dispatch_info
    pl_timer timer;
    uint64_t ts_last;
    uint64_t ts_peak;
    uint64_t ts_sum;
    uint64_t samples[PL_ARRAY_SIZE(((struct pl_dispatch_info *) NULL)->samples)];
    int ts_idx;
};

static void pass_destroy(pl_dispatch dp, struct pass *pass)
{
    if (!pass)
        return;

    pl_buf_destroy(dp->gpu, &pass->ubo);
    pl_pass_destroy(dp->gpu, &pass->pass);
    pl_timer_destroy(dp->gpu, &pass->timer);
    pl_free(pass);
}

pl_dispatch pl_dispatch_create(pl_log log, pl_gpu gpu)
{
    struct pl_dispatch_t *dp = pl_zalloc_ptr(NULL, dp);
    pl_mutex_init(&dp->lock);
    dp->log = log;
    dp->gpu = gpu;
    dp->max_passes = MAX_PASSES;
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        dp->tmp[i] = pl_str_builder_alloc(dp);

    return dp;
}

void pl_dispatch_destroy(pl_dispatch *ptr)
{
    pl_dispatch dp = *ptr;
    if (!dp)
        return;

    for (int i = 0; i < dp->passes.num; i++)
        pass_destroy(dp, dp->passes.elem[i]);
    for (int i = 0; i < dp->shaders.num; i++)
        pl_shader_free(&dp->shaders.elem[i]);

    pl_mutex_destroy(&dp->lock);
    pl_free(dp);
    *ptr = NULL;
}

pl_shader pl_dispatch_begin_ex(pl_dispatch dp, bool unique)
{
    pl_mutex_lock(&dp->lock);

    struct pl_shader_params params = {
        .id = unique ? dp->current_ident++ : 0,
        .gpu = dp->gpu,
        .index = dp->current_index,
        .dynamic_constants = dp->dynamic_constants,
    };

    pl_shader sh = NULL;
    PL_ARRAY_POP(dp->shaders, &sh);
    pl_mutex_unlock(&dp->lock);

    if (sh) {
        pl_shader_reset(sh, &params);
        return sh;
    }

    return pl_shader_alloc(dp->log, &params);
}

void pl_dispatch_mark_dynamic(pl_dispatch dp, bool dynamic)
{
    dp->dynamic_constants = dynamic;
}

void pl_dispatch_callback(pl_dispatch dp, void *priv,
                          void (*cb)(void *priv, const struct pl_dispatch_info *))
{
    dp->info_callback = cb;
    dp->info_priv = priv;
}

pl_shader pl_dispatch_begin(pl_dispatch dp)
{
    return pl_dispatch_begin_ex(dp, false);
}

static bool add_pass_var(pl_dispatch dp, void *tmp, struct pass *pass,
                         struct pl_pass_params *params,
                         const struct pl_shader_var *sv, struct pass_var *pv,
                         bool greedy)
{
    pl_gpu gpu = dp->gpu;
    if (pv->type)
        return true;

    // Try not to use push constants for "large" values like matrices in the
    // first pass, since this is likely to exceed the VGPR/pushc size budgets
    bool try_pushc = greedy || (sv->var.dim_m == 1 && sv->var.dim_a == 1) || sv->dynamic;
    if (try_pushc && gpu->glsl.vulkan && gpu->limits.max_pushc_size) {
        pv->layout = pl_std430_layout(params->push_constants_size, &sv->var);
        size_t new_size = pv->layout.offset + pv->layout.size;
        if (new_size <= gpu->limits.max_pushc_size) {
            params->push_constants_size = new_size;
            pv->type = PASS_VAR_PUSHC;
            return true;
        }
    }

    // If we haven't placed all PCs yet, don't place anything else, since
    // we want to try and fit more stuff into PCs before "giving up"
    if (!greedy)
        return true;

    int num_locs = sv->var.dim_v * sv->var.dim_m * sv->var.dim_a;
    bool can_var = pass->num_var_locs + num_locs <= gpu->limits.max_variable_comps;

    // Attempt using uniform buffer next. The GLSL version 440 check is due
    // to explicit offsets on UBO entries. In theory we could leave away
    // the offsets and support UBOs for older GL as well, but this is a nice
    // safety net for driver bugs (and also rules out potentially buggy drivers)
    // Also avoid UBOs for highly dynamic stuff since that requires synchronizing
    // the UBO writes every frame
    bool try_ubo = !can_var || !sv->dynamic;
    if (try_ubo && gpu->glsl.version >= 440 && gpu->limits.max_ubo_size) {
        if (sh_buf_desc_append(tmp, gpu, &pass->ubo_desc, &pv->layout, sv->var)) {
            pv->type = PASS_VAR_UBO;
            return true;
        }
    }

    // Otherwise, use global uniforms
    if (can_var) {
        pv->type = PASS_VAR_GLOBAL;
        pv->index = params->num_variables;
        pv->layout = pl_var_host_layout(0, &sv->var);
        PL_ARRAY_APPEND_RAW(tmp, params->variables, params->num_variables, sv->var);
        pass->num_var_locs += num_locs;
        return true;
    }

    // Ran out of variable binding methods. The most likely scenario in which
    // this can happen is if we're using a GPU that does not support global
    // input vars and we've exhausted the UBO size limits.
    PL_ERR(dp, "Unable to add input variable: possibly exhausted "
           "variable count / UBO size limits?");
    return false;
}

#define ADD(b, ...)     pl_str_builder_addf(b, __VA_ARGS__)
#define ADD_CAT(b, cat) pl_str_builder_concat(b, cat)
#define ADD_CONST(b, s) pl_str_builder_const_str(b, s)

static void add_var(pl_str_builder body, const struct pl_var *var)
{
    const char *type = pl_var_glsl_type_name(*var);
    if (var->dim_a > 1) {
        ADD(body, "%s "$"[%d];\n", type, sh_ident_unpack(var->name), var->dim_a);
    } else {
        ADD(body, "%s "$";\n", type, sh_ident_unpack(var->name));
    }
}

static int cmp_buffer_var(const void *pa, const void *pb)
{
    const struct pl_buffer_var * const *a = pa, * const *b = pb;
    return PL_CMP((*a)->layout.offset, (*b)->layout.offset);
}

static void add_buffer_vars(pl_dispatch dp, void *tmp, pl_str_builder body,
                            const struct pl_buffer_var *vars, int num)
{
    // Sort buffer vars by offset
    PL_ARRAY_RESIZE(dp, dp->buf_tmp, num);
    for (int i = 0; i < num; i++)
        dp->buf_tmp.elem[i] = &vars[i];
    qsort(dp->buf_tmp.elem, num, sizeof(&vars[0]), cmp_buffer_var);

    ADD(body, "{\n");
    for (int i = 0; i < num; i++) {
        const struct pl_buffer_var *bv = dp->buf_tmp.elem[i];
        // Add an explicit offset wherever possible
        if (dp->gpu->glsl.version >= 440)
            ADD(body, "    layout(offset=%zu) ", bv->layout.offset);
        add_var(body, &bv->var);
    }
    ADD(body, "};\n");
}

struct generate_params {
    void *tmp;
    pl_shader sh;
    struct pass *pass;
    struct pl_pass_params *pass_params;
    ident_t out_mat;
    ident_t out_off;
    int vert_idx;
};

static void generate_shaders(pl_dispatch dp,
                             const struct generate_params *params,
                             pl_str_builder *out_vert_builder,
                             pl_str_builder *out_glsl_builder)
{
    pl_gpu gpu = dp->gpu;
    pl_shader sh = params->sh;
    void *tmp = params->tmp;
    struct pass *pass = params->pass;
    struct pl_pass_params *pass_params = params->pass_params;
    pl_str_builder shader_body = sh_finalize_internal(sh);

    pl_str_builder pre = dp->tmp[TMP_PRELUDE];
    ADD(pre, "#version %d%s\n", gpu->glsl.version,
        (gpu->glsl.gles && gpu->glsl.version > 100) ? " es" : "");
    if (pass_params->type == PL_PASS_COMPUTE)
        ADD(pre, "#extension GL_ARB_compute_shader : enable\n");

    // Enable this unconditionally if the GPU supports it, since we have no way
    // of knowing whether subgroups are being used or not
    if (gpu->glsl.subgroup_size) {
        ADD(pre, "#extension GL_KHR_shader_subgroup_basic : enable \n"
                 "#extension GL_KHR_shader_subgroup_vote : enable \n"
                 "#extension GL_KHR_shader_subgroup_arithmetic : enable \n"
                 "#extension GL_KHR_shader_subgroup_ballot : enable \n"
                 "#extension GL_KHR_shader_subgroup_shuffle : enable \n"
                 "#extension GL_KHR_shader_subgroup_clustered : enable \n"
                 "#extension GL_KHR_shader_subgroup_quad : enable \n");
    }

    // Enable all extensions needed for different types of input
    bool has_ssbo = false, has_ubo = false, has_img = false, has_texel = false,
         has_ext = false, has_nofmt = false, has_gather = false;
    for (int i = 0; i < sh->descs.num; i++) {
        switch (sh->descs.elem[i].desc.type) {
        case PL_DESC_BUF_UNIFORM: has_ubo = true; break;
        case PL_DESC_BUF_STORAGE: has_ssbo = true; break;
        case PL_DESC_BUF_TEXEL_UNIFORM: has_texel = true; break;
        case PL_DESC_BUF_TEXEL_STORAGE: {
            pl_buf buf = sh->descs.elem[i].binding.object;
            has_nofmt |= !buf->params.format->glsl_format;
            has_texel = true;
            break;
        }
        case PL_DESC_STORAGE_IMG: {
            pl_tex tex = sh->descs.elem[i].binding.object;
            has_nofmt |= !tex->params.format->glsl_format;
            has_img = true;
            break;
        }
        case PL_DESC_SAMPLED_TEX: {
            pl_tex tex = sh->descs.elem[i].binding.object;
            has_gather |= tex->params.format->gatherable;
            switch (tex->sampler_type) {
            case PL_SAMPLER_NORMAL: break;
            case PL_SAMPLER_RECT: break;
            case PL_SAMPLER_EXTERNAL: has_ext = true; break;
            case PL_SAMPLER_TYPE_COUNT: pl_unreachable();
            }
            break;
        }

        case PL_DESC_INVALID:
        case PL_DESC_TYPE_COUNT:
            pl_unreachable();
        }
    }

    if (has_img && !gpu->glsl.gles)
        ADD(pre, "#extension GL_ARB_shader_image_load_store : enable\n");
    if (has_ubo)
        ADD(pre, "#extension GL_ARB_uniform_buffer_object : enable\n");
    if (has_ssbo)
        ADD(pre, "#extension GL_ARB_shader_storage_buffer_object : enable\n");
    if (has_texel)
        ADD(pre, "#extension GL_ARB_texture_buffer_object : enable\n");
    if (has_ext) {
        if (gpu->glsl.version >= 300) {
            ADD(pre, "#extension GL_OES_EGL_image_external_essl3 : enable\n");
        } else {
            ADD(pre, "#extension GL_OES_EGL_image_external : enable\n");
        }
    }
    if (has_nofmt)
        ADD(pre, "#extension GL_EXT_shader_image_load_formatted : enable\n");
    if (has_gather && !gpu->glsl.gles)
        ADD(pre, "#extension GL_ARB_texture_gather : enable\n");

    if (gpu->glsl.gles) {
        // Use 32-bit precision for floats if possible
        ADD(pre, "#ifdef GL_FRAGMENT_PRECISION_HIGH \n"
                 "precision highp float;            \n"
                 "#else                             \n"
                 "precision mediump float;          \n"
                 "#endif                            \n");

        // Always use 16-bit precision for samplers
        ADD(pre, "precision mediump sampler2D; \n");
        if (gpu->limits.max_tex_1d_dim)
            ADD(pre, "precision mediump sampler1D; \n");
        if (gpu->limits.max_tex_3d_dim && gpu->glsl.version > 100)
            ADD(pre, "precision mediump sampler3D; \n");

        // Integer math has a good chance of caring about precision
        ADD(pre, "precision highp int; \n");
    }

    // textureLod() doesn't work on external/rect samplers, simply disable
    // LOD sampling in this case. We don't currently support mipmaps anyway.
    for (int i = 0; i < sh->descs.num; i++) {
        if (pass_params->descriptors[i].type != PL_DESC_SAMPLED_TEX)
            continue;
        pl_tex tex = sh->descs.elem[i].binding.object;
        if (tex->sampler_type != PL_SAMPLER_NORMAL) {
            ADD(pre, "#define textureLod(t, p, b) texture(t, p) \n"
                     "#define textureLodOffset(t, p, b, o)    \\\n"
                     "        textureOffset(t, p, o)            \n");
            break;
        }
    }

    // Add all of the push constants as their own element
    if (pass_params->push_constants_size) {
        // We re-use add_buffer_vars to make sure variables are sorted, this
        // is important because the push constants can be out-of-order in
        // `pass->vars`
        PL_ARRAY(struct pl_buffer_var) pc_bvars = {0};
        for (int i = 0; i < sh->vars.num; i++) {
            if (pass->vars[i].type != PASS_VAR_PUSHC)
                continue;

            PL_ARRAY_APPEND(tmp, pc_bvars, (struct pl_buffer_var) {
                .var = sh->vars.elem[i].var,
                .layout = pass->vars[i].layout,
            });
        }

        ADD(pre, "layout(std430, push_constant) uniform PushC ");
        add_buffer_vars(dp, tmp, pre, pc_bvars.elem, pc_bvars.num);
    }

    // Add all of the specialization constants
    for (int i = 0; i < sh->consts.num; i++) {
        static const char *types[PL_VAR_TYPE_COUNT] = {
            [PL_VAR_SINT]   = "int",
            [PL_VAR_UINT]   = "uint",
            [PL_VAR_FLOAT]  = "float",
        };

        const struct pl_shader_const *sc = &sh->consts.elem[i];
        ADD(pre, "layout(constant_id=%"PRIu32") const %s "$" = 1; \n",
            pass_params->constants[i].id, types[sc->type],
            sh_ident_unpack(sc->name));
    }

    static const char sampler_prefixes[PL_FMT_TYPE_COUNT] = {
        [PL_FMT_FLOAT]  = ' ',
        [PL_FMT_UNORM]  = ' ',
        [PL_FMT_SNORM]  = ' ',
        [PL_FMT_UINT]   = 'u',
        [PL_FMT_SINT]   = 'i',
    };

    // Add all of the required descriptors
    for (int i = 0; i < sh->descs.num; i++) {
        const struct pl_shader_desc *sd = &sh->descs.elem[i];
        const struct pl_desc *desc = &pass_params->descriptors[i];

        switch (desc->type) {
        case PL_DESC_SAMPLED_TEX: {
            static const char *types[][4] = {
                [PL_SAMPLER_NORMAL][1]  = "sampler1D",
                [PL_SAMPLER_NORMAL][2]  = "sampler2D",
                [PL_SAMPLER_NORMAL][3]  = "sampler3D",
                [PL_SAMPLER_RECT][2]    = "sampler2DRect",
                [PL_SAMPLER_EXTERNAL][2] = "samplerExternalOES",
            };

            pl_tex tex = sd->binding.object;
            int dims = pl_tex_params_dimension(tex->params);
            const char *type = types[tex->sampler_type][dims];
            char prefix = sampler_prefixes[tex->params.format->type];
            ident_t id = sh_ident_unpack(desc->name);
            pl_assert(type && prefix);

            // Vulkan requires explicit bindings; GL always sets the
            // bindings manually to avoid relying on the user doing so
            if (gpu->glsl.vulkan) {
                ADD(pre, "layout(binding=%d) uniform %c%s "$";\n",
                    desc->binding, prefix, type, id);
            } else if (gpu->glsl.gles && prefix != ' ') {
                ADD(pre, "uniform highp %c%s "$";\n", prefix, type, id);
            } else {
                ADD(pre, "uniform %c%s "$";\n", prefix, type, id);
            }
            break;
        }

        case PL_DESC_STORAGE_IMG: {
            static const char *types[] = {
                [1] = "image1D",
                [2] = "image2D",
                [3] = "image3D",
            };

            // For better compatibility, we have to explicitly label the
            // type of data we will be reading/writing to this image.
            pl_tex tex = sd->binding.object;
            const char *format = tex->params.format->glsl_format;
            int dims = pl_tex_params_dimension(tex->params);
            if (gpu->glsl.vulkan) {
                if (format) {
                    ADD(pre, "layout(binding=%d, %s) ", desc->binding, format);
                } else {
                    ADD(pre, "layout(binding=%d) ", desc->binding);
                }
            } else if (format) {
                ADD(pre, "layout(%s) ", format);
            }

            ADD_CONST(pre, pl_desc_access_glsl_name(desc->access));
            if (sd->memory & PL_MEMORY_COHERENT)
                ADD(pre, " coherent");
            if (sd->memory & PL_MEMORY_VOLATILE)
                ADD(pre, " volatile");
            ADD(pre, " restrict uniform %s "$";\n",
                types[dims], sh_ident_unpack(desc->name));
            break;
        }

        case PL_DESC_BUF_UNIFORM:
            if (gpu->glsl.vulkan) {
                ADD(pre, "layout(std140, binding=%d) ", desc->binding);
            } else {
                ADD(pre, "layout(std140) ");
            }
            ADD(pre, "uniform "$" ", sh_ident_unpack(desc->name));
            add_buffer_vars(dp, tmp, pre, sd->buffer_vars, sd->num_buffer_vars);
            break;

        case PL_DESC_BUF_STORAGE:
            if (gpu->glsl.version >= 140)
                ADD(pre, "layout(std430, binding=%d) ", desc->binding);
            ADD_CONST(pre, pl_desc_access_glsl_name(desc->access));
            if (sd->memory & PL_MEMORY_COHERENT)
                ADD(pre, " coherent");
            if (sd->memory & PL_MEMORY_VOLATILE)
                ADD(pre, " volatile");
            ADD(pre, " restrict buffer "$" ", sh_ident_unpack(desc->name));
            add_buffer_vars(dp, tmp, pre, sd->buffer_vars, sd->num_buffer_vars);
            break;

        case PL_DESC_BUF_TEXEL_UNIFORM: {
            pl_buf buf = sd->binding.object;
            char prefix = sampler_prefixes[buf->params.format->type];
            if (gpu->glsl.vulkan)
                ADD(pre, "layout(binding=%d) ", desc->binding);
            ADD(pre, "uniform %csamplerBuffer "$";\n", prefix,
                sh_ident_unpack(desc->name));
            break;
        }

        case PL_DESC_BUF_TEXEL_STORAGE: {
            pl_buf buf = sd->binding.object;
            const char *format = buf->params.format->glsl_format;
            char prefix = sampler_prefixes[buf->params.format->type];
            if (gpu->glsl.vulkan) {
                if (format) {
                    ADD(pre, "layout(binding=%d, %s) ", desc->binding, format);
                } else {
                    ADD(pre, "layout(binding=%d) ", desc->binding);
                }
            } else if (format) {
                ADD(pre, "layout(%s) ", format);
            }

            ADD_CONST(pre, pl_desc_access_glsl_name(desc->access));
            if (sd->memory & PL_MEMORY_COHERENT)
                ADD(pre, " coherent");
            if (sd->memory & PL_MEMORY_VOLATILE)
                ADD(pre, " volatile");
            ADD(pre, " restrict uniform %cimageBuffer "$";\n",
                prefix, sh_ident_unpack(desc->name));
            break;
        }

        case PL_DESC_INVALID:
        case PL_DESC_TYPE_COUNT:
            pl_unreachable();
        }
    }

    // Add all of the remaining variables
    for (int i = 0; i < sh->vars.num; i++) {
        const struct pl_var *var = &sh->vars.elem[i].var;
        const struct pass_var *pv = &pass->vars[i];
        if (pv->type != PASS_VAR_GLOBAL)
            continue;
        ADD(pre, "uniform ");
        add_var(pre, var);
    }

    pl_str_builder glsl = dp->tmp[TMP_MAIN];
    ADD_CAT(glsl, pre);

    switch(pass_params->type) {
    case PL_PASS_RASTER: {
        pl_assert(params->vert_idx >= 0);
        pl_str_builder vert_head = dp->tmp[TMP_VERT_HEAD];
        pl_str_builder vert_body = dp->tmp[TMP_VERT_BODY];

        // Older GLSL doesn't support the use of explicit locations
        bool has_loc = gpu->glsl.version >= 430;

        // Set up a trivial vertex shader
        ADD_CAT(vert_head, pre);
        ADD(vert_body, "void main() {\n");
        for (int i = 0; i < sh->vas.num; i++) {
            const struct pl_vertex_attrib *va = &pass_params->vertex_attribs[i];
            const struct pl_shader_va *sva = &sh->vas.elem[i];
            const char *type = va->fmt->glsl_type;

            // Use the pl_shader_va for the name in the fragment shader since
            // the pl_vertex_attrib is already mangled for the vertex shader
            ident_t id = sh_ident_unpack(sva->attr.name);

            if (has_loc) {
                ADD(vert_head, "layout(location=%d) in %s "$";\n",
                    va->location, type, sh_ident_unpack(va->name));
            } else {
                ADD(vert_head, "in %s "$";\n", type, sh_ident_unpack(va->name));
            }

            if (i == params->vert_idx) {
                pl_assert(va->fmt->num_components == 2);
                ADD(vert_body, "vec2 va_pos = "$"; \n", sh_ident_unpack(va->name));
                if (params->out_mat)
                    ADD(vert_body, "va_pos = "$" * va_pos; \n", params->out_mat);
                if (params->out_off)
                    ADD(vert_body, "va_pos += "$"; \n", params->out_off);
                ADD(vert_body, "gl_Position = vec4(va_pos, 0.0, 1.0); \n");
            } else {
                // Everything else is just blindly passed through
                if (has_loc) {
                    ADD(vert_head, "layout(location=%d) out %s "$";\n",
                        va->location, type, id);
                    ADD(glsl, "layout(location=%d) in %s "$";\n",
                        va->location, type, id);
                } else {
                    ADD(vert_head, "out %s "$";\n", type, id);
                    ADD(glsl, "in %s "$";\n", type, id);
                }
                ADD(vert_body, $" = "$";\n", id, sh_ident_unpack(va->name));
            }
        }

        ADD(vert_body, "}");
        ADD_CAT(vert_head, vert_body);
        pl_hash_merge(&pass->signature, pl_str_builder_hash(vert_head));
        *out_vert_builder = vert_head;

        if (has_loc) {
            ADD(glsl, "layout(location=0) out vec4 out_color;\n");
        } else {
            ADD(glsl, "out vec4 out_color;\n");
        }
        break;
    }
    case PL_PASS_COMPUTE:
        ADD(glsl, "layout (local_size_x = %d, local_size_y = %d) in;\n",
            sh->group_size[0], sh->group_size[1]);
        break;
    case PL_PASS_INVALID:
    case PL_PASS_TYPE_COUNT:
        pl_unreachable();
    }

    // Set up the main shader body
    ADD_CAT(glsl, shader_body);
    ADD(glsl, "void main() {\n");

    pl_assert(sh->input == PL_SHADER_SIG_NONE);
    switch (pass_params->type) {
    case PL_PASS_RASTER:
        pl_assert(sh->output == PL_SHADER_SIG_COLOR);
        ADD(glsl, "out_color = "$"();\n", sh->name);
        break;
    case PL_PASS_COMPUTE:
        ADD(glsl, $"();\n", sh->name);
        break;
    case PL_PASS_INVALID:
    case PL_PASS_TYPE_COUNT:
        pl_unreachable();
    }

    ADD(glsl, "}");

    pl_hash_merge(&pass->signature, pl_str_builder_hash(glsl));
    *out_glsl_builder = glsl;
}

#undef ADD
#undef ADD_CAT

#define pass_age(pass) (dp->current_index - (pass)->last_index)

static int cmp_pass_age(const void *ptra, const void *ptrb)
{
    const struct pass *a = *(const struct pass **) ptra;
    const struct pass *b = *(const struct pass **) ptrb;
    return b->last_index - a->last_index;
}

static void garbage_collect_passes(pl_dispatch dp)
{
    if (dp->passes.num <= dp->max_passes)
        return;

    // Garbage collect oldest passes, starting at the middle
    qsort(dp->passes.elem, dp->passes.num, sizeof(struct pass *), cmp_pass_age);
    int idx = dp->passes.num / 2;
    while (idx < dp->passes.num && pass_age(dp->passes.elem[idx]) < MIN_AGE)
        idx++;

    for (int i = idx; i < dp->passes.num; i++)
        pass_destroy(dp, dp->passes.elem[i]);

    int num_evicted = dp->passes.num - idx;
    dp->passes.num = idx;

    if (num_evicted) {
        PL_DEBUG(dp, "Evicted %d passes from dispatch cache, consider "
                 "using more dynamic shaders", num_evicted);
    } else {
        dp->max_passes *= 2;
    }
}

static struct pass *finalize_pass(pl_dispatch dp, pl_shader sh,
                                  pl_tex target, int vert_idx,
                                  const struct pl_blend_params *blend, bool load,
                                  const struct pl_dispatch_vertex_params *vparams,
                                  const pl_transform2x2 *proj)
{
    struct pass *pass = pl_alloc_ptr(dp, pass);
    *pass = (struct pass) {
        .signature = 0x0, // updated incrementally below
        .last_index = dp->current_index,
        .ubo_desc = {
            .desc = {
                .name = sh_ident_pack(sh_fresh(sh, "UBO")),
                .type = PL_DESC_BUF_UNIFORM,
            },
        },
    };

    // For identifiers tied to the lifetime of this shader
    void *tmp = sh->tmp;

    struct pl_pass_params params = {
        .type = pl_shader_is_compute(sh) ? PL_PASS_COMPUTE : PL_PASS_RASTER,
        .num_descriptors = sh->descs.num,
        .vertex_type = vparams ? vparams->vertex_type : PL_PRIM_TRIANGLE_STRIP,
        .vertex_stride = vparams ? vparams->vertex_stride : 0,
        .blend_params = blend,
    };

    struct generate_params gen_params = {
        .tmp = tmp,
        .pass = pass,
        .pass_params = &params,
        .sh = sh,
        .vert_idx = vert_idx,
    };

    if (params.type == PL_PASS_RASTER) {
        assert(target);
        params.target_format = target->params.format;
        params.load_target = load;

        // Fill in the vertex attributes array
        params.num_vertex_attribs = sh->vas.num;
        params.vertex_attribs = pl_calloc_ptr(tmp, sh->vas.num, params.vertex_attribs);

        int va_loc = 0;
        for (int i = 0; i < sh->vas.num; i++) {
            struct pl_vertex_attrib *va = &params.vertex_attribs[i];
            *va = sh->vas.elem[i].attr;

            // Mangle the name to make sure it doesn't conflict with the
            // fragment shader input, this will be converted back to a legal
            // string by the shader compilation code
            va->name = sh_ident_pack(sh_fresh(sh, "va"));

            // Place the vertex attribute
            va->location = va_loc;
            if (!vparams) {
                va->offset = params.vertex_stride;
                params.vertex_stride += va->fmt->texel_size;
            }

            // The number of vertex attribute locations consumed by a vertex
            // attribute is the number of vec4s it consumes, rounded up
            const size_t va_loc_size = sizeof(float[4]);
            va_loc += PL_DIV_UP(va->fmt->texel_size, va_loc_size);
        }

        // Hash in the raster state configuration
        pl_hash_merge(&pass->signature, (uint64_t) params.vertex_type);
        pl_hash_merge(&pass->signature, (uint64_t) params.vertex_stride);
        pl_hash_merge(&pass->signature, (uint64_t) params.load_target);
        pl_hash_merge(&pass->signature, target->params.format->signature);
        if (blend) {
            pl_static_assert(sizeof(*blend) == sizeof(enum pl_blend_mode) * 4);
            pl_hash_merge(&pass->signature, pl_var_hash(*blend));
        }

        // Load projection matrix if required
        if (proj && memcmp(&proj->mat, &pl_matrix2x2_identity, sizeof(proj->mat)) != 0) {
            gen_params.out_mat = sh_var(sh, (struct pl_shader_var) {
                .var = pl_var_mat2("proj"),
                .data = PL_TRANSPOSE_2X2(proj->mat.m),
            });
        }

        if (proj && (proj->c[0] || proj->c[1])) {
            gen_params.out_off = sh_var(sh, (struct pl_shader_var) {
                .var = pl_var_vec2("offset"),
                .data = proj->c,
            });
        }
    }

    // Place all of the compile-time constants
    uint8_t *constant_data = NULL;
    if (sh->consts.num) {
        params.num_constants = sh->consts.num;
        params.constants = pl_alloc(tmp, sh->consts.num * sizeof(struct pl_constant));

        // Compute offsets
        size_t total_size = 0;
        uint32_t const_id = 0;
        for (int i = 0; i < sh->consts.num; i++) {
            params.constants[i] = (struct pl_constant) {
                .type = sh->consts.elem[i].type,
                .id = const_id++,
                .offset = total_size,
            };
            total_size += pl_var_type_size(sh->consts.elem[i].type);
        }

        // Write values into the constants buffer
        params.constant_data = constant_data = pl_alloc(pass, total_size);
        for (int i = 0; i < sh->consts.num; i++) {
            const struct pl_shader_const *sc = &sh->consts.elem[i];
            void *data = constant_data + params.constants[i].offset;
            memcpy(data, sc->data, pl_var_type_size(sc->type));
        }
    }

    // Place all the variables; these will dynamically end up in different
    // locations based on what the underlying GPU supports (UBOs, pushc, etc.)
    //
    // We go through the list twice, once to place stuff that we definitely
    // want inside PCs, and then a second time to opportunistically place the rest.
    pass->vars = pl_calloc_ptr(pass, sh->vars.num, pass->vars);
    for (int i = 0; i < sh->vars.num; i++) {
        if (!add_pass_var(dp, tmp, pass, &params, &sh->vars.elem[i], &pass->vars[i], false))
            goto error;
    }
    for (int i = 0; i < sh->vars.num; i++) {
        if (!add_pass_var(dp, tmp, pass, &params, &sh->vars.elem[i], &pass->vars[i], true))
            goto error;
    }

    // Now that we know the variable placement, finalize pushc/UBO sizes
    params.push_constants_size = PL_ALIGN2(params.push_constants_size, 4);
    size_t ubo_size = sh_buf_desc_size(&pass->ubo_desc);
    if (ubo_size) {
        pass->ubo_index = sh->descs.num;
        PL_ARRAY_APPEND(sh, sh->descs, pass->ubo_desc); // don't mangle names
    };

    // Place and fill in the descriptors
    const int num_descs = sh->descs.num;
    int binding[PL_DESC_TYPE_COUNT] = {0};
    params.num_descriptors = num_descs;
    params.descriptors = pl_calloc_ptr(tmp, num_descs, params.descriptors);
    for (int i = 0; i < num_descs; i++) {
        struct pl_desc *desc = &params.descriptors[i];
        *desc = sh->descs.elem[i].desc;
        desc->binding = binding[pl_desc_namespace(dp->gpu, desc->type)]++;
    }

    // Finalize the shader and look it up in the pass cache
    pl_str_builder vert_builder = NULL, glsl_builder = NULL;
    generate_shaders(dp, &gen_params, &vert_builder, &glsl_builder);
    for (int i = 0; i < dp->passes.num; i++) {
        struct pass *p = dp->passes.elem[i];
        if (p->signature != pass->signature)
            continue;

        // Found existing shader, re-use directly
        if (p->ubo)
            sh->descs.elem[p->ubo_index].binding.object = p->ubo;
        pl_free(p->run_params.constant_data);
        p->run_params.constant_data = pl_steal(p, constant_data);
        p->last_index = dp->current_index;
        pl_free(pass);
        return p;
    }

    // Need to compile new shader, execute templates now
    if (vert_builder) {
        pl_str vert = pl_str_builder_exec(vert_builder);
        params.vertex_shader = (char *) vert.buf;
    }
    pl_str glsl = pl_str_builder_exec(glsl_builder);
    params.glsl_shader = (char *) glsl.buf;

    // Turn all shader identifiers into actual strings before passing it
    // to the `pl_gpu`
#define FIX_IDENT(name) \
    name = sh_ident_tostr(sh_ident_unpack(name))
    for (int i = 0; i < params.num_variables; i++)
        FIX_IDENT(params.variables[i].name);
    for (int i = 0; i < params.num_descriptors; i++)
        FIX_IDENT(params.descriptors[i].name);
    for (int i = 0; i < params.num_vertex_attribs; i++)
        FIX_IDENT(params.vertex_attribs[i].name);
#undef FIX_IDENT

    pass->pass = pl_pass_create(dp->gpu, &params);
    if (!pass->pass) {
        PL_ERR(dp, "Failed creating render pass for dispatch");
        // Add it anyway
    }

    struct pl_pass_run_params *rparams = &pass->run_params;
    rparams->pass = pass->pass;
    rparams->constant_data = constant_data;
    rparams->push_constants = pl_zalloc(pass, params.push_constants_size);
    rparams->desc_bindings = pl_calloc_ptr(pass, params.num_descriptors,
                                           rparams->desc_bindings);

    if (ubo_size && pass->pass) {
        // Create the UBO
        pass->ubo = pl_buf_create(dp->gpu, pl_buf_params(
            .size = ubo_size,
            .uniform = true,
            .host_writable = true,
        ));

        if (!pass->ubo) {
            PL_ERR(dp, "Failed creating uniform buffer for dispatch");
            goto error;
        }

        sh->descs.elem[pass->ubo_index].binding.object = pass->ubo;
    }

    if (params.type == PL_PASS_RASTER && !vparams) {
        // Generate the vertex array placeholder
        rparams->vertex_count = 4; // single quad
        size_t vert_size = rparams->vertex_count * params.vertex_stride;
        rparams->vertex_data = pl_zalloc(pass, vert_size);
    }

    pass->timer = pl_timer_create(dp->gpu);

    PL_ARRAY_APPEND(dp, dp->passes, pass);
    return pass;

error:
    pass_destroy(dp, pass);
    return NULL;
}

static void update_pass_var(pl_dispatch dp, struct pass *pass,
                            const struct pl_shader_var *sv, struct pass_var *pv)
{
    struct pl_var_layout host_layout = pl_var_host_layout(0, &sv->var);
    pl_assert(host_layout.size);

    // Use the cache to skip updates if possible
    if (pv->cached_data && !memcmp(sv->data, pv->cached_data, host_layout.size))
        return;
    if (!pv->cached_data)
        pv->cached_data = pl_alloc(pass, host_layout.size);
    memcpy(pv->cached_data, sv->data, host_layout.size);

    struct pl_pass_run_params *rparams = &pass->run_params;
    switch (pv->type) {
    case PASS_VAR_NONE:
        pl_unreachable();
    case PASS_VAR_GLOBAL: {
        struct pl_var_update vu = {
            .index = pv->index,
            .data  = sv->data,
        };
        PL_ARRAY_APPEND_RAW(pass, rparams->var_updates, rparams->num_var_updates, vu);
        break;
    }
    case PASS_VAR_UBO: {
        pl_assert(pass->ubo);
        const size_t offset = pv->layout.offset;
        if (host_layout.stride == pv->layout.stride) {
            pl_assert(host_layout.size == pv->layout.size);
            pl_buf_write(dp->gpu, pass->ubo, offset, sv->data, host_layout.size);
        } else {
            // Coalesce strided UBO write into a single pl_buf_write to avoid
            // unnecessary synchronization overhead by assembling the correctly
            // strided upload in RAM
            pl_grow(dp, &dp->ubo_tmp, pv->layout.size);
            uint8_t * const tmp = dp->ubo_tmp;
            const uint8_t *src = sv->data;
            const uint8_t *end = src + host_layout.size;
            uint8_t *dst = tmp;
            while (src < end) {
                memcpy(dst, src, host_layout.stride);
                src += host_layout.stride;
                dst += pv->layout.stride;
            }
            pl_buf_write(dp->gpu, pass->ubo, offset, tmp, pv->layout.size);
        }
        break;
    }
    case PASS_VAR_PUSHC:
        pl_assert(rparams->push_constants);
        memcpy_layout(rparams->push_constants, pv->layout, sv->data, host_layout);
        break;
    };
}

static void compute_vertex_attribs(pl_dispatch dp, pl_shader sh,
                                   int width, int height, ident_t *out_scale)
{
    // Simulate vertex attributes using global definitions
    *out_scale = sh_var(sh, (struct pl_shader_var) {
        .var     = pl_var_vec2("out_scale"),
        .data    = &(float[2]){ 1.0 / width, 1.0 / height },
        .dynamic = true,
    });

    GLSLP("#define frag_pos(id) (vec2(id) + vec2(0.5))  \n"
          "#define frag_map(id) ("$" * frag_pos(id))    \n"
          "#define gl_FragCoord vec4(frag_pos(gl_GlobalInvocationID), 0.0, 1.0) \n",
          *out_scale);

    for (int n = 0; n < sh->vas.num; n++) {
        const struct pl_shader_va *sva = &sh->vas.elem[n];

        ident_t points[4];
        for (int i = 0; i < PL_ARRAY_SIZE(points); i++) {
            points[i] = sh_var(sh, (struct pl_shader_var) {
                .var  = pl_var_from_fmt(sva->attr.fmt, "pt"),
                .data = sva->data[i],
            });
        }

        GLSLP("#define "$"_map(id) "
             "(mix(mix("$", "$", frag_map(id).x), "
             "     mix("$", "$", frag_map(id).x), "
             "frag_map(id).y)) \n"
             "#define "$" ("$"_map(gl_GlobalInvocationID)) \n",
             sh_ident_unpack(sva->attr.name),
             points[0], points[1], points[2], points[3],
             sh_ident_unpack(sva->attr.name),
             sh_ident_unpack(sva->attr.name));
    }
}

static const char *map_blend_mode(enum pl_blend_mode mode)
{
    switch (mode) {
    case PL_BLEND_ZERO:                return "0.0";
    case PL_BLEND_ONE:                 return "1.0";
    case PL_BLEND_SRC_ALPHA:           return "color.a";
    case PL_BLEND_ONE_MINUS_SRC_ALPHA: return "(1.0 - color.a)";
    case PL_BLEND_MODE_COUNT: break;
    }

    pl_unreachable();
}

static void translate_compute_shader(pl_dispatch dp, pl_shader sh,
                                     const pl_rect2d *rc,
                                     const struct pl_dispatch_params *params)
{
    int width = abs(pl_rect_w(*rc)), height = abs(pl_rect_h(*rc));
    if (sh->transpose)
        PL_SWAP(width, height);
    ident_t out_scale;
    compute_vertex_attribs(dp, sh, width, height, &out_scale);

    // Simulate a framebuffer using storage images
    pl_assert(params->target->params.storable);
    pl_assert(sh->output == PL_SHADER_SIG_COLOR);
    ident_t fbo = sh_desc(sh, (struct pl_shader_desc) {
        .binding.object = params->target,
        .desc = {
            .name    = "out_image",
            .type    = PL_DESC_STORAGE_IMG,
            .access  = params->blend_params ? PL_DESC_ACCESS_READWRITE
                                            : PL_DESC_ACCESS_WRITEONLY,
        },
    });

    ident_t base = sh_var(sh, (struct pl_shader_var) {
        .data    = &(int[2]){
            // When dealing with flipped coordinates, we need to subtract one
            // from the effective sample position; this is because instead of
            // IDs normally spanning from [0, size - 1], the inverted math
            // would end up with [size, 1] instead.
            //
            // Put another way, because we are dealing with integer coordinates,
            // the calculated position is rounded towards zero - but when the
            // coordinate system is flipped, that ends up rounding up instead
            // of down.
            rc->x0 - (rc->x0 > rc->x1),
            rc->y0 - (rc->y0 > rc->y1),
        },
        .dynamic = true,
        .var     = {
            .name  = "base",
            .type  = PL_VAR_SINT,
            .dim_v = 2,
            .dim_m = 1,
            .dim_a = 1,
        },
    });

    int dx = rc->x0 > rc->x1 ? -1 : 1, dy = rc->y0 > rc->y1 ? -1 : 1;
    GLSL("ivec2 dir = ivec2(%d, %d);\n", dx, dy); // hard-code, not worth var
    GLSL("ivec2 pos = "$" + dir * ivec2(gl_GlobalInvocationID).%c%c;\n",
         base, sh->transpose ? 'y' : 'x', sh->transpose ? 'x' : 'y');
    GLSL("vec2 fpos = "$" * vec2(gl_GlobalInvocationID);\n", out_scale);
    GLSL("if (fpos.x < 1.0 && fpos.y < 1.0) {\n");
    if (params->blend_params) {
        GLSL("vec4 orig = imageLoad("$", pos);\n", fbo);
        GLSL("color = vec4(color.rgb * vec3(%s), color.a * %s) \n"
             "      + vec4(orig.rgb  * vec3(%s), orig.a  * %s);\n",
             map_blend_mode(params->blend_params->src_rgb),
             map_blend_mode(params->blend_params->src_alpha),
             map_blend_mode(params->blend_params->dst_rgb),
             map_blend_mode(params->blend_params->dst_alpha));
    }
    GLSL("imageStore("$", pos, color);\n", fbo);
    GLSL("}\n");
    sh->output = PL_SHADER_SIG_NONE;
}

static void run_pass(pl_dispatch dp, pl_shader sh, struct pass *pass)
{
    pl_shader_info shader = &sh->info->info;
    pl_pass_run(dp->gpu, &pass->run_params);

    for (uint64_t ts; (ts = pl_timer_query(dp->gpu, pass->timer));) {
        PL_TRACE(dp, "Spent %.3f ms on shader: %s", ts / 1e6, shader->description);

        uint64_t old = pass->samples[pass->ts_idx];
        pass->samples[pass->ts_idx] = ts;
        pass->ts_last = ts;
        pass->ts_peak = PL_MAX(pass->ts_peak, ts);
        pass->ts_sum += ts;
        pass->ts_idx = (pass->ts_idx + 1) % PL_ARRAY_SIZE(pass->samples);

        if (old) {
            pass->ts_sum -= old;
            if (old == pass->ts_peak) {
                uint64_t new_peak = 0;
                for (int i = 0; i < PL_ARRAY_SIZE(pass->samples); i++)
                    new_peak = PL_MAX(new_peak, pass->samples[i]);
                pass->ts_peak = new_peak;
            }
        }
    }

    if (!dp->info_callback)
        return;

    struct pl_dispatch_info info;
    info.signature = pass->signature;
    info.shader = shader;

    // Test to see if the ring buffer already wrapped around once
    if (pass->samples[pass->ts_idx]) {
        info.num_samples = PL_ARRAY_SIZE(pass->samples);
        int num_wrapped = info.num_samples - pass->ts_idx;
        memcpy(info.samples, &pass->samples[pass->ts_idx],
               num_wrapped * sizeof(info.samples[0]));
        memcpy(&info.samples[num_wrapped], pass->samples,
               pass->ts_idx * sizeof(info.samples[0]));
    } else {
        info.num_samples = pass->ts_idx;
        memcpy(info.samples, pass->samples,
               pass->ts_idx * sizeof(info.samples[0]));
    }

    info.last = pass->ts_last;
    info.peak = pass->ts_peak;
    info.average = pass->ts_sum / PL_MAX(info.num_samples, 1);
    dp->info_callback(dp->info_priv, &info);
}

bool pl_dispatch_finish(pl_dispatch dp, const struct pl_dispatch_params *params)
{
    pl_shader sh = *params->shader;
    bool ret = false;
    pl_mutex_lock(&dp->lock);

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (sh->input != PL_SHADER_SIG_NONE || sh->output != PL_SHADER_SIG_COLOR) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    const struct pl_tex_params *tpars = &params->target->params;
    if (pl_tex_params_dimension(*tpars) != 2 || !tpars->renderable) {
        PL_ERR(dp, "Trying to dispatch a shader using an invalid target "
               "texture. The target must be a renderable 2D texture.");
        goto error;
    }

    const struct pl_gpu_limits *limits = &dp->gpu->limits;
    bool can_compute = tpars->storable;
    if (can_compute && params->blend_params)
        can_compute = tpars->format->caps & PL_FMT_CAP_READWRITE;

    if (pl_shader_is_compute(sh) && !can_compute) {
        PL_ERR(dp, "Trying to dispatch using a compute shader with a "
               "non-storable or incompatible target texture.");
        goto error;
    } else if (can_compute && limits->compute_queues > limits->fragment_queues) {
        if (sh_try_compute(sh, 16, 16, true, 0))
            PL_TRACE(dp, "Upgrading fragment shader to compute shader.");
    }

    pl_rect2d rc = params->rect;
    if (!pl_rect_w(rc)) {
        rc.x0 = 0;
        rc.x1 = tpars->w;
    }
    if (!pl_rect_h(rc)) {
        rc.y0 = 0;
        rc.y1 = tpars->h;
    }

    int w, h, tw = abs(pl_rect_w(rc)), th = abs(pl_rect_h(rc));
    if (pl_shader_output_size(sh, &w, &h) && (w != tw || h != th))
    {
        PL_ERR(dp, "Trying to dispatch a shader with explicit output size "
               "requirements %dx%d%s using a target rect of size %dx%d.",
               w, h, sh->transpose ? " (transposed)" : "", tw, th);
        goto error;
    }

    int vert_idx = -1;
    const pl_transform2x2 *proj = NULL;
    if (pl_shader_is_compute(sh)) {
        // Translate the compute shader to simulate vertices etc.
        translate_compute_shader(dp, sh, &rc, params);
    } else {
        // Add the vertex information encoding the position
        pl_rect2df vert_rect = {
            .x0 = 2.0 * rc.x0 / tpars->w - 1.0,
            .y0 = 2.0 * rc.y0 / tpars->h - 1.0,
            .x1 = 2.0 * rc.x1 / tpars->w - 1.0,
            .y1 = 2.0 * rc.y1 / tpars->h - 1.0,
        };

        if (sh->transpose) {
            static const pl_transform2x2 transpose_proj = {{{
                { 0, 1 },
                { 1, 0 },
            }}};
            proj = &transpose_proj;
            PL_SWAP(vert_rect.x0, vert_rect.y0);
            PL_SWAP(vert_rect.x1, vert_rect.y1);
        }

        sh_attr_vec2(sh, "position", &vert_rect);
        vert_idx = sh->vas.num - 1;
    }

    // We need to set pl_pass_params.load_target when either blending is
    // enabled or we're drawing to some scissored sub-rect of the texture
    pl_rect2d full = { 0, 0, tpars->w, tpars->h };
    pl_rect2d rc_norm = rc;
    pl_rect2d_normalize(&rc_norm);
    rc_norm.x0 = PL_MAX(rc_norm.x0, 0);
    rc_norm.y0 = PL_MAX(rc_norm.y0, 0);
    rc_norm.x1 = PL_MIN(rc_norm.x1, tpars->w);
    rc_norm.y1 = PL_MIN(rc_norm.y1, tpars->h);
    bool load = params->blend_params || !pl_rect2d_eq(rc_norm, full);

    struct pass *pass = finalize_pass(dp, sh, params->target, vert_idx,
                                      params->blend_params, load, NULL, proj);

    // Silently return on failed passes
    if (!pass || !pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sh->descs.elem[i].binding;

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the vertex data
    if (rparams->vertex_data) {
        uintptr_t vert_base = (uintptr_t) rparams->vertex_data;
        size_t stride = rparams->pass->params.vertex_stride;
        for (int i = 0; i < sh->vas.num; i++) {
            const struct pl_shader_va *sva = &sh->vas.elem[i];
            struct pl_vertex_attrib *va = &rparams->pass->params.vertex_attribs[i];

            size_t size = sva->attr.fmt->texel_size;
            uintptr_t va_base = vert_base + va->offset; // use placed offset
            for (int n = 0; n < 4; n++)
                memcpy((void *) (va_base + n * stride), sva->data[n], size);
        }
    }

    // For compute shaders: also update the dispatch dimensions
    if (pl_shader_is_compute(sh)) {
        int width = abs(pl_rect_w(rc)),
            height = abs(pl_rect_h(rc));
        if (sh->transpose)
            PL_SWAP(width, height);
        // Round up to make sure we don't leave off a part of the target
        int block_w = sh->group_size[0],
            block_h = sh->group_size[1],
            num_x   = PL_DIV_UP(width, block_w),
            num_y   = PL_DIV_UP(height, block_h);

        rparams->compute_groups[0] = num_x;
        rparams->compute_groups[1] = num_y;
        rparams->compute_groups[2] = 1;
    } else {
        // Update the scissors for performance
        rparams->scissors = rc_norm;
    }

    // Dispatch the actual shader
    rparams->target = params->target;
    rparams->timer = PL_DEF(params->timer, pass->timer);
    run_pass(dp, sh, pass);

    ret = true;
    // fall through

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        pl_str_builder_reset(dp->tmp[i]);

    pl_mutex_unlock(&dp->lock);
    pl_dispatch_abort(dp, params->shader);
    return ret;
}

bool pl_dispatch_compute(pl_dispatch dp, const struct pl_dispatch_compute_params *params)
{
    pl_shader sh = *params->shader;
    bool ret = false;
    pl_mutex_lock(&dp->lock);

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (sh->input != PL_SHADER_SIG_NONE) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    if (!pl_shader_is_compute(sh)) {
        PL_ERR(dp, "Trying to dispatch a non-compute shader using "
               "`pl_dispatch_compute`!");
        goto error;
    }

    if (sh->vas.num) {
        if (!params->width || !params->height) {
            PL_ERR(dp, "Trying to dispatch a targetless compute shader that "
                   "uses vertex attributes, this requires specifying the size "
                   "of the effective rendering area!");
            goto error;
        }

        compute_vertex_attribs(dp, sh, params->width, params->height,
                               &(ident_t){0});
    }

    struct pass *pass = finalize_pass(dp, sh, NULL, -1, NULL, false, NULL, NULL);

    // Silently return on failed passes
    if (!pass || !pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sh->descs.elem[i].binding;

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the dispatch size
    int groups = 1;
    for (int i = 0; i < 3; i++) {
        groups *= params->dispatch_size[i];
        rparams->compute_groups[i] = params->dispatch_size[i];
    }

    if (!groups) {
        pl_assert(params->width && params->height);
        int block_w = sh->group_size[0],
            block_h = sh->group_size[1],
            num_x   = PL_DIV_UP(params->width, block_w),
            num_y   = PL_DIV_UP(params->height, block_h);

        rparams->compute_groups[0] = num_x;
        rparams->compute_groups[1] = num_y;
        rparams->compute_groups[2] = 1;
    }

    // Dispatch the actual shader
    rparams->timer = PL_DEF(params->timer, pass->timer);
    run_pass(dp, sh, pass);

    ret = true;
    // fall through

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        pl_str_builder_reset(dp->tmp[i]);

    pl_mutex_unlock(&dp->lock);
    pl_dispatch_abort(dp, params->shader);
    return ret;
}

bool pl_dispatch_vertex(pl_dispatch dp, const struct pl_dispatch_vertex_params *params)
{
    pl_shader sh = *params->shader;
    bool ret = false;
    pl_mutex_lock(&dp->lock);

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (sh->input != PL_SHADER_SIG_NONE || sh->output != PL_SHADER_SIG_COLOR) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    const struct pl_tex_params *tpars = &params->target->params;
    if (pl_tex_params_dimension(*tpars) != 2 || !tpars->renderable) {
        PL_ERR(dp, "Trying to dispatch a shader using an invalid target "
               "texture. The target must be a renderable 2D texture.");
        goto error;
    }

    if (pl_shader_is_compute(sh)) {
        PL_ERR(dp, "Trying to dispatch a compute shader using pl_dispatch_vertex.");
        goto error;
    }

    if (sh->vas.num) {
        PL_ERR(dp, "Trying to dispatch a custom vertex shader with already "
               "attached vertex attributes.");
        goto error;
    }

    if (sh->transpose) {
        PL_ERR(dp, "Trying to dispatch a transposed shader using "
               "pl_dispatch_vertex, unlikely to be correct. Erroring as a "
               "safety precaution!");
        goto error;
    }

    int pos_idx = params->vertex_position_idx;
    if (pos_idx < 0 || pos_idx >= params->num_vertex_attribs) {
        PL_ERR(dp, "Vertex position index out of range?");
        goto error;
    }

    // Attach all of the vertex attributes to the shader manually
    sh->vas.num = params->num_vertex_attribs;
    PL_ARRAY_RESIZE(sh, sh->vas, sh->vas.num);
    for (int i = 0; i < params->num_vertex_attribs; i++) {
        ident_t id = sh_fresh(sh, params->vertex_attribs[i].name);
        sh->vas.elem[i].attr = params->vertex_attribs[i];
        sh->vas.elem[i].attr.name = sh_ident_pack(id);
        GLSLP("#define %s "$"\n", params->vertex_attribs[i].name, id);
    }

    // Compute the coordinate projection matrix
    pl_transform2x2 proj = pl_transform2x2_identity;
    switch (params->vertex_coords) {
    case PL_COORDS_ABSOLUTE:
        proj.mat.m[0][0] /= tpars->w;
        proj.mat.m[1][1] /= tpars->h;
        // fall through
    case PL_COORDS_RELATIVE:
        proj.mat.m[0][0] *= 2.0;
        proj.mat.m[1][1] *= 2.0;
        proj.c[0] -= 1.0;
        proj.c[1] -= 1.0;
        // fall through
    case PL_COORDS_NORMALIZED:
        if (params->vertex_flipped) {
            proj.mat.m[1][1] = -proj.mat.m[1][1];
            proj.c[1] += 2.0;
        }
        break;
    }

    struct pass *pass = finalize_pass(dp, sh, params->target, pos_idx,
                                      params->blend_params, true, params, &proj);

    // Silently return on failed passes
    if (!pass || !pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sh->descs.elem[i].binding;

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the scissors
    rparams->scissors = params->scissors;
    if (params->vertex_flipped) {
        rparams->scissors.y0 = tpars->h - rparams->scissors.y0;
        rparams->scissors.y1 = tpars->h - rparams->scissors.y1;
    }
    pl_rect2d_normalize(&rparams->scissors);

    // Dispatch the actual shader
    rparams->target = params->target;
    rparams->vertex_count = params->vertex_count;
    rparams->vertex_data = params->vertex_data;
    rparams->vertex_buf = params->vertex_buf;
    rparams->buf_offset = params->buf_offset;
    rparams->index_data = params->index_data;
    rparams->index_fmt = params->index_fmt;
    rparams->index_buf = params->index_buf;
    rparams->index_offset = params->index_offset;
    rparams->timer = PL_DEF(params->timer, pass->timer);
    run_pass(dp, sh, pass);

    ret = true;
    // fall through

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        pl_str_builder_reset(dp->tmp[i]);

    pl_mutex_unlock(&dp->lock);
    pl_dispatch_abort(dp, params->shader);
    return ret;
}

void pl_dispatch_abort(pl_dispatch dp, pl_shader *psh)
{
    pl_shader sh = *psh;
    if (!sh)
        return;

    // Free unused memory as early as possible
    sh_deref(sh);

    // Re-add the shader to the internal pool of shaders
    pl_mutex_lock(&dp->lock);
    PL_ARRAY_APPEND(dp, dp->shaders, sh);
    pl_mutex_unlock(&dp->lock);
    *psh = NULL;
}

void pl_dispatch_reset_frame(pl_dispatch dp)
{
    pl_mutex_lock(&dp->lock);

    dp->current_ident = 0;
    dp->current_index++;
    garbage_collect_passes(dp);

    pl_mutex_unlock(&dp->lock);
}

size_t pl_dispatch_save(pl_dispatch dp, uint8_t *out)
{
    return pl_cache_save(pl_gpu_cache(dp->gpu), out, out ? SIZE_MAX : 0);
}

void pl_dispatch_load(pl_dispatch dp, const uint8_t *cache)
{
    pl_cache_load(pl_gpu_cache(dp->gpu), cache, SIZE_MAX);
}
