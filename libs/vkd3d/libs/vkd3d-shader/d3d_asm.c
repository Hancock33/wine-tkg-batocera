/*
 * Copyright 2002-2003 Jason Edmeades
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2004 Christian Costa
 * Copyright 2005 Oliver Stieber
 * Copyright 2006 Ivan Gyurdiev
 * Copyright 2007-2008, 2013 Stefan Dösinger for CodeWeavers
 * Copyright 2009-2011 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "vkd3d_shader_private.h"

#include <stdio.h>
#include <math.h>

static const char * const shader_register_names[] =
{
    [VKD3DSPR_ADDR              ] = "a",
    [VKD3DSPR_ATTROUT           ] = "oD",
    [VKD3DSPR_COLOROUT          ] = "oC",
    [VKD3DSPR_COMBINED_SAMPLER  ] = "s",
    [VKD3DSPR_CONST             ] = "c",
    [VKD3DSPR_CONSTBOOL         ] = "b",
    [VKD3DSPR_CONSTBUFFER       ] = "cb",
    [VKD3DSPR_CONSTINT          ] = "i",
    [VKD3DSPR_COVERAGE          ] = "vCoverage",
    [VKD3DSPR_DEPTHOUT          ] = "oDepth",
    [VKD3DSPR_DEPTHOUTGE        ] = "oDepthGE",
    [VKD3DSPR_DEPTHOUTLE        ] = "oDepthLE",
    [VKD3DSPR_FORKINSTID        ] = "vForkInstanceId",
    [VKD3DSPR_FUNCTIONBODY      ] = "fb",
    [VKD3DSPR_FUNCTIONPOINTER   ] = "fp",
    [VKD3DSPR_GROUPSHAREDMEM    ] = "g",
    [VKD3DSPR_GSINSTID          ] = "vGSInstanceID",
    [VKD3DSPR_IDXTEMP           ] = "x",
    [VKD3DSPR_IMMCONST          ] = "l",
    [VKD3DSPR_IMMCONST64        ] = "d",
    [VKD3DSPR_IMMCONSTBUFFER    ] = "icb",
    [VKD3DSPR_INCONTROLPOINT    ] = "vicp",
    [VKD3DSPR_INPUT             ] = "v",
    [VKD3DSPR_JOININSTID        ] = "vJoinInstanceId",
    [VKD3DSPR_LABEL             ] = "l",
    [VKD3DSPR_LOCALTHREADID     ] = "vThreadIDInGroup",
    [VKD3DSPR_LOCALTHREADINDEX  ] = "vThreadIDInGroupFlattened",
    [VKD3DSPR_LOOP              ] = "aL",
    [VKD3DSPR_NULL              ] = "null",
    [VKD3DSPR_OUTCONTROLPOINT   ] = "vocp",
    [VKD3DSPR_OUTPOINTID        ] = "vOutputControlPointID",
    [VKD3DSPR_OUTPUT            ] = "o",
    [VKD3DSPR_OUTSTENCILREF     ] = "oStencilRef",
    [VKD3DSPR_PARAMETER         ] = "parameter",
    [VKD3DSPR_PATCHCONST        ] = "vpc",
    [VKD3DSPR_POINT_COORD       ] = "vPointCoord",
    [VKD3DSPR_PREDICATE         ] = "p",
    [VKD3DSPR_PRIMID            ] = "primID",
    [VKD3DSPR_RASTERIZER        ] = "rasterizer",
    [VKD3DSPR_RESOURCE          ] = "t",
    [VKD3DSPR_SAMPLEMASK        ] = "oMask",
    [VKD3DSPR_SAMPLER           ] = "s",
    [VKD3DSPR_SSA               ] = "sr",
    [VKD3DSPR_STREAM            ] = "m",
    [VKD3DSPR_TEMP              ] = "r",
    [VKD3DSPR_TESSCOORD         ] = "vDomainLocation",
    [VKD3DSPR_TEXCRDOUT         ] = "oT",
    [VKD3DSPR_TEXTURE           ] = "t",
    [VKD3DSPR_THREADGROUPID     ] = "vThreadGroupID",
    [VKD3DSPR_THREADID          ] = "vThreadID",
    [VKD3DSPR_UAV               ] = "u",
    [VKD3DSPR_UNDEF             ] = "undef",
    [VKD3DSPR_WAVELANECOUNT     ] = "vWaveLaneCount",
    [VKD3DSPR_WAVELANEINDEX     ] = "vWaveLaneIndex",
};

struct vkd3d_d3d_asm_colours
{
    const char *reset;
    const char *error;
    const char *literal;
    const char *modifier;
    const char *opcode;
    const char *reg;
    const char *swizzle;
    const char *version;
    const char *write_mask;
    const char *label;
};

struct vkd3d_d3d_asm_compiler
{
    struct vkd3d_string_buffer buffer;
    struct vkd3d_shader_version shader_version;
    struct vkd3d_d3d_asm_colours colours;
    enum vsir_asm_flags flags;
    const struct vkd3d_shader_instruction *current;
};

static void shader_dump_global_flags(struct vkd3d_d3d_asm_compiler *compiler, enum vsir_global_flags global_flags)
{
    unsigned int i;

    static const struct
    {
        enum vsir_global_flags flag;
        const char *name;
    }
    global_flag_info[] =
    {
        {VKD3DSGF_REFACTORING_ALLOWED,               "refactoringAllowed"},
        {VKD3DSGF_FORCE_EARLY_DEPTH_STENCIL,         "forceEarlyDepthStencil"},
        {VKD3DSGF_ENABLE_RAW_AND_STRUCTURED_BUFFERS, "enableRawAndStructuredBuffers"},
        {VKD3DSGF_ENABLE_MINIMUM_PRECISION,          "enableMinimumPrecision"},
        {VKD3DSGF_SKIP_OPTIMIZATION,                 "skipOptimization"},
        {VKD3DSGF_ENABLE_DOUBLE_PRECISION_FLOAT_OPS, "enableDoublePrecisionFloatOps"},
        {VKD3DSGF_ENABLE_11_1_DOUBLE_EXTENSIONS,     "enable11_1DoubleExtensions"},
    };

    for (i = 0; i < ARRAY_SIZE(global_flag_info); ++i)
    {
        if (global_flags & global_flag_info[i].flag)
        {
            vkd3d_string_buffer_printf(&compiler->buffer, "%s", global_flag_info[i].name);
            global_flags &= ~global_flag_info[i].flag;
            if (global_flags)
                vkd3d_string_buffer_printf(&compiler->buffer, " | ");
        }
    }

    if (global_flags)
        vkd3d_string_buffer_printf(&compiler->buffer, "unknown_flags(%#"PRIx64")", (uint64_t)global_flags);
}

static void shader_dump_atomic_op_flags(struct vkd3d_d3d_asm_compiler *compiler, uint32_t atomic_flags)
{
    if (atomic_flags & VKD3DARF_SEQ_CST)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_seqCst");
        atomic_flags &= ~VKD3DARF_SEQ_CST;
    }
    if (atomic_flags & VKD3DARF_VOLATILE)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_volatile");
        atomic_flags &= ~VKD3DARF_VOLATILE;
    }

    if (atomic_flags)
        vkd3d_string_buffer_printf(&compiler->buffer, "_unknown_flags(%#x)", atomic_flags);
}

static void shader_dump_sync_flags(struct vkd3d_d3d_asm_compiler *compiler, uint32_t sync_flags)
{
    if (sync_flags & VKD3DSSF_GLOBAL_UAV)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_uglobal");
        sync_flags &= ~VKD3DSSF_GLOBAL_UAV;
    }
    if (sync_flags & VKD3DSSF_THREAD_GROUP_UAV)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_ugroup");
        sync_flags &= ~VKD3DSSF_THREAD_GROUP_UAV;
    }
    if (sync_flags & VKD3DSSF_GROUP_SHARED_MEMORY)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_g");
        sync_flags &= ~VKD3DSSF_GROUP_SHARED_MEMORY;
    }
    if (sync_flags & VKD3DSSF_THREAD_GROUP)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_t");
        sync_flags &= ~VKD3DSSF_THREAD_GROUP;
    }

    if (sync_flags)
        vkd3d_string_buffer_printf(&compiler->buffer, "_unknown_flags(%#x)", sync_flags);
}

static void shader_dump_precise_flags(struct vkd3d_d3d_asm_compiler *compiler, uint32_t flags)
{
    if (!(flags & VKD3DSI_PRECISE_XYZW))
        return;

    vkd3d_string_buffer_printf(&compiler->buffer, " [precise");
    if (flags != VKD3DSI_PRECISE_XYZW)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "(%s%s%s%s)",
                flags & VKD3DSI_PRECISE_X ? "x" : "",
                flags & VKD3DSI_PRECISE_Y ? "y" : "",
                flags & VKD3DSI_PRECISE_Z ? "z" : "",
                flags & VKD3DSI_PRECISE_W ? "w" : "");
    }
    vkd3d_string_buffer_printf(&compiler->buffer, "]");
}

static void shader_dump_uav_flags(struct vkd3d_d3d_asm_compiler *compiler, uint32_t uav_flags)
{
    if (uav_flags & VKD3DSUF_GLOBALLY_COHERENT)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_glc");
        uav_flags &= ~VKD3DSUF_GLOBALLY_COHERENT;
    }
    if (uav_flags & VKD3DSUF_ORDER_PRESERVING_COUNTER)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_opc");
        uav_flags &= ~VKD3DSUF_ORDER_PRESERVING_COUNTER;
    }
    if (uav_flags & VKD3DSUF_RASTERISER_ORDERED_VIEW)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "_rov");
        uav_flags &= ~VKD3DSUF_RASTERISER_ORDERED_VIEW;
    }

    if (uav_flags)
        vkd3d_string_buffer_printf(&compiler->buffer, "_unknown_flags(%#x)", uav_flags);
}

static void shader_print_tessellator_domain(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, enum vkd3d_tessellator_domain d, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *domain;

    switch (d)
    {
        case VKD3D_TESSELLATOR_DOMAIN_LINE:
            domain = "domain_isoline";
            break;
        case VKD3D_TESSELLATOR_DOMAIN_TRIANGLE:
            domain = "domain_tri";
            break;
        case VKD3D_TESSELLATOR_DOMAIN_QUAD:
            domain = "domain_quad";
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled tessellator domain %#x>%s%s",
                    prefix, compiler->colours.error, d, compiler->colours.reset, suffix);
            return;
    }

    vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, domain, suffix);
}

static void shader_print_tessellator_output_primitive(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, enum vkd3d_shader_tessellator_output_primitive p, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *primitive;

    switch (p)
    {
        case VKD3D_SHADER_TESSELLATOR_OUTPUT_POINT:
            primitive = "output_point";
            break;
        case VKD3D_SHADER_TESSELLATOR_OUTPUT_LINE:
            primitive = "output_line";
            break;
        case VKD3D_SHADER_TESSELLATOR_OUTPUT_TRIANGLE_CW:
            primitive = "output_triangle_cw";
            break;
        case VKD3D_SHADER_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
            primitive = "output_triangle_ccw";
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled tessellator output primitive %#x>%s%s",
                    prefix, compiler->colours.error, p, compiler->colours.reset, suffix);
            return;
    }

    vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, primitive, suffix);
}

static void shader_print_tessellator_partitioning(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, enum vkd3d_shader_tessellator_partitioning p, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *partitioning;

    switch (p)
    {
        case VKD3D_SHADER_TESSELLATOR_PARTITIONING_INTEGER:
            partitioning = "partitioning_integer";
            break;
        case VKD3D_SHADER_TESSELLATOR_PARTITIONING_POW2:
            partitioning = "partitioning_pow2";
            break;
        case VKD3D_SHADER_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
            partitioning = "partitioning_fractional_odd";
            break;
        case VKD3D_SHADER_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
            partitioning = "partitioning_fractional_even";
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled tessellator partitioning %#x>%s%s",
                    prefix, compiler->colours.error, p, compiler->colours.reset, suffix);
            return;
    }

    vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, partitioning, suffix);
}

static void shader_print_input_sysval_semantic(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, enum vkd3d_shader_input_sysval_semantic semantic, const char *suffix)
{
    unsigned int i;

    static const struct
    {
        enum vkd3d_shader_input_sysval_semantic sysval_semantic;
        const char *sysval_name;
    }
    shader_input_sysval_semantic_names[] =
    {
        {VKD3D_SIV_POSITION,                   "position"},
        {VKD3D_SIV_CLIP_DISTANCE,              "clip_distance"},
        {VKD3D_SIV_CULL_DISTANCE,              "cull_distance"},
        {VKD3D_SIV_RENDER_TARGET_ARRAY_INDEX,  "render_target_array_index"},
        {VKD3D_SIV_VIEWPORT_ARRAY_INDEX,       "viewport_array_index"},
        {VKD3D_SIV_VERTEX_ID,                  "vertex_id"},
        {VKD3D_SIV_INSTANCE_ID,                "instance_id"},
        {VKD3D_SIV_PRIMITIVE_ID,               "primitive_id"},
        {VKD3D_SIV_IS_FRONT_FACE,              "is_front_face"},
        {VKD3D_SIV_SAMPLE_INDEX,               "sample_index"},
        {VKD3D_SIV_QUAD_U0_TESS_FACTOR,        "finalQuadUeq0EdgeTessFactor"},
        {VKD3D_SIV_QUAD_V0_TESS_FACTOR,        "finalQuadVeq0EdgeTessFactor"},
        {VKD3D_SIV_QUAD_U1_TESS_FACTOR,        "finalQuadUeq1EdgeTessFactor"},
        {VKD3D_SIV_QUAD_V1_TESS_FACTOR,        "finalQuadVeq1EdgeTessFactor"},
        {VKD3D_SIV_QUAD_U_INNER_TESS_FACTOR,   "finalQuadUInsideTessFactor"},
        {VKD3D_SIV_QUAD_V_INNER_TESS_FACTOR,   "finalQuadVInsideTessFactor"},
        {VKD3D_SIV_TRIANGLE_U_TESS_FACTOR,     "finalTriUeq0EdgeTessFactor"},
        {VKD3D_SIV_TRIANGLE_V_TESS_FACTOR,     "finalTriVeq0EdgeTessFactor"},
        {VKD3D_SIV_TRIANGLE_W_TESS_FACTOR,     "finalTriWeq0EdgeTessFactor"},
        {VKD3D_SIV_TRIANGLE_INNER_TESS_FACTOR, "finalTriInsideTessFactor"},
        {VKD3D_SIV_LINE_DETAIL_TESS_FACTOR,    "finalLineDetailTessFactor"},
        {VKD3D_SIV_LINE_DENSITY_TESS_FACTOR,   "finalLineDensityTessFactor"},
    };

    for (i = 0; i < ARRAY_SIZE(shader_input_sysval_semantic_names); ++i)
    {
        if (shader_input_sysval_semantic_names[i].sysval_semantic != semantic)
            continue;

        vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%s",
                prefix, shader_input_sysval_semantic_names[i].sysval_name, suffix);
        return;
    }

    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s<unhandled input sysval semantic %#x>%s%s",
            prefix, compiler->colours.error, semantic, compiler->colours.reset, suffix);
}

static void shader_print_resource_type(struct vkd3d_d3d_asm_compiler *compiler, enum vkd3d_shader_resource_type type)
{
    static const char *const resource_type_names[] =
    {
        [VKD3D_SHADER_RESOURCE_NONE             ] = "none",
        [VKD3D_SHADER_RESOURCE_BUFFER           ] = "buffer",
        [VKD3D_SHADER_RESOURCE_TEXTURE_1D       ] = "texture1d",
        [VKD3D_SHADER_RESOURCE_TEXTURE_2D       ] = "texture2d",
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DMS     ] = "texture2dms",
        [VKD3D_SHADER_RESOURCE_TEXTURE_3D       ] = "texture3d",
        [VKD3D_SHADER_RESOURCE_TEXTURE_CUBE     ] = "texturecube",
        [VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY  ] = "texture1darray",
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY  ] = "texture2darray",
        [VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY] = "texture2dmsarray",
        [VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY] = "texturecubearray",
    };

    if (type < ARRAY_SIZE(resource_type_names))
        vkd3d_string_buffer_printf(&compiler->buffer, "%s", resource_type_names[type]);
    else
        vkd3d_string_buffer_printf(&compiler->buffer, "%s<unhandled resource type %#x>%s",
                compiler->colours.error, type, compiler->colours.reset);
}

static void shader_print_data_type(struct vkd3d_d3d_asm_compiler *compiler, enum vkd3d_data_type type)
{
    static const char *const data_type_names[] =
    {
        [VKD3D_DATA_FLOAT    ] = "float",
        [VKD3D_DATA_INT      ] = "int",
        [VKD3D_DATA_UINT     ] = "uint",
        [VKD3D_DATA_UNORM    ] = "unorm",
        [VKD3D_DATA_SNORM    ] = "snorm",
        [VKD3D_DATA_OPAQUE   ] = "opaque",
        [VKD3D_DATA_MIXED    ] = "mixed",
        [VKD3D_DATA_DOUBLE   ] = "double",
        [VKD3D_DATA_CONTINUED] = "<continued>",
        [VKD3D_DATA_UNUSED   ] = "<unused>",
        [VKD3D_DATA_UINT8    ] = "uint8",
        [VKD3D_DATA_UINT64   ] = "uint64",
        [VKD3D_DATA_BOOL     ] = "bool",
        [VKD3D_DATA_UINT16   ] = "uint16",
        [VKD3D_DATA_HALF     ] = "half",
    };

    if (type < ARRAY_SIZE(data_type_names))
        vkd3d_string_buffer_printf(&compiler->buffer, "%s", data_type_names[type]);
    else
        vkd3d_string_buffer_printf(&compiler->buffer, "%s<unhandled data type %#x>%s",
                compiler->colours.error, type, compiler->colours.reset);
}

static void shader_dump_resource_data_type(struct vkd3d_d3d_asm_compiler *compiler, const enum vkd3d_data_type *type)
{
    int i;

    vkd3d_string_buffer_printf(&compiler->buffer, "(");

    for (i = 0; i < 4; i++)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "%s", i == 0 ? "" : ",");
        shader_print_data_type(compiler, type[i]);
    }

    vkd3d_string_buffer_printf(&compiler->buffer, ")");
}

static void shader_print_dcl_usage(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_semantic *semantic, uint32_t flags, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int usage_idx;
    const char *usage;
    bool indexed;

    if (semantic->resource.reg.reg.type == VKD3DSPR_COMBINED_SAMPLER)
    {
        switch (semantic->resource_type)
        {
            case VKD3D_SHADER_RESOURCE_TEXTURE_2D:
                usage = "2d";
                break;
            case VKD3D_SHADER_RESOURCE_TEXTURE_3D:
                usage = "volume";
                break;
            case VKD3D_SHADER_RESOURCE_TEXTURE_CUBE:
                usage = "cube";
                break;
            default:
                vkd3d_string_buffer_printf(buffer, "%s%s<unhandled resource type %#x>%s%s",
                        prefix, compiler->colours.error, semantic->resource_type, compiler->colours.reset, suffix);
                return;
        }

        vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, usage, suffix);
        return;
    }

    if (semantic->resource.reg.reg.type == VKD3DSPR_RESOURCE || semantic->resource.reg.reg.type == VKD3DSPR_UAV)
    {
        vkd3d_string_buffer_printf(buffer, "%s", prefix);
        if (semantic->resource.reg.reg.type == VKD3DSPR_RESOURCE)
            vkd3d_string_buffer_printf(buffer, "resource_");

        shader_print_resource_type(compiler, semantic->resource_type);
        if (semantic->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
                || semantic->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
        {
            vkd3d_string_buffer_printf(buffer, "(%u)", semantic->sample_count);
        }
        if (semantic->resource.reg.reg.type == VKD3DSPR_UAV)
            shader_dump_uav_flags(compiler, flags);
        vkd3d_string_buffer_printf(buffer, " ");
        shader_dump_resource_data_type(compiler, semantic->resource_data_type);
        vkd3d_string_buffer_printf(buffer, "%s", suffix);
        return;
    }

    /* Pixel shaders 3.0 don't have usage semantics. */
    if (!vkd3d_shader_ver_ge(&compiler->shader_version, 3, 0)
            && compiler->shader_version.type == VKD3D_SHADER_TYPE_PIXEL)
        return;

    indexed = false;
    usage_idx = semantic->usage_idx;
    switch (semantic->usage)
    {
        case VKD3D_DECL_USAGE_POSITION:
            usage = "position";
            indexed = true;
            break;
        case VKD3D_DECL_USAGE_BLEND_INDICES:
            usage = "blend";
            break;
        case VKD3D_DECL_USAGE_BLEND_WEIGHT:
            usage = "weight";
            break;
        case VKD3D_DECL_USAGE_NORMAL:
            usage = "normal";
            indexed = true;
            break;
        case VKD3D_DECL_USAGE_PSIZE:
            usage = "psize";
            break;
        case VKD3D_DECL_USAGE_COLOR:
            if (semantic->usage_idx)
            {
                usage = "specular";
                indexed = true;
                --usage_idx;
                break;
            }
            usage = "color";
            break;
        case VKD3D_DECL_USAGE_TEXCOORD:
            usage = "texcoord";
            indexed = true;
            break;
        case VKD3D_DECL_USAGE_TANGENT:
            usage = "tangent";
            break;
        case VKD3D_DECL_USAGE_BINORMAL:
            usage = "binormal";
            break;
        case VKD3D_DECL_USAGE_TESS_FACTOR:
            usage = "tessfactor";
            break;
        case VKD3D_DECL_USAGE_POSITIONT:
            usage = "positiont";
            indexed = true;
            break;
        case VKD3D_DECL_USAGE_FOG:
            usage = "fog";
            break;
        case VKD3D_DECL_USAGE_DEPTH:
            usage = "depth";
            break;
        case VKD3D_DECL_USAGE_SAMPLE:
            usage = "sample";
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled usage %#x, index %u>%s%s",
                    prefix, compiler->colours.error, semantic->usage, usage_idx, compiler->colours.reset, suffix);
            return;
    }

    if (indexed)
        vkd3d_string_buffer_printf(buffer, "%s%s%u%s", prefix, usage, usage_idx, suffix);
    else
        vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, usage, suffix);
}

