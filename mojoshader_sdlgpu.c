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
#include <spirv/spirv.h>

/* Max entries for each register file type */
#define MAX_REG_FILE_F 8192
#define MAX_REG_FILE_I 2047
#define MAX_REG_FILE_B 2047

/* The destination shader format to use */
static SDL_GPUShaderFormat shader_format =
#ifdef __APPLE__
    SDL_GPU_SHADERFORMAT_MSL;
#else
    SDL_GPU_SHADERFORMAT_SPIRV;
#endif

typedef struct ShaderEntry
{
    uint64_t hash;
    uint32_t offset;
    uint32_t size;
} ShaderEntry;

typedef struct ShaderBlob
{
    uint64_t hash;
    void* binary;
} ShaderBlob;

struct MOJOSHADER_sdlContext
{
    SDL_GPUDevice *device;
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

    uint8_t *uniform_staging;
    uint32_t uniform_staging_length;

    MOJOSHADER_sdlShaderData *bound_vshader_data;
    MOJOSHADER_sdlShaderData *bound_pshader_data;
    MOJOSHADER_sdlProgram *bound_program;
    HashTable *linker_cache;

    struct
    {
        SDL_GPUShaderFormat format;
        uint32_t numShaders;
        ShaderEntry *hashes;
        ShaderBlob *shaders;
    } blob;
};

struct MOJOSHADER_sdlShaderData
{
    const MOJOSHADER_parseData *parseData;
    uint16_t tag;
    uint32_t refcount;
    uint32_t samplerSlots;
    int32_t uniformBufferSize;
};

struct MOJOSHADER_sdlProgram
{
    SDL_GPUShader *vertexShader;
    SDL_GPUShader *pixelShader;
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

static uint8_t update_uniform_buffer(
    MOJOSHADER_sdlContext *ctx,
    SDL_GPUCommandBuffer *cb,
    MOJOSHADER_sdlShaderData *shader,
    float *regF,
    int *regI,
    uint8_t *regB
) {
    int32_t i, j;
    int32_t offset;
    uint32_t *contentsI;

    if (shader->uniformBufferSize > ctx->uniform_staging_length)
    {
        ctx->free_fn(ctx->uniform_staging, ctx->malloc_data);
        ctx->uniform_staging = ctx->malloc_fn(shader->uniformBufferSize, ctx->malloc_data);
        ctx->uniform_staging_length = shader->uniformBufferSize;
    } // if

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
                    ctx->uniform_staging + offset,
                    &regF[4 * index],
                    size * 16
                );
                break;

            case MOJOSHADER_UNIFORM_INT:
                memcpy(
                    ctx->uniform_staging + offset,
                    &regI[4 * index],
                    size * 16
                );
                break;

            case MOJOSHADER_UNIFORM_BOOL:
                contentsI = (uint32_t *) (ctx->uniform_staging + offset);
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

    return 1; // FIXME: Return 0 when uniform data is unchanged
} // update_uniform_buffer

/* Public API */

unsigned int MOJOSHADER_sdlGetShaderFormats(void)
{
    return shader_format;
} // MOJOSHADER_sdlGetShaderFormats

