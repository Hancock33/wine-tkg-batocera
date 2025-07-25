/*
 * Copyright 2017 Józef Kucia for CodeWeavers
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 * Copyright 2002-2003 The wine-d3d team
 * Copyright 2002-2003 2004 Jason Edmeades
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2006 Stefan Dösinger
 * Copyright 2006-2011, 2013 Stefan Dösinger for CodeWeavers
 * Copyright 2007 Henri Verbeet
 * Copyright 2008-2009 Henri Verbeet for CodeWeavers
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

#ifndef __VKD3D_SHADER_PRIVATE_H
#define __VKD3D_SHADER_PRIVATE_H

#define NONAMELESSUNION
#include "vkd3d_common.h"
#include "vkd3d_memory.h"
#include "vkd3d_shader.h"
#include "wine/list.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define VKD3D_VEC4_SIZE 4
#define VKD3D_DVEC2_SIZE 2

#define VKD3D_SHADER_COMPONENT_TYPE_COUNT (VKD3D_SHADER_COMPONENT_INT16 + 1)
#define VKD3D_SHADER_MINIMUM_PRECISION_COUNT (VKD3D_SHADER_MINIMUM_PRECISION_UINT_16 + 1)
#define VKD3D_SHADER_DESCRIPTOR_TYPE_COUNT (VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER + 1)
#define VKD3D_SHADER_RESOURCE_TYPE_COUNT (VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY + 1)

#define VKD3D_MAX_STREAM_COUNT 4

enum vkd3d_shader_error
{
    VKD3D_SHADER_ERROR_DXBC_INVALID_SIZE                = 1,
    VKD3D_SHADER_ERROR_DXBC_INVALID_MAGIC               = 2,
    VKD3D_SHADER_ERROR_DXBC_INVALID_CHECKSUM            = 3,
    VKD3D_SHADER_ERROR_DXBC_INVALID_VERSION             = 4,
    VKD3D_SHADER_ERROR_DXBC_INVALID_CHUNK_OFFSET        = 5,
    VKD3D_SHADER_ERROR_DXBC_INVALID_CHUNK_SIZE          = 6,
    VKD3D_SHADER_ERROR_DXBC_OUT_OF_MEMORY               = 7,
    VKD3D_SHADER_ERROR_DXBC_INVALID_SIGNATURE           = 8,
    VKD3D_SHADER_ERROR_DXBC_INVALID_STRING_REFERENCE    = 9,
    VKD3D_SHADER_ERROR_DXBC_INVALID_COMPONENT_TYPE      = 10,

    VKD3D_SHADER_ERROR_TPF_MISMATCHED_CF                = 1000,
    VKD3D_SHADER_ERROR_TPF_INVALID_REGISTER_RANGE       = 1001,
    VKD3D_SHADER_ERROR_TPF_OUT_OF_MEMORY                = 1002,
    VKD3D_SHADER_ERROR_TPF_INVALID_REGISTER_INDEX_COUNT = 1003,
    VKD3D_SHADER_ERROR_TPF_TOO_MANY_REGISTERS           = 1004,
    VKD3D_SHADER_ERROR_TPF_INVALID_IO_REGISTER          = 1005,
    VKD3D_SHADER_ERROR_TPF_INVALID_INDEX_RANGE_DCL      = 1006,
    VKD3D_SHADER_ERROR_TPF_INVALID_CASE_VALUE           = 1007,
    VKD3D_SHADER_ERROR_TPF_INVALID_REGISTER_DIMENSION   = 1008,
    VKD3D_SHADER_ERROR_TPF_INVALID_REGISTER_SWIZZLE     = 1009,
    VKD3D_SHADER_ERROR_TPF_INVALID_REGISTER_DCL         = 1010,

    VKD3D_SHADER_WARNING_TPF_MASK_NOT_CONTIGUOUS        = 1300,
    VKD3D_SHADER_WARNING_TPF_UNHANDLED_INDEX_RANGE_MASK = 1301,
    VKD3D_SHADER_WARNING_TPF_UNHANDLED_REGISTER_MASK    = 1302,
    VKD3D_SHADER_WARNING_TPF_UNHANDLED_REGISTER_SWIZZLE = 1303,

    VKD3D_SHADER_ERROR_SPV_DESCRIPTOR_BINDING_NOT_FOUND = 2000,
    VKD3D_SHADER_ERROR_SPV_INVALID_REGISTER_TYPE        = 2001,
    VKD3D_SHADER_ERROR_SPV_INVALID_DESCRIPTOR_BINDING   = 2002,
    VKD3D_SHADER_ERROR_SPV_DESCRIPTOR_IDX_UNSUPPORTED   = 2003,
    VKD3D_SHADER_ERROR_SPV_UNSUPPORTED_FEATURE          = 2004,
    VKD3D_SHADER_ERROR_SPV_OUT_OF_MEMORY                = 2005,
    VKD3D_SHADER_ERROR_SPV_INVALID_TYPE                 = 2006,
    VKD3D_SHADER_ERROR_SPV_INVALID_HANDLER              = 2007,
    VKD3D_SHADER_ERROR_SPV_NOT_IMPLEMENTED              = 2008,
    VKD3D_SHADER_ERROR_SPV_INVALID_SHADER               = 2009,

    VKD3D_SHADER_WARNING_SPV_INVALID_SWIZZLE            = 2300,
    VKD3D_SHADER_WARNING_SPV_INVALID_UAV_FLAGS          = 2301,
    VKD3D_SHADER_WARNING_SPV_IGNORING_FLAG              = 2302,

    VKD3D_SHADER_ERROR_RS_OUT_OF_MEMORY                 = 3000,
    VKD3D_SHADER_ERROR_RS_INVALID_VERSION               = 3001,
    VKD3D_SHADER_ERROR_RS_INVALID_ROOT_PARAMETER_TYPE   = 3002,
    VKD3D_SHADER_ERROR_RS_INVALID_DESCRIPTOR_RANGE_TYPE = 3003,
    VKD3D_SHADER_ERROR_RS_MIXED_DESCRIPTOR_RANGE_TYPES  = 3004,

    VKD3D_SHADER_ERROR_PP_INVALID_SYNTAX                = 4000,
    VKD3D_SHADER_ERROR_PP_ERROR_DIRECTIVE               = 4001,
    VKD3D_SHADER_ERROR_PP_INCLUDE_FAILED                = 4002,

    VKD3D_SHADER_WARNING_PP_ALREADY_DEFINED             = 4300,
    VKD3D_SHADER_WARNING_PP_INVALID_DIRECTIVE           = 4301,
    VKD3D_SHADER_WARNING_PP_ARGUMENT_COUNT_MISMATCH     = 4302,
    VKD3D_SHADER_WARNING_PP_UNKNOWN_DIRECTIVE           = 4303,
    VKD3D_SHADER_WARNING_PP_UNTERMINATED_MACRO          = 4304,
    VKD3D_SHADER_WARNING_PP_UNTERMINATED_IF             = 4305,
    VKD3D_SHADER_WARNING_PP_DIV_BY_ZERO                 = 4306,

    VKD3D_SHADER_ERROR_HLSL_INVALID_SYNTAX              = 5000,
    VKD3D_SHADER_ERROR_HLSL_INVALID_MODIFIER            = 5001,
    VKD3D_SHADER_ERROR_HLSL_INVALID_TYPE                = 5002,
    VKD3D_SHADER_ERROR_HLSL_MODIFIES_CONST              = 5003,
    VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC            = 5004,
    VKD3D_SHADER_ERROR_HLSL_NOT_DEFINED                 = 5005,
    VKD3D_SHADER_ERROR_HLSL_REDEFINED                   = 5006,
    VKD3D_SHADER_ERROR_HLSL_WRONG_PARAMETER_COUNT       = 5007,
    VKD3D_SHADER_ERROR_HLSL_INVALID_SIZE                = 5008,
    VKD3D_SHADER_ERROR_HLSL_MISSING_INITIALIZER         = 5009,
    VKD3D_SHADER_ERROR_HLSL_INVALID_LVALUE              = 5010,
    VKD3D_SHADER_ERROR_HLSL_INVALID_WRITEMASK           = 5011,
    VKD3D_SHADER_ERROR_HLSL_INVALID_INDEX               = 5012,
    VKD3D_SHADER_ERROR_HLSL_INVALID_SEMANTIC            = 5013,
    VKD3D_SHADER_ERROR_HLSL_INVALID_RETURN              = 5014,
    VKD3D_SHADER_ERROR_HLSL_OVERLAPPING_RESERVATIONS    = 5015,
    VKD3D_SHADER_ERROR_HLSL_INVALID_RESERVATION         = 5016,
    VKD3D_SHADER_ERROR_HLSL_NOT_IMPLEMENTED             = 5017,
    VKD3D_SHADER_ERROR_HLSL_INVALID_TEXEL_OFFSET        = 5018,
    VKD3D_SHADER_ERROR_HLSL_OFFSET_OUT_OF_BOUNDS        = 5019,
    VKD3D_SHADER_ERROR_HLSL_INCOMPATIBLE_PROFILE        = 5020,
    VKD3D_SHADER_ERROR_HLSL_DIVISION_BY_ZERO            = 5021,
    VKD3D_SHADER_ERROR_HLSL_NON_STATIC_OBJECT_REF       = 5022,
    VKD3D_SHADER_ERROR_HLSL_INVALID_THREAD_COUNT        = 5023,
    VKD3D_SHADER_ERROR_HLSL_MISSING_ATTRIBUTE           = 5024,
    VKD3D_SHADER_ERROR_HLSL_RECURSIVE_CALL              = 5025,
    VKD3D_SHADER_ERROR_HLSL_INCONSISTENT_SAMPLER        = 5026,
    VKD3D_SHADER_ERROR_HLSL_NON_FINITE_RESULT           = 5027,
    VKD3D_SHADER_ERROR_HLSL_DUPLICATE_SWITCH_CASE       = 5028,
    VKD3D_SHADER_ERROR_HLSL_MISSING_TECHNIQUE           = 5029,
    VKD3D_SHADER_ERROR_HLSL_UNKNOWN_MODIFIER            = 5030,
    VKD3D_SHADER_ERROR_HLSL_INVALID_STATE_BLOCK_ENTRY   = 5031,
    VKD3D_SHADER_ERROR_HLSL_FAILED_FORCED_UNROLL        = 5032,
    VKD3D_SHADER_ERROR_HLSL_INVALID_PROFILE             = 5033,
    VKD3D_SHADER_ERROR_HLSL_MISPLACED_COMPILE           = 5034,
    VKD3D_SHADER_ERROR_HLSL_INVALID_DOMAIN              = 5035,
    VKD3D_SHADER_ERROR_HLSL_INVALID_CONTROL_POINT_COUNT = 5036,
    VKD3D_SHADER_ERROR_HLSL_INVALID_OUTPUT_PRIMITIVE    = 5037,
    VKD3D_SHADER_ERROR_HLSL_INVALID_PARTITIONING        = 5038,
    VKD3D_SHADER_ERROR_HLSL_MISPLACED_SAMPLER_STATE     = 5039,
    VKD3D_SHADER_ERROR_HLSL_AMBIGUOUS_CALL              = 5040,
    VKD3D_SHADER_ERROR_HLSL_DUPLICATE_PATCH             = 5041,
    VKD3D_SHADER_ERROR_HLSL_INVALID_MAX_VERTEX_COUNT    = 5042,
    VKD3D_SHADER_ERROR_HLSL_MISSING_PRIMITIVE_TYPE      = 5043,
    VKD3D_SHADER_ERROR_HLSL_MISPLACED_STREAM_OUTPUT     = 5044,
    VKD3D_SHADER_ERROR_HLSL_MISSING_INPUT_PATCH         = 5045,

    VKD3D_SHADER_WARNING_HLSL_IMPLICIT_TRUNCATION       = 5300,
    VKD3D_SHADER_WARNING_HLSL_DIVISION_BY_ZERO          = 5301,
    VKD3D_SHADER_WARNING_HLSL_UNKNOWN_ATTRIBUTE         = 5302,
    VKD3D_SHADER_WARNING_HLSL_IMAGINARY_NUMERIC_RESULT  = 5303,
    VKD3D_SHADER_WARNING_HLSL_NON_FINITE_RESULT         = 5304,
    VKD3D_SHADER_WARNING_HLSL_IGNORED_ATTRIBUTE         = 5305,
    VKD3D_SHADER_WARNING_HLSL_IGNORED_DEFAULT_VALUE     = 5306,
    VKD3D_SHADER_WARNING_HLSL_IGNORED_MODIFIER          = 5307,

    VKD3D_SHADER_ERROR_GLSL_INTERNAL                    = 6000,
    VKD3D_SHADER_ERROR_GLSL_BINDING_NOT_FOUND           = 6001,
    VKD3D_SHADER_ERROR_GLSL_UNSUPPORTED                 = 6002,

    VKD3D_SHADER_ERROR_D3DBC_UNEXPECTED_EOF             = 7000,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_VERSION_TOKEN      = 7001,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_OPCODE             = 7002,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_RESOURCE_TYPE      = 7003,
    VKD3D_SHADER_ERROR_D3DBC_OUT_OF_MEMORY              = 7004,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_REGISTER_INDEX     = 7005,
    VKD3D_SHADER_ERROR_D3DBC_UNDECLARED_SEMANTIC        = 7006,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_REGISTER_TYPE      = 7007,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_REGISTER_COUNT     = 7008,
    VKD3D_SHADER_ERROR_D3DBC_NOT_IMPLEMENTED            = 7009,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_PROFILE            = 7010,
    VKD3D_SHADER_ERROR_D3DBC_INVALID_WRITEMASK          = 7011,

    VKD3D_SHADER_WARNING_D3DBC_IGNORED_INSTRUCTION_FLAGS= 7300,

    VKD3D_SHADER_ERROR_DXIL_OUT_OF_MEMORY               = 8000,
    VKD3D_SHADER_ERROR_DXIL_INVALID_SIZE                = 8001,
    VKD3D_SHADER_ERROR_DXIL_INVALID_CHUNK_OFFSET        = 8002,
    VKD3D_SHADER_ERROR_DXIL_INVALID_CHUNK_SIZE          = 8003,
    VKD3D_SHADER_ERROR_DXIL_INVALID_BITCODE             = 8004,
    VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND_COUNT       = 8005,
    VKD3D_SHADER_ERROR_DXIL_INVALID_TYPE_TABLE          = 8006,
    VKD3D_SHADER_ERROR_DXIL_INVALID_VALUE_SYMTAB        = 8007,
    VKD3D_SHADER_ERROR_DXIL_UNSUPPORTED_BITCODE_FORMAT  = 8008,
    VKD3D_SHADER_ERROR_DXIL_INVALID_FUNCTION_DCL        = 8009,
    VKD3D_SHADER_ERROR_DXIL_INVALID_TYPE_ID             = 8010,
    VKD3D_SHADER_ERROR_DXIL_INVALID_MODULE              = 8011,
    VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND             = 8012,
    VKD3D_SHADER_ERROR_DXIL_UNHANDLED_INTRINSIC         = 8013,
    VKD3D_SHADER_ERROR_DXIL_INVALID_METADATA            = 8014,
    VKD3D_SHADER_ERROR_DXIL_INVALID_ENTRY_POINT         = 8015,
    VKD3D_SHADER_ERROR_DXIL_INVALID_SIGNATURE           = 8016,
    VKD3D_SHADER_ERROR_DXIL_INVALID_PROPERTIES          = 8017,
    VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES           = 8018,
    VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCE_HANDLE     = 8019,
    VKD3D_SHADER_ERROR_DXIL_INVALID_CONSTANT            = 8020,

    VKD3D_SHADER_WARNING_DXIL_UNKNOWN_MAGIC_NUMBER      = 8300,
    VKD3D_SHADER_WARNING_DXIL_UNKNOWN_SHADER_TYPE       = 8301,
    VKD3D_SHADER_WARNING_DXIL_INVALID_BLOCK_LENGTH      = 8302,
    VKD3D_SHADER_WARNING_DXIL_INVALID_MODULE_LENGTH     = 8303,
    VKD3D_SHADER_WARNING_DXIL_IGNORING_OPERANDS         = 8304,
    VKD3D_SHADER_WARNING_DXIL_TYPE_MISMATCH             = 8305,
    VKD3D_SHADER_WARNING_DXIL_ENTRY_POINT_MISMATCH      = 8306,
    VKD3D_SHADER_WARNING_DXIL_INVALID_MASK              = 8307,
    VKD3D_SHADER_WARNING_DXIL_INVALID_OPERATION         = 8308,
    VKD3D_SHADER_WARNING_DXIL_IGNORING_ATTACHMENT       = 8309,
    VKD3D_SHADER_WARNING_DXIL_UNDEFINED_OPERAND         = 8310,

    VKD3D_SHADER_ERROR_VSIR_NOT_IMPLEMENTED             = 9000,
    VKD3D_SHADER_ERROR_VSIR_INVALID_OPCODE              = 9001,
    VKD3D_SHADER_ERROR_VSIR_INVALID_REGISTER_TYPE       = 9002,
    VKD3D_SHADER_ERROR_VSIR_INVALID_WRITE_MASK          = 9003,
    VKD3D_SHADER_ERROR_VSIR_INVALID_MODIFIERS           = 9004,
    VKD3D_SHADER_ERROR_VSIR_INVALID_SHIFT               = 9005,
    VKD3D_SHADER_ERROR_VSIR_INVALID_SWIZZLE             = 9006,
    VKD3D_SHADER_ERROR_VSIR_INVALID_PRECISION           = 9007,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DATA_TYPE           = 9008,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DIMENSION           = 9009,
    VKD3D_SHADER_ERROR_VSIR_INVALID_INDEX_COUNT         = 9010,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DEST_COUNT          = 9011,
    VKD3D_SHADER_ERROR_VSIR_INVALID_SOURCE_COUNT        = 9012,
    VKD3D_SHADER_ERROR_VSIR_DUPLICATE_DCL_TEMPS         = 9013,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DCL_TEMPS           = 9014,
    VKD3D_SHADER_ERROR_VSIR_INVALID_INDEX               = 9015,
    VKD3D_SHADER_ERROR_VSIR_INVALID_CONTROL_FLOW        = 9016,
    VKD3D_SHADER_ERROR_VSIR_INVALID_SSA_USAGE           = 9017,
    VKD3D_SHADER_ERROR_VSIR_INVALID_TESSELLATION        = 9018,
    VKD3D_SHADER_ERROR_VSIR_INVALID_GS                  = 9019,
    VKD3D_SHADER_ERROR_VSIR_INVALID_PARAMETER           = 9020,
    VKD3D_SHADER_ERROR_VSIR_MISSING_SEMANTIC            = 9021,
    VKD3D_SHADER_ERROR_VSIR_INVALID_SIGNATURE           = 9022,
    VKD3D_SHADER_ERROR_VSIR_INVALID_RANGE               = 9023,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DESCRIPTOR_TYPE     = 9024,
    VKD3D_SHADER_ERROR_VSIR_INVALID_RESOURCE_TYPE       = 9025,
    VKD3D_SHADER_ERROR_VSIR_INVALID_DESCRIPTOR_COUNT    = 9026,

    VKD3D_SHADER_WARNING_VSIR_DYNAMIC_DESCRIPTOR_ARRAY  = 9300,

    VKD3D_SHADER_ERROR_MSL_INTERNAL                     = 10000,
    VKD3D_SHADER_ERROR_MSL_BINDING_NOT_FOUND            = 10001,
    VKD3D_SHADER_ERROR_MSL_UNSUPPORTED                  = 10002,

    VKD3D_SHADER_ERROR_FX_NOT_IMPLEMENTED               = 11000,
    VKD3D_SHADER_ERROR_FX_INVALID_VERSION               = 11001,
    VKD3D_SHADER_ERROR_FX_INVALID_DATA                  = 11002,
    VKD3D_SHADER_ERROR_FX_INVALID_SIZE                  = 11003,
    VKD3D_SHADER_ERROR_FX_OUT_OF_MEMORY                 = 11004,
};

enum vkd3d_shader_opcode
{
    VSIR_OP_ABS,
    VSIR_OP_ACOS,
    VSIR_OP_ADD,
    VSIR_OP_AND,
    VSIR_OP_ASIN,
    VSIR_OP_ATAN,
    VSIR_OP_ATOMIC_AND,
    VSIR_OP_ATOMIC_CMP_STORE,
    VSIR_OP_ATOMIC_IADD,
    VSIR_OP_ATOMIC_IMAX,
    VSIR_OP_ATOMIC_IMIN,
    VSIR_OP_ATOMIC_OR,
    VSIR_OP_ATOMIC_UMAX,
    VSIR_OP_ATOMIC_UMIN,
    VSIR_OP_ATOMIC_XOR,
    VSIR_OP_BEM,
    VSIR_OP_BFI,
    VSIR_OP_BFREV,
    VSIR_OP_BRANCH,
    VSIR_OP_BREAK,
    VSIR_OP_BREAKC,
    VSIR_OP_BREAKP,
    VSIR_OP_BUFINFO,
    VSIR_OP_CALL,
    VSIR_OP_CALLNZ,
    VSIR_OP_CASE,
    VSIR_OP_CHECK_ACCESS_FULLY_MAPPED,
    VSIR_OP_CMP,
    VSIR_OP_CND,
    VSIR_OP_CONTINUE,
    VSIR_OP_CONTINUEP,
    VSIR_OP_COS,
    VSIR_OP_COUNTBITS,
    VSIR_OP_CRS,
    VSIR_OP_CUT,
    VSIR_OP_CUT_STREAM,
    VSIR_OP_DADD,
    VSIR_OP_DCL,
    VSIR_OP_DCL_CONSTANT_BUFFER,
    VSIR_OP_DCL_FUNCTION_BODY,
    VSIR_OP_DCL_FUNCTION_TABLE,
    VSIR_OP_DCL_GLOBAL_FLAGS,
    VSIR_OP_DCL_GS_INSTANCES,
    VSIR_OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT,
    VSIR_OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT,
    VSIR_OP_DCL_HS_MAX_TESSFACTOR,
    VSIR_OP_DCL_IMMEDIATE_CONSTANT_BUFFER,
    VSIR_OP_DCL_INDEX_RANGE,
    VSIR_OP_DCL_INDEXABLE_TEMP,
    VSIR_OP_DCL_INPUT,
    VSIR_OP_DCL_INPUT_CONTROL_POINT_COUNT,
    VSIR_OP_DCL_INPUT_PRIMITIVE,
    VSIR_OP_DCL_INPUT_PS,
    VSIR_OP_DCL_INPUT_PS_SGV,
    VSIR_OP_DCL_INPUT_PS_SIV,
    VSIR_OP_DCL_INPUT_SGV,
    VSIR_OP_DCL_INPUT_SIV,
    VSIR_OP_DCL_INTERFACE,
    VSIR_OP_DCL_OUTPUT,
    VSIR_OP_DCL_OUTPUT_CONTROL_POINT_COUNT,
    VSIR_OP_DCL_OUTPUT_SGV,
    VSIR_OP_DCL_OUTPUT_SIV,
    VSIR_OP_DCL_OUTPUT_TOPOLOGY,
    VSIR_OP_DCL_RESOURCE_RAW,
    VSIR_OP_DCL_RESOURCE_STRUCTURED,
    VSIR_OP_DCL_SAMPLER,
    VSIR_OP_DCL_STREAM,
    VSIR_OP_DCL_TEMPS,
    VSIR_OP_DCL_TESSELLATOR_DOMAIN,
    VSIR_OP_DCL_TESSELLATOR_OUTPUT_PRIMITIVE,
    VSIR_OP_DCL_TESSELLATOR_PARTITIONING,
    VSIR_OP_DCL_TGSM_RAW,
    VSIR_OP_DCL_TGSM_STRUCTURED,
    VSIR_OP_DCL_THREAD_GROUP,
    VSIR_OP_DCL_UAV_RAW,
    VSIR_OP_DCL_UAV_STRUCTURED,
    VSIR_OP_DCL_UAV_TYPED,
    VSIR_OP_DCL_VERTICES_OUT,
    VSIR_OP_DDIV,
    VSIR_OP_DEF,
    VSIR_OP_DEFAULT,
    VSIR_OP_DEFB,
    VSIR_OP_DEFI,
    VSIR_OP_DEQO,
    VSIR_OP_DFMA,
    VSIR_OP_DGEO,
    VSIR_OP_DISCARD,
    VSIR_OP_DIV,
    VSIR_OP_DLT,
    VSIR_OP_DMAX,
    VSIR_OP_DMIN,
    VSIR_OP_DMOV,
    VSIR_OP_DMOVC,
    VSIR_OP_DMUL,
    VSIR_OP_DNE,
    VSIR_OP_DP2,
    VSIR_OP_DP2ADD,
    VSIR_OP_DP3,
    VSIR_OP_DP4,
    VSIR_OP_DRCP,
    VSIR_OP_DST,
    VSIR_OP_DSX,
    VSIR_OP_DSX_COARSE,
    VSIR_OP_DSX_FINE,
    VSIR_OP_DSY,
    VSIR_OP_DSY_COARSE,
    VSIR_OP_DSY_FINE,
    VSIR_OP_DTOF,
    VSIR_OP_DTOI,
    VSIR_OP_DTOU,
    VSIR_OP_ELSE,
    VSIR_OP_EMIT,
    VSIR_OP_EMIT_STREAM,
    VSIR_OP_ENDIF,
    VSIR_OP_ENDLOOP,
    VSIR_OP_ENDREP,
    VSIR_OP_ENDSWITCH,
    VSIR_OP_EQO,
    VSIR_OP_EQU,
    VSIR_OP_EVAL_CENTROID,
    VSIR_OP_EVAL_SAMPLE_INDEX,
    VSIR_OP_EXP,
    VSIR_OP_EXPP,
    VSIR_OP_F16TOF32,
    VSIR_OP_F32TOF16,
    VSIR_OP_FCALL,
    VSIR_OP_FIRSTBIT_HI,
    VSIR_OP_FIRSTBIT_LO,
    VSIR_OP_FIRSTBIT_SHI,
    VSIR_OP_FRC,
    VSIR_OP_FREM,
    VSIR_OP_FTOD,
    VSIR_OP_FTOI,
    VSIR_OP_FTOU,
    VSIR_OP_GATHER4,
    VSIR_OP_GATHER4_C,
    VSIR_OP_GATHER4_C_S,
    VSIR_OP_GATHER4_PO,
    VSIR_OP_GATHER4_PO_C,
    VSIR_OP_GATHER4_PO_C_S,
    VSIR_OP_GATHER4_PO_S,
    VSIR_OP_GATHER4_S,
    VSIR_OP_GEO,
    VSIR_OP_GEU,
    VSIR_OP_HCOS,
    VSIR_OP_HS_CONTROL_POINT_PHASE,
    VSIR_OP_HS_DECLS,
    VSIR_OP_HS_FORK_PHASE,
    VSIR_OP_HS_JOIN_PHASE,
    VSIR_OP_HSIN,
    VSIR_OP_HTAN,
    VSIR_OP_IADD,
    VSIR_OP_IBFE,
    VSIR_OP_IDIV,
    VSIR_OP_IEQ,
    VSIR_OP_IF,
    VSIR_OP_IFC,
    VSIR_OP_IGE,
    VSIR_OP_ILT,
    VSIR_OP_IMAD,
    VSIR_OP_IMAX,
    VSIR_OP_IMIN,
    VSIR_OP_IMM_ATOMIC_ALLOC,
    VSIR_OP_IMM_ATOMIC_AND,
    VSIR_OP_IMM_ATOMIC_CMP_EXCH,
    VSIR_OP_IMM_ATOMIC_CONSUME,
    VSIR_OP_IMM_ATOMIC_EXCH,
    VSIR_OP_IMM_ATOMIC_IADD,
    VSIR_OP_IMM_ATOMIC_IMAX,
    VSIR_OP_IMM_ATOMIC_IMIN,
    VSIR_OP_IMM_ATOMIC_OR,
    VSIR_OP_IMM_ATOMIC_UMAX,
    VSIR_OP_IMM_ATOMIC_UMIN,
    VSIR_OP_IMM_ATOMIC_XOR,
    VSIR_OP_IMUL,
    VSIR_OP_IMUL_LOW,
    VSIR_OP_INE,
    VSIR_OP_INEG,
    VSIR_OP_IREM,
    VSIR_OP_ISFINITE,
    VSIR_OP_ISHL,
    VSIR_OP_ISHR,
    VSIR_OP_ISINF,
    VSIR_OP_ISNAN,
    VSIR_OP_ITOD,
    VSIR_OP_ITOF,
    VSIR_OP_ITOI,
    VSIR_OP_LABEL,
    VSIR_OP_LD,
    VSIR_OP_LD2DMS,
    VSIR_OP_LD2DMS_S,
    VSIR_OP_LD_RAW,
    VSIR_OP_LD_RAW_S,
    VSIR_OP_LD_S,
    VSIR_OP_LD_STRUCTURED,
    VSIR_OP_LD_STRUCTURED_S,
    VSIR_OP_LD_UAV_TYPED,
    VSIR_OP_LD_UAV_TYPED_S,
    VSIR_OP_LIT,
    VSIR_OP_LOD,
    VSIR_OP_LOG,
    VSIR_OP_LOGP,
    VSIR_OP_LOOP,
    VSIR_OP_LRP,
    VSIR_OP_LTO,
    VSIR_OP_LTU,
    VSIR_OP_M3x2,
    VSIR_OP_M3x3,
    VSIR_OP_M3x4,
    VSIR_OP_M4x3,
    VSIR_OP_M4x4,
    VSIR_OP_MAD,
    VSIR_OP_MAX,
    VSIR_OP_MIN,
    VSIR_OP_MOV,
    VSIR_OP_MOVA,
    VSIR_OP_MOVC,
    VSIR_OP_MSAD,
    VSIR_OP_MUL,
    VSIR_OP_NEO,
    VSIR_OP_NEU,
    VSIR_OP_NOP,
    VSIR_OP_NOT,
    VSIR_OP_NRM,
    VSIR_OP_OR,
    VSIR_OP_ORD,
    VSIR_OP_PHASE,
    VSIR_OP_PHI,
    VSIR_OP_POW,
    VSIR_OP_QUAD_READ_ACROSS_D,
    VSIR_OP_QUAD_READ_ACROSS_X,
    VSIR_OP_QUAD_READ_ACROSS_Y,
    VSIR_OP_QUAD_READ_LANE_AT,
    VSIR_OP_RCP,
    VSIR_OP_REP,
    VSIR_OP_RESINFO,
    VSIR_OP_RET,
    VSIR_OP_RETP,
    VSIR_OP_ROUND_NE,
    VSIR_OP_ROUND_NI,
    VSIR_OP_ROUND_PI,
    VSIR_OP_ROUND_Z,
    VSIR_OP_RSQ,
    VSIR_OP_SAMPLE,
    VSIR_OP_SAMPLE_B,
    VSIR_OP_SAMPLE_B_CL_S,
    VSIR_OP_SAMPLE_C,
    VSIR_OP_SAMPLE_C_CL_S,
    VSIR_OP_SAMPLE_C_LZ,
    VSIR_OP_SAMPLE_C_LZ_S,
    VSIR_OP_SAMPLE_CL_S,
    VSIR_OP_SAMPLE_GRAD,
    VSIR_OP_SAMPLE_GRAD_CL_S,
    VSIR_OP_SAMPLE_INFO,
    VSIR_OP_SAMPLE_LOD,
    VSIR_OP_SAMPLE_LOD_S,
    VSIR_OP_SAMPLE_POS,
    VSIR_OP_SETP,
    VSIR_OP_SGE,
    VSIR_OP_SGN,
    VSIR_OP_SIN,
    VSIR_OP_SINCOS,
    VSIR_OP_SLT,
    VSIR_OP_SQRT,
    VSIR_OP_STORE_RAW,
    VSIR_OP_STORE_STRUCTURED,
    VSIR_OP_STORE_UAV_TYPED,
    VSIR_OP_SUB,
    VSIR_OP_SWAPC,
    VSIR_OP_SWITCH,
    VSIR_OP_SWITCH_MONOLITHIC,
    VSIR_OP_SYNC,
    VSIR_OP_TAN,
    VSIR_OP_TEX,
    VSIR_OP_TEXBEM,
    VSIR_OP_TEXBEML,
    VSIR_OP_TEXCOORD,
    VSIR_OP_TEXCRD,
    VSIR_OP_TEXDEPTH,
    VSIR_OP_TEXDP3,
    VSIR_OP_TEXDP3TEX,
    VSIR_OP_TEXKILL,
    VSIR_OP_TEXLD,
    VSIR_OP_TEXLDD,
    VSIR_OP_TEXLDL,
    VSIR_OP_TEXM3x2DEPTH,
    VSIR_OP_TEXM3x2PAD,
    VSIR_OP_TEXM3x2TEX,
    VSIR_OP_TEXM3x3,
    VSIR_OP_TEXM3x3DIFF,
    VSIR_OP_TEXM3x3PAD,
    VSIR_OP_TEXM3x3SPEC,
    VSIR_OP_TEXM3x3TEX,
    VSIR_OP_TEXM3x3VSPEC,
    VSIR_OP_TEXREG2AR,
    VSIR_OP_TEXREG2GB,
    VSIR_OP_TEXREG2RGB,
    VSIR_OP_UBFE,
    VSIR_OP_UDIV,
    VSIR_OP_UDIV_SIMPLE,
    VSIR_OP_UGE,
    VSIR_OP_ULT,
    VSIR_OP_UMAX,
    VSIR_OP_UMIN,
    VSIR_OP_UMUL,
    VSIR_OP_UNO,
    VSIR_OP_UREM,
    VSIR_OP_USHR,
    VSIR_OP_UTOD,
    VSIR_OP_UTOF,
    VSIR_OP_UTOU,
    VSIR_OP_WAVE_ACTIVE_ALL_EQUAL,
    VSIR_OP_WAVE_ACTIVE_BALLOT,
    VSIR_OP_WAVE_ACTIVE_BIT_AND,
    VSIR_OP_WAVE_ACTIVE_BIT_OR,
    VSIR_OP_WAVE_ACTIVE_BIT_XOR,
    VSIR_OP_WAVE_ALL_BIT_COUNT,
    VSIR_OP_WAVE_ALL_TRUE,
    VSIR_OP_WAVE_ANY_TRUE,
    VSIR_OP_WAVE_IS_FIRST_LANE,
    VSIR_OP_WAVE_OP_ADD,
    VSIR_OP_WAVE_OP_IMAX,
    VSIR_OP_WAVE_OP_IMIN,
    VSIR_OP_WAVE_OP_MAX,
    VSIR_OP_WAVE_OP_MIN,
    VSIR_OP_WAVE_OP_MUL,
    VSIR_OP_WAVE_OP_UMAX,
    VSIR_OP_WAVE_OP_UMIN,
    VSIR_OP_WAVE_PREFIX_BIT_COUNT,
    VSIR_OP_WAVE_READ_LANE_AT,
    VSIR_OP_WAVE_READ_LANE_FIRST,
    VSIR_OP_XOR,

    VSIR_OP_INVALID,

    VSIR_OP_COUNT,
};

const char *vsir_opcode_get_name(enum vkd3d_shader_opcode op, const char *error);

enum vkd3d_shader_register_type
{
    VKD3DSPR_TEMP,
    VKD3DSPR_INPUT,
    VKD3DSPR_CONST,
    VKD3DSPR_ADDR,
    VKD3DSPR_TEXTURE,
    VKD3DSPR_RASTOUT,
    VKD3DSPR_ATTROUT,
    VKD3DSPR_TEXCRDOUT,
    VKD3DSPR_OUTPUT,
    VKD3DSPR_CONSTINT,
    VKD3DSPR_COLOROUT,
    VKD3DSPR_DEPTHOUT,
    VKD3DSPR_COMBINED_SAMPLER,
    VKD3DSPR_CONSTBOOL,
    VKD3DSPR_LOOP,
    VKD3DSPR_TEMPFLOAT16,
    VKD3DSPR_MISCTYPE,
    VKD3DSPR_LABEL,
    VKD3DSPR_PREDICATE,
    VKD3DSPR_IMMCONST,
    VKD3DSPR_IMMCONST64,
    VKD3DSPR_CONSTBUFFER,
    VKD3DSPR_IMMCONSTBUFFER,
    VKD3DSPR_PRIMID,
    VKD3DSPR_NULL,
    VKD3DSPR_SAMPLER,
    VKD3DSPR_RESOURCE,
    VKD3DSPR_UAV,
    VKD3DSPR_OUTPOINTID,
    VKD3DSPR_FORKINSTID,
    VKD3DSPR_JOININSTID,
    VKD3DSPR_INCONTROLPOINT,
    VKD3DSPR_OUTCONTROLPOINT,
    VKD3DSPR_PATCHCONST,
    VKD3DSPR_TESSCOORD,
    VKD3DSPR_GROUPSHAREDMEM,
    VKD3DSPR_THREADID,
    VKD3DSPR_THREADGROUPID,
    VKD3DSPR_LOCALTHREADID,
    VKD3DSPR_LOCALTHREADINDEX,
    VKD3DSPR_IDXTEMP,
    VKD3DSPR_STREAM,
    VKD3DSPR_FUNCTIONBODY,
    VKD3DSPR_FUNCTIONPOINTER,
    VKD3DSPR_COVERAGE,
    VKD3DSPR_SAMPLEMASK,
    VKD3DSPR_GSINSTID,
    VKD3DSPR_DEPTHOUTGE,
    VKD3DSPR_DEPTHOUTLE,
    VKD3DSPR_RASTERIZER,
    VKD3DSPR_OUTSTENCILREF,
    VKD3DSPR_UNDEF,
    VKD3DSPR_SSA,
    VKD3DSPR_WAVELANECOUNT,
    VKD3DSPR_WAVELANEINDEX,
    VKD3DSPR_PARAMETER,
    VKD3DSPR_POINT_COORD,

    VKD3DSPR_COUNT,

    VKD3DSPR_INVALID = ~0u,
};

enum vsir_rastout_register
{
    VSIR_RASTOUT_POSITION   = 0x0,
    VSIR_RASTOUT_FOG        = 0x1,
    VSIR_RASTOUT_POINT_SIZE = 0x2,
};

enum vkd3d_shader_register_precision
{
    VKD3D_SHADER_REGISTER_PRECISION_DEFAULT,
    VKD3D_SHADER_REGISTER_PRECISION_MIN_FLOAT_16,
    VKD3D_SHADER_REGISTER_PRECISION_MIN_FLOAT_10,
    VKD3D_SHADER_REGISTER_PRECISION_MIN_INT_16,
    VKD3D_SHADER_REGISTER_PRECISION_MIN_UINT_16,

    VKD3D_SHADER_REGISTER_PRECISION_COUNT,

    VKD3D_SHADER_REGISTER_PRECISION_INVALID = ~0u,
};

enum vkd3d_data_type
{
    VKD3D_DATA_FLOAT,
    VKD3D_DATA_INT,
    VKD3D_DATA_UINT,
    VKD3D_DATA_UNORM,
    VKD3D_DATA_SNORM,
    VKD3D_DATA_OPAQUE,
    VKD3D_DATA_MIXED,
    VKD3D_DATA_DOUBLE,
    VKD3D_DATA_CONTINUED,
    VKD3D_DATA_UNUSED,
    VKD3D_DATA_UINT8,
    VKD3D_DATA_UINT64,
    VKD3D_DATA_BOOL,
    VKD3D_DATA_UINT16,
    VKD3D_DATA_HALF,

    VKD3D_DATA_COUNT,
};

static inline bool data_type_is_integer(enum vkd3d_data_type data_type)
{
    return data_type == VKD3D_DATA_INT || data_type == VKD3D_DATA_UINT8 || data_type == VKD3D_DATA_UINT16
            || data_type == VKD3D_DATA_UINT || data_type == VKD3D_DATA_UINT64;
}

static inline bool data_type_is_bool(enum vkd3d_data_type data_type)
{
    return data_type == VKD3D_DATA_BOOL;
}

static inline bool data_type_is_floating_point(enum vkd3d_data_type data_type)
{
    return data_type == VKD3D_DATA_HALF || data_type == VKD3D_DATA_FLOAT || data_type == VKD3D_DATA_DOUBLE;
}

static inline bool data_type_is_64_bit(enum vkd3d_data_type data_type)
{
    return data_type == VKD3D_DATA_DOUBLE || data_type == VKD3D_DATA_UINT64;
}

enum vsir_dimension
{
    VSIR_DIMENSION_NONE,
    VSIR_DIMENSION_SCALAR,
    VSIR_DIMENSION_VEC4,

    VSIR_DIMENSION_COUNT,
};

enum vkd3d_shader_src_modifier
{
    VKD3DSPSM_NONE = 0,
    VKD3DSPSM_NEG = 1,
    VKD3DSPSM_BIAS = 2,
    VKD3DSPSM_BIASNEG = 3,
    VKD3DSPSM_SIGN = 4,
    VKD3DSPSM_SIGNNEG = 5,
    VKD3DSPSM_COMP = 6,
    VKD3DSPSM_X2 = 7,
    VKD3DSPSM_X2NEG = 8,
    VKD3DSPSM_DZ = 9,
    VKD3DSPSM_DW = 10,
    VKD3DSPSM_ABS = 11,
    VKD3DSPSM_ABSNEG = 12,
    VKD3DSPSM_NOT = 13,
    VKD3DSPSM_COUNT,
};

#define VKD3DSP_WRITEMASK_0   0x1u /* .x r */
#define VKD3DSP_WRITEMASK_1   0x2u /* .y g */
#define VKD3DSP_WRITEMASK_2   0x4u /* .z b */
#define VKD3DSP_WRITEMASK_3   0x8u /* .w a */
#define VKD3DSP_WRITEMASK_ALL 0xfu /* all */