static void shader_print_src_param(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_src_param *param, const char *suffix);

static void shader_print_float_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, float f, const char *suffix)
{
    const char *sign = "";

    if (isfinite(f) && signbit(f))
    {
        sign = "-";
        f = -f;
    }

    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%s", prefix, sign, compiler->colours.literal);
    vkd3d_string_buffer_print_f32(&compiler->buffer, f);
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s", compiler->colours.reset, suffix);
}

static void shader_print_double_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, double d, const char *suffix)
{
    const char *sign = "";

    if (isfinite(d) && signbit(d))
    {
        sign = "-";
        d = -d;
    }

    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%s", prefix, sign, compiler->colours.literal);
    vkd3d_string_buffer_print_f64(&compiler->buffer, d);
    vkd3d_string_buffer_printf(&compiler->buffer, "l%s%s", compiler->colours.reset, suffix);
}

static void shader_print_int_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, int i, const char *suffix)
{
    /* Note that we need to handle INT_MIN here as well. */
    if (i < 0)
        vkd3d_string_buffer_printf(&compiler->buffer, "%s-%s%u%s%s",
                prefix, compiler->colours.literal, -(unsigned int)i, compiler->colours.reset, suffix);
    else
        vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%d%s%s",
                prefix, compiler->colours.literal, i, compiler->colours.reset, suffix);
}

