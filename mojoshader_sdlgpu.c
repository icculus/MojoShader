/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

#ifdef USE_SDL3 /* Private define, for now */

#include <SDL3/SDL.h>

#define SDL_GPU_SHADERCROSS_IMPLEMENTATION
#include "spirv/SDL_gpu_shadercross.h"

/* Max entries for each register file type */
#define MAX_REG_FILE_F 8192
#define MAX_REG_FILE_I 2047
#define MAX_REG_FILE_B 2047

struct MOJOSHADER_sdlContext
{
    SDL_GpuDevice *device;
    const char *profile;

    MOJOSHADER_malloc malloc_fn;
    MOJOSHADER_free free_fn;
    void *malloc_data;

    /* The constant register files...
     * !!! FIXME: Man, it kills me how much memory this takes...
     * !!! FIXME:  ... make this dynamically allocated on demand.
     */
    float vs_reg_file_f[MAX_REG_FILE_F * 4];
    int32_t vs_reg_file_i[MAX_REG_FILE_I * 4];
    uint8_t vs_reg_file_b[MAX_REG_FILE_B * 4];
    float ps_reg_file_f[MAX_REG_FILE_F * 4];
    int32_t ps_reg_file_i[MAX_REG_FILE_I * 4];
    uint8_t ps_reg_file_b[MAX_REG_FILE_B * 4];

    MOJOSHADER_sdlShaderData *bound_vshader_data;
    MOJOSHADER_sdlShaderData *bound_pshader_data;
    MOJOSHADER_sdlProgram *bound_program;
    HashTable *linker_cache;
};

struct MOJOSHADER_sdlShaderData
{
    const MOJOSHADER_parseData *parseData;
    uint16_t tag;
    uint32_t refcount;
    uint32_t samplerSlots;
};

struct MOJOSHADER_sdlProgram
{
    SDL_GpuShader *vertexShader;
    SDL_GpuShader *pixelShader;
    MOJOSHADER_sdlShaderData *vertexShaderData;
    MOJOSHADER_sdlShaderData *pixelShaderData;
};

/* Error state... */

static char error_buffer[1024] = { '\0' };

static void set_error(const char *str)
{
    snprintf(error_buffer, sizeof (error_buffer), "%s", str);
} // set_error

static inline void out_of_memory(void)
{
    set_error("out of memory");
} // out_of_memory

/* Internals */

typedef struct LinkedShaderData
{
    MOJOSHADER_sdlShaderData *vertex;
    MOJOSHADER_sdlShaderData *fragment;
    MOJOSHADER_sdlVertexAttribute vertexAttributes[16];
    uint32_t vertexAttributeCount;
} LinkedShaderData;

static uint32_t hash_shaders(const void *sym, void *data)
{
    (void) data;
    const LinkedShaderData *s = (const LinkedShaderData *) sym;
    const uint32_t HASH_FACTOR = 31;
    uint32_t hash = s->vertexAttributeCount;
    for (uint32_t i = 0; i < s->vertexAttributeCount; i += 1)
    {
        hash = hash * HASH_FACTOR + s->vertexAttributes[i].usage;
        hash = hash * HASH_FACTOR + s->vertexAttributes[i].usageIndex;
        hash = hash * HASH_FACTOR + s->vertexAttributes[i].vertexElementFormat;
    }
    hash = hash * HASH_FACTOR + s->vertex->tag;
    hash = hash * HASH_FACTOR + s->fragment->tag;
    return hash;
} // hash_shaders

static int match_shaders(const void *_a, const void *_b, void *data)
{
    (void) data;
    const LinkedShaderData *a = (const LinkedShaderData *) _a;
    const LinkedShaderData *b = (const LinkedShaderData *) _b;

    const uint16_t av = (a->vertex) ? a->vertex->tag : 0;
    const uint16_t bv = (b->vertex) ? b->vertex->tag : 0;
    if (av != bv)
        return 0;

    const uint16_t af = (a->fragment) ? a->fragment->tag : 0;
    const uint16_t bf = (b->fragment) ? b->fragment->tag : 0;
    if (af != bf)
        return 0;

    if (a->vertexAttributeCount != b->vertexAttributeCount)
        return 0;

    for (uint32_t i = 0; i < a->vertexAttributeCount; i += 1)
    {
        if (a->vertexAttributes[i].usage != b->vertexAttributes[i].usage)
        {
            return 0;
        }
        if (a->vertexAttributes[i].usageIndex != b->vertexAttributes[i].usageIndex)
        {
            return 0;
        }
        if (a->vertexAttributes[i].vertexElementFormat != b->vertexAttributes[i].vertexElementFormat)
        {
            return 0;
        }
    }

    return 1;
} // match_shaders