enum vkd3d_shader_dst_modifier
{
    VKD3DSPDM_NONE = 0,
    VKD3DSPDM_SATURATE = 1,
    VKD3DSPDM_PARTIALPRECISION = 2,
    VKD3DSPDM_MSAMPCENTROID = 4,
    VKD3DSPDM_MASK = 7,
};

enum vkd3d_shader_interpolation_mode
{
    VKD3DSIM_NONE = 0,
    VKD3DSIM_CONSTANT = 1,
    VKD3DSIM_LINEAR = 2,
    VKD3DSIM_LINEAR_CENTROID = 3,
    VKD3DSIM_LINEAR_NOPERSPECTIVE = 4,
    VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID = 5,
    VKD3DSIM_LINEAR_SAMPLE = 6,
    VKD3DSIM_LINEAR_NOPERSPECTIVE_SAMPLE = 7,

    VKD3DSIM_COUNT = 8,
};

enum vsir_global_flags
{
    VKD3DSGF_REFACTORING_ALLOWED               = 0x01,
    VKD3DSGF_ENABLE_DOUBLE_PRECISION_FLOAT_OPS = 0x02,
    VKD3DSGF_FORCE_EARLY_DEPTH_STENCIL         = 0x04,
    VKD3DSGF_ENABLE_RAW_AND_STRUCTURED_BUFFERS = 0x08,
    VKD3DSGF_SKIP_OPTIMIZATION                 = 0x10,
    VKD3DSGF_ENABLE_MINIMUM_PRECISION          = 0x20,
    VKD3DSGF_ENABLE_11_1_DOUBLE_EXTENSIONS     = 0x40,
    VKD3DSGF_ENABLE_SHADER_EXTENSIONS          = 0x80, /* never emitted? */
    VKD3DSGF_BIND_FOR_DURATION                 =     0x100,
    VKD3DSGF_ENABLE_VP_AND_RT_ARRAY_INDEX      =     0x200,
    VKD3DSGF_ENABLE_INNER_COVERAGE             =     0x400,
    VKD3DSGF_ENABLE_STENCIL_REF                =     0x800,
    VKD3DSGF_ENABLE_TILED_RESOURCE_INTRINSICS  =    0x1000,
    VKD3DSGF_ENABLE_RELAXED_TYPED_UAV_FORMATS  =    0x2000,
    VKD3DSGF_ENABLE_LVL_9_COMPARISON_FILTERING =    0x4000,
    VKD3DSGF_ENABLE_UP_TO_64_UAVS              =    0x8000,
    VKD3DSGF_ENABLE_UAVS_AT_EVERY_STAGE        =   0x10000,
    VKD3DSGF_ENABLE_CS4_RAW_STRUCTURED_BUFFERS =   0x20000,
    VKD3DSGF_ENABLE_RASTERIZER_ORDERED_VIEWS   =   0x40000,
    VKD3DSGF_ENABLE_WAVE_INTRINSICS            =   0x80000,
    VKD3DSGF_ENABLE_INT64                      =  0x100000,
    VKD3DSGF_ENABLE_VIEWID                     =  0x200000,
    VKD3DSGF_ENABLE_BARYCENTRICS               =  0x400000,
    VKD3DSGF_FORCE_NATIVE_LOW_PRECISION        =  0x800000,
    VKD3DSGF_ENABLE_SHADINGRATE                = 0x1000000,
    VKD3DSGF_ENABLE_RAYTRACING_TIER_1_1        = 0x2000000,
    VKD3DSGF_ENABLE_SAMPLER_FEEDBACK           = 0x4000000,
    VKD3DSGF_ENABLE_ATOMIC_INT64_ON_TYPED_RESOURCE                =   0x8000000,
    VKD3DSGF_ENABLE_ATOMIC_INT64_ON_GROUP_SHARED                  =  0x10000000,
    VKD3DSGF_ENABLE_DERIVATIVES_IN_MESH_AND_AMPLIFICATION_SHADERS =  0x20000000,
    VKD3DSGF_ENABLE_RESOURCE_DESCRIPTOR_HEAP_INDEXING             =  0x40000000,
    VKD3DSGF_ENABLE_SAMPLER_DESCRIPTOR_HEAP_INDEXING              =  0x80000000,
    VKD3DSGF_ENABLE_ATOMIC_INT64_ON_DESCRIPTOR_HEAP_RESOURCE      = 0x100000000ull,
};