static void shader_print_uint_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, unsigned int i, const char *suffix)
{
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%u%s%s",
            prefix, compiler->colours.literal, i, compiler->colours.reset, suffix);
}

static void shader_print_uint64_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, uint64_t i, const char *suffix)
{
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%"PRIu64"%s%s",
            prefix, compiler->colours.literal, i, compiler->colours.reset, suffix);
}

static void shader_print_hex_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, unsigned int i, const char *suffix)
{
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s0x%08x%s%s",
            prefix, compiler->colours.literal, i, compiler->colours.reset, suffix);
}

static void shader_print_bool_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, unsigned int b, const char *suffix)
{
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%s%s%s", prefix,
            compiler->colours.literal, b ? "true" : "false", compiler->colours.reset, suffix);
}

static void shader_print_untyped_literal(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, uint32_t u, const char *suffix)
{
    union
    {
        uint32_t u;
        float f;
    } value;
    unsigned int exponent = (u >> 23) & 0xff;

    value.u = u;

    if (exponent != 0 && exponent != 0xff)
        return shader_print_float_literal(compiler, prefix, value.f, suffix);

    if (u <= 10000)
        return shader_print_uint_literal(compiler, prefix, value.u, suffix);

    return shader_print_hex_literal(compiler, prefix, value.u, suffix);
}

static void shader_print_subscript(struct vkd3d_d3d_asm_compiler *compiler,
        unsigned int offset, const struct vkd3d_shader_src_param *rel_addr)
{
    if (rel_addr)
        shader_print_src_param(compiler, "[", rel_addr, " + ");
    shader_print_uint_literal(compiler, rel_addr ? "" : "[", offset, "]");
}

static void shader_print_subscript_range(struct vkd3d_d3d_asm_compiler *compiler,
        unsigned int offset_first, unsigned int offset_last)
{
    shader_print_uint_literal(compiler, "[", offset_first, ":");
    if (offset_last != ~0u)
        shader_print_uint_literal(compiler, "", offset_last, "]");
    else
        vkd3d_string_buffer_printf(&compiler->buffer, "*]");
}

static void shader_print_register(struct vkd3d_d3d_asm_compiler *compiler, const char *prefix,
        const struct vkd3d_shader_register *reg, bool is_declaration, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int offset = reg->idx[0].offset;
    bool is_descriptor = false;

    static const char * const rastout_reg_names[] = {"oPos", "oFog", "oPts"};
    static const char * const misctype_reg_names[] = {"vPos", "vFace"};

    vkd3d_string_buffer_printf(buffer, "%s%s", prefix,
            reg->type == VKD3DSPR_LABEL ? compiler->colours.label : compiler->colours.reg);
    switch (reg->type)
    {
        case VKD3DSPR_RASTOUT:
            vkd3d_string_buffer_printf(buffer, "%s", rastout_reg_names[offset]);
            break;

        case VKD3DSPR_MISCTYPE:
            if (offset > 1)
                vkd3d_string_buffer_printf(buffer, "%s<unhandled misctype %#x>%s",
                        compiler->colours.error, offset, compiler->colours.reset);
            else
                vkd3d_string_buffer_printf(buffer, "%s", misctype_reg_names[offset]);
            break;

        case VKD3DSPR_COMBINED_SAMPLER:
        case VKD3DSPR_SAMPLER:
        case VKD3DSPR_CONSTBUFFER:
        case VKD3DSPR_RESOURCE:
        case VKD3DSPR_UAV:
            is_descriptor = true;
            /* fall through */

        default:
            if (reg->type < ARRAY_SIZE(shader_register_names) && shader_register_names[reg->type])
                vkd3d_string_buffer_printf(buffer, "%s", shader_register_names[reg->type]);
            else
                vkd3d_string_buffer_printf(buffer, "%s<unhandled register type %#x>%s",
                        compiler->colours.error, reg->type, compiler->colours.reset);
            break;
    }

    if (reg->type == VKD3DSPR_IMMCONST)
    {
        bool untyped = false;

        switch (compiler->current->opcode)
        {
            case VSIR_OP_MOV:
            case VSIR_OP_MOVC:
                untyped = true;
                break;

            default:
                break;
        }

        vkd3d_string_buffer_printf(buffer, "%s(", compiler->colours.reset);
        switch (reg->dimension)
        {
            case VSIR_DIMENSION_SCALAR:
                switch (reg->data_type)
                {
                    case VKD3D_DATA_FLOAT:
                        if (untyped)
                            shader_print_untyped_literal(compiler, "", reg->u.immconst_u32[0], "");
                        else
                            shader_print_float_literal(compiler, "", reg->u.immconst_f32[0], "");
                        break;
                    case VKD3D_DATA_INT:
                        shader_print_int_literal(compiler, "", reg->u.immconst_u32[0], "");
                        break;
                    case VKD3D_DATA_UINT:
                        shader_print_uint_literal(compiler, "", reg->u.immconst_u32[0], "");
                        break;
                    default:
                        vkd3d_string_buffer_printf(buffer, "%s<unhandled data type %#x>%s",
                                compiler->colours.error, reg->data_type, compiler->colours.reset);
                        break;
                }
                break;

            case VSIR_DIMENSION_VEC4:
                switch (reg->data_type)
                {
                    case VKD3D_DATA_FLOAT:
                        if (untyped)
                        {
                            shader_print_untyped_literal(compiler, "", reg->u.immconst_u32[0], "");
                            shader_print_untyped_literal(compiler, ", ", reg->u.immconst_u32[1], "");
                            shader_print_untyped_literal(compiler, ", ", reg->u.immconst_u32[2], "");
                            shader_print_untyped_literal(compiler, ", ", reg->u.immconst_u32[3], "");
                        }
                        else
                        {
                            shader_print_float_literal(compiler, "", reg->u.immconst_f32[0], "");
                            shader_print_float_literal(compiler, ", ", reg->u.immconst_f32[1], "");
                            shader_print_float_literal(compiler, ", ", reg->u.immconst_f32[2], "");
                            shader_print_float_literal(compiler, ", ", reg->u.immconst_f32[3], "");
                        }
                        break;
                    case VKD3D_DATA_INT:
                        shader_print_int_literal(compiler, "", reg->u.immconst_u32[0], "");
                        shader_print_int_literal(compiler, ", ", reg->u.immconst_u32[1], "");
                        shader_print_int_literal(compiler, ", ", reg->u.immconst_u32[2], "");
                        shader_print_int_literal(compiler, ", ", reg->u.immconst_u32[3], "");
                        break;
                    case VKD3D_DATA_UINT:
                        shader_print_uint_literal(compiler, "", reg->u.immconst_u32[0], "");
                        shader_print_uint_literal(compiler, ", ", reg->u.immconst_u32[1], "");
                        shader_print_uint_literal(compiler, ", ", reg->u.immconst_u32[2], "");
                        shader_print_uint_literal(compiler, ", ", reg->u.immconst_u32[3], "");
                        break;
                    default:
                        vkd3d_string_buffer_printf(buffer, "%s<unhandled data type %#x>%s",
                                compiler->colours.error, reg->data_type, compiler->colours.reset);
                        break;
                }
                break;

            default:
                vkd3d_string_buffer_printf(buffer, "%s<unhandled immconst dimension %#x>%s",
                        compiler->colours.error, reg->dimension, compiler->colours.reset);
                break;
        }
        vkd3d_string_buffer_printf(buffer, ")");
    }
    else if (reg->type == VKD3DSPR_IMMCONST64)
    {
        vkd3d_string_buffer_printf(buffer, "%s(", compiler->colours.reset);
        /* A double2 vector is treated as a float4 vector in enum vsir_dimension. */
        if (reg->dimension == VSIR_DIMENSION_SCALAR || reg->dimension == VSIR_DIMENSION_VEC4)
        {
            if (reg->data_type == VKD3D_DATA_DOUBLE)
            {
                shader_print_double_literal(compiler, "", reg->u.immconst_f64[0], "");
                if (reg->dimension == VSIR_DIMENSION_VEC4)
                    shader_print_double_literal(compiler, ", ", reg->u.immconst_f64[1], "");
            }
            else if (reg->data_type == VKD3D_DATA_UINT64)
            {
                shader_print_uint64_literal(compiler, "", reg->u.immconst_u64[0], "");
                if (reg->dimension == VSIR_DIMENSION_VEC4)
                    shader_print_uint64_literal(compiler, "", reg->u.immconst_u64[1], "");
            }
            else
            {
                vkd3d_string_buffer_printf(buffer, "%s<unhandled data type %#x>%s",
                        compiler->colours.error, reg->data_type, compiler->colours.reset);
            }
        }
        else
        {
            vkd3d_string_buffer_printf(buffer, "%s<unhandled immconst64 dimension %#x>%s",
                    compiler->colours.error, reg->dimension, compiler->colours.reset);
        }
        vkd3d_string_buffer_printf(buffer, ")");
    }
    else if (compiler->flags & VSIR_ASM_FLAG_DUMP_ALL_INDICES)
    {
        unsigned int i = 0;

        if (reg->idx_count == 0 || reg->idx[0].rel_addr)
        {
            vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.reset);
        }
        else
        {
            vkd3d_string_buffer_printf(buffer, "%u%s", offset, compiler->colours.reset);
            i = 1;
        }

        for (; i < reg->idx_count; ++i)
            shader_print_subscript(compiler, reg->idx[i].offset, reg->idx[i].rel_addr);
    }
    else if (reg->type != VKD3DSPR_RASTOUT
            && reg->type != VKD3DSPR_MISCTYPE
            && reg->type != VKD3DSPR_NULL
            && reg->type != VKD3DSPR_DEPTHOUT)
    {
        if (offset != ~0u)
        {
            bool is_sm_5_1 = vkd3d_shader_ver_ge(&compiler->shader_version, 5, 1);

            if (reg->idx[0].rel_addr || reg->type == VKD3DSPR_IMMCONSTBUFFER
                    || reg->type == VKD3DSPR_INCONTROLPOINT || reg->type == VKD3DSPR_OUTCONTROLPOINT
                    || (reg->type == VKD3DSPR_INPUT && (compiler->shader_version.type == VKD3D_SHADER_TYPE_GEOMETRY
                    || compiler->shader_version.type == VKD3D_SHADER_TYPE_HULL)))
            {
                vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.reset);
                shader_print_subscript(compiler, offset, reg->idx[0].rel_addr);
            }
            else
            {
                vkd3d_string_buffer_printf(buffer, "%u%s", offset, compiler->colours.reset);
            }

            /* For sm 5.1 descriptor declarations we need to print the register range instead of
             * a single register index. */
            if (is_descriptor && is_declaration && is_sm_5_1)
            {
                shader_print_subscript_range(compiler, reg->idx[1].offset, reg->idx[2].offset);
            }
            else if (reg->type != VKD3DSPR_SSA)
            {
                /* For descriptors in sm < 5.1 we move the reg->idx values up one slot
                 * to normalise with 5.1.
                 * Here we should ignore it if it's a descriptor in sm < 5.1. */
                if (reg->idx[1].offset != ~0u && (!is_descriptor || is_sm_5_1))
                    shader_print_subscript(compiler, reg->idx[1].offset, reg->idx[1].rel_addr);

                if (reg->idx[2].offset != ~0u)
                    shader_print_subscript(compiler, reg->idx[2].offset, reg->idx[2].rel_addr);
            }
        }
        else
        {
            vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.reset);
        }

        if (reg->type == VKD3DSPR_FUNCTIONPOINTER)
            shader_print_subscript(compiler, reg->u.fp_body_idx, NULL);
    }
    else
    {
        vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.reset);
    }
    vkd3d_string_buffer_printf(buffer, "%s", suffix);
}