static void nuke_shaders(
    const void *_ctx,
    const void *key,
    const void *value,
    void *data
) {
    MOJOSHADER_sdlContext *ctx = (MOJOSHADER_sdlContext *) _ctx;
    (void) data;
    ctx->free_fn((void *) key, ctx->malloc_data); // this was a LinkedShaderData struct.
    MOJOSHADER_sdlDeleteProgram(ctx, (MOJOSHADER_sdlProgram *) value);
} // nuke_shaders

static void update_uniform_buffer(
    MOJOSHADER_sdlContext *ctx,
    SDL_GpuCommandBuffer *cb,
    MOJOSHADER_sdlShaderData *shader
) {
    int32_t i, j;
    int32_t offset;
    uint8_t *contents;
    uint32_t content_size;
    uint32_t *contentsI;
    float *regF; int *regI; uint8_t *regB;

    if (shader == NULL || shader->parseData->uniform_count == 0)
        return;

    if (shader->parseData->shader_type == MOJOSHADER_TYPE_VERTEX)
    {
        regF = ctx->vs_reg_file_f;
        regI = ctx->vs_reg_file_i;
        regB = ctx->vs_reg_file_b;
    } // if
    else
    {
        regF = ctx->ps_reg_file_f;
        regI = ctx->ps_reg_file_i;
        regB = ctx->ps_reg_file_b;
    } // else
    content_size = 0;

    for (i = 0; i < shader->parseData->uniform_count; i++)
    {
        const int32_t arrayCount = shader->parseData->uniforms[i].array_count;
        const int32_t size = arrayCount ? arrayCount : 1;
        content_size += size * 16;
    } // for

    contents = (uint8_t*) ctx->malloc_fn(content_size, ctx->malloc_data);

    offset = 0;
    for (i = 0; i < shader->parseData->uniform_count; i++)
    {
        const int32_t index = shader->parseData->uniforms[i].index;
        const int32_t arrayCount = shader->parseData->uniforms[i].array_count;
        const int32_t size = arrayCount ? arrayCount : 1;

        switch (shader->parseData->uniforms[i].type)
        {
            case MOJOSHADER_UNIFORM_FLOAT:
                memcpy(
                    contents + offset,
                    &regF[4 * index],
                    size * 16
                );
                break;

            case MOJOSHADER_UNIFORM_INT:
                memcpy(
                    contents + offset,
                    &regI[4 * index],
                    size * 16
                );
                break;

            case MOJOSHADER_UNIFORM_BOOL:
                contentsI = (uint32_t *) (contents + offset);
                for (j = 0; j < size; j++)
                    contentsI[j * 4] = regB[index + j];
                break;

            default:
                set_error(
                    "SOMETHING VERY WRONG HAPPENED WHEN UPDATING UNIFORMS"
                );
                assert(0);
                break;
        } // switch

        offset += size * 16;
    } // for

    if (shader->parseData->shader_type == MOJOSHADER_TYPE_VERTEX)
    {
		SDL_GpuPushVertexUniformData(
			cb,
			0,
			contents,
			content_size
		);
    } // if
    else
    {
		SDL_GpuPushFragmentUniformData(
			cb,
			0,
			contents,
			content_size
		);
    } // else

    ctx->free_fn(contents, ctx->malloc_data);
} // update_uniform_buffer

/* Public API */

unsigned int MOJOSHADER_sdlGetShaderFormats(void)
{
    return SDL_ShaderCross_GetShaderFormats();
} // MOJOSHADER_sdlGetShaderFormats