enum vkd3d_shader_sync_flags
{
    VKD3DSSF_THREAD_GROUP        = 0x1,
    VKD3DSSF_GROUP_SHARED_MEMORY = 0x2,
    VKD3DSSF_THREAD_GROUP_UAV    = 0x4,
    VKD3DSSF_GLOBAL_UAV          = 0x8,
};

enum vkd3d_shader_uav_flags
{
    VKD3DSUF_GLOBALLY_COHERENT        = 0x002,
    VKD3DSUF_RASTERISER_ORDERED_VIEW  = 0x004,
    VKD3DSUF_ORDER_PRESERVING_COUNTER = 0x100,
};

enum vkd3d_shader_atomic_rmw_flags
{
    VKD3DARF_SEQ_CST  = 0x1,
    VKD3DARF_VOLATILE = 0x2,
};

enum vkd3d_tessellator_domain
{
    VKD3D_TESSELLATOR_DOMAIN_INVALID   = 0,

    VKD3D_TESSELLATOR_DOMAIN_LINE      = 1,
    VKD3D_TESSELLATOR_DOMAIN_TRIANGLE  = 2,
    VKD3D_TESSELLATOR_DOMAIN_QUAD      = 3,

    VKD3D_TESSELLATOR_DOMAIN_COUNT     = 4,
};