static void shader_print_precision(struct vkd3d_d3d_asm_compiler *compiler, const struct vkd3d_shader_register *reg)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *precision;

    if (reg->precision == VKD3D_SHADER_REGISTER_PRECISION_DEFAULT)
        return;

    switch (reg->precision)
    {
        case VKD3D_SHADER_REGISTER_PRECISION_MIN_FLOAT_16:
            precision = "min16f";
            break;

        case VKD3D_SHADER_REGISTER_PRECISION_MIN_FLOAT_10:
            precision = "min2_8f";
            break;

        case VKD3D_SHADER_REGISTER_PRECISION_MIN_INT_16:
            precision = "min16i";
            break;

        case VKD3D_SHADER_REGISTER_PRECISION_MIN_UINT_16:
            precision = "min16u";
            break;

        default:
            vkd3d_string_buffer_printf(buffer, " {%s<unhandled precision %#x>%s}",
                    compiler->colours.error, reg->precision, compiler->colours.reset);
            return;
    }
    vkd3d_string_buffer_printf(buffer, " {%s%s%s}", compiler->colours.modifier, precision, compiler->colours.reset);
}

static void shader_print_non_uniform(struct vkd3d_d3d_asm_compiler *compiler, const struct vkd3d_shader_register *reg)
{
    if (reg->non_uniform)
        vkd3d_string_buffer_printf(&compiler->buffer, " {%snonuniform%s}",
                compiler->colours.modifier, compiler->colours.reset);
}

static void shader_print_reg_type(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_register *reg, const char *suffix)
{
    static const char *dimensions[] =
    {
        [VSIR_DIMENSION_NONE]   = "",
        [VSIR_DIMENSION_SCALAR] = "s:",
        [VSIR_DIMENSION_VEC4]   = "v4:",
    };

    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *dimension;

    if (!(compiler->flags & VSIR_ASM_FLAG_DUMP_TYPES))
    {
        vkd3d_string_buffer_printf(buffer, "%s%s", prefix, suffix);
        return;
    }

    if (reg->data_type == VKD3D_DATA_UNUSED)
        return;

    if (reg->dimension < ARRAY_SIZE(dimensions))
        dimension = dimensions[reg->dimension];
    else
        dimension = "??";

    vkd3d_string_buffer_printf(buffer, "%s <%s", prefix, dimension);
    shader_print_data_type(compiler, reg->data_type);
    vkd3d_string_buffer_printf(buffer, ">%s", suffix);
}

static void shader_print_write_mask(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, uint32_t mask, const char *suffix)
{
    unsigned int i = 0;
    char buffer[5];

    if (mask == 0)
    {
        vkd3d_string_buffer_printf(&compiler->buffer, "%s%s", prefix, suffix);
        return;
    }

    if (mask & VKD3DSP_WRITEMASK_0)
        buffer[i++] = 'x';
    if (mask & VKD3DSP_WRITEMASK_1)
        buffer[i++] = 'y';
    if (mask & VKD3DSP_WRITEMASK_2)
        buffer[i++] = 'z';
    if (mask & VKD3DSP_WRITEMASK_3)
        buffer[i++] = 'w';
    buffer[i++] = '\0';

    vkd3d_string_buffer_printf(&compiler->buffer, "%s.%s%s%s%s", prefix,
            compiler->colours.write_mask, buffer, compiler->colours.reset, suffix);
}

static void shader_print_dst_param(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_dst_param *param, bool is_declaration, const char *suffix)
{
    uint32_t write_mask = param->write_mask;

    shader_print_register(compiler, prefix, &param->reg, is_declaration, "");

    if (write_mask && param->reg.dimension == VSIR_DIMENSION_VEC4)
    {
        if (data_type_is_64_bit(param->reg.data_type))
            write_mask = vsir_write_mask_32_from_64(write_mask);

        shader_print_write_mask(compiler, "", write_mask, "");
    }

    shader_print_precision(compiler, &param->reg);
    shader_print_non_uniform(compiler, &param->reg);
    shader_print_reg_type(compiler, "", &param->reg, suffix);
}

static void shader_print_src_param(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_src_param *param, const char *suffix)
{
    enum vkd3d_shader_src_modifier src_modifier = param->modifiers;
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    uint32_t swizzle = param->swizzle;
    const char *modifier = "";
    bool is_abs = false;

    if (src_modifier == VKD3DSPSM_NEG
            || src_modifier == VKD3DSPSM_BIASNEG
            || src_modifier == VKD3DSPSM_SIGNNEG
            || src_modifier == VKD3DSPSM_X2NEG
            || src_modifier == VKD3DSPSM_ABSNEG)
        modifier = "-";
    else if (src_modifier == VKD3DSPSM_COMP)
        modifier = "1-";
    else if (src_modifier == VKD3DSPSM_NOT)
        modifier = "!";
    vkd3d_string_buffer_printf(buffer, "%s%s", prefix, modifier);

    if (src_modifier == VKD3DSPSM_ABS || src_modifier == VKD3DSPSM_ABSNEG)
        is_abs = true;

    shader_print_register(compiler, is_abs ? "|" : "", &param->reg, false, "");

    switch (src_modifier)
    {
        case VKD3DSPSM_NONE:
        case VKD3DSPSM_NEG:
        case VKD3DSPSM_COMP:
        case VKD3DSPSM_ABS:
        case VKD3DSPSM_ABSNEG:
        case VKD3DSPSM_NOT:
            break;
        case VKD3DSPSM_BIAS:
        case VKD3DSPSM_BIASNEG:
            vkd3d_string_buffer_printf(buffer, "_bias");
            break;
        case VKD3DSPSM_SIGN:
        case VKD3DSPSM_SIGNNEG:
            vkd3d_string_buffer_printf(buffer, "_bx2");
            break;
        case VKD3DSPSM_X2:
        case VKD3DSPSM_X2NEG:
            vkd3d_string_buffer_printf(buffer, "_x2");
            break;
        case VKD3DSPSM_DZ:
            vkd3d_string_buffer_printf(buffer, "_dz");
            break;
        case VKD3DSPSM_DW:
            vkd3d_string_buffer_printf(buffer, "_dw");
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "_%s<unhandled modifier %#x>%s",
                    compiler->colours.error, src_modifier, compiler->colours.reset);
            break;
    }

    if (param->reg.type != VKD3DSPR_IMMCONST && param->reg.type != VKD3DSPR_IMMCONST64
            && param->reg.dimension == VSIR_DIMENSION_VEC4)
    {
        static const char swizzle_chars[] = "xyzw";

        unsigned int swizzle_x, swizzle_y, swizzle_z, swizzle_w;

        if (data_type_is_64_bit(param->reg.data_type))
            swizzle = vsir_swizzle_32_from_64(swizzle);

        swizzle_x = vsir_swizzle_get_component(swizzle, 0);
        swizzle_y = vsir_swizzle_get_component(swizzle, 1);
        swizzle_z = vsir_swizzle_get_component(swizzle, 2);
        swizzle_w = vsir_swizzle_get_component(swizzle, 3);

        if (swizzle_x == swizzle_y && swizzle_x == swizzle_z && swizzle_x == swizzle_w)
            vkd3d_string_buffer_printf(buffer, ".%s%c%s", compiler->colours.swizzle,
                    swizzle_chars[swizzle_x], compiler->colours.reset);
        else
            vkd3d_string_buffer_printf(buffer, ".%s%c%c%c%c%s", compiler->colours.swizzle,
                    swizzle_chars[swizzle_x], swizzle_chars[swizzle_y],
                    swizzle_chars[swizzle_z], swizzle_chars[swizzle_w], compiler->colours.reset);
    }

    if (is_abs)
        vkd3d_string_buffer_printf(buffer, "|");

    shader_print_precision(compiler, &param->reg);
    shader_print_non_uniform(compiler, &param->reg);
    shader_print_reg_type(compiler, "", &param->reg, suffix);
}

static void shader_dump_ins_modifiers(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vkd3d_shader_dst_param *dst)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    uint32_t mmask = dst->modifiers;

    switch (dst->shift)
    {
        case 0:
            break;
        case 13:
            vkd3d_string_buffer_printf(buffer, "_d8");
            break;
        case 14:
            vkd3d_string_buffer_printf(buffer, "_d4");
            break;
        case 15:
            vkd3d_string_buffer_printf(buffer, "_d2");
            break;
        case 1:
            vkd3d_string_buffer_printf(buffer, "_x2");
            break;
        case 2:
            vkd3d_string_buffer_printf(buffer, "_x4");
            break;
        case 3:
            vkd3d_string_buffer_printf(buffer, "_x8");
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "_unhandled_shift(%d)", dst->shift);
            break;
    }

    if (mmask & VKD3DSPDM_SATURATE)
        vkd3d_string_buffer_printf(buffer, "_sat");
    if (mmask & VKD3DSPDM_PARTIALPRECISION)
        vkd3d_string_buffer_printf(buffer, "_pp");
    if (mmask & VKD3DSPDM_MSAMPCENTROID)
        vkd3d_string_buffer_printf(buffer, "_centroid");

    mmask &= ~VKD3DSPDM_MASK;
    if (mmask) FIXME("Unrecognised modifier %#x.\n", mmask);
}