MOJOSHADER_sdlContext *MOJOSHADER_sdlCreateContext(
    SDL_GpuDevice *device,
    MOJOSHADER_malloc m,
    MOJOSHADER_free f,
    void *malloc_d
) {
    MOJOSHADER_sdlContext* resultCtx;

    if (m == NULL) m = MOJOSHADER_internal_malloc;
    if (f == NULL) f = MOJOSHADER_internal_free;

    resultCtx = (MOJOSHADER_sdlContext*) m(sizeof(MOJOSHADER_sdlContext), malloc_d);
    if (resultCtx == NULL)
    {
        out_of_memory();
        goto init_fail;
    } // if

    SDL_memset(resultCtx, '\0', sizeof(MOJOSHADER_sdlContext));
    resultCtx->device = device;
    resultCtx->profile = "spirv"; /* always use spirv and interop with SDL_gpu_spirvcross */

    resultCtx->malloc_fn = m;
    resultCtx->free_fn = f;
    resultCtx->malloc_data = malloc_d;

    return resultCtx;

init_fail:
    if (resultCtx != NULL)
        f(resultCtx, malloc_d);
    return NULL;
} // MOJOSHADER_sdlCreateContext

const char *MOJOSHADER_sdlGetError(
    MOJOSHADER_sdlContext *ctx
) {
    return error_buffer;
} // MOJOSHADER_sdlGetError

void MOJOSHADER_sdlDestroyContext(
    MOJOSHADER_sdlContext *ctx
) {
    if (ctx->linker_cache)
        hash_destroy(ctx->linker_cache, ctx);

    ctx->free_fn(ctx, ctx->malloc_data);
} // MOJOSHADER_sdlDestroyContext

static uint16_t shaderTagCounter = 1;

MOJOSHADER_sdlShaderData *MOJOSHADER_sdlCompileShader(
    MOJOSHADER_sdlContext *ctx,
    const char *mainfn,
    const unsigned char *tokenbuf,
    const unsigned int bufsize,
    const MOJOSHADER_swizzle *swiz,
    const unsigned int swizcount,
    const MOJOSHADER_samplerMap *smap,
    const unsigned int smapcount
) {
    MOJOSHADER_sdlShaderData *shader = NULL;;
    int maxSamplerIndex = 0;
    int i;

    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(
        "spirv", mainfn,
        tokenbuf, bufsize,
        swiz, swizcount,
        smap, smapcount,
        ctx->malloc_fn,
        ctx->free_fn,
        ctx->malloc_data
    );

    if (pd->error_count > 0)
    {
        set_error(pd->errors[0].error);
        goto parse_shader_fail;
    } // if

    shader = (MOJOSHADER_sdlShaderData*) ctx->malloc_fn(sizeof(MOJOSHADER_sdlShaderData), ctx->malloc_data);
    if (shader == NULL)
    {
        out_of_memory();
        goto parse_shader_fail;
    } // if

    shader->parseData = pd;
    shader->refcount = 1;
    shader->tag = shaderTagCounter++;

	/* XNA allows empty shader slots in the middle, so we have to find the actual max binding index */
	for (i = 0; i < pd->sampler_count; i += 1)
	{
		if (pd->samplers[i].index > maxSamplerIndex)
		{
			maxSamplerIndex = pd->samplers[i].index;
		}
	}

    shader->samplerSlots = (uint32_t) maxSamplerIndex + 1;

    return shader;

parse_shader_fail:
    MOJOSHADER_freeParseData(pd);
    if (shader != NULL)
        ctx->free_fn(shader, ctx->malloc_data);
    return NULL;
} // MOJOSHADER_sdlCompileShader