#define VKD3DSI_NONE                    0x0
#define VKD3DSI_TEXLD_PROJECT           0x1
#define VKD3DSI_TEXLD_BIAS              0x2
#define VKD3DSI_INDEXED_DYNAMIC         0x4
#define VKD3DSI_RESINFO_RCP_FLOAT       0x1
#define VKD3DSI_RESINFO_UINT            0x2
#define VKD3DSI_SAMPLE_INFO_UINT        0x1
#define VKD3DSI_SAMPLER_COMPARISON_MODE 0x1
#define VKD3DSI_SHIFT_UNMASKED          0x1
#define VKD3DSI_WAVE_PREFIX             0x1

#define VKD3DSI_PRECISE_X         0x100
#define VKD3DSI_PRECISE_Y         0x200
#define VKD3DSI_PRECISE_Z         0x400
#define VKD3DSI_PRECISE_W         0x800
#define VKD3DSI_PRECISE_XYZW      (VKD3DSI_PRECISE_X | VKD3DSI_PRECISE_Y \
                                  | VKD3DSI_PRECISE_Z | VKD3DSI_PRECISE_W)
#define VKD3DSI_PRECISE_SHIFT     8

enum vkd3d_shader_rel_op
{
    VKD3D_SHADER_REL_OP_GT = 1,
    VKD3D_SHADER_REL_OP_EQ = 2,
    VKD3D_SHADER_REL_OP_GE = 3,
    VKD3D_SHADER_REL_OP_LT = 4,
    VKD3D_SHADER_REL_OP_NE = 5,
    VKD3D_SHADER_REL_OP_LE = 6,
};