static void shader_print_primitive_type(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, const struct vkd3d_shader_primitive_type *p, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *primitive_type;

    switch (p->type)
    {
        case VKD3D_PT_UNDEFINED:
            primitive_type = "undefined";
            break;
        case VKD3D_PT_POINTLIST:
            primitive_type = "pointlist";
            break;
        case VKD3D_PT_LINELIST:
            primitive_type = "linelist";
            break;
        case VKD3D_PT_LINESTRIP:
            primitive_type = "linestrip";
            break;
        case VKD3D_PT_TRIANGLELIST:
            primitive_type = "trianglelist";
            break;
        case VKD3D_PT_TRIANGLESTRIP:
            primitive_type = "trianglestrip";
            break;
        case VKD3D_PT_TRIANGLEFAN:
            primitive_type = "trianglefan";
            break;
        case VKD3D_PT_LINELIST_ADJ:
            primitive_type = "linelist_adj";
            break;
        case VKD3D_PT_LINESTRIP_ADJ:
            primitive_type = "linestrip_adj";
            break;
        case VKD3D_PT_TRIANGLELIST_ADJ:
            primitive_type = "trianglelist_adj";
            break;
        case VKD3D_PT_TRIANGLESTRIP_ADJ:
            primitive_type = "trianglestrip_adj";
            break;
        case VKD3D_PT_PATCH:
            vkd3d_string_buffer_printf(buffer, "%spatch%u%s", prefix, p->patch_vertex_count, suffix);
            return;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled primitive type %#x>%s%s",
                    prefix, compiler->colours.error, p->type, compiler->colours.reset, suffix);
            return;
    }
    vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, primitive_type, suffix);
}

static void shader_print_interpolation_mode(struct vkd3d_d3d_asm_compiler *compiler,
        const char *prefix, enum vkd3d_shader_interpolation_mode m, const char *suffix)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    const char *mode;

    switch (m)
    {
        case VKD3DSIM_CONSTANT:
            mode = "constant";
            break;
        case VKD3DSIM_LINEAR:
            mode = "linear";
            break;
        case VKD3DSIM_LINEAR_CENTROID:
            mode = "linear centroid";
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE:
            mode = "linear noperspective";
            break;
        case VKD3DSIM_LINEAR_SAMPLE:
            mode = "linear sample";
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID:
            mode = "linear noperspective centroid";
            break;
        case VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE:
            mode = "linear noperspective sample";
            break;
        default:
            vkd3d_string_buffer_printf(buffer, "%s%s<unhandled interpolation mode %#x>%s%s",
                    prefix, compiler->colours.error, m, compiler->colours.reset, suffix);
            return;
    }
    vkd3d_string_buffer_printf(buffer, "%s%s%s", prefix, mode, suffix);
}

const char *shader_get_type_prefix(enum vkd3d_shader_type type)
{
    switch (type)
    {
        case VKD3D_SHADER_TYPE_VERTEX:
            return "vs";

        case VKD3D_SHADER_TYPE_HULL:
            return "hs";

        case VKD3D_SHADER_TYPE_DOMAIN:
            return "ds";

        case VKD3D_SHADER_TYPE_GEOMETRY:
            return "gs";

        case VKD3D_SHADER_TYPE_PIXEL:
            return "ps";

        case VKD3D_SHADER_TYPE_COMPUTE:
            return "cs";

        case VKD3D_SHADER_TYPE_EFFECT:
            return "fx";

        case VKD3D_SHADER_TYPE_TEXTURE:
            return "tx";

        case VKD3D_SHADER_TYPE_LIBRARY:
            return "lib";

        default:
            FIXME("Unhandled shader type %#x.\n", type);
            return "unknown";
    }
}

static void shader_dump_instruction_flags(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vkd3d_shader_instruction *ins)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;

    switch (ins->opcode)
    {
        case VSIR_OP_BREAKP:
        case VSIR_OP_CONTINUEP:
        case VSIR_OP_DISCARD:
        case VSIR_OP_IF:
        case VSIR_OP_RETP:
            switch (ins->flags)
            {
                case VKD3D_SHADER_CONDITIONAL_OP_NZ:
                    vkd3d_string_buffer_printf(buffer, "_nz");
                    break;
                case VKD3D_SHADER_CONDITIONAL_OP_Z:
                    vkd3d_string_buffer_printf(buffer, "_z");
                    break;
                default:
                    vkd3d_string_buffer_printf(buffer, "_unrecognized(%#x)", ins->flags);
                    break;
            }
            break;

        case VSIR_OP_IFC:
        case VSIR_OP_BREAKC:
            switch (ins->flags)
            {
                case VKD3D_SHADER_REL_OP_GT:
                    vkd3d_string_buffer_printf(buffer, "_gt");
                    break;
                case VKD3D_SHADER_REL_OP_EQ:
                    vkd3d_string_buffer_printf(buffer, "_eq");
                    break;
                case VKD3D_SHADER_REL_OP_GE:
                    vkd3d_string_buffer_printf(buffer, "_ge");
                    break;
                case VKD3D_SHADER_REL_OP_LT:
                    vkd3d_string_buffer_printf(buffer, "_lt");
                    break;
                case VKD3D_SHADER_REL_OP_NE:
                    vkd3d_string_buffer_printf(buffer, "_ne");
                    break;
                case VKD3D_SHADER_REL_OP_LE:
                    vkd3d_string_buffer_printf(buffer, "_le");
                    break;
                default:
                    vkd3d_string_buffer_printf(buffer, "_(%u)", ins->flags);
                    break;
            }
            break;

        case VSIR_OP_RESINFO:
            switch (ins->flags)
            {
                case VKD3DSI_NONE:
                    break;
                case VKD3DSI_RESINFO_RCP_FLOAT:
                    vkd3d_string_buffer_printf(buffer, "_rcpFloat");
                    break;
                case VKD3DSI_RESINFO_UINT:
                    vkd3d_string_buffer_printf(buffer, "_uint");
                    break;
                default:
                    vkd3d_string_buffer_printf(buffer, "_unrecognized(%#x)", ins->flags);
                    break;
            }
            break;

        case VSIR_OP_SAMPLE_INFO:
            switch (ins->flags)
            {
                case VKD3DSI_NONE:
                    break;
                case VKD3DSI_SAMPLE_INFO_UINT:
                    vkd3d_string_buffer_printf(buffer, "_uint");
                    break;
                default:
                    vkd3d_string_buffer_printf(buffer, "_unrecognized(%#x)", ins->flags);
                    break;
            }
            break;

        case VSIR_OP_IMM_ATOMIC_CMP_EXCH:
        case VSIR_OP_IMM_ATOMIC_IADD:
        case VSIR_OP_IMM_ATOMIC_AND:
        case VSIR_OP_IMM_ATOMIC_IMAX:
        case VSIR_OP_IMM_ATOMIC_IMIN:
        case VSIR_OP_IMM_ATOMIC_OR:
        case VSIR_OP_IMM_ATOMIC_UMAX:
        case VSIR_OP_IMM_ATOMIC_UMIN:
        case VSIR_OP_IMM_ATOMIC_EXCH:
        case VSIR_OP_IMM_ATOMIC_XOR:
            shader_dump_atomic_op_flags(compiler, ins->flags);
            break;

        case VSIR_OP_SYNC:
            shader_dump_sync_flags(compiler, ins->flags);
            break;

        case VSIR_OP_TEXLD:
            if (vkd3d_shader_ver_ge(&compiler->shader_version, 2, 0))
            {
                if (ins->flags & VKD3DSI_TEXLD_PROJECT)
                    vkd3d_string_buffer_printf(buffer, "p");
                else if (ins->flags & VKD3DSI_TEXLD_BIAS)
                    vkd3d_string_buffer_printf(buffer, "b");
            }
            break;

        case VSIR_OP_WAVE_OP_ADD:
        case VSIR_OP_WAVE_OP_IMAX:
        case VSIR_OP_WAVE_OP_IMIN:
        case VSIR_OP_WAVE_OP_MAX:
        case VSIR_OP_WAVE_OP_MIN:
        case VSIR_OP_WAVE_OP_MUL:
        case VSIR_OP_WAVE_OP_UMAX:
        case VSIR_OP_WAVE_OP_UMIN:
            vkd3d_string_buffer_printf(&compiler->buffer, (ins->flags & VKD3DSI_WAVE_PREFIX) ? "_prefix" : "_active");
            break;

        case VSIR_OP_ISHL:
        case VSIR_OP_ISHR:
        case VSIR_OP_USHR:
            if (ins->flags & VKD3DSI_SHIFT_UNMASKED)
                vkd3d_string_buffer_printf(buffer, "_unmasked");
            /* fall through */
        default:
            shader_dump_precise_flags(compiler, ins->flags);
            break;
    }
}

static void shader_dump_register_space(struct vkd3d_d3d_asm_compiler *compiler, unsigned int register_space)
{
    if (vkd3d_shader_ver_ge(&compiler->shader_version, 5, 1))
        shader_print_uint_literal(compiler, ", space=", register_space, "");
}

static void shader_print_opcode(struct vkd3d_d3d_asm_compiler *compiler, enum vkd3d_shader_opcode opcode)
{
    vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%s", compiler->colours.opcode,
            vsir_opcode_get_name(opcode, "<unknown>"), compiler->colours.reset);
}

static void shader_dump_icb(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vkd3d_shader_immediate_constant_buffer *icb)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int i, j;

    vkd3d_string_buffer_printf(buffer, " {\n");
    if (icb->component_count == 1)
    {
        for (i = 0; i < icb->element_count; )
        {
            for (j = 0; i < icb->element_count && j < 4; ++i, ++j)
                shader_print_hex_literal(compiler, !j ? "    " : ", ", icb->data[i], "");
            vkd3d_string_buffer_printf(buffer, "\n");
        }
    }
    else
    {
        VKD3D_ASSERT(icb->component_count == VKD3D_VEC4_SIZE);
        for (i = 0; i < icb->element_count; ++i)
        {
            shader_print_hex_literal(compiler, "    {", icb->data[4 * i + 0], "");
            shader_print_hex_literal(compiler, ", ", icb->data[4 * i + 1], "");
            shader_print_hex_literal(compiler, ", ", icb->data[4 * i + 2], "");
            shader_print_hex_literal(compiler, ", ", icb->data[4 * i + 3], "},\n");
        }
    }
    vkd3d_string_buffer_printf(buffer, "}");
}

