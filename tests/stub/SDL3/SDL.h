/* Minimal SDL3 stand-in used ONLY by the loader tests.
 *
 * It exists so tests/test_loader.cpp can compile and run the generated s3pack.h
 * without a GPU or an SDL installation: if a pack-format change breaks the
 * loader, the test suite fails instead of a user's game. */
#ifndef SSSTUDIO_TEST_STUB_SDL_H
#define SSSTUDIO_TEST_STUB_SDL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int32_t Sint32;

#define SDL_malloc(sz) malloc(sz)
#define SDL_free(p) free(p)
#define SDL_memcpy(d, s, n) memcpy((d), (s), (n))
#define SDL_memset(d, v, n) memset((d), (v), (n))
#define SDL_memcmp(a, b, n) memcmp((a), (b), (n))
#define SDL_vsnprintf(buf, n, fmt, ap) vsnprintf((buf), (n), (fmt), (ap))
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_zerop(x) memset((x), 0, sizeof(*(x)))
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_assert(x) ((void)0)
#define SDL_Log(...) ((void)0)

typedef enum SDL_GPUShaderFormat {
    SDL_GPU_SHADERFORMAT_INVALID = 0,
    SDL_GPU_SHADERFORMAT_PRIVATE = 1u << 0,
    SDL_GPU_SHADERFORMAT_SPIRV = 1u << 1,
    SDL_GPU_SHADERFORMAT_DXBC = 1u << 2,
    SDL_GPU_SHADERFORMAT_DXIL = 1u << 3,
    SDL_GPU_SHADERFORMAT_MSL = 1u << 4,
    SDL_GPU_SHADERFORMAT_METALLIB = 1u << 5
} SDL_GPUShaderFormat;

typedef enum SDL_GPUShaderStage {
    SDL_GPU_SHADERSTAGE_VERTEX = 0,
    SDL_GPU_SHADERSTAGE_FRAGMENT = 1
} SDL_GPUShaderStage;

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUShader SDL_GPUShader;
typedef struct SDL_GPUComputePipeline SDL_GPUComputePipeline;

typedef struct SDL_GPUShaderCreateInfo {
    size_t code_size;
    const Uint8 *code;
    const char *entrypoint;
    SDL_GPUShaderFormat format;
    SDL_GPUShaderStage stage;
    Uint32 num_samplers;
    Uint32 num_storage_textures;
    Uint32 num_storage_buffers;
    Uint32 num_uniform_buffers;
    Uint32 props;
} SDL_GPUShaderCreateInfo;

typedef struct SDL_GPUComputePipelineCreateInfo {
    size_t code_size;
    const Uint8 *code;
    const char *entrypoint;
    SDL_GPUShaderFormat format;
    Uint32 num_samplers;
    Uint32 num_readonly_storage_textures;
    Uint32 num_readonly_storage_buffers;
    Uint32 num_readwrite_storage_textures;
    Uint32 num_readwrite_storage_buffers;
    Uint32 num_uniform_buffers;
    Uint32 threadcount_x;
    Uint32 threadcount_y;
    Uint32 threadcount_z;
    Uint32 props;
} SDL_GPUComputePipelineCreateInfo;

#ifdef __cplusplus
extern "C" {
#endif

/* The test harness provides these; see tests/stub/sdl_stub.c. */
const char *SDL_GetError(void);
void *SDL_LoadFile(const char *path, size_t *datasize);
SDL_GPUShaderFormat SDL_GetGPUShaderFormats(SDL_GPUDevice *device);
SDL_GPUShader *SDL_CreateGPUShader(SDL_GPUDevice *device, const SDL_GPUShaderCreateInfo *info);
SDL_GPUComputePipeline *SDL_CreateGPUComputePipeline(SDL_GPUDevice *device,
                                                     const SDL_GPUComputePipelineCreateInfo *info);
void SDL_ReleaseGPUShader(SDL_GPUDevice *device, SDL_GPUShader *shader);
void SDL_ReleaseGPUComputePipeline(SDL_GPUDevice *device, SDL_GPUComputePipeline *pipeline);

/* Test hooks: what the fake device claims to support, and the last create call. */
void SSSTUDIO_StubSetSupportedFormats(SDL_GPUShaderFormat formats);
const SDL_GPUShaderCreateInfo *SSSTUDIO_StubLastShaderInfo(void);
const SDL_GPUComputePipelineCreateInfo *SSSTUDIO_StubLastComputeInfo(void);

#ifdef __cplusplus
}
#endif

#endif /* SSSTUDIO_TEST_STUB_SDL_H */