enum vkd3d_shader_conditional_op
{
    VKD3D_SHADER_CONDITIONAL_OP_NZ = 0,
    VKD3D_SHADER_CONDITIONAL_OP_Z  = 1
};

#define MAX_REG_OUTPUT 32

enum vkd3d_shader_type
{
    VKD3D_SHADER_TYPE_PIXEL,
    VKD3D_SHADER_TYPE_VERTEX,
    VKD3D_SHADER_TYPE_GEOMETRY,
    VKD3D_SHADER_TYPE_HULL,
    VKD3D_SHADER_TYPE_DOMAIN,
    VKD3D_SHADER_TYPE_GRAPHICS_COUNT,

    VKD3D_SHADER_TYPE_COMPUTE = VKD3D_SHADER_TYPE_GRAPHICS_COUNT,

    VKD3D_SHADER_TYPE_EFFECT,
    VKD3D_SHADER_TYPE_TEXTURE,
    VKD3D_SHADER_TYPE_LIBRARY,
    VKD3D_SHADER_TYPE_COUNT,
};

struct vkd3d_shader_message_context;

struct vkd3d_shader_version
{
    enum vkd3d_shader_type type;
    uint8_t major;
    uint8_t minor;
};

struct vkd3d_shader_immediate_constant_buffer
{
    unsigned int register_idx;
    enum vkd3d_data_type data_type;
    /* total count is element_count * component_count */
    unsigned int element_count;
    unsigned int component_count;
    bool is_null;
    uint32_t data[];
};

struct vkd3d_shader_indexable_temp
{
    unsigned int register_idx;
    unsigned int register_size;
    unsigned int alignment;
    enum vkd3d_data_type data_type;
    unsigned int component_count;
    bool has_function_scope;
    const struct vkd3d_shader_immediate_constant_buffer *initialiser;
};

struct vkd3d_shader_register_index
{
    struct vkd3d_shader_src_param *rel_addr;
    unsigned int offset;
    /* address is known to fall within the object (for optimisation) */
    bool is_in_bounds;
};

struct vkd3d_shader_register
{
    enum vkd3d_shader_register_type type;
    enum vkd3d_shader_register_precision precision;
    bool non_uniform;
    enum vkd3d_data_type data_type;
    struct vkd3d_shader_register_index idx[3];
    unsigned int idx_count;
    enum vsir_dimension dimension;
    /* known address alignment for optimisation, or zero */
    unsigned int alignment;
    union vsir_immediate_constant
    {
        uint32_t immconst_u32[VKD3D_VEC4_SIZE];
        float immconst_f32[VKD3D_VEC4_SIZE];
        uint64_t immconst_u64[VKD3D_DVEC2_SIZE];
        double immconst_f64[VKD3D_DVEC2_SIZE];
        unsigned fp_body_idx;
    } u;
};

void vsir_register_init(struct vkd3d_shader_register *reg, enum vkd3d_shader_register_type reg_type,
        enum vkd3d_data_type data_type, unsigned int idx_count);

static inline bool vsir_register_is_descriptor(const struct vkd3d_shader_register *reg)
{
    switch (reg->type)
    {
        case VKD3DSPR_SAMPLER:
        case VKD3DSPR_RESOURCE:
        case VKD3DSPR_CONSTBUFFER:
        case VKD3DSPR_UAV:
            return true;

        default:
            return false;
    }
}

struct vkd3d_shader_dst_param
{
    struct vkd3d_shader_register reg;
    uint32_t write_mask;
    uint32_t modifiers;
    unsigned int shift;
};

struct vkd3d_shader_src_param
{
    struct vkd3d_shader_register reg;
    uint32_t swizzle;
    enum vkd3d_shader_src_modifier modifiers;
};

void vsir_src_param_init(struct vkd3d_shader_src_param *param, enum vkd3d_shader_register_type reg_type,
        enum vkd3d_data_type data_type, unsigned int idx_count);
void vsir_dst_param_init(struct vkd3d_shader_dst_param *param, enum vkd3d_shader_register_type reg_type,
        enum vkd3d_data_type data_type, unsigned int idx_count);
void vsir_dst_param_init_null(struct vkd3d_shader_dst_param *dst);
void vsir_src_param_init_label(struct vkd3d_shader_src_param *param, unsigned int label_id);

struct vkd3d_shader_index_range
{
    struct vkd3d_shader_dst_param dst;
    unsigned int register_count;
};

struct vkd3d_shader_register_range
{
    unsigned int space;
    unsigned int first, last;
};

struct vkd3d_shader_resource
{
    struct vkd3d_shader_dst_param reg;
    struct vkd3d_shader_register_range range;
};

enum vkd3d_decl_usage
{
    VKD3D_DECL_USAGE_POSITION             = 0,
    VKD3D_DECL_USAGE_BLEND_WEIGHT         = 1,
    VKD3D_DECL_USAGE_BLEND_INDICES        = 2,
    VKD3D_DECL_USAGE_NORMAL               = 3,
    VKD3D_DECL_USAGE_PSIZE                = 4,
    VKD3D_DECL_USAGE_TEXCOORD             = 5,
    VKD3D_DECL_USAGE_TANGENT              = 6,
    VKD3D_DECL_USAGE_BINORMAL             = 7,
    VKD3D_DECL_USAGE_TESS_FACTOR          = 8,
    VKD3D_DECL_USAGE_POSITIONT            = 9,
    VKD3D_DECL_USAGE_COLOR                = 10,
    VKD3D_DECL_USAGE_FOG                  = 11,
    VKD3D_DECL_USAGE_DEPTH                = 12,
    VKD3D_DECL_USAGE_SAMPLE               = 13
};

struct vkd3d_shader_semantic
{
    enum vkd3d_decl_usage usage;
    unsigned int usage_idx;
    enum vkd3d_shader_resource_type resource_type;
    unsigned int sample_count;
    enum vkd3d_data_type resource_data_type[VKD3D_VEC4_SIZE];
    struct vkd3d_shader_resource resource;
};

enum vkd3d_shader_input_sysval_semantic
{
    VKD3D_SIV_NONE                         = 0,
    VKD3D_SIV_POSITION                     = 1,
    VKD3D_SIV_CLIP_DISTANCE                = 2,
    VKD3D_SIV_CULL_DISTANCE                = 3,
    VKD3D_SIV_RENDER_TARGET_ARRAY_INDEX    = 4,
    VKD3D_SIV_VIEWPORT_ARRAY_INDEX         = 5,
    VKD3D_SIV_VERTEX_ID                    = 6,
    VKD3D_SIV_PRIMITIVE_ID                 = 7,
    VKD3D_SIV_INSTANCE_ID                  = 8,
    VKD3D_SIV_IS_FRONT_FACE                = 9,
    VKD3D_SIV_SAMPLE_INDEX                 = 10,
    VKD3D_SIV_QUAD_U0_TESS_FACTOR          = 11,
    VKD3D_SIV_QUAD_V0_TESS_FACTOR          = 12,
    VKD3D_SIV_QUAD_U1_TESS_FACTOR          = 13,
    VKD3D_SIV_QUAD_V1_TESS_FACTOR          = 14,
    VKD3D_SIV_QUAD_U_INNER_TESS_FACTOR     = 15,
    VKD3D_SIV_QUAD_V_INNER_TESS_FACTOR     = 16,
    VKD3D_SIV_TRIANGLE_U_TESS_FACTOR       = 17,
    VKD3D_SIV_TRIANGLE_V_TESS_FACTOR       = 18,
    VKD3D_SIV_TRIANGLE_W_TESS_FACTOR       = 19,
    VKD3D_SIV_TRIANGLE_INNER_TESS_FACTOR   = 20,
    VKD3D_SIV_LINE_DETAIL_TESS_FACTOR      = 21,
    VKD3D_SIV_LINE_DENSITY_TESS_FACTOR     = 22,
};

#define SM1_COLOR_REGISTER_OFFSET 8
#define SM1_RASTOUT_REGISTER_OFFSET 10

#define SIGNATURE_TARGET_LOCATION_UNUSED (~0u)

struct signature_element
{
    /* sort_index is not a property of the signature element, it is just a
     * convenience field used to retain the original order in a signature and
     * recover it after having permuted the signature itself. */
    unsigned int sort_index;
    const char *semantic_name;
    unsigned int semantic_index;
    unsigned int stream_index;
    enum vkd3d_shader_sysval_semantic sysval_semantic;
    enum vkd3d_shader_component_type component_type;
    /* Register index in the source shader. */
    unsigned int register_index;
    unsigned int register_count;
    unsigned int mask;
    unsigned int used_mask;
    enum vkd3d_shader_minimum_precision min_precision;
    enum vkd3d_shader_interpolation_mode interpolation_mode;
    /* Register index / location in the target shader.
     * If SIGNATURE_TARGET_LOCATION_UNUSED, this element should not be written. */
    unsigned int target_location;
};

struct shader_signature
{
    struct signature_element *elements;
    size_t elements_capacity;
    unsigned int element_count;
};

static inline bool vsir_sysval_semantic_is_tess_factor(enum vkd3d_shader_sysval_semantic sysval_semantic)
{
    return sysval_semantic >= VKD3D_SHADER_SV_TESS_FACTOR_QUADEDGE
        && sysval_semantic <= VKD3D_SHADER_SV_TESS_FACTOR_LINEDEN;
}