static bool load_precompiled_blob(MOJOSHADER_sdlContext *ctx)
{
    int32_t i, hashIndex, probes;
    uint8_t *usedEntries;
    uint64_t hash;
    ShaderEntry *entry;
    ShaderBlob *shader;

    SDL_IOStream *blob = SDL_IOFromFile("MojoShaderPrecompiled.bin", "rb");
    if (blob == NULL)
        return false;

    /* First, read the number of shaders */
    SDL_ReadIO(blob, &ctx->blob.numShaders, sizeof(uint32_t));

    /* Allocate storage for the shader data */
    ctx->blob.hashes = (ShaderEntry*) SDL_malloc(
        ctx->blob.numShaders * sizeof(ShaderEntry)
    );
    ctx->blob.shaders = (ShaderBlob*) SDL_malloc(
        ctx->blob.numShaders * sizeof(ShaderBlob)
    );

    /* Keep track of the hash table entries we've used */
    usedEntries = (uint8_t*) SDL_calloc(ctx->blob.numShaders, sizeof(uint8_t));

    /* Read and store the shader hashes */
    for (i = 0; i < ctx->blob.numShaders; i += 1)
    {
        SDL_ReadIO(blob, &hash, sizeof(uint64_t));
        hashIndex = hash % ctx->blob.numShaders;

        /* Find the first usable index */
        for (probes = 0; probes < ctx->blob.numShaders; probes += 1)
        {
            hashIndex = (hashIndex + 1) % ctx->blob.numShaders;
            if (usedEntries[hashIndex] == 0)
            {
                usedEntries[hashIndex] = 1;
                break;
            }
        }

        ctx->blob.hashes[hashIndex].hash = hash;
        SDL_ReadIO(blob, &ctx->blob.hashes[hashIndex].offset, sizeof(uint32_t));
        SDL_ReadIO(blob, &ctx->blob.hashes[hashIndex].size, sizeof(uint32_t));
    }

    SDL_free(usedEntries);

    /* Read the shader blobs */
    for (i = 0; i < ctx->blob.numShaders; i += 1)
    {
        entry = &ctx->blob.hashes[i];
        shader = &ctx->blob.shaders[i];

        SDL_SeekIO(blob, entry->offset, SDL_IO_SEEK_SET);
        shader->binary = SDL_malloc(entry->size);
        SDL_ReadIO(blob, shader->binary, entry->size);

        /* Assign the hash value to the shader */
        shader->hash = entry->hash;
    }

    SDL_CloseIO(blob);
    return true;
} // load_precompiled_blob

MOJOSHADER_sdlContext *MOJOSHADER_sdlCreateContext(
    SDL_GPUDevice *device,
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

    if (load_precompiled_blob(resultCtx))
    {
        /* Just validate the bytecode, calculate a hash to find in blobCache */
        resultCtx->profile = "bytecode";
        resultCtx->blob.format = SDL_GetGPUShaderFormats(device);
    }
    else
    {
        resultCtx->profile = (shader_format == SDL_GPU_SHADERFORMAT_SPIRV) ? "spirv" : "metal";
    }

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
    uint32_t i;

    if (ctx->linker_cache)
        hash_destroy(ctx->linker_cache, ctx);

    ctx->free_fn(ctx->uniform_staging, ctx->malloc_data);

    if (ctx->blob.numShaders > 0)
    {
        for (i = 0; i < ctx->blob.numShaders; i += 1)
            SDL_free(ctx->blob.shaders[i].binary);
        SDL_free(ctx->blob.hashes);
        SDL_free(ctx->blob.shaders);
    } // if

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
        ctx->profile, mainfn,
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

    shader->uniformBufferSize = 0;
    for (i = 0; i < pd->uniform_count; i++)
    {
        shader->uniformBufferSize += SDL_max(pd->uniforms[i].array_count, 1);
    } // for
    shader->uniformBufferSize *= 16; // Yes, even the bool registers are this size

    return shader;

parse_shader_fail:
    MOJOSHADER_freeParseData(pd);
    if (shader != NULL)
        ctx->free_fn(shader, ctx->malloc_data);
    return NULL;
} // MOJOSHADER_sdlCompileShader

static inline uint64_t hash_vertex_shader(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *vshader,
    MOJOSHADER_sdlVertexAttribute *vertexAttributes,
    int vertexAttributeCount)
{
    // TODO: Combine d3dbc hash with vertex attribute hash
    return 0;
}

static inline uint64_t hash_pixel_shader(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *pshader)
{
    // TODO: Calculate hash of pshader d3dbc
    return 0;
}