static void shader_dump_instruction(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vkd3d_shader_instruction *ins)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int i;

    compiler->current = ins;

    if (ins->predicate)
        shader_print_src_param(compiler, "(", ins->predicate, ") ");

    /* PixWin marks instructions with the coissue flag with a '+' */
    if (ins->coissue)
        vkd3d_string_buffer_printf(buffer, "+");

    shader_print_opcode(compiler, ins->opcode);

    switch (ins->opcode)
    {
        case VSIR_OP_DCL:
        case VSIR_OP_DCL_UAV_TYPED:
            vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.opcode);
            shader_print_dcl_usage(compiler, "_", &ins->declaration.semantic, ins->flags, "");
            shader_dump_ins_modifiers(compiler, &ins->declaration.semantic.resource.reg);
            vkd3d_string_buffer_printf(buffer, "%s", compiler->colours.reset);
            shader_print_register(compiler, " ", &ins->declaration.semantic.resource.reg.reg, true, "");
            shader_dump_register_space(compiler, ins->declaration.semantic.resource.range.space);
            break;

        case VSIR_OP_DCL_CONSTANT_BUFFER:
            shader_print_register(compiler, " ", &ins->declaration.cb.src.reg, true, "");
            if (vkd3d_shader_ver_ge(&compiler->shader_version, 6, 0))
                shader_print_subscript(compiler, ins->declaration.cb.size, NULL);
            else if (vkd3d_shader_ver_ge(&compiler->shader_version, 5, 1))
                shader_print_subscript(compiler, ins->declaration.cb.size / VKD3D_VEC4_SIZE / sizeof(float), NULL);
            vkd3d_string_buffer_printf(buffer, ", %s",
                    ins->flags & VKD3DSI_INDEXED_DYNAMIC ? "dynamicIndexed" : "immediateIndexed");
            shader_dump_register_space(compiler, ins->declaration.cb.range.space);
            break;

        case VSIR_OP_DCL_FUNCTION_BODY:
            vkd3d_string_buffer_printf(buffer, " fb%u", ins->declaration.index);
            break;

        case VSIR_OP_DCL_FUNCTION_TABLE:
            vkd3d_string_buffer_printf(buffer, " ft%u = {...}", ins->declaration.index);
            break;

        case VSIR_OP_DCL_GLOBAL_FLAGS:
            vkd3d_string_buffer_printf(buffer, " ");
            shader_dump_global_flags(compiler, ins->declaration.global_flags);
            break;

        case VSIR_OP_DCL_HS_MAX_TESSFACTOR:
            shader_print_float_literal(compiler, " ", ins->declaration.max_tessellation_factor, "");
            break;

        case VSIR_OP_DCL_IMMEDIATE_CONSTANT_BUFFER:
            shader_dump_icb(compiler, ins->declaration.icb);
            break;

        case VSIR_OP_DCL_INDEX_RANGE:
            shader_print_dst_param(compiler, " ", &ins->declaration.index_range.dst, true, "");
            shader_print_uint_literal(compiler, " ", ins->declaration.index_range.register_count, "");
            break;

        case VSIR_OP_DCL_INDEXABLE_TEMP:
            vkd3d_string_buffer_printf(buffer, " %sx%u%s", compiler->colours.reg,
                    ins->declaration.indexable_temp.register_idx, compiler->colours.reset);
            shader_print_subscript(compiler, ins->declaration.indexable_temp.register_size, NULL);
            shader_print_uint_literal(compiler, ", ", ins->declaration.indexable_temp.component_count, "");
            if (ins->declaration.indexable_temp.alignment)
                shader_print_uint_literal(compiler, ", align ", ins->declaration.indexable_temp.alignment, "");
            if (ins->declaration.indexable_temp.initialiser)
                shader_dump_icb(compiler, ins->declaration.indexable_temp.initialiser);
            break;

        case VSIR_OP_DCL_INPUT_PS:
            shader_print_interpolation_mode(compiler, " ", ins->flags, "");
            shader_print_dst_param(compiler, " ", &ins->declaration.dst, true, "");
            break;

        case VSIR_OP_DCL_INPUT_PS_SGV:
        case VSIR_OP_DCL_INPUT_SGV:
        case VSIR_OP_DCL_INPUT_SIV:
        case VSIR_OP_DCL_OUTPUT_SGV:
        case VSIR_OP_DCL_OUTPUT_SIV:
            shader_print_dst_param(compiler, " ", &ins->declaration.register_semantic.reg, true, "");
            shader_print_input_sysval_semantic(compiler, ", ", ins->declaration.register_semantic.sysval_semantic, "");
            break;

        case VSIR_OP_DCL_INPUT_PS_SIV:
            shader_print_interpolation_mode(compiler, " ", ins->flags, "");
            shader_print_dst_param(compiler, " ", &ins->declaration.register_semantic.reg, true, "");
            shader_print_input_sysval_semantic(compiler, ", ", ins->declaration.register_semantic.sysval_semantic, "");
            break;

        case VSIR_OP_DCL_INPUT:
        case VSIR_OP_DCL_OUTPUT:
            shader_print_dst_param(compiler, " ", &ins->declaration.dst, true, "");
            break;

        case VSIR_OP_DCL_INPUT_PRIMITIVE:
        case VSIR_OP_DCL_OUTPUT_TOPOLOGY:
            shader_print_primitive_type(compiler, " ", &ins->declaration.primitive_type, "");
            break;

        case VSIR_OP_DCL_INTERFACE:
            vkd3d_string_buffer_printf(buffer, " fp%u", ins->declaration.fp.index);
            shader_print_subscript(compiler, ins->declaration.fp.array_size, NULL);
            shader_print_subscript(compiler, ins->declaration.fp.body_count, NULL);
            vkd3d_string_buffer_printf(buffer, " = {...}");
            break;

        case VSIR_OP_DCL_RESOURCE_RAW:
            shader_print_dst_param(compiler, " ", &ins->declaration.raw_resource.resource.reg, true, "");
            shader_dump_register_space(compiler, ins->declaration.raw_resource.resource.range.space);
            break;

        case VSIR_OP_DCL_RESOURCE_STRUCTURED:
            shader_print_dst_param(compiler, " ", &ins->declaration.structured_resource.resource.reg, true, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.structured_resource.byte_stride, "");
            shader_dump_register_space(compiler, ins->declaration.structured_resource.resource.range.space);
            break;

        case VSIR_OP_DCL_SAMPLER:
            shader_print_register(compiler, " ", &ins->declaration.sampler.src.reg, true,
                    ins->flags == VKD3DSI_SAMPLER_COMPARISON_MODE ? ", comparisonMode" : "");
            shader_dump_register_space(compiler, ins->declaration.sampler.range.space);
            break;

        case VSIR_OP_DCL_TEMPS:
        case VSIR_OP_DCL_GS_INSTANCES:
        case VSIR_OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
        case VSIR_OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT:
        case VSIR_OP_DCL_INPUT_CONTROL_POINT_COUNT:
        case VSIR_OP_DCL_OUTPUT_CONTROL_POINT_COUNT:
        case VSIR_OP_DCL_VERTICES_OUT:
            shader_print_uint_literal(compiler, " ", ins->declaration.count, "");
            break;

        case VSIR_OP_DCL_TESSELLATOR_DOMAIN:
            shader_print_tessellator_domain(compiler, " ", ins->declaration.tessellator_domain, "");
            break;

        case VSIR_OP_DCL_TESSELLATOR_OUTPUT_PRIMITIVE:
            shader_print_tessellator_output_primitive(compiler, " ", ins->declaration.tessellator_output_primitive, "");
            break;

        case VSIR_OP_DCL_TESSELLATOR_PARTITIONING:
            shader_print_tessellator_partitioning(compiler, " ", ins->declaration.tessellator_partitioning, "");
            break;

        case VSIR_OP_DCL_TGSM_RAW:
            shader_print_dst_param(compiler, " ", &ins->declaration.tgsm_raw.reg, true, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.tgsm_raw.byte_count, "");
            break;

        case VSIR_OP_DCL_TGSM_STRUCTURED:
            shader_print_dst_param(compiler, " ", &ins->declaration.tgsm_structured.reg, true, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.tgsm_structured.byte_stride, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.tgsm_structured.structure_count, "");
            break;

        case VSIR_OP_DCL_THREAD_GROUP:
            shader_print_uint_literal(compiler, " ", ins->declaration.thread_group_size.x, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.thread_group_size.y, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.thread_group_size.z, "");
            break;

        case VSIR_OP_DCL_UAV_RAW:
            shader_dump_uav_flags(compiler, ins->flags);
            shader_print_dst_param(compiler, " ", &ins->declaration.raw_resource.resource.reg, true, "");
            shader_dump_register_space(compiler, ins->declaration.raw_resource.resource.range.space);
            break;

        case VSIR_OP_DCL_UAV_STRUCTURED:
            shader_dump_uav_flags(compiler, ins->flags);
            shader_print_dst_param(compiler, " ", &ins->declaration.structured_resource.resource.reg, true, "");
            shader_print_uint_literal(compiler, ", ", ins->declaration.structured_resource.byte_stride, "");
            shader_dump_register_space(compiler, ins->declaration.structured_resource.resource.range.space);
            break;

        case VSIR_OP_DEF:
            vkd3d_string_buffer_printf(buffer, " %sc%u%s", compiler->colours.reg,
                    ins->dst[0].reg.idx[0].offset, compiler->colours.reset);
            shader_print_float_literal(compiler, " = ", ins->src[0].reg.u.immconst_f32[0], "");
            shader_print_float_literal(compiler, ", ", ins->src[0].reg.u.immconst_f32[1], "");
            shader_print_float_literal(compiler, ", ", ins->src[0].reg.u.immconst_f32[2], "");
            shader_print_float_literal(compiler, ", ", ins->src[0].reg.u.immconst_f32[3], "");
            break;

        case VSIR_OP_DEFI:
            vkd3d_string_buffer_printf(buffer, " %si%u%s", compiler->colours.reg,
                    ins->dst[0].reg.idx[0].offset, compiler->colours.reset);
            shader_print_int_literal(compiler, " = ", ins->src[0].reg.u.immconst_u32[0], "");
            shader_print_int_literal(compiler, ", ", ins->src[0].reg.u.immconst_u32[1], "");
            shader_print_int_literal(compiler, ", ", ins->src[0].reg.u.immconst_u32[2], "");
            shader_print_int_literal(compiler, ", ", ins->src[0].reg.u.immconst_u32[3], "");
            break;

        case VSIR_OP_DEFB:
            vkd3d_string_buffer_printf(buffer, " %sb%u%s", compiler->colours.reg,
                    ins->dst[0].reg.idx[0].offset, compiler->colours.reset);
            shader_print_bool_literal(compiler, " = ", ins->src[0].reg.u.immconst_u32[0], "");
            break;

        default:
            shader_dump_instruction_flags(compiler, ins);

            if (ins->resource_type != VKD3D_SHADER_RESOURCE_NONE)
            {
                vkd3d_string_buffer_printf(buffer, "_indexable(");
                if (ins->raw)
                    vkd3d_string_buffer_printf(buffer, "raw_");
                if (ins->structured)
                    vkd3d_string_buffer_printf(buffer, "structured_");
                shader_print_resource_type(compiler, ins->resource_type);
                if (ins->resource_stride)
                    shader_print_uint_literal(compiler, ", stride=", ins->resource_stride, "");
                vkd3d_string_buffer_printf(buffer, ")");
            }

            if (vkd3d_shader_instruction_has_texel_offset(ins))
            {
                shader_print_int_literal(compiler, "(", ins->texel_offset.u, "");
                shader_print_int_literal(compiler, ",", ins->texel_offset.v, "");
                shader_print_int_literal(compiler, ",", ins->texel_offset.w, ")");
            }

            if (ins->resource_data_type[0] != VKD3D_DATA_FLOAT
                    || ins->resource_data_type[1] != VKD3D_DATA_FLOAT
                    || ins->resource_data_type[2] != VKD3D_DATA_FLOAT
                    || ins->resource_data_type[3] != VKD3D_DATA_FLOAT)
                shader_dump_resource_data_type(compiler, ins->resource_data_type);

            for (i = 0; i < ins->dst_count; ++i)
            {
                shader_dump_ins_modifiers(compiler, &ins->dst[i]);
                shader_print_dst_param(compiler, !i ? " " : ", ", &ins->dst[i], false, "");
            }

            /* Other source tokens */
            for (i = ins->dst_count; i < (ins->dst_count + ins->src_count); ++i)
            {
                shader_print_src_param(compiler, !i ? " " : ", ", &ins->src[i - ins->dst_count], "");
            }
            break;
    }

    vkd3d_string_buffer_printf(buffer, "\n");
}