static inline bool vsir_sysval_semantic_is_clip_cull(enum vkd3d_shader_sysval_semantic sysval_semantic)
{
    return sysval_semantic == VKD3D_SHADER_SV_CLIP_DISTANCE || sysval_semantic == VKD3D_SHADER_SV_CULL_DISTANCE;
}

struct signature_element *vsir_signature_find_element_for_reg(const struct shader_signature *signature,
        unsigned int reg_idx, unsigned int write_mask);
bool vsir_signature_find_sysval(const struct shader_signature *signature,
        enum vkd3d_shader_sysval_semantic sysval, unsigned int semantic_index, unsigned int *element_index);
void shader_signature_cleanup(struct shader_signature *signature);

struct vsir_features
{
    /* The shader requires rasteriser-ordered views. */
    bool rovs;
};

struct dxbc_shader_desc
{
    const uint32_t *byte_code;
    size_t byte_code_size;
    bool is_dxil;
    struct shader_signature input_signature;
    struct shader_signature output_signature;
    struct shader_signature patch_constant_signature;
    struct vsir_features features;
};

struct vkd3d_shader_register_semantic
{
    struct vkd3d_shader_dst_param reg;
    enum vkd3d_shader_input_sysval_semantic sysval_semantic;
};

struct vkd3d_shader_sampler
{
    struct vkd3d_shader_src_param src;
    struct vkd3d_shader_register_range range;
};

struct vkd3d_shader_constant_buffer
{
    struct vkd3d_shader_src_param src;
    unsigned int size;
    struct vkd3d_shader_register_range range;
};

struct vkd3d_shader_structured_resource
{
    struct vkd3d_shader_resource resource;
    unsigned int byte_stride;
};

struct vkd3d_shader_raw_resource
{
    struct vkd3d_shader_resource resource;
};

struct vkd3d_shader_tgsm
{
    unsigned int size;
    unsigned int stride;
};

struct vkd3d_shader_tgsm_raw
{
    struct vkd3d_shader_dst_param reg;
    unsigned int alignment;
    unsigned int byte_count;
    bool zero_init;
};

struct vkd3d_shader_tgsm_structured
{
    struct vkd3d_shader_dst_param reg;
    unsigned int alignment;
    unsigned int byte_stride;
    unsigned int structure_count;
    bool zero_init;
};

struct vsir_thread_group_size
{
    unsigned int x, y, z;
};

struct vkd3d_shader_function_table_pointer
{
    unsigned int index;
    unsigned int array_size;
    unsigned int body_count;
    unsigned int table_count;
};

struct vkd3d_shader_texel_offset
{
    signed char u, v, w;
};

enum vkd3d_primitive_type
{
    VKD3D_PT_UNDEFINED                    = 0,
    VKD3D_PT_POINTLIST                    = 1,
    VKD3D_PT_LINELIST                     = 2,
    VKD3D_PT_LINESTRIP                    = 3,
    VKD3D_PT_TRIANGLELIST                 = 4,
    VKD3D_PT_TRIANGLESTRIP                = 5,
    VKD3D_PT_TRIANGLEFAN                  = 6,
    VKD3D_PT_LINELIST_ADJ                 = 10,
    VKD3D_PT_LINESTRIP_ADJ                = 11,
    VKD3D_PT_TRIANGLELIST_ADJ             = 12,
    VKD3D_PT_TRIANGLESTRIP_ADJ            = 13,
    VKD3D_PT_PATCH                        = 14,

    VKD3D_PT_COUNT                        = 15,
};

struct vkd3d_shader_primitive_type
{
    enum vkd3d_primitive_type type;
    unsigned int patch_vertex_count;
};

struct vkd3d_shader_location
{
    const char *source_name;
    unsigned int line, column;
};

struct vkd3d_shader_instruction
{
    struct vkd3d_shader_location location;
    enum vkd3d_shader_opcode opcode;
    uint32_t flags;
    unsigned int dst_count;
    unsigned int src_count;
    struct vkd3d_shader_dst_param *dst;
    struct vkd3d_shader_src_param *src;
    struct vkd3d_shader_texel_offset texel_offset;
    enum vkd3d_shader_resource_type resource_type;
    unsigned int resource_stride;
    enum vkd3d_data_type resource_data_type[VKD3D_VEC4_SIZE];
    bool coissue, structured, raw;
    const struct vkd3d_shader_src_param *predicate;
    union
    {
        enum vsir_global_flags global_flags;
        struct vkd3d_shader_semantic semantic;
        struct vkd3d_shader_register_semantic register_semantic;
        struct vkd3d_shader_primitive_type primitive_type;
        struct vkd3d_shader_dst_param dst;
        struct vkd3d_shader_constant_buffer cb;
        struct vkd3d_shader_sampler sampler;
        unsigned int count;
        unsigned int index;
        const struct vkd3d_shader_immediate_constant_buffer *icb;
        struct vkd3d_shader_raw_resource raw_resource;
        struct vkd3d_shader_structured_resource structured_resource;
        struct vkd3d_shader_tgsm_raw tgsm_raw;
        struct vkd3d_shader_tgsm_structured tgsm_structured;
        struct vsir_thread_group_size thread_group_size;
        enum vkd3d_tessellator_domain tessellator_domain;
        enum vkd3d_shader_tessellator_output_primitive tessellator_output_primitive;
        enum vkd3d_shader_tessellator_partitioning tessellator_partitioning;
        float max_tessellation_factor;
        struct vkd3d_shader_index_range index_range;
        struct vkd3d_shader_indexable_temp indexable_temp;
        struct vkd3d_shader_function_table_pointer fp;
    } declaration;
};

static inline bool vkd3d_shader_ver_ge(const struct vkd3d_shader_version *v, unsigned int major, unsigned int minor)
{
    return v->major > major || (v->major == major && v->minor >= minor);
}

static inline bool vkd3d_shader_ver_le(const struct vkd3d_shader_version *v, unsigned int major, unsigned int minor)
{
    return v->major < major || (v->major == major && v->minor <= minor);
}

void vsir_instruction_init(struct vkd3d_shader_instruction *ins,
        const struct vkd3d_shader_location *location, enum vkd3d_shader_opcode opcode);

static inline bool vkd3d_shader_instruction_has_texel_offset(const struct vkd3d_shader_instruction *ins)
{
    return ins->texel_offset.u || ins->texel_offset.v || ins->texel_offset.w;
}

static inline bool register_is_constant(const struct vkd3d_shader_register *reg)
{
    return (reg->type == VKD3DSPR_IMMCONST || reg->type == VKD3DSPR_IMMCONST64);
}

static inline bool register_is_undef(const struct vkd3d_shader_register *reg)
{
    return reg->type == VKD3DSPR_UNDEF;
}

static inline bool register_is_constant_or_undef(const struct vkd3d_shader_register *reg)
{
    return register_is_constant(reg) || register_is_undef(reg);
}

static inline bool register_is_numeric_array(const struct vkd3d_shader_register *reg)
{
    return (reg->type == VKD3DSPR_IMMCONSTBUFFER || reg->type == VKD3DSPR_IDXTEMP
            || reg->type == VKD3DSPR_GROUPSHAREDMEM);
}

static inline bool vsir_register_is_label(const struct vkd3d_shader_register *reg)
{
    return reg->type == VKD3DSPR_LABEL;
}

static inline bool register_is_ssa(const struct vkd3d_shader_register *reg)
{
    return reg->type == VKD3DSPR_SSA;
}

struct vkd3d_shader_param_node
{
    struct vkd3d_shader_param_node *next;
    uint8_t param[];
};

struct vkd3d_shader_param_allocator
{
    struct vkd3d_shader_param_node *head;
    struct vkd3d_shader_param_node *current;
    unsigned int count;
    unsigned int stride;
    unsigned int index;
};

void *shader_param_allocator_get(struct vkd3d_shader_param_allocator *allocator, unsigned int count);

static inline struct vkd3d_shader_src_param *shader_src_param_allocator_get(
        struct vkd3d_shader_param_allocator *allocator, unsigned int count)
{
    VKD3D_ASSERT(allocator->stride == sizeof(struct vkd3d_shader_src_param));
    return shader_param_allocator_get(allocator, count);
}

static inline struct vkd3d_shader_dst_param *shader_dst_param_allocator_get(
        struct vkd3d_shader_param_allocator *allocator, unsigned int count)
{
    VKD3D_ASSERT(allocator->stride == sizeof(struct vkd3d_shader_dst_param));
    return shader_param_allocator_get(allocator, count);
}

struct vkd3d_shader_instruction_array
{
    struct vkd3d_shader_instruction *elements;
    size_t capacity;
    size_t count;

    struct vkd3d_shader_param_allocator src_params;
    struct vkd3d_shader_param_allocator dst_params;
    struct vkd3d_shader_immediate_constant_buffer **icbs;
    size_t icb_capacity;
    size_t icb_count;

    struct vkd3d_shader_src_param *outpointid_param;
};

bool shader_instruction_array_init(struct vkd3d_shader_instruction_array *instructions, unsigned int reserve);
bool shader_instruction_array_reserve(struct vkd3d_shader_instruction_array *instructions, unsigned int reserve);
bool shader_instruction_array_insert_at(struct vkd3d_shader_instruction_array *instructions,
        unsigned int idx, unsigned int count);
bool shader_instruction_array_add_icb(struct vkd3d_shader_instruction_array *instructions,
        struct vkd3d_shader_immediate_constant_buffer *icb);
bool shader_instruction_array_clone_instruction(struct vkd3d_shader_instruction_array *instructions,
        unsigned int dst, unsigned int src);
void shader_instruction_array_destroy(struct vkd3d_shader_instruction_array *instructions);

enum vkd3d_shader_config_flags
{
    VKD3D_SHADER_CONFIG_FLAG_FORCE_VALIDATION = 0x00000001,
};

enum vsir_control_flow_type
{
    VSIR_CF_STRUCTURED,
    VSIR_CF_BLOCKS,
};

enum vsir_normalisation_level
{
    VSIR_NORMALISED_SM1,
    VSIR_NORMALISED_SM4,
    VSIR_NORMALISED_HULL_CONTROL_POINT_IO,
    VSIR_NORMALISED_SM6,
};

struct vkd3d_shader_descriptor_info1
{
    enum vkd3d_shader_descriptor_type type;
    unsigned int register_space;
    unsigned int register_index;
    unsigned int register_id;
    enum vkd3d_shader_resource_type resource_type;
    enum vkd3d_data_type resource_data_type;
    unsigned int flags;
    unsigned int sample_count;
    unsigned int buffer_size;
    unsigned int structure_stride;
    unsigned int count;
    uint32_t uav_flags;
};

struct vkd3d_shader_scan_descriptor_info1
{
    struct vkd3d_shader_descriptor_info1 *descriptors;
    unsigned int descriptor_count;
};

const struct vkd3d_shader_descriptor_info1 *vkd3d_shader_find_descriptor(
    const struct vkd3d_shader_scan_descriptor_info1 *info,
    enum vkd3d_shader_descriptor_type type, unsigned int register_id);
void vkd3d_shader_free_scan_descriptor_info1(struct vkd3d_shader_scan_descriptor_info1 *scan_descriptor_info);

struct vsir_program
{
    struct vkd3d_shader_version shader_version;
    struct vkd3d_shader_instruction_array instructions;

    struct shader_signature input_signature;
    struct shader_signature output_signature;
    struct shader_signature patch_constant_signature;

    struct vkd3d_shader_scan_descriptor_info1 descriptors;
    bool has_descriptor_info;

    unsigned int parameter_count;
    const struct vkd3d_shader_parameter1 *parameters;
    bool free_parameters;