static inline ShaderBlob *fetch_blob_shader(
    MOJOSHADER_sdlContext *ctx,
    uint64_t hash,
    uint32_t *size)
{
    int32_t probes, searchIndex;
    for (probes = 0; probes < ctx->blob.numShaders; probes += 1)
    {
        searchIndex = (hash + probes) % ctx->blob.numShaders;
        if (ctx->blob.hashes[searchIndex].hash == hash)
        {
            *size = ctx->blob.hashes[searchIndex].size;
            return &ctx->blob.shaders[searchIndex];
        }
    }
    set_error("MojoShaderPrecompiled.bin is incomplete!!!");
    return NULL;
} // fetch_blob_shader

static MOJOSHADER_sdlProgram *compile_blob_program(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *vshader,
    MOJOSHADER_sdlShaderData *pshader,
    MOJOSHADER_sdlVertexAttribute *vertexAttributes,
    int vertexAttributeCount)
{
    uint64_t hash;
    ShaderBlob *vblob, *pblob;
    uint32_t vlen, plen;
    SDL_GPUShaderCreateInfo createInfo;
    MOJOSHADER_sdlProgram *program = (MOJOSHADER_sdlProgram*) ctx->malloc_fn(sizeof(MOJOSHADER_sdlProgram),
                                                                             ctx->malloc_data);
    if (program == NULL)
    {
        out_of_memory();
        return NULL;
    } // if

    // TODO: Maybe add the format to the blob header?
    SDL_assert(ctx->blob.format & SDL_GPU_SHADERFORMAT_PRIVATE);

    hash = hash_vertex_shader(ctx, vshader, vertexAttributes, vertexAttributeCount);
    vblob = fetch_blob_shader(ctx, hash, &vlen);

    hash = hash_pixel_shader(ctx, pshader);
    pblob = fetch_blob_shader(ctx, hash, &plen);

    if ((vblob == NULL) || (pblob == NULL))
    {
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    SDL_zero(createInfo);
    createInfo.code = (const Uint8*) vblob->binary;
    createInfo.code_size = vlen;
    createInfo.entrypoint = vshader->parseData->mainfn;
    createInfo.format = SDL_GPU_SHADERFORMAT_PRIVATE;
    createInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    createInfo.num_samplers = vshader->samplerSlots;
    createInfo.num_uniform_buffers = 1;

    program->vertexShader = SDL_CreateGPUShader(
        ctx->device,
        &createInfo
    );

    if (program->vertexShader == NULL)
    {
        set_error(SDL_GetError());
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    createInfo.code = (const Uint8*) pblob->binary;
    createInfo.code_size = plen;
    createInfo.entrypoint = pshader->parseData->mainfn;
    createInfo.format = ctx->blob.format;
    createInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    createInfo.num_samplers = pshader->samplerSlots;

    program->pixelShader = SDL_CreateGPUShader(
        ctx->device,
        &createInfo
    );

    if (program->pixelShader == NULL)
    {
        set_error(SDL_GetError());
        SDL_ReleaseGPUShader(ctx->device, program->vertexShader);
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    return program;
} // compile_blob_program

static MOJOSHADER_sdlProgram *compile_program(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlShaderData *vshader,
    MOJOSHADER_sdlShaderData *pshader,
    MOJOSHADER_sdlVertexAttribute *vertexAttributes,
    int vertexAttributeCount)
{
    SDL_GPUShaderCreateInfo createInfo;
    MOJOSHADER_sdlProgram *program = (MOJOSHADER_sdlProgram*) ctx->malloc_fn(sizeof(MOJOSHADER_sdlProgram),
                                                                             ctx->malloc_data);
    if (program == NULL)
    {
        out_of_memory();
        return NULL;
    } // if

    size_t vshaderCodeSize = vshader->parseData->output_len;
    size_t pshaderCodeSize = pshader->parseData->output_len;

    if (shader_format == SDL_GPU_SHADERFORMAT_SPIRV)
    {
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

        vshaderCodeSize -= sizeof(SpirvPatchTable);
        pshaderCodeSize -= sizeof(SpirvPatchTable);
    }

    SDL_zero(createInfo);
    createInfo.code = (const Uint8*) vshader->parseData->output;
    createInfo.code_size = vshaderCodeSize;
    createInfo.entrypoint = vshader->parseData->mainfn;
    createInfo.format = shader_format;
    createInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    createInfo.num_samplers = vshader->samplerSlots;
    createInfo.num_uniform_buffers = 1;

    program->vertexShader = SDL_CreateGPUShader(
        ctx->device,
        &createInfo
    );

    if (program->vertexShader == NULL)
    {
        set_error(SDL_GetError());
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    createInfo.code = (const Uint8*) pshader->parseData->output;
    createInfo.code_size = pshaderCodeSize;
    createInfo.entrypoint = pshader->parseData->mainfn;
    createInfo.format = shader_format;
    createInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    createInfo.num_samplers = pshader->samplerSlots;

    program->pixelShader = SDL_CreateGPUShader(
        ctx->device,
        &createInfo
    );

    if (program->pixelShader == NULL)
    {
        set_error(SDL_GetError());
        SDL_ReleaseGPUShader(ctx->device, program->vertexShader);
        ctx->free_fn(program, ctx->malloc_data);
        return NULL;
    } // if

    return program;
} // compile_program

MOJOSHADER_sdlProgram *MOJOSHADER_sdlLinkProgram(
    MOJOSHADER_sdlContext *ctx,
    MOJOSHADER_sdlVertexAttribute *vertexAttributes,
    int vertexAttributeCount
) {
    MOJOSHADER_sdlProgram *program = NULL;

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

    if (ctx->blob.numShaders > 0)
    {
        program = compile_blob_program(ctx, vshader, pshader,
                                       vertexAttributes, vertexAttributeCount);
    } // if
    else
    {
        program = compile_program(ctx, vshader, pshader,
                                  vertexAttributes, vertexAttributeCount);
    } // else

    if (program == NULL)
        return NULL;

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
        SDL_ReleaseGPUShader(ctx->device, p->vertexShader);
    if (p->pixelShader != NULL)
        SDL_ReleaseGPUShader(ctx->device, p->pixelShader);
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
    if (shader == NULL)
        return 0;
    return shader->uniformBufferSize;
} // MOJOSHADER_sdlGetUniformBufferSize

void MOJOSHADER_sdlUpdateUniformBuffers(MOJOSHADER_sdlContext *ctx,
                                        SDL_GPUCommandBuffer *cb)
{
    if (MOJOSHADER_sdlGetUniformBufferSize(ctx->bound_program->vertexShaderData) > 0)
    {
        if (update_uniform_buffer(ctx, cb, ctx->bound_program->vertexShaderData,
                                  ctx->vs_reg_file_f,
                                  ctx->vs_reg_file_i,
                                  ctx->vs_reg_file_b))
        {
            SDL_PushGPUVertexUniformData(
                cb,
                0,
                ctx->uniform_staging,
                ctx->bound_program->vertexShaderData->uniformBufferSize
            );
        } // if
    } // if
    if (MOJOSHADER_sdlGetUniformBufferSize(ctx->bound_program->pixelShaderData) > 0)
    {
        if (update_uniform_buffer(ctx, cb, ctx->bound_program->pixelShaderData,
                                  ctx->ps_reg_file_f,
                                  ctx->ps_reg_file_i,
                                  ctx->ps_reg_file_b))
        {
            SDL_PushGPUFragmentUniformData(
                cb,
                0,
                ctx->uniform_staging,
                ctx->bound_program->pixelShaderData->uniformBufferSize
            );
        } // if
    } // if
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
    SDL_GPUShader **vshader,
    SDL_GPUShader **pshader
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
