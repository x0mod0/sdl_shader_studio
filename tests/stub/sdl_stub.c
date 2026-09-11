/* Fake SDL GPU device for the loader tests. Records what the loader asked for
 * so a test can assert the create-info the loader would hand to the driver. */
#include <SDL3/SDL.h>

static SDL_GPUShaderFormat g_supported = SDL_GPU_SHADERFORMAT_SPIRV;
static SDL_GPUShaderCreateInfo g_last_shader;
static SDL_GPUComputePipelineCreateInfo g_last_compute;
static char g_error[256] = "";

const char *SDL_GetError(void) { return g_error; }

void *SDL_LoadFile(const char *path, size_t *datasize) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(g_error, sizeof(g_error), "no such file: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (datasize) *datasize = (size_t)size;
    return data;
}

SDL_GPUShaderFormat SDL_GetGPUShaderFormats(SDL_GPUDevice *device) {
    (void)device;
    return g_supported;
}

SDL_GPUShader *SDL_CreateGPUShader(SDL_GPUDevice *device, const SDL_GPUShaderCreateInfo *info) {
    (void)device;
    g_last_shader = *info;
    return (SDL_GPUShader *)0x1;  /* non-NULL sentinel */
}

SDL_GPUComputePipeline *SDL_CreateGPUComputePipeline(SDL_GPUDevice *device,
                                                     const SDL_GPUComputePipelineCreateInfo *info) {
    (void)device;
    g_last_compute = *info;
    return (SDL_GPUComputePipeline *)0x2;
}

void SDL_ReleaseGPUShader(SDL_GPUDevice *device, SDL_GPUShader *shader) {
    (void)device;
    (void)shader;
}

void SDL_ReleaseGPUComputePipeline(SDL_GPUDevice *device, SDL_GPUComputePipeline *pipeline) {
    (void)device;
    (void)pipeline;
}

void SSSTUDIO_StubSetSupportedFormats(SDL_GPUShaderFormat formats) { g_supported = formats; }
const SDL_GPUShaderCreateInfo *SSSTUDIO_StubLastShaderInfo(void) { return &g_last_shader; }
const SDL_GPUComputePipelineCreateInfo *SSSTUDIO_StubLastComputeInfo(void) { return &g_last_compute; }