MOJOSHADER_sdlProgram *MOJOSHADER_sdlLinkProgram(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlVertexAttribute *vertexAttributes,
    int vertexAttributeCount
) {
    MOJOSHADER_sdlProgram *program = NULL;
    SDL_GpuShaderCreateInfo createInfo;

    MOJOSHADER_sdlShaderData *vshader = ctx->bound_vshader_data;
    MOJOSHADER_sdlShaderData *pshader = ctx->bound_pshader_data;

    if ((vshader == NULL) || (pshader == NULL)) /* Both shaders MUST exist! */
        return NULL;

    if (ctx->linker_cache == NULL)
    {
        ctx->linker_cache = hash_create(NULL, hash_shaders, match_shaders,
                                        nuke_shaders, 0, ctx->malloc_fn,
                                        ctx->free_fn, ctx->malloc_data);

        if (ctx->linker_cache == NULL)
        {
            out_of_memory();
            return NULL;
        } // if
    } // if

    LinkedShaderData shaders;
    shaders.vertex = vshader;
    shaders.fragment = pshader;
    memset(shaders.vertexAttributes, 0, sizeof(MOJOSHADER_sdlVertexAttribute) * 16);
    shaders.vertexAttributeCount = vertexAttributeCount;
    for (int i = 0; i < vertexAttributeCount; i += 1)
    {
        shaders.vertexAttributes[i] = vertexAttributes[i];
    }

    const void *val = NULL;

    if (hash_find(ctx->linker_cache, &shaders, &val))
    {
        ctx->bound_program = (MOJOSHADER_sdlProgram *) val;
        return ctx->bound_program;
    }

    program = (MOJOSHADER_sdlProgram*) ctx->malloc_fn(sizeof(MOJOSHADER_sdlProgram), ctx->malloc_data);

    if (program == NULL)
    {
        out_of_memory();
        return NULL;
    } // if

	// We have to patch the SPIR-V output to ensure type consistency. The non-float types are:
	// BYTE4  - 5
	// SHORT2 - 6
	// SHORT4 - 7
	int vDataLen = vshader->parseData->output_len - sizeof(SpirvPatchTable);
	SpirvPatchTable *vTable = (SpirvPatchTable *) &vshader->parseData->output[vDataLen];

	for (int i = 0; i < vertexAttributeCount; i += 1)
	{
		MOJOSHADER_sdlVertexAttribute *element = &vertexAttributes[i];
		uint32 typeDecl, typeLoad;
		SpvOp opcodeLoad;

		if (element->vertexElementFormat >= 5 && element->vertexElementFormat <= 7)
		{
			typeDecl = element->vertexElementFormat == 5 ? vTable->tid_uvec4_p : vTable->tid_ivec4_p;
			typeLoad = element->vertexElementFormat == 5 ? vTable->tid_uvec4 : vTable->tid_ivec4;
			opcodeLoad = element->vertexElementFormat == 5 ? SpvOpConvertUToF : SpvOpConvertSToF;
		}
		else
		{
			typeDecl = vTable->tid_vec4_p;
			typeLoad = vTable->tid_vec4;
			opcodeLoad = SpvOpCopyObject;
		}

		uint32_t typeDeclOffset = vTable->attrib_type_offsets[element->usage][element->usageIndex];
		((uint32_t*)vshader->parseData->output)[typeDeclOffset] = typeDecl;
		for (uint32_t j = 0; j < vTable->attrib_type_load_offsets[element->usage][element->usageIndex].num_loads; j += 1)
		{
			uint32_t typeLoadOffset = vTable->attrib_type_load_offsets[element->usage][element->usageIndex].load_types[j];
			uint32_t opcodeLoadOffset = vTable->attrib_type_load_offsets[element->usage][element->usageIndex].load_opcodes[j];
			uint32_t *ptr_to_opcode_u32 = &((uint32_t*)vshader->parseData->output)[opcodeLoadOffset];
			((uint32_t*)vshader->parseData->output)[typeLoadOffset] = typeLoad;
			*ptr_to_opcode_u32 = (*ptr_to_opcode_u32 & 0xFFFF0000) | opcodeLoad;
		}
	}

    MOJOSHADER_spirv_link_attributes(vshader->parseData, pshader->parseData, 0);

    SDL_zero(createInfo);
    createInfo.code = (const Uint8*) vshader->parseData->output;
    createInfo.codeSize = vshader->parseData->output_len - sizeof(SpirvPatchTable);
    createInfo.entryPointName = vshader->parseData->mainfn;
    createInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    createInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    createInfo.samplerCount = vshader->samplerSlots;
    createInfo.uniformBufferCount = 1;

    program->vertexShader = SDL_ShaderCross_CompileFromSPIRV(
        ctx->device,
        &createInfo,
        SDL_FALSE
    );

    if (program->vertexShader == NULL)
    {
        set_error(SDL_GetError());
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    createInfo.code = (const Uint8*) pshader->parseData->output;
    createInfo.codeSize = pshader->parseData->output_len - sizeof(SpirvPatchTable);
    createInfo.entryPointName = pshader->parseData->mainfn;
    createInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    createInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    createInfo.samplerCount = pshader->samplerSlots;

    program->pixelShader = SDL_ShaderCross_CompileFromSPIRV(
        ctx->device,
        &createInfo,
        SDL_FALSE
    );

    if (program->pixelShader == NULL)
    {
        set_error(SDL_GetError());
        SDL_GpuReleaseShader(ctx->device, program->vertexShader);
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    program->vertexShaderData = vshader;
    program->pixelShaderData = pshader;

    LinkedShaderData *item = (LinkedShaderData *) ctx->malloc_fn(sizeof (LinkedShaderData),
                                                         ctx->malloc_data);

    if (item == NULL)
    {
        MOJOSHADER_sdlDeleteProgram(ctx, program);
    }

    memcpy(item, &shaders, sizeof(LinkedShaderData));
    if (hash_insert(ctx->linker_cache, item, program) != 1)
    {
        ctx->free_fn(item, ctx->malloc_data);
        MOJOSHADER_sdlDeleteProgram(ctx, program);
        out_of_memory();
        return NULL;
    }

    ctx->bound_program = program;
    return program;
} // MOJOSHADER_sdlLinkProgram

void MOJOSHADER_sdlShaderAddRef(MOJOSHADER_sdlShaderData *shader)
{
    if (shader != NULL)
        shader->refcount++;
} // MOJOSHADER_sdlShaderAddRef

void MOJOSHADER_sdlDeleteShader(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *shader
) {
    if (shader != NULL)
    {
        if (shader->refcount > 1)
            shader->refcount--;
        else
        {
            // See if this was bound as an unlinked program anywhere...
            if (ctx->linker_cache)
            {
                const void *key = NULL;
                void *iter = NULL;
                int morekeys = hash_iter_keys(ctx->linker_cache, &key, &iter);
                while (morekeys)
                {
                    const LinkedShaderData *shaders = (const LinkedShaderData *) key;
                    // Do this here so we don't confuse the iteration by removing...
                    morekeys = hash_iter_keys(ctx->linker_cache, &key, &iter);
                    if ((shaders->vertex == shader) || (shaders->fragment == shader))
                    {
                        // Deletes the linked program
                        hash_remove(ctx->linker_cache, shaders, ctx);
                    } // if
                } // while
            } // if

            MOJOSHADER_freeParseData(shader->parseData);
            ctx->free_fn(shader, ctx->malloc_data);
        } // else
    } // if
} // MOJOSHADER_sdlDeleteShader

const MOJOSHADER_parseData *MOJOSHADER_sdlGetShaderParseData(
    MOJOSHADER_sdlShaderData *shader
) {
    return (shader != NULL) ? shader->parseData : NULL;
} // MOJOSHADER_sdlGetShaderParseData

void MOJOSHADER_sdlDeleteProgram(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlProgram *p
) {
    if (ctx->bound_program == p)
        ctx->bound_program = NULL;
    if (p->vertexShader != NULL)
        SDL_GpuReleaseShader(ctx->device, p->vertexShader);
    if (p->pixelShader != NULL)
        SDL_GpuReleaseShader(ctx->device, p->pixelShader);
    ctx->free_fn(p, ctx->malloc_data);
} // MOJOSHADER_sdlDeleteProgram

void MOJOSHADER_sdlBindProgram(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlProgram *p
) {
    ctx->bound_program = p;
} // MOJOSHADER_sdlBindProgram

void MOJOSHADER_sdlBindShaders(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *vshader,
    MOJOSHADER_sdlShaderData *pshader
) {
    MOJOSHADER_sdlProgram *program = NULL;
    ctx->bound_vshader_data = vshader;
    ctx->bound_pshader_data = pshader;
} // MOJOSHADER_sdlBindShaders

void MOJOSHADER_sdlGetBoundShaderData(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData **vshaderdata,
    MOJOSHADER_sdlShaderData **pshaderdata
) {
    if (vshaderdata != NULL)
    {
        *vshaderdata = ctx->bound_vshader_data;
    } // if
    if (pshaderdata != NULL)
    {
        *pshaderdata = ctx->bound_pshader_data;
    } // if
} // MOJOSHADER_sdlGetBoundShaderData

void MOJOSHADER_sdlMapUniformBufferMemory(
    MOJOSHADER_sdlContext *ctx,
    float **vsf, int **vsi, unsigned char **vsb,
    float **psf, int **psi, unsigned char **psb
) {
    *vsf = ctx->vs_reg_file_f;
    *vsi = ctx->vs_reg_file_i;
    *vsb = ctx->vs_reg_file_b;
    *psf = ctx->ps_reg_file_f;
    *psi = ctx->ps_reg_file_i;
    *psb = ctx->ps_reg_file_b;
} // MOJOSHADER_sdlMapUniformBufferMemory

void MOJOSHADER_sdlUnmapUniformBufferMemory(MOJOSHADER_sdlContext *ctx)
{
    /* no-op! real work done in sdlUpdateUniformBuffers */
} // MOJOSHADER_sdlUnmapUniformBufferMemory

int MOJOSHADER_sdlGetUniformBufferSize(MOJOSHADER_sdlShaderData *shader)
{
    int32_t i;
    int32_t buflen = 0;
    const int32_t uniformSize = 16; // Yes, even the bool registers
    for (i = 0; i < shader->parseData->uniform_count; i++)
    {
        const int32_t arrayCount = shader->parseData->uniforms[i].array_count;
        buflen += (arrayCount ? arrayCount : 1) * uniformSize;
    } // for

    return buflen;
} // MOJOSHADER_sdlGetUniformBufferSize

void MOJOSHADER_sdlUpdateUniformBuffers(MOJOSHADER_sdlContext *ctx,
                                        SDL_GpuCommandBuffer *cb)
{
    if (MOJOSHADER_sdlGetUniformBufferSize(ctx->bound_program->vertexShaderData) > 0)
        update_uniform_buffer(ctx, cb, ctx->bound_program->vertexShaderData);
    if (MOJOSHADER_sdlGetUniformBufferSize(ctx->bound_program->pixelShaderData) > 0)
        update_uniform_buffer(ctx, cb, ctx->bound_program->pixelShaderData);
} // MOJOSHADER_sdlUpdateUniformBuffers

int MOJOSHADER_sdlGetVertexAttribLocation(
    MOJOSHADER_sdlShaderData *vert,
    MOJOSHADER_usage usage, int index
) {
    int32_t i;
    if (vert == NULL)
        return -1;

    for (i = 0; i < vert->parseData->attribute_count; i++)
    {
        if (vert->parseData->attributes[i].usage == usage &&
            vert->parseData->attributes[i].index == index)
        {
            return i;
        } // if
    } // for

    // failure
    return -1;
} // MOJOSHADER_sdlGetVertexAttribLocation

void MOJOSHADER_sdlGetShaders(
    MOJOSHADER_sdlContext *ctx,
    SDL_GpuShader **vshader,
    SDL_GpuShader **pshader
) {
    assert(ctx->bound_program != NULL);
    if (vshader != NULL)
        *vshader = ctx->bound_program->vertexShader;
    if (pshader != NULL)
        *pshader = ctx->bound_program->pixelShader;
} // MOJOSHADER_sdlGetShaders

unsigned int MOJOSHADER_sdlGetSamplerSlots(MOJOSHADER_sdlShaderData *shader)
{
    assert(shader != NULL);
    return shader->samplerSlots;
} // MOJOSHADER_sdlGetSamplerSlots

#endif // USE_SDL3