static const char *get_sysval_semantic_name(enum vkd3d_shader_sysval_semantic semantic)
{
    switch (semantic)
    {
        case VKD3D_SHADER_SV_NONE:                      return "NONE";
        case VKD3D_SHADER_SV_POSITION:                  return "POS";
        case VKD3D_SHADER_SV_CLIP_DISTANCE:             return "CLIPDST";
        case VKD3D_SHADER_SV_CULL_DISTANCE:             return "CULLDST";
        case VKD3D_SHADER_SV_RENDER_TARGET_ARRAY_INDEX: return "RTINDEX";
        case VKD3D_SHADER_SV_VIEWPORT_ARRAY_INDEX:      return "VPINDEX";
        case VKD3D_SHADER_SV_VERTEX_ID:                 return "VERTID";
        case VKD3D_SHADER_SV_PRIMITIVE_ID:              return "PRIMID";
        case VKD3D_SHADER_SV_INSTANCE_ID:               return "INSTID";
        case VKD3D_SHADER_SV_IS_FRONT_FACE:             return "FFACE";
        case VKD3D_SHADER_SV_SAMPLE_INDEX:              return "SAMPLE";
        case VKD3D_SHADER_SV_TESS_FACTOR_QUADEDGE:      return "QUADEDGE";
        case VKD3D_SHADER_SV_TESS_FACTOR_QUADINT:       return "QUADINT";
        case VKD3D_SHADER_SV_TESS_FACTOR_TRIEDGE:       return "TRIEDGE";
        case VKD3D_SHADER_SV_TESS_FACTOR_TRIINT:        return "TRIINT";
        case VKD3D_SHADER_SV_TESS_FACTOR_LINEDET:       return "LINEDET";
        case VKD3D_SHADER_SV_TESS_FACTOR_LINEDEN:       return "LINEDEN";
        case VKD3D_SHADER_SV_TARGET:                    return "TARGET";
        case VKD3D_SHADER_SV_DEPTH:                     return "DEPTH";
        case VKD3D_SHADER_SV_COVERAGE:                  return "COVERAGE";
        case VKD3D_SHADER_SV_DEPTH_GREATER_EQUAL:       return "DEPTHGE";
        case VKD3D_SHADER_SV_DEPTH_LESS_EQUAL:          return "DEPTHLE";
        case VKD3D_SHADER_SV_STENCIL_REF:               return "STENCILREF";
        default:                                        return "??";
    }
}

static const char *get_component_type_name(enum vkd3d_shader_component_type type)
{
    switch (type)
    {
        case VKD3D_SHADER_COMPONENT_VOID:       return "void";
        case VKD3D_SHADER_COMPONENT_UINT:       return "uint";
        case VKD3D_SHADER_COMPONENT_INT:        return "int";
        case VKD3D_SHADER_COMPONENT_FLOAT:      return "float";
        case VKD3D_SHADER_COMPONENT_BOOL:       return "bool";
        case VKD3D_SHADER_COMPONENT_DOUBLE:     return "double";
        case VKD3D_SHADER_COMPONENT_UINT64:     return "uint64";
        case VKD3D_SHADER_COMPONENT_INT64:      return "int64";
        case VKD3D_SHADER_COMPONENT_FLOAT16:    return "float16";
        case VKD3D_SHADER_COMPONENT_UINT16:     return "uint16";
        case VKD3D_SHADER_COMPONENT_INT16:      return "int16";
        case VKD3D_SHADER_COMPONENT_TYPE_FORCE_32BIT:
            break;
    }

    return "??";
}

static const char *get_minimum_precision_name(enum vkd3d_shader_minimum_precision prec)
{
    switch (prec)
    {
        case VKD3D_SHADER_MINIMUM_PRECISION_NONE:      return "NONE";
        case VKD3D_SHADER_MINIMUM_PRECISION_FLOAT_16:  return "FLOAT_16";
        case VKD3D_SHADER_MINIMUM_PRECISION_FIXED_8_2: return "FIXED_8_2";
        case VKD3D_SHADER_MINIMUM_PRECISION_INT_16:    return "INT_16";
        case VKD3D_SHADER_MINIMUM_PRECISION_UINT_16:   return "UINT_16";
        default:                                       return "??";
    }
}

static const char *get_semantic_register_name(enum vkd3d_shader_sysval_semantic semantic)
{
    switch (semantic)
    {
        case VKD3D_SHADER_SV_PRIMITIVE_ID:        return "primID";
        case VKD3D_SHADER_SV_DEPTH:               return "oDepth";
        case VKD3D_SHADER_SV_DEPTH_GREATER_EQUAL: return "oDepthGE";
        case VKD3D_SHADER_SV_DEPTH_LESS_EQUAL:    return "oDepthLE";
            /* SV_Coverage has name vCoverage when used as an input,
             * but it doesn't appear in the signature in that case. */
        case VKD3D_SHADER_SV_COVERAGE:            return "oMask";
        case VKD3D_SHADER_SV_STENCIL_REF:         return "oStencilRef";
        default:                                  return "??";
    }
}

static enum vkd3d_result dump_dxbc_signature(struct vkd3d_d3d_asm_compiler *compiler,
        const char *name, const char *register_name, const struct shader_signature *signature)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int i;

    if (signature->element_count == 0)
        return VKD3D_OK;

    vkd3d_string_buffer_printf(buffer, "%s%s%s\n",
            compiler->colours.opcode, name, compiler->colours.reset);

    for (i = 0; i < signature->element_count; ++i)
    {
        struct signature_element *element = &signature->elements[i];

        vkd3d_string_buffer_printf(buffer, "%s.param%s %s", compiler->colours.opcode,
                compiler->colours.reset, element->semantic_name);

        if (element->semantic_index != 0)
            vkd3d_string_buffer_printf(buffer, "%u", element->semantic_index);

        if (element->register_index != -1)
        {
            shader_print_write_mask(compiler, "", element->mask, "");
            vkd3d_string_buffer_printf(buffer, ", %s%s%d%s", compiler->colours.reg,
                    register_name, element->register_index, compiler->colours.reset);
            shader_print_write_mask(compiler, "", element->used_mask, "");
        }
        else
        {
            vkd3d_string_buffer_printf(buffer, ", %s%s%s", compiler->colours.reg,
                    get_semantic_register_name(element->sysval_semantic), compiler->colours.reset);
        }

        if (!element->component_type && !element->sysval_semantic
                && !element->min_precision && !element->stream_index)
            goto done;

        vkd3d_string_buffer_printf(buffer, ", %s",
                get_component_type_name(element->component_type));

        if (!element->sysval_semantic && !element->min_precision && !element->stream_index)
            goto done;

        vkd3d_string_buffer_printf(buffer, ", %s",
                get_sysval_semantic_name(element->sysval_semantic));

        if (!element->min_precision && !element->stream_index)
            goto done;

        vkd3d_string_buffer_printf(buffer, ", %s",
                get_minimum_precision_name(element->min_precision));

        if (!element->stream_index)
            goto done;

        vkd3d_string_buffer_printf(buffer, ", m%u",
                element->stream_index);

    done:
        vkd3d_string_buffer_printf(buffer, "\n");
    }

    return VKD3D_OK;
}

static enum vkd3d_result dump_dxbc_signatures(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vsir_program *program)
{
    enum vkd3d_result ret;

    if ((ret = dump_dxbc_signature(compiler, ".input",
            program->shader_version.type == VKD3D_SHADER_TYPE_DOMAIN ? "vicp" : "v",
            &program->input_signature)) < 0)
        return ret;

    if ((ret = dump_dxbc_signature(compiler, ".output", "o",
            &program->output_signature)) < 0)
        return ret;

    if ((ret = dump_dxbc_signature(compiler, ".patch_constant",
            program->shader_version.type == VKD3D_SHADER_TYPE_DOMAIN ? "vpc" : "o",
            &program->patch_constant_signature)) < 0)
        return ret;

    return VKD3D_OK;
}

static void shader_print_descriptor_name(struct vkd3d_d3d_asm_compiler *compiler,
        enum vkd3d_shader_descriptor_type t, unsigned int id)
{
    const char *type = NULL;

    switch (t)
    {
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
            type = shader_register_names[VKD3DSPR_RESOURCE];
            break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
            type = shader_register_names[VKD3DSPR_UAV];
            break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
            type = shader_register_names[VKD3DSPR_CONSTBUFFER];
            break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
            type = shader_register_names[VKD3DSPR_SAMPLER];
            break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_FORCE_32BIT:
            break;
    }

    if (type)
        vkd3d_string_buffer_printf(&compiler->buffer, "%s%s%u%s",
                compiler->colours.reg, type, id, compiler->colours.reset);
    else
        vkd3d_string_buffer_printf(&compiler->buffer, "%s<unhandled descriptor type %#x>%u%s",
                compiler->colours.error, t, id, compiler->colours.reset);
}

static void shader_print_descriptors(struct vkd3d_d3d_asm_compiler *compiler,
        const struct vkd3d_shader_scan_descriptor_info1 *descriptors)
{
    struct vkd3d_string_buffer *buffer = &compiler->buffer;
    unsigned int i;

    vkd3d_string_buffer_printf(buffer, "%s.descriptors%s\n",
            compiler->colours.opcode, compiler->colours.reset);
    for (i = 0; i < descriptors->descriptor_count; ++i)
    {
        const struct vkd3d_shader_descriptor_info1 *d = &descriptors->descriptors[i];

        vkd3d_string_buffer_printf(buffer, "%s.descriptor%s ", compiler->colours.opcode, compiler->colours.reset);
        shader_print_descriptor_name(compiler, d->type, d->register_id);
        shader_print_subscript_range(compiler, d->register_index,
                d->count == ~0u ? ~0u : d->register_index + d->count - 1);
        shader_dump_register_space(compiler, d->register_space);

        if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_SRV || d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_UAV)
        {
            vkd3d_string_buffer_printf(buffer, ", ");
            shader_print_resource_type(compiler, d->resource_type);
            vkd3d_string_buffer_printf(buffer, " <v4:");
            shader_print_data_type(compiler, d->resource_data_type);
            if (d->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMS
                    || d->resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DMSARRAY)
                shader_print_uint_literal(compiler, ", ", d->sample_count, "");
            vkd3d_string_buffer_printf(buffer, ">");
        }

        if (d->buffer_size)
            shader_print_hex_literal(compiler, ", size=", d->buffer_size, "");
        if (d->structure_stride)
            shader_print_hex_literal(compiler, ", stride=", d->structure_stride, "");
        if (d->flags)
            shader_print_hex_literal(compiler, ", flags=", d->flags, "");
        if (d->uav_flags)
            shader_print_hex_literal(compiler, ", uav_flags=", d->uav_flags, "");
        vkd3d_string_buffer_printf(buffer, "\n");
    }
}