    unsigned int input_control_point_count, output_control_point_count;
    struct vsir_thread_group_size thread_group_size;
    unsigned int flat_constant_count[3];
    unsigned int block_count;
    unsigned int temp_count;
    unsigned int ssa_count;
    enum vsir_global_flags global_flags;
    bool use_vocp;
    bool has_point_size;
    bool has_point_coord;
    bool has_fog;
    uint8_t diffuse_written_mask;
    enum vsir_control_flow_type cf_type;
    enum vsir_normalisation_level normalisation_level;
    enum vkd3d_tessellator_domain tess_domain;
    enum vkd3d_shader_tessellator_partitioning tess_partitioning;
    enum vkd3d_shader_tessellator_output_primitive tess_output_primitive;
    enum vkd3d_primitive_type input_primitive, output_topology;
    unsigned int vertices_out_count;

    uint32_t io_dcls[VKD3D_BITMAP_SIZE(VKD3DSPR_COUNT)];

    struct vsir_features features;

    const char **block_names;
    size_t block_name_count;
};

enum vkd3d_result vsir_allocate_temp_registers(struct vsir_program *program,
        struct vkd3d_shader_message_context *message_context);
void vsir_program_cleanup(struct vsir_program *program);
int vsir_program_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_code *out,
        struct vkd3d_shader_message_context *message_context);
const struct vkd3d_shader_parameter1 *vsir_program_get_parameter(
        const struct vsir_program *program, enum vkd3d_shader_parameter_name name);
bool vsir_program_init(struct vsir_program *program, const struct vkd3d_shader_compile_info *compile_info,
        const struct vkd3d_shader_version *version, unsigned int reserve, enum vsir_control_flow_type cf_type,
        enum vsir_normalisation_level normalisation_level);
enum vkd3d_result vsir_program_transform(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_message_context *message_context);
enum vkd3d_result vsir_program_transform_early(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_message_context *message_context);
enum vkd3d_result vsir_program_validate(struct vsir_program *program, uint64_t config_flags,
        const char *source_name, struct vkd3d_shader_message_context *message_context);
struct vkd3d_shader_src_param *vsir_program_create_outpointid_param(
        struct vsir_program *program);
bool vsir_instruction_init_with_params(struct vsir_program *program,
        struct vkd3d_shader_instruction *ins, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_opcode opcode, unsigned int dst_count, unsigned int src_count);

static inline struct vkd3d_shader_dst_param *vsir_program_get_dst_params(
        struct vsir_program *program, unsigned int count)
{
    return shader_dst_param_allocator_get(&program->instructions.dst_params, count);
}

static inline struct vkd3d_shader_src_param *vsir_program_get_src_params(
        struct vsir_program *program, unsigned int count)
{
    return shader_src_param_allocator_get(&program->instructions.src_params, count);
}

struct vkd3d_shader_parser
{
    struct vkd3d_shader_message_context *message_context;
    struct vkd3d_shader_location location;
    struct vsir_program *program;
    bool failed;
};

void vkd3d_shader_parser_error(struct vkd3d_shader_parser *parser,
        enum vkd3d_shader_error error, const char *format, ...) VKD3D_PRINTF_FUNC(3, 4);
void vkd3d_shader_parser_init(struct vkd3d_shader_parser *parser, struct vsir_program *program,
        struct vkd3d_shader_message_context *message_context, const char *source_name);
void vkd3d_shader_parser_warning(struct vkd3d_shader_parser *parser,
        enum vkd3d_shader_error error, const char *format, ...) VKD3D_PRINTF_FUNC(3, 4);

void vsir_program_trace(const struct vsir_program *program);

const char *shader_get_type_prefix(enum vkd3d_shader_type type);

struct vkd3d_string_buffer
{
    char *buffer;
    size_t buffer_size, content_size;
};

struct vkd3d_string_buffer_cache
{
    struct vkd3d_string_buffer **buffers;
    size_t count, max_count, capacity;
};

enum vsir_asm_flags
{
    VSIR_ASM_FLAG_NONE = 0,
    VSIR_ASM_FLAG_DUMP_TYPES = 0x1,
    VSIR_ASM_FLAG_DUMP_ALL_INDICES = 0x2,
    VSIR_ASM_FLAG_DUMP_SIGNATURES = 0x4,
    VSIR_ASM_FLAG_DUMP_DESCRIPTORS = 0x8,
};

enum vkd3d_result d3d_asm_compile(const struct vsir_program *program,
        const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, enum vsir_asm_flags flags);
void vkd3d_string_buffer_cleanup(struct vkd3d_string_buffer *buffer);
struct vkd3d_string_buffer *vkd3d_string_buffer_get(struct vkd3d_string_buffer_cache *list);
void vkd3d_string_buffer_init(struct vkd3d_string_buffer *buffer);
void vkd3d_string_buffer_cache_cleanup(struct vkd3d_string_buffer_cache *list);
void vkd3d_string_buffer_cache_init(struct vkd3d_string_buffer_cache *list);
void vkd3d_string_buffer_clear(struct vkd3d_string_buffer *buffer);
void vkd3d_string_buffer_truncate(struct vkd3d_string_buffer *buffer, size_t size);
int vkd3d_string_buffer_print_f32(struct vkd3d_string_buffer *buffer, float f);
int vkd3d_string_buffer_print_f64(struct vkd3d_string_buffer *buffer, double d);
int vkd3d_string_buffer_print_string_escaped(struct vkd3d_string_buffer *buffer, const char *s, size_t len);
int vkd3d_string_buffer_printf(struct vkd3d_string_buffer *buffer, const char *format, ...) VKD3D_PRINTF_FUNC(2, 3);
void vkd3d_string_buffer_release(struct vkd3d_string_buffer_cache *list, struct vkd3d_string_buffer *buffer);
#define vkd3d_string_buffer_trace(buffer) \
        vkd3d_string_buffer_trace_(buffer, __FUNCTION__)
void vkd3d_string_buffer_trace_(const struct vkd3d_string_buffer *buffer, const char *function);
int vkd3d_string_buffer_vprintf(struct vkd3d_string_buffer *buffer, const char *format, va_list args);
void vkd3d_shader_code_from_string_buffer(struct vkd3d_shader_code *code, struct vkd3d_string_buffer *buffer);

struct vkd3d_bytecode_buffer
{
    uint8_t *data;
    size_t size, capacity;
    int status;
};

/* Align to the next 4-byte offset, and return that offset. */
size_t bytecode_align(struct vkd3d_bytecode_buffer *buffer);
size_t bytecode_put_bytes(struct vkd3d_bytecode_buffer *buffer, const void *bytes, size_t size);
size_t bytecode_put_bytes_unaligned(struct vkd3d_bytecode_buffer *buffer, const void *bytes, size_t size);
size_t bytecode_reserve_bytes(struct vkd3d_bytecode_buffer *buffer, size_t size);
void set_u32(struct vkd3d_bytecode_buffer *buffer, size_t offset, uint32_t value);
void set_string(struct vkd3d_bytecode_buffer *buffer, size_t offset, const char *string, size_t length);

static inline size_t put_u32(struct vkd3d_bytecode_buffer *buffer, uint32_t value)
{
    return bytecode_put_bytes(buffer, &value, sizeof(value));
}

static inline size_t put_f32(struct vkd3d_bytecode_buffer *buffer, float value)
{
    return bytecode_put_bytes(buffer, &value, sizeof(value));
}

static inline size_t put_string(struct vkd3d_bytecode_buffer *buffer, const char *string)
{
    return bytecode_put_bytes(buffer, string, strlen(string) + 1);
}

static inline size_t bytecode_get_size(struct vkd3d_bytecode_buffer *buffer)
{
    return buffer->size;
}

uint32_t vkd3d_parse_integer(const char *s);

struct vkd3d_shader_message_context
{
    enum vkd3d_shader_log_level log_level;
    struct vkd3d_string_buffer messages;
};

void vkd3d_shader_message_context_cleanup(struct vkd3d_shader_message_context *context);
bool vkd3d_shader_message_context_copy_messages(struct vkd3d_shader_message_context *context, char **out);
void vkd3d_shader_message_context_init(struct vkd3d_shader_message_context *context,
        enum vkd3d_shader_log_level log_level);
void vkd3d_shader_message_context_trace_messages_(const struct vkd3d_shader_message_context *context,
        const char *function);
#define vkd3d_shader_message_context_trace_messages(context) \
        vkd3d_shader_message_context_trace_messages_(context, __FUNCTION__)
void vkd3d_shader_error(struct vkd3d_shader_message_context *context, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_error error, const char *format, ...) VKD3D_PRINTF_FUNC(4, 5);
void vkd3d_shader_verror(struct vkd3d_shader_message_context *context, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_error error, const char *format, va_list args);
void vkd3d_shader_vnote(struct vkd3d_shader_message_context *context, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_log_level level, const char *format, va_list args);
void vkd3d_shader_warning(struct vkd3d_shader_message_context *context, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_error error, const char *format, ...) VKD3D_PRINTF_FUNC(4, 5);
void vkd3d_shader_vwarning(struct vkd3d_shader_message_context *context, const struct vkd3d_shader_location *location,
        enum vkd3d_shader_error error, const char *format, va_list args);

uint64_t vkd3d_shader_init_config_flags(void);
void vkd3d_shader_trace_text_(const char *text, size_t size, const char *function);
#define vkd3d_shader_trace_text(text, size) \
        vkd3d_shader_trace_text_(text, size, __FUNCTION__)

bool sm1_register_from_semantic_name(const struct vkd3d_shader_version *version, const char *semantic_name,
        unsigned int semantic_index, bool output, enum vkd3d_shader_sysval_semantic *sysval,
        enum vkd3d_shader_register_type *type, unsigned int *reg);
bool sm1_usage_from_semantic_name(const char *semantic_name,
        uint32_t semantic_index, enum vkd3d_decl_usage *usage, uint32_t *usage_idx);
bool sm4_register_from_semantic_name(const struct vkd3d_shader_version *version,
        const char *semantic_name, bool output, enum vkd3d_shader_register_type *type, bool *has_idx);
bool shader_sm4_is_scalar_register(const struct vkd3d_shader_register *reg);
bool sm4_sysval_semantic_from_semantic_name(enum vkd3d_shader_sysval_semantic *sysval_semantic,
        const struct vkd3d_shader_version *version, bool semantic_compat_mapping, enum vkd3d_tessellator_domain domain,
        const char *semantic_name, unsigned int semantic_idx, bool output,
        bool is_patch_constant_func, bool is_primitive);

int d3dbc_parse(const struct vkd3d_shader_compile_info *compile_info, uint64_t config_flags,
        struct vkd3d_shader_message_context *message_context, struct vsir_program *program);
int dxil_parse(const struct vkd3d_shader_compile_info *compile_info, uint64_t config_flags,
        struct vkd3d_shader_message_context *message_context, struct vsir_program *program);
int tpf_parse(const struct vkd3d_shader_compile_info *compile_info, uint64_t config_flags,
        struct vkd3d_shader_message_context *message_context, struct vsir_program *program);
int fx_parse(const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);
int tx_parse(const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

void free_dxbc_shader_desc(struct dxbc_shader_desc *desc);

int shader_extract_from_dxbc(const struct vkd3d_shader_code *dxbc,
        struct vkd3d_shader_message_context *message_context, const char *source_name, struct dxbc_shader_desc *desc);
int shader_parse_input_signature(const struct vkd3d_shader_code *dxbc,
        struct vkd3d_shader_message_context *message_context, struct shader_signature *signature);

int d3dbc_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, const struct vkd3d_shader_code *ctab,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

int glsl_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_scan_combined_resource_sampler_info *combined_sampler_info,
        const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

#define SPIRV_MAX_SRC_COUNT 6

int spirv_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

int msl_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_code *out,
        struct vkd3d_shader_message_context *message_context);

int tpf_compile(struct vsir_program *program, uint64_t config_flags, const struct vkd3d_shader_code *rdef,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

enum vkd3d_md5_variant
{
    VKD3D_MD5_STANDARD,
    VKD3D_MD5_DXBC,
};

void vkd3d_compute_md5(const void *dxbc, size_t size, uint32_t checksum[4], enum vkd3d_md5_variant variant);

int preproc_lexer_parse(const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

int hlsl_compile_shader(const struct vkd3d_shader_code *hlsl, const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *out, struct vkd3d_shader_message_context *message_context);

static inline enum vkd3d_shader_component_type vkd3d_component_type_from_data_type(
        enum vkd3d_data_type data_type)
{
    switch (data_type)
    {
        case VKD3D_DATA_HALF: /* Minimum precision. TODO: native 16-bit */
        case VKD3D_DATA_FLOAT:
        case VKD3D_DATA_UNORM:
        case VKD3D_DATA_SNORM:
            return VKD3D_SHADER_COMPONENT_FLOAT;
        case VKD3D_DATA_UINT16: /* Minimum precision. TODO: native 16-bit */
        case VKD3D_DATA_UINT:
            return VKD3D_SHADER_COMPONENT_UINT;
        case VKD3D_DATA_INT:
            return VKD3D_SHADER_COMPONENT_INT;
        case VKD3D_DATA_DOUBLE:
            return VKD3D_SHADER_COMPONENT_DOUBLE;
        case VKD3D_DATA_UINT64:
            return VKD3D_SHADER_COMPONENT_UINT64;
        case VKD3D_DATA_BOOL:
            return VKD3D_SHADER_COMPONENT_BOOL;
        default:
            FIXME("Unhandled data type %#x.\n", data_type);
            /* fall-through */
        case VKD3D_DATA_MIXED:
            return VKD3D_SHADER_COMPONENT_UINT;
    }
}

static inline enum vkd3d_data_type vkd3d_data_type_from_component_type(
        enum vkd3d_shader_component_type component_type)
{
    switch (component_type)
    {
        case VKD3D_SHADER_COMPONENT_FLOAT:
            return VKD3D_DATA_FLOAT;
        case VKD3D_SHADER_COMPONENT_UINT:
            return VKD3D_DATA_UINT;
        case VKD3D_SHADER_COMPONENT_INT:
            return VKD3D_DATA_INT;
        case VKD3D_SHADER_COMPONENT_DOUBLE:
            return VKD3D_DATA_DOUBLE;
        default:
            FIXME("Unhandled component type %#x.\n", component_type);
            return VKD3D_DATA_FLOAT;
    }
}

static inline bool component_type_is_64_bit(enum vkd3d_shader_component_type component_type)
{
    return component_type == VKD3D_SHADER_COMPONENT_DOUBLE || component_type == VKD3D_SHADER_COMPONENT_UINT64;
}

static inline unsigned int vsir_write_mask_get_component_idx(uint32_t write_mask)
{
    unsigned int i;

    VKD3D_ASSERT(write_mask);
    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (write_mask & (VKD3DSP_WRITEMASK_0 << i))
            return i;
    }

    FIXME("Invalid write mask %#x.\n", write_mask);
    return 0;
}

static inline unsigned int vsir_write_mask_component_count(uint32_t write_mask)
{
    unsigned int count = vkd3d_popcount(write_mask & VKD3DSP_WRITEMASK_ALL);
    VKD3D_ASSERT(1 <= count && count <= VKD3D_VEC4_SIZE);
    return count;
}

static inline unsigned int vkd3d_write_mask_from_component_count(unsigned int component_count)
{
    VKD3D_ASSERT(component_count <= VKD3D_VEC4_SIZE);
    return (VKD3DSP_WRITEMASK_0 << component_count) - 1;
}

static inline uint32_t vsir_write_mask_64_from_32(uint32_t write_mask32)
{
    switch (write_mask32)
    {
        case VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1:
            return VKD3DSP_WRITEMASK_0;

        case VKD3DSP_WRITEMASK_2 | VKD3DSP_WRITEMASK_3:
            return VKD3DSP_WRITEMASK_1;

        case VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1 | VKD3DSP_WRITEMASK_2 | VKD3DSP_WRITEMASK_3:
            return VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1;

        default:
            ERR("Invalid 32 bit writemask when converting to 64 bit: %#x.\n", write_mask32);
            return VKD3DSP_WRITEMASK_0;
    }
}

static inline uint32_t vsir_write_mask_32_from_64(uint32_t write_mask64)
{
    switch (write_mask64)
    {
        case VKD3DSP_WRITEMASK_0:
            return VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1;

        case VKD3DSP_WRITEMASK_1:
            return VKD3DSP_WRITEMASK_2 | VKD3DSP_WRITEMASK_3;

        case VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1:
            return VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1 | VKD3DSP_WRITEMASK_2 | VKD3DSP_WRITEMASK_3;

        default:
            ERR("Invalid 64 bit writemask: %#x.\n", write_mask64);
            return VKD3DSP_WRITEMASK_0;
    }
}

const char *debug_vsir_writemask(unsigned int writemask);

static inline uint32_t vsir_swizzle_64_from_32(uint32_t swizzle32)
{
    switch (swizzle32)
    {
        case VKD3D_SHADER_SWIZZLE(X, Y, X, Y):
            return VKD3D_SHADER_SWIZZLE(X, X, X, X);

        case VKD3D_SHADER_SWIZZLE(X, Y, Z, W):
            return VKD3D_SHADER_SWIZZLE(X, Y, X, X);

        case VKD3D_SHADER_SWIZZLE(Z, W, X, Y):
            return VKD3D_SHADER_SWIZZLE(Y, X, X, X);

        case VKD3D_SHADER_SWIZZLE(Z, W, Z, W):
            return VKD3D_SHADER_SWIZZLE(Y, Y, X, X);

        default:
            ERR("Invalid 32 bit swizzle when converting to 64 bit: %#x.\n", swizzle32);
            return VKD3D_SHADER_SWIZZLE(X, X, X, X);
    }
}

static inline uint32_t vsir_swizzle_32_from_64(uint32_t swizzle64)
{
    switch (swizzle64)
    {
        case VKD3D_SHADER_SWIZZLE(X, X, X, X):
            return VKD3D_SHADER_SWIZZLE(X, Y, X, Y);

        case VKD3D_SHADER_SWIZZLE(X, Y, X, X):
            return VKD3D_SHADER_SWIZZLE(X, Y, Z, W);

        case VKD3D_SHADER_SWIZZLE(Y, X, X, X):
            return VKD3D_SHADER_SWIZZLE(Z, W, X, Y);

        case VKD3D_SHADER_SWIZZLE(Y, Y, X, X):
            return VKD3D_SHADER_SWIZZLE(Z, W, Z, W);

        default:
            ERR("Invalid 64 bit swizzle: %#x.\n", swizzle64);
            return VKD3D_SHADER_SWIZZLE(X, Y, X, Y);
    }
}

static inline unsigned int vsir_swizzle_get_component(uint32_t swizzle, unsigned int idx)
{
    return (swizzle >> VKD3D_SHADER_SWIZZLE_SHIFT(idx)) & VKD3D_SHADER_SWIZZLE_MASK;
}

static inline void vsir_swizzle_set_component(uint32_t *swizzle, unsigned int idx, unsigned int component)
{
    *swizzle &= ~(VKD3D_SHADER_SWIZZLE_MASK << VKD3D_SHADER_SWIZZLE_SHIFT(idx));
    *swizzle |= component << VKD3D_SHADER_SWIZZLE_SHIFT(idx);
}

static inline unsigned int vkd3d_compact_swizzle(uint32_t swizzle, uint32_t write_mask)
{
    unsigned int i, compacted_swizzle = 0;

    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (write_mask & (VKD3DSP_WRITEMASK_0 << i))
        {
            compacted_swizzle <<= VKD3D_SHADER_SWIZZLE_SHIFT(1);
            compacted_swizzle |= vsir_swizzle_get_component(swizzle, i);
        }
    }

    return compacted_swizzle;
}

static inline uint32_t vsir_swizzle_from_writemask(unsigned int writemask)
{
    static const unsigned int swizzles[16] =
    {
        0,
        VKD3D_SHADER_SWIZZLE(X, X, X, X),
        VKD3D_SHADER_SWIZZLE(Y, Y, Y, Y),
        VKD3D_SHADER_SWIZZLE(X, Y, X, X),
        VKD3D_SHADER_SWIZZLE(Z, Z, Z, Z),
        VKD3D_SHADER_SWIZZLE(X, Z, X, X),
        VKD3D_SHADER_SWIZZLE(Y, Z, X, X),
        VKD3D_SHADER_SWIZZLE(X, Y, Z, X),
        VKD3D_SHADER_SWIZZLE(W, W, W, W),
        VKD3D_SHADER_SWIZZLE(X, W, X, X),
        VKD3D_SHADER_SWIZZLE(Y, W, X, X),
        VKD3D_SHADER_SWIZZLE(X, Y, W, X),
        VKD3D_SHADER_SWIZZLE(Z, W, X, X),
        VKD3D_SHADER_SWIZZLE(X, Z, W, X),
        VKD3D_SHADER_SWIZZLE(Y, Z, W, X),
        VKD3D_SHADER_SWIZZLE(X, Y, Z, W),
    };

    return swizzles[writemask & 0xf];
}

struct vkd3d_struct
{
    enum vkd3d_shader_structure_type type;
    const void *next;
};

#define vkd3d_find_struct(c, t) vkd3d_find_struct_(c, VKD3D_SHADER_STRUCTURE_TYPE_##t)
static inline void *vkd3d_find_struct_(const struct vkd3d_struct *chain,
        enum vkd3d_shader_structure_type type)
{
    while (chain)
    {
        if (chain->type == type)
            return (void *)chain;

        chain = chain->next;
    }

    return NULL;
}

#define VKD3D_DXBC_HEADER_SIZE (8 * sizeof(uint32_t))
#define VKD3D_DXBC_CHUNK_ALIGNMENT sizeof(uint32_t)

#define DXBC_MAX_SECTION_COUNT 7

#define DXBC_SFI0_REQUIRES_DOUBLES                              0x00000001u
#define DXBC_SFI0_REQUIRES_EARLY_DEPTH_STENCIL                  0x00000002u
#define DXBC_SFI0_REQUIRES_UAVS_AT_EVERY_STAGE                  0x00000004u
#define DXBC_SFI0_REQUIRES_64_UAVS                              0x00000008u
#define DXBC_SFI0_REQUIRES_MINIMUM_PRECISION                    0x00000010u
#define DXBC_SFI0_REQUIRES_11_1_DOUBLE_EXTENSIONS               0x00000020u
#define DXBC_SFI0_REQUIRES_11_1_SHADER_EXTENSIONS               0x00000040u
#define DXBC_SFI0_REQUIRES_LEVEL_9_COMPARISON_FILTERING         0x00000080u
#define DXBC_SFI0_REQUIRES_TILED_RESOURCES                      0x00000100u
#define DXBC_SFI0_REQUIRES_STENCIL_REF                          0x00000200u
#define DXBC_SFI0_REQUIRES_INNER_COVERAGE                       0x00000400u
#define DXBC_SFI0_REQUIRES_TYPED_UAV_LOAD_ADDITIONAL_FORMATS    0x00000800u
#define DXBC_SFI0_REQUIRES_ROVS                                 0x00001000u
#define DXBC_SFI0_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER 0x00002000u

struct dxbc_writer
{
    unsigned int section_count;
    struct vkd3d_shader_dxbc_section_desc sections[DXBC_MAX_SECTION_COUNT];
};

void dxbc_writer_add_section(struct dxbc_writer *dxbc, uint32_t tag, const void *data, size_t size);
void dxbc_writer_init(struct dxbc_writer *dxbc);
int dxbc_writer_write(struct dxbc_writer *dxbc, struct vkd3d_shader_code *code);

#endif  /* __VKD3D_SHADER_PRIVATE_H */