enum vkd3d_result d3d_asm_compile(const struct vsir_program *program,
        const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, enum vsir_asm_flags flags)
{
    const struct vkd3d_shader_version *shader_version = &program->shader_version;
    enum vkd3d_shader_compile_option_formatting_flags formatting;
    struct vkd3d_d3d_asm_compiler compiler =
    {
        .flags = flags,
    };
    enum vkd3d_result result = VKD3D_OK;
    struct vkd3d_string_buffer *buffer;
    unsigned int indent, i, j;
    const char *indent_str;

    static const struct vkd3d_d3d_asm_colours no_colours =
    {
        .reset = "",
        .error = "",
        .literal = "",
        .modifier = "",
        .opcode = "",
        .reg = "",
        .swizzle = "",
        .version = "",
        .write_mask = "",
        .label = "",
    };
    static const struct vkd3d_d3d_asm_colours colours =
    {
        .reset = "\x1b[m",
        .error = "\x1b[97;41m",
        .literal = "\x1b[95m",
        .modifier = "\x1b[36m",
        .opcode = "\x1b[96;1m",
        .reg = "\x1b[96m",
        .swizzle = "\x1b[93m",
        .version = "\x1b[36m",
        .write_mask = "\x1b[93m",
        .label = "\x1b[91m",
    };

    formatting = VKD3D_SHADER_COMPILE_OPTION_FORMATTING_INDENT
            | VKD3D_SHADER_COMPILE_OPTION_FORMATTING_HEADER;
    if (compile_info)
    {
        for (i = 0; i < compile_info->option_count; ++i)
        {
            const struct vkd3d_shader_compile_option *option = &compile_info->options[i];

            if (option->name == VKD3D_SHADER_COMPILE_OPTION_FORMATTING)
                formatting = option->value;
        }
    }

    if (formatting & VKD3D_SHADER_COMPILE_OPTION_FORMATTING_COLOUR)
        compiler.colours = colours;
    else
        compiler.colours = no_colours;
    if (formatting & VKD3D_SHADER_COMPILE_OPTION_FORMATTING_INDENT)
        indent_str = "    ";
    else
        indent_str = "";
    /* The signatures we emit only make sense for DXBC shaders. d3dbc doesn't
     * even have an explicit concept of signature. */
    if (formatting & VKD3D_SHADER_COMPILE_OPTION_FORMATTING_IO_SIGNATURES && shader_version->major >= 4)
        compiler.flags |= VSIR_ASM_FLAG_DUMP_SIGNATURES;

    buffer = &compiler.buffer;
    vkd3d_string_buffer_init(buffer);

    compiler.shader_version = *shader_version;
    shader_version = &compiler.shader_version;
    vkd3d_string_buffer_printf(buffer, "%s%s_%u_%u%s\n", compiler.colours.version,
            shader_get_type_prefix(shader_version->type), shader_version->major,
            shader_version->minor, compiler.colours.reset);

    if (compiler.flags & VSIR_ASM_FLAG_DUMP_SIGNATURES
            && (result = dump_dxbc_signatures(&compiler, program)) < 0)
    {
        vkd3d_string_buffer_cleanup(buffer);
        return result;
    }

    if (compiler.flags & VSIR_ASM_FLAG_DUMP_DESCRIPTORS)
        shader_print_descriptors(&compiler, &program->descriptors);

    if (compiler.flags & (VSIR_ASM_FLAG_DUMP_SIGNATURES | VSIR_ASM_FLAG_DUMP_DESCRIPTORS))
        vkd3d_string_buffer_printf(buffer, "%s.text%s\n", compiler.colours.opcode, compiler.colours.reset);

    indent = 0;
    for (i = 0; i < program->instructions.count; ++i)
    {
        struct vkd3d_shader_instruction *ins = &program->instructions.elements[i];

        switch (ins->opcode)
        {
            case VSIR_OP_ELSE:
            case VSIR_OP_ENDIF:
            case VSIR_OP_ENDLOOP:
            case VSIR_OP_ENDSWITCH:
                if (indent)
                    --indent;
                break;

            case VSIR_OP_LABEL:
            case VSIR_OP_HS_DECLS:
            case VSIR_OP_HS_CONTROL_POINT_PHASE:
            case VSIR_OP_HS_FORK_PHASE:
            case VSIR_OP_HS_JOIN_PHASE:
                indent = 0;
                break;

            default:
                break;
        }

        for (j = 0; j < indent; ++j)
        {
            vkd3d_string_buffer_printf(buffer, "%s", indent_str);
        }

        shader_dump_instruction(&compiler, ins);

        switch (ins->opcode)
        {
            case VSIR_OP_ELSE:
            case VSIR_OP_IF:
            case VSIR_OP_IFC:
            case VSIR_OP_LOOP:
            case VSIR_OP_SWITCH:
            case VSIR_OP_LABEL:
                ++indent;
                break;

            default:
                break;
        }
    }

    vkd3d_shader_code_from_string_buffer(out, buffer);

    return result;
}

/* This is meant exclusively for development use. Therefore, differently from
 * dump_dxbc_signature(), it doesn't try particularly hard to make the output
 * nice or easily parsable, and it dumps all fields, not just the DXBC ones.
 * This format isn't meant to be stable. */
static void trace_signature(const struct shader_signature *signature, const char *signature_type)
{
    struct vkd3d_string_buffer buffer;
    unsigned int i;

    TRACE("%s signature:%s\n", signature_type, signature->element_count == 0 ? " empty" : "");

    vkd3d_string_buffer_init(&buffer);

    for (i = 0; i < signature->element_count; ++i)
    {
        const struct signature_element *element = &signature->elements[i];

        vkd3d_string_buffer_clear(&buffer);

        vkd3d_string_buffer_printf(&buffer, "Element %u: %s %u-%u %s", i,
                get_component_type_name(element->component_type),
                element->register_index, element->register_index + element->register_count,
                element->semantic_name);
        if (element->semantic_index != -1)
            vkd3d_string_buffer_printf(&buffer, "%u", element->semantic_index);
        vkd3d_string_buffer_printf(&buffer,
                " mask %#x used_mask %#x sysval %s min_precision %s interpolation %u stream %u",
                element->mask, element->used_mask, get_sysval_semantic_name(element->sysval_semantic),
                get_minimum_precision_name(element->min_precision), element->interpolation_mode,
                element->stream_index);
        if (element->target_location != -1)
            vkd3d_string_buffer_printf(&buffer, " target %u", element->target_location);
        else
            vkd3d_string_buffer_printf(&buffer, " unused");

        TRACE("%s\n", buffer.buffer);
    }

    vkd3d_string_buffer_cleanup(&buffer);
}

static void shader_print_io_declaration(struct vkd3d_string_buffer *buffer, enum vkd3d_shader_register_type type)
{
    switch (type)
    {
#define X(x) case VKD3DSPR_ ## x: vkd3d_string_buffer_printf(buffer, #x); return;
        X(TEMP)
        X(INPUT)
        X(CONST)
        X(ADDR)
        X(TEXTURE)
        X(RASTOUT)
        X(ATTROUT)
        X(TEXCRDOUT)
        X(OUTPUT)
        X(CONSTINT)
        X(COLOROUT)
        X(DEPTHOUT)
        X(COMBINED_SAMPLER)
        X(CONSTBOOL)
        X(LOOP)
        X(TEMPFLOAT16)
        X(MISCTYPE)
        X(LABEL)
        X(PREDICATE)
        X(IMMCONST)
        X(IMMCONST64)
        X(CONSTBUFFER)
        X(IMMCONSTBUFFER)
        X(PRIMID)
        X(NULL)
        X(SAMPLER)
        X(RESOURCE)
        X(UAV)
        X(OUTPOINTID)
        X(FORKINSTID)
        X(JOININSTID)
        X(INCONTROLPOINT)
        X(OUTCONTROLPOINT)
        X(PATCHCONST)
        X(TESSCOORD)
        X(GROUPSHAREDMEM)
        X(THREADID)
        X(THREADGROUPID)
        X(LOCALTHREADID)
        X(LOCALTHREADINDEX)
        X(IDXTEMP)
        X(STREAM)
        X(FUNCTIONBODY)
        X(FUNCTIONPOINTER)
        X(COVERAGE)
        X(SAMPLEMASK)
        X(GSINSTID)
        X(DEPTHOUTGE)
        X(DEPTHOUTLE)
        X(RASTERIZER)
        X(OUTSTENCILREF)
        X(UNDEF)
        X(SSA)
        X(WAVELANECOUNT)
        X(WAVELANEINDEX)
        X(PARAMETER)
        X(POINT_COORD)
#undef X
        case VKD3DSPR_INVALID:
        case VKD3DSPR_COUNT:
            break;
    }

    vkd3d_string_buffer_printf(buffer, "<invalid register type %#x>", type);
}

static void trace_io_declarations(const struct vsir_program *program)
{
    struct vkd3d_string_buffer buffer;
    bool empty = true;
    unsigned int i;

    vkd3d_string_buffer_init(&buffer);

    vkd3d_string_buffer_printf(&buffer, "Input/output declarations:");

    for (i = 0; i < sizeof(program->io_dcls) * CHAR_BIT; ++i)
    {
        if (!bitmap_is_set(program->io_dcls, i))
            continue;

        vkd3d_string_buffer_printf(&buffer, empty ? " " : " | ");
        shader_print_io_declaration(&buffer, i);
        empty = false;
    }

    if (empty)
        vkd3d_string_buffer_printf(&buffer, " empty");

    TRACE("%s\n", buffer.buffer);

    vkd3d_string_buffer_cleanup(&buffer);
}

void vsir_program_trace(const struct vsir_program *program)
{
    const unsigned int flags = VSIR_ASM_FLAG_DUMP_TYPES | VSIR_ASM_FLAG_DUMP_ALL_INDICES
            | VSIR_ASM_FLAG_DUMP_SIGNATURES | VSIR_ASM_FLAG_DUMP_DESCRIPTORS;
    struct vkd3d_shader_code code;
    const char *p, *q, *end;

    trace_signature(&program->input_signature, "Input");
    trace_signature(&program->output_signature, "Output");
    trace_signature(&program->patch_constant_signature, "Patch-constant");
    trace_io_declarations(program);

    if (d3d_asm_compile(program, NULL, &code, flags) != VKD3D_OK)
        return;

    end = (const char *)code.code + code.size;
    for (p = code.code; p < end; p = q)
    {
        if (!(q = memchr(p, '\n', end - p)))
            q = end;
        else
            ++q;
        TRACE("    %.*s", (int)(q - p), p);
    }

    vkd3d_shader_free_shader_code(&code);
}
