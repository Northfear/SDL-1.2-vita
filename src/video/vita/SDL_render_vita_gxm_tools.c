/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/types.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>

#include "SDL_error.h"

#include "SDL_render_vita_gxm_tools.h"
#include "SDL_render_vita_gxm_memory.h"
#include "SDL_render_vita_gxm_shaders.h"

VITA_GXM_RenderData *data;
static SceKernelMemBlockType textureMemBlockType = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
static int gxm_finish_wait = 1;
volatile unsigned int *notificationMem;
SceGxmNotification flipFragmentNotif;

#ifdef VITA_HW_ACCEL
#define NOTIF_NUM 1024
int notification_busy[NOTIF_NUM];
#endif


void init_orthographic_matrix(float *m, float left, float right, float bottom, float top, float near, float far)
{
    m[0x0] = 2.0f/(right-left);
    m[0x4] = 0.0f;
    m[0x8] = 0.0f;
    m[0xC] = -(right+left)/(right-left);

    m[0x1] = 0.0f;
    m[0x5] = 2.0f/(top-bottom);
    m[0x9] = 0.0f;
    m[0xD] = -(top+bottom)/(top-bottom);

    m[0x2] = 0.0f;
    m[0x6] = 0.0f;
    m[0xA] = -2.0f/(far-near);
    m[0xE] = (far+near)/(far-near);

    m[0x3] = 0.0f;
    m[0x7] = 0.0f;
    m[0xB] = 0.0f;
    m[0xF] = 1.0f;
}

static void* patcher_host_alloc(void *user_data, unsigned int size)
{
    (void)user_data;
    void *mem = SDL_malloc(size);
    return mem;
}

static void patcher_host_free(void *user_data, void *mem)
{
    (void)user_data;
    SDL_free(mem);
}

static int tex_format_to_bytespp(SceGxmTextureFormat format)
{
    switch (format & 0x9f000000U) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P8:
        return 1;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S5S5U6:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
        return 2;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8:
        return 3;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S32:
    default:
        return 4;
    }
}

static void make_fragment_programs(VITA_GXM_RenderData *data, fragment_programs *out, const SceGxmBlendInfo *blend_info)
{
    int err = sceGxmShaderPatcherCreateFragmentProgram(
        data->shaderPatcher,
        data->textureFragmentProgramId,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        0,
        blend_info,
        textureVertexProgramGxp,
        &out->texture
    );

    if (err != 0) {
        SDL_SetError("Patcher create fragment failed: %d\n", err);
        return;
    }
}

static void free_fragment_programs(VITA_GXM_RenderData *data, fragment_programs *out)
{
    sceGxmShaderPatcherReleaseFragmentProgram(data->shaderPatcher, out->texture);
}

#ifdef VITA_HW_ACCEL
static void set_alpha_blend(int blend_enabled)
{
    fragment_programs *in;

    if (blend_enabled)
        in = &data->blendFragmentPrograms.blend_mode_blend;
    else
        in = &data->blendFragmentPrograms.blend_mode_none;

    data->textureFragmentProgram = in->texture;
}

void *pool_malloc(VITA_GXM_RenderData *data, unsigned int size)
{
    if ((data->pool_index + size) < VITA_GXM_POOL_SIZE) {
        void *addr = (void *)((unsigned int)data->pool_addr[data->current_pool] + data->pool_index);
        data->pool_index += size;
        return addr;
    }
    SDL_SetError("POOL OVERFLOW\n");
    return NULL;
}

void *pool_memalign(VITA_GXM_RenderData *data, unsigned int size, unsigned int alignment)
{
    unsigned int new_index = (data->pool_index + alignment - 1) & ~(alignment - 1);
    if ((new_index + size) < VITA_GXM_POOL_SIZE) {
        void *addr = (void *)((unsigned int)data->pool_addr[data->current_pool] + new_index);
        data->pool_index = new_index + size;
        return addr;
    }
    SDL_SetError("POOL OVERFLOW\n");
    return NULL;
}
#endif

static void display_callback(const void *callback_data)
{
    SceDisplayFrameBuf framebuf;
    const VITA_GXM_DisplayData *display_data = (const VITA_GXM_DisplayData *)callback_data;

    SDL_memset(&framebuf, 0x00, sizeof(SceDisplayFrameBuf));
    framebuf.size        = sizeof(SceDisplayFrameBuf);
    framebuf.base        = display_data->address;
    framebuf.pitch       = VITA_GXM_SCREEN_STRIDE;
    framebuf.pixelformat = VITA_GXM_PIXEL_FORMAT;
    framebuf.width       = VITA_GXM_SCREEN_WIDTH;
    framebuf.height      = VITA_GXM_SCREEN_HEIGHT;
    sceDisplaySetFrameBuf(&framebuf, SCE_DISPLAY_SETBUF_NEXTFRAME);

    if (display_data->vblank_wait) {
        sceDisplayWaitVblankStart();
    }
}

int gxm_init()
{
    data = (VITA_GXM_RenderData *) SDL_calloc(1, sizeof(VITA_GXM_RenderData));

    unsigned int i, x, y;
    int err;

    SceGxmInitializeParams initializeParams;
    SDL_memset(&initializeParams, 0, sizeof(SceGxmInitializeParams));
    initializeParams.flags                          = 0;
    initializeParams.displayQueueMaxPendingCount    = VITA_GXM_PENDING_SWAPS;
    initializeParams.displayQueueCallback           = display_callback;
    initializeParams.displayQueueCallbackDataSize   = sizeof(VITA_GXM_DisplayData);
    initializeParams.parameterBufferSize            = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;

    err = sceGxmInitialize(&initializeParams);

    if (err != SCE_OK) {
        SDL_SetError("gxm init failed: %d\n", err);
        return err;
    }

    // allocate ring buffer memory using default sizes
    void *vdmRingBuffer = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
        4,
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->vdmRingBufferUid);

    void *vertexRingBuffer = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
        4,
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->vertexRingBufferUid);

    void *fragmentRingBuffer = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
        4,
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->fragmentRingBufferUid);

    unsigned int fragmentUsseRingBufferOffset;
    void *fragmentUsseRingBuffer = mem_fragment_usse_alloc(
        SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
        &data->fragmentUsseRingBufferUid,
        &fragmentUsseRingBufferOffset);

    SDL_memset(&data->contextParams, 0, sizeof(SceGxmContextParams));
    data->contextParams.hostMem                       = SDL_malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    data->contextParams.hostMemSize                   = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    data->contextParams.vdmRingBufferMem              = vdmRingBuffer;
    data->contextParams.vdmRingBufferMemSize          = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    data->contextParams.vertexRingBufferMem           = vertexRingBuffer;
    data->contextParams.vertexRingBufferMemSize       = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    data->contextParams.fragmentRingBufferMem         = fragmentRingBuffer;
    data->contextParams.fragmentRingBufferMemSize     = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    data->contextParams.fragmentUsseRingBufferMem     = fragmentUsseRingBuffer;
    data->contextParams.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    data->contextParams.fragmentUsseRingBufferOffset  = fragmentUsseRingBufferOffset;

    err = sceGxmCreateContext(&data->contextParams, &data->gxm_context);
    if (err != SCE_OK) {
        SDL_SetError("create context failed: %d\n", err);
        return err;
    }

    // set up parameters
    SceGxmRenderTargetParams renderTargetParams;
    SDL_memset(&renderTargetParams, 0, sizeof(SceGxmRenderTargetParams));
    renderTargetParams.flags                = 0;
    renderTargetParams.width                = VITA_GXM_SCREEN_WIDTH;
    renderTargetParams.height               = VITA_GXM_SCREEN_HEIGHT;
    renderTargetParams.scenesPerFrame       = 1;
    renderTargetParams.multisampleMode      = 0;
    renderTargetParams.multisampleLocations = 0;
    renderTargetParams.driverMemBlock       = -1; // Invalid UID

    // create the render target
    err = sceGxmCreateRenderTarget(&renderTargetParams, &data->renderTarget);
    if (err != SCE_OK) {
        SDL_SetError("render target creation failed: %d\n", err);
        return err;
    }

    // allocate memory and sync objects for display buffers
    for (i = 0; i < VITA_GXM_BUFFERS; i++) {

        // allocate memory for display
        data->displayBufferData[i] = mem_gpu_alloc(
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            4 * VITA_GXM_SCREEN_STRIDE * VITA_GXM_SCREEN_HEIGHT,
            SCE_GXM_COLOR_SURFACE_ALIGNMENT,
            SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
            &data->displayBufferUid[i]);

        // memset the buffer to black
        SDL_memset(data->displayBufferData[i], 0, VITA_GXM_SCREEN_HEIGHT * VITA_GXM_SCREEN_STRIDE * 4);

        // initialize a color surface for this display buffer
        err = sceGxmColorSurfaceInit(
            &data->displaySurface[i],
            VITA_GXM_COLOR_FORMAT,
            SCE_GXM_COLOR_SURFACE_LINEAR,
            SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            VITA_GXM_SCREEN_WIDTH,
            VITA_GXM_SCREEN_HEIGHT,
            VITA_GXM_SCREEN_STRIDE,
            data->displayBufferData[i]
        );

        if (err != SCE_OK) {
            SDL_SetError("color surface init failed: %d\n", err);
            return err;
        }

        // create a sync object that we will associate with this buffer
        err = sceGxmSyncObjectCreate(&data->displayBufferSync[i]);
        if (err != SCE_OK) {
            SDL_SetError("sync object creation failed: %d\n", err);
            return err;
        }
    }

    // compute the memory footprint of the depth buffer
    const unsigned int alignedWidth = ALIGN(VITA_GXM_SCREEN_WIDTH, SCE_GXM_TILE_SIZEX);
    const unsigned int alignedHeight = ALIGN(VITA_GXM_SCREEN_HEIGHT, SCE_GXM_TILE_SIZEY);

    unsigned int sampleCount = alignedWidth * alignedHeight;
    unsigned int depthStrideInSamples = alignedWidth;

    // allocate the depth buffer
    data->depthBufferData = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        4 * sampleCount,
        SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT,
        SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
        &data->depthBufferUid);

    // allocate the stencil buffer
    data->stencilBufferData = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        4 * sampleCount,
        SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT,
        SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
        &data->stencilBufferUid);

    // create the SceGxmDepthStencilSurface structure
    err = sceGxmDepthStencilSurfaceInit(
        &data->depthSurface,
        SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
        SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
        depthStrideInSamples,
        data->depthBufferData,
        data->stencilBufferData);

    // set the stencil test reference (this is currently assumed to always remain 1 after here for region clipping)
    sceGxmSetFrontStencilRef(data->gxm_context, 1);

    // set the stencil function (this wouldn't actually be needed, as the set clip rectangle function has to call this at the begginning of every scene)
    sceGxmSetFrontStencilFunc(
        data->gxm_context,
        SCE_GXM_STENCIL_FUNC_ALWAYS,
        SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP,
        0xFF,
        0xFF);

    // set buffer sizes for this sample
    const unsigned int patcherBufferSize        = 64*1024;
    const unsigned int patcherVertexUsseSize    = 64*1024;
    const unsigned int patcherFragmentUsseSize  = 64*1024;

    // allocate memory for buffers and USSE code
    void *patcherBuffer = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        patcherBufferSize,
        4,
        SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
        &data->patcherBufferUid);

    unsigned int patcherVertexUsseOffset;
    void *patcherVertexUsse = mem_vertex_usse_alloc(
        patcherVertexUsseSize,
        &data->patcherVertexUsseUid,
        &patcherVertexUsseOffset);

    unsigned int patcherFragmentUsseOffset;
    void *patcherFragmentUsse = mem_fragment_usse_alloc(
        patcherFragmentUsseSize,
        &data->patcherFragmentUsseUid,
        &patcherFragmentUsseOffset);

    // create a shader patcher
    SceGxmShaderPatcherParams patcherParams;
    SDL_memset(&patcherParams, 0, sizeof(SceGxmShaderPatcherParams));
    patcherParams.userData                  = NULL;
    patcherParams.hostAllocCallback         = &patcher_host_alloc;
    patcherParams.hostFreeCallback          = &patcher_host_free;
    patcherParams.bufferAllocCallback       = NULL;
    patcherParams.bufferFreeCallback        = NULL;
    patcherParams.bufferMem                 = patcherBuffer;
    patcherParams.bufferMemSize             = patcherBufferSize;
    patcherParams.vertexUsseAllocCallback   = NULL;
    patcherParams.vertexUsseFreeCallback    = NULL;
    patcherParams.vertexUsseMem             = patcherVertexUsse;
    patcherParams.vertexUsseMemSize         = patcherVertexUsseSize;
    patcherParams.vertexUsseOffset          = patcherVertexUsseOffset;
    patcherParams.fragmentUsseAllocCallback = NULL;
    patcherParams.fragmentUsseFreeCallback  = NULL;
    patcherParams.fragmentUsseMem           = patcherFragmentUsse;
    patcherParams.fragmentUsseMemSize       = patcherFragmentUsseSize;
    patcherParams.fragmentUsseOffset        = patcherFragmentUsseOffset;

    err = sceGxmShaderPatcherCreate(&patcherParams, &data->shaderPatcher);
    if (err != SCE_OK) {
        SDL_SetError("shader patcher creation failed: %d\n", err);
        return err;
    }

    err = sceGxmProgramCheck(textureVertexProgramGxp);
    if (err != SCE_OK) {
        SDL_SetError("check program (texture vertex) failed: %d\n", err);
        return err;
    }

    err = sceGxmProgramCheck(textureFragmentProgramGxp);
    if (err != SCE_OK) {
        SDL_SetError("check program (texture fragment) failed: %d\n", err);
        return err;
    }

    err = sceGxmProgramCheck(clearVertexProgramGxp);
    if (err != SCE_OK) {
        SDL_SetError("check program (clear vertex) failed: %d\n", err);
        return err;
    }

    err = sceGxmProgramCheck(clearFragmentProgramGxp);
    if (err != SCE_OK) {
        SDL_SetError("check program (clear fragment) failed: %d\n", err);
        return err;
    }

    // register programs with the patcher
    err = sceGxmShaderPatcherRegisterProgram(data->shaderPatcher, textureVertexProgramGxp, &data->textureVertexProgramId);
    if (err != SCE_OK) {
        SDL_SetError("register program (texture vertex) failed: %d\n", err);
        return err;
    }

    err = sceGxmShaderPatcherRegisterProgram(data->shaderPatcher, textureFragmentProgramGxp, &data->textureFragmentProgramId);
    if (err != SCE_OK) {
        SDL_SetError("register program (texture fragment) failed: %d\n", err);
        return err;
    }

    err = sceGxmShaderPatcherRegisterProgram(data->shaderPatcher, clearVertexProgramGxp, &data->clearVertexProgramId);
    if (err != SCE_OK) {
        SDL_SetError("register program (clear vertex) failed: %d\n", err);
        return err;
    }

    err = sceGxmShaderPatcherRegisterProgram(data->shaderPatcher, clearFragmentProgramGxp, &data->clearFragmentProgramId);
    if (err != SCE_OK) {
        SDL_SetError("register program (clear fragment) failed: %d\n", err);
        return err;
    }

    {
        // get attributes by name to create vertex format bindings
        const SceGxmProgramParameter *paramClearPositionAttribute = sceGxmProgramFindParameterByName(clearVertexProgramGxp, "aPosition");

        // create clear vertex format
        SceGxmVertexAttribute clearVertexAttributes[1];
        SceGxmVertexStream clearVertexStreams[1];
        clearVertexAttributes[0].streamIndex    = 0;
        clearVertexAttributes[0].offset         = 0;
        clearVertexAttributes[0].format         = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        clearVertexAttributes[0].componentCount = 2;
        clearVertexAttributes[0].regIndex       = sceGxmProgramParameterGetResourceIndex(paramClearPositionAttribute);
        clearVertexStreams[0].stride            = sizeof(clear_vertex);
        clearVertexStreams[0].indexSource       = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

        // create clear programs
        err = sceGxmShaderPatcherCreateVertexProgram(
            data->shaderPatcher,
            data->clearVertexProgramId,
            clearVertexAttributes,
            1,
            clearVertexStreams,
            1,
            &data->clearVertexProgram
        );
        if (err != SCE_OK) {
            SDL_SetError("create program (clear vertex) failed: %d\n", err);
            return err;
        }

        err = sceGxmShaderPatcherCreateFragmentProgram(
            data->shaderPatcher,
            data->clearFragmentProgramId,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            0,
            NULL,
            clearVertexProgramGxp,
            &data->clearFragmentProgram
        );
        if (err != SCE_OK) {
            SDL_SetError("create program (clear fragment) failed: %d\n", err);
            return err;
        }

        // create the clear triangle vertex/index data
        data->clearVertices = (clear_vertex *)mem_gpu_alloc(
            SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
            3*sizeof(clear_vertex),
            4,
            SCE_GXM_MEMORY_ATTRIB_READ,
            &data->clearVerticesUid
        );

        data->clearVertices[0].x = -1.0f;
        data->clearVertices[0].y = -1.0f;
        data->clearVertices[1].x =  3.0f;
        data->clearVertices[1].y = -1.0f;
        data->clearVertices[2].x = -1.0f;
        data->clearVertices[2].y =  3.0f;
    }

    // Allocate a 64k * 2 bytes = 128 KiB buffer and store all possible
    // 16-bit indices in linear ascending order, so we can use this for
    // all drawing operations where we don't want to use indexing.
    data->linearIndices = (uint16_t *)mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        4*sizeof(uint16_t),
        sizeof(uint16_t),
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->linearIndicesUid
    );

    for (uint16_t i = 0; i < 4; ++i)
    {
        data->linearIndices[i] = i;
    }

    const SceGxmProgramParameter *paramTexturePositionAttribute = sceGxmProgramFindParameterByName(textureVertexProgramGxp, "aPosition");
    const SceGxmProgramParameter *paramTextureTexcoordAttribute = sceGxmProgramFindParameterByName(textureVertexProgramGxp, "aTexcoord");

    // create texture vertex format
    SceGxmVertexAttribute textureVertexAttributes[2];
    SceGxmVertexStream textureVertexStreams[1];
    /* x,y,z: 3 float 32 bits */
    textureVertexAttributes[0].streamIndex = 0;
    textureVertexAttributes[0].offset = 0;
    textureVertexAttributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    textureVertexAttributes[0].componentCount = 3; // (x, y, z)
    textureVertexAttributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(paramTexturePositionAttribute);
    /* u,v: 2 floats 32 bits */
    textureVertexAttributes[1].streamIndex = 0;
    textureVertexAttributes[1].offset = 12; // (x, y, z) * 4 = 12 bytes
    textureVertexAttributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    textureVertexAttributes[1].componentCount = 2; // (u, v)
    textureVertexAttributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(paramTextureTexcoordAttribute);
    // 16 bit (short) indices
    textureVertexStreams[0].stride = sizeof(texture_vertex);
    textureVertexStreams[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

    // create texture shaders
    err = sceGxmShaderPatcherCreateVertexProgram(
        data->shaderPatcher,
        data->textureVertexProgramId,
        textureVertexAttributes,
        2,
        textureVertexStreams,
        1,
        &data->textureVertexProgram
    );
    if (err != SCE_OK) {
        SDL_SetError("create program (texture vertex) failed: %d\n", err);
        return err;
    }

    // Fill SceGxmBlendInfo
    static const SceGxmBlendInfo blend_info_none = {
        .colorFunc = SCE_GXM_BLEND_FUNC_NONE,
        .alphaFunc = SCE_GXM_BLEND_FUNC_NONE,
        .colorSrc  = SCE_GXM_BLEND_FACTOR_ZERO,
        .colorDst  = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaSrc  = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaDst  = SCE_GXM_BLEND_FACTOR_ZERO,
        .colorMask = SCE_GXM_COLOR_MASK_ALL
    };
    // Create variations of the fragment program based on blending mode
    make_fragment_programs(data, &data->blendFragmentPrograms.blend_mode_none, &blend_info_none);

#ifdef VITA_HW_ACCEL
    static const SceGxmBlendInfo blend_info_blend = {
        .colorFunc = SCE_GXM_BLEND_FUNC_ADD,
        .alphaFunc = SCE_GXM_BLEND_FUNC_ADD,
        .colorSrc  = SCE_GXM_BLEND_FACTOR_SRC_ALPHA,
        .colorDst  = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaSrc  = SCE_GXM_BLEND_FACTOR_ONE,
        .alphaDst  = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorMask = SCE_GXM_COLOR_MASK_ALL
    };
    make_fragment_programs(data, &data->blendFragmentPrograms.blend_mode_blend, &blend_info_blend);
#endif

    // Default to none blending mode
    fragment_programs *in = &data->blendFragmentPrograms.blend_mode_none;
    data->textureFragmentProgram = in->texture;

    // find vertex uniforms by name and cache parameter information
    data->textureWvpParam = (SceGxmProgramParameter *)sceGxmProgramFindParameterByName(textureVertexProgramGxp, "wvp");
    data->clearClearColorParam = (SceGxmProgramParameter *)sceGxmProgramFindParameterByName(clearFragmentProgramGxp, "uClearColor");

    // Allocate memory for screen vertices
    data->screenVertices = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        4 * sizeof(texture_vertex),
        sizeof(texture_vertex),
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->screenVerticesUid
    );

    init_orthographic_matrix(data->ortho_matrix, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);

    notificationMem = sceGxmGetNotificationRegion();
    flipFragmentNotif.address = notificationMem;
    flipFragmentNotif.value = 1;

#ifdef VITA_HW_ACCEL
    for (int i = 0; i < NOTIF_NUM; ++i)
    {
        notification_busy[i] = 0;
    }

    // Allocate memory for the memory pool
    data->pool_addr[0] = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        VITA_GXM_POOL_SIZE,
        sizeof(void *),
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->poolUid[0]
    );

    data->pool_addr[1] = mem_gpu_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        VITA_GXM_POOL_SIZE,
        sizeof(void *),
        SCE_GXM_MEMORY_ATTRIB_READ,
        &data->poolUid[1]
    );

    data->pool_index = 0;
    data->current_pool = 0;
#endif

    data->backBufferIndex = 0;
    data->frontBufferIndex = 0;

    return 0;
}

void gxm_finish()
{
    gxm_wait_rendering_done();

    // clean up allocations
    sceGxmShaderPatcherReleaseVertexProgram(data->shaderPatcher, data->textureVertexProgram);
    sceGxmShaderPatcherReleaseFragmentProgram(data->shaderPatcher, data->textureFragmentProgram);
    sceGxmShaderPatcherReleaseFragmentProgram(data->shaderPatcher, data->clearFragmentProgram);
    sceGxmShaderPatcherReleaseVertexProgram(data->shaderPatcher, data->clearVertexProgram);

    free_fragment_programs(data, &data->blendFragmentPrograms.blend_mode_none);
#ifdef VITA_HW_ACCEL
    free_fragment_programs(data, &data->blendFragmentPrograms.blend_mode_blend);
#endif

    mem_gpu_free(data->linearIndicesUid);
    mem_gpu_free(data->clearVerticesUid);

    // wait until display queue is finished before deallocating display buffers
    sceGxmDisplayQueueFinish();

    for (size_t i = 0; i < VITA_GXM_BUFFERS; i++)
    {
        // clear the buffer then deallocate
        SDL_memset(data->displayBufferData[i], 0, VITA_GXM_SCREEN_HEIGHT * VITA_GXM_SCREEN_STRIDE * 4);
        mem_gpu_free(data->displayBufferUid[i]);

        // destroy the sync object
        sceGxmSyncObjectDestroy(data->displayBufferSync[i]);
    }

    // free the depth and stencil buffer
    mem_gpu_free(data->depthBufferUid);
    mem_gpu_free(data->stencilBufferUid);

    // unregister programs and destroy shader patcher
    sceGxmShaderPatcherUnregisterProgram(data->shaderPatcher, data->textureFragmentProgramId);
    sceGxmShaderPatcherUnregisterProgram(data->shaderPatcher, data->textureVertexProgramId);
    sceGxmShaderPatcherUnregisterProgram(data->shaderPatcher, data->clearFragmentProgramId);
    sceGxmShaderPatcherUnregisterProgram(data->shaderPatcher, data->clearVertexProgramId);

    sceGxmShaderPatcherDestroy(data->shaderPatcher);
    mem_fragment_usse_free(data->patcherFragmentUsseUid);
    mem_vertex_usse_free(data->patcherVertexUsseUid);
    mem_gpu_free(data->patcherBufferUid);

    // destroy the render target
    sceGxmDestroyRenderTarget(data->renderTarget);

    // destroy the gxm context
    sceGxmDestroyContext(data->gxm_context);
    mem_fragment_usse_free(data->fragmentUsseRingBufferUid);
    mem_gpu_free(data->fragmentRingBufferUid);
    mem_gpu_free(data->vertexRingBufferUid);
    mem_gpu_free(data->vdmRingBufferUid);
    SDL_free(data->contextParams.hostMem);
    mem_gpu_free(data->screenVerticesUid);
#ifdef VITA_HW_ACCEL
    mem_gpu_free(data->poolUid[0]);
    mem_gpu_free(data->poolUid[1]);
#endif
    // terminate libgxm
    sceGxmTerminate();

    SDL_free(data);
}

void free_gxm_texture(gxm_texture *texture)
{
    if (texture) {
        if (texture->palette_UID) {
            mem_gpu_free(texture->palette_UID);
        }
        mem_gpu_free(texture->data_UID);
#ifdef VITA_HW_ACCEL
        if (texture->gxm_rendertarget) {
            sceGxmDestroyRenderTarget(texture->gxm_rendertarget);
        }
        if (texture->depth_UID) {
            mem_gpu_free(texture->depth_UID);
        }
        notification_busy[texture->notification_id] = 0;
#endif
        SDL_free(texture);
    }
}

SceGxmTextureFormat gxm_texture_get_format(const gxm_texture *texture)
{
    return sceGxmTextureGetFormat(&texture->gxm_tex);
}

unsigned int gxm_texture_get_width(const gxm_texture *texture)
{
    return sceGxmTextureGetWidth(&texture->gxm_tex);
}

unsigned int gxm_texture_get_height(const gxm_texture *texture)
{
    return sceGxmTextureGetHeight(&texture->gxm_tex);
}

unsigned int gxm_texture_get_stride(const gxm_texture *texture)
{
    return ((gxm_texture_get_width(texture) + 7) & ~7)
        * tex_format_to_bytespp(gxm_texture_get_format(texture));
}

void *gxm_texture_get_datap(const gxm_texture *texture)
{
    return sceGxmTextureGetData(&texture->gxm_tex);
}

void *gxm_texture_get_palette(const gxm_texture *texture)
{
	return sceGxmTextureGetPalette(&texture->gxm_tex);
}

void gxm_texture_set_alloc_memblock_type(SceKernelMemBlockType type)
{
    textureMemBlockType = (type == 0) ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : type;
}

gxm_texture* create_gxm_texture(unsigned int w, unsigned int h, SceGxmTextureFormat format)
{
    gxm_texture *texture = SDL_malloc(sizeof(gxm_texture));
    if (!texture)
        return NULL;

    const int tex_size =  ((w + 7) & ~ 7) * h * tex_format_to_bytespp(format);

    /* Allocate a GPU buffer for the texture */
    void *texture_data = mem_gpu_alloc(
        textureMemBlockType,
        tex_size,
        SCE_GXM_TEXTURE_ALIGNMENT,
        SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
        &texture->data_UID
    );

    if (!texture_data) {
        free(texture);
        return NULL;
    }

    /* Clear the texture */
    SDL_memset(texture_data, 0, tex_size);

    /* Create the gxm texture */
    sceGxmTextureInitLinear( &texture->gxm_tex, texture_data, format, w, h, 0);

    if ((format & 0x9f000000U) == SCE_GXM_TEXTURE_BASE_FORMAT_P8) {
        const int pal_size = 256 * sizeof(uint32_t);

        void *texture_palette = mem_gpu_alloc(
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            pal_size,
            SCE_GXM_PALETTE_ALIGNMENT,
            SCE_GXM_MEMORY_ATTRIB_READ,
            &texture->palette_UID);

        if (!texture_palette) {
            texture->palette_UID = 0;
            free_gxm_texture(texture);
            return NULL;
        }

        SDL_memset(texture_palette, 0, pal_size);

        sceGxmTextureSetPalette(&texture->gxm_tex, texture_palette);
    } else {
        texture->palette_UID = 0;
    }

#ifdef VITA_HW_ACCEL
    {
        void *depthBufferData;
        const uint32_t aligneColorSurfacedStride = ALIGN(w, 8);
        const uint32_t alignedWidth = ALIGN(w, SCE_GXM_TILE_SIZEX);
        const uint32_t alignedHeight = ALIGN(h, SCE_GXM_TILE_SIZEY);
        uint32_t sampleCount = alignedWidth*alignedHeight;
        uint32_t depthStrideInSamples = alignedWidth;

        SceGxmColorFormat colorFormat;

        switch (format)
        {
            case SCE_GXM_TEXTURE_FORMAT_P8_ABGR:
                colorFormat = SCE_GXM_COLOR_FORMAT_A8;  //kinda wrong
                break;
            case SCE_GXM_TEXTURE_FORMAT_A1R5G5B5:
                colorFormat = SCE_GXM_COLOR_FORMAT_A1R5G5B5;
                break;
            case SCE_GXM_TEXTURE_FORMAT_R5G6B5:
                colorFormat = SCE_GXM_COLOR_FORMAT_R5G6B5;
                break;
            case SCE_GXM_TEXTURE_FORMAT_U8U8U8_RGB:
                colorFormat = SCE_GXM_COLOR_FORMAT_U8U8U8_RGB;
                break;
            case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB:
                colorFormat = SCE_GXM_COLOR_FORMAT_A8R8G8B8;
                break;
            case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR:
                colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
                break;
            default:
                SDL_SetError("Invalid texture format!\n");
                break;
        }

        int err = sceGxmColorSurfaceInit(
            &texture->gxm_colorsurface,
            colorFormat,
            SCE_GXM_COLOR_SURFACE_LINEAR,
            SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            w,
            h,
            aligneColorSurfacedStride,
            texture_data
        );

        if (err < 0) {
            free_gxm_texture(texture);
            SDL_SetError("color surface init failed: %d\n", err);
            return NULL;
        }

        // allocate it
        depthBufferData = mem_gpu_alloc(
            SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
            4*sampleCount,
            SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT,
            SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
            &texture->depth_UID);

        // create the SceGxmDepthStencilSurface structure
        err = sceGxmDepthStencilSurfaceInit(
            &texture->gxm_depthstencil,
            SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
            SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
            depthStrideInSamples,
            depthBufferData,
            NULL);

        if (err < 0) {
            free_gxm_texture(texture);
            SDL_SetError("depth stencil init failed: %d\n", err);
            return NULL;
        }

        {
            SceGxmRenderTarget *tgt = NULL;

            // set up parameters
            SceGxmRenderTargetParams renderTargetParams;
            memset(&renderTargetParams, 0, sizeof(SceGxmRenderTargetParams));
            renderTargetParams.flags = 0;
            renderTargetParams.width = w;
            renderTargetParams.height = h;
            renderTargetParams.scenesPerFrame = 1;
            renderTargetParams.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
            renderTargetParams.multisampleLocations = 0;
            renderTargetParams.driverMemBlock = -1;

            // create the render target
            err = sceGxmCreateRenderTarget(&renderTargetParams, &tgt);

            texture->gxm_rendertarget = tgt;

            if (err < 0) {
                free_gxm_texture(texture);
                SDL_SetError("create render target failed: %d\n", err);
                return NULL;
            }
        }
    }

    for (int i = 0; i < NOTIF_NUM; ++i)
    {
        if (!notification_busy[i])
        {
            notification_busy[i] = 1;
            texture->notification_id = i;
            texture->fragment_notif.address = notificationMem + i;
            texture->fragment_notif.value = 1;
            *texture->fragment_notif.address = texture->fragment_notif.value;
            break;
        }
    }
#endif

    return texture;
}

void gxm_init_texture_scale(const gxm_texture *texture, float x, float y, float x_scale, float y_scale)
{
    const int tex_w = gxm_texture_get_width(texture);
    const int tex_h = gxm_texture_get_height(texture);
    const float w = x_scale * tex_w;
    const float h = y_scale * tex_h;

    data->screenVertices[0].x = (x / VITA_GXM_SCREEN_WIDTH * 2.0f) - 1.0f;
    data->screenVertices[0].y = (y / VITA_GXM_SCREEN_HEIGHT * 2.0f - 1.0f) * -1.0f;
    data->screenVertices[0].z = +0.5f;
    data->screenVertices[0].u = 0.0f;
    data->screenVertices[0].v = 0.0f;

    data->screenVertices[1].x = (x + w) / VITA_GXM_SCREEN_WIDTH * 2.0f - 1.0f;
    data->screenVertices[1].y = (y / VITA_GXM_SCREEN_HEIGHT * 2.0f - 1.0f) * -1.0f;
    data->screenVertices[1].z = +0.5f;
    data->screenVertices[1].u = 1.0f;
    data->screenVertices[1].v = 0.0f;

    data->screenVertices[2].x = (x / VITA_GXM_SCREEN_WIDTH * 2.0f) - 1.0f;
    data->screenVertices[2].y = ((y + h) / VITA_GXM_SCREEN_HEIGHT * 2.0f - 1.0f) * -1.0f;
    data->screenVertices[2].z = +0.5f;
    data->screenVertices[2].u = 0.0f;
    data->screenVertices[2].v = 1.0f;

    data->screenVertices[3].x = (x + w) / VITA_GXM_SCREEN_WIDTH * 2.0f - 1.0f;
    data->screenVertices[3].y = ((y + h) / VITA_GXM_SCREEN_HEIGHT * 2.0f - 1.0f) * -1.0f;
    data->screenVertices[3].z = +0.5f;
    data->screenVertices[3].u = 1.0f;
    data->screenVertices[3].v = 1.0f;
}

void gxm_start_drawing()
{
    sceGxmBeginScene(
        data->gxm_context,
        0,
        data->renderTarget,
        NULL,
        NULL,
        data->displayBufferSync[data->backBufferIndex],
        &data->displaySurface[data->backBufferIndex],
        &data->depthSurface
    );

    *flipFragmentNotif.address = 0;
}

void gxm_wait_rendering_done()
{
    sceGxmFinish(data->gxm_context);
}

void gxm_end_drawing()
{
    sceGxmEndScene(data->gxm_context, NULL, &flipFragmentNotif);
}

void gxm_swap_buffers()
{
    data->displayData.address = data->displayBufferData[data->backBufferIndex];

    SceCommonDialogUpdateParam updateParam;
    SDL_memset(&updateParam, 0, sizeof(updateParam));

    updateParam.renderTarget.colorFormat    = VITA_GXM_COLOR_FORMAT;
    updateParam.renderTarget.surfaceType    = SCE_GXM_COLOR_SURFACE_LINEAR;
    updateParam.renderTarget.width          = VITA_GXM_SCREEN_WIDTH;
    updateParam.renderTarget.height         = VITA_GXM_SCREEN_HEIGHT;
    updateParam.renderTarget.strideInPixels = VITA_GXM_SCREEN_STRIDE;

    updateParam.renderTarget.colorSurfaceData = data->displayBufferData[data->backBufferIndex];
    updateParam.renderTarget.depthSurfaceData = data->depthBufferData;

    updateParam.displaySyncObject = (SceGxmSyncObject *)data->displayBufferSync[data->backBufferIndex];

    sceCommonDialogUpdate(&updateParam);

    if (gxm_finish_wait && *flipFragmentNotif.address != flipFragmentNotif.value)
    {
        sceGxmFinish(data->gxm_context);
    }

    sceGxmDisplayQueueAddEntry(
        data->displayBufferSync[data->frontBufferIndex],    // OLD fb
        data->displayBufferSync[data->backBufferIndex],     // NEW fb
        &data->displayData
    );

    // update buffer indices
    data->frontBufferIndex = data->backBufferIndex;
    data->backBufferIndex = (data->backBufferIndex + 1) % VITA_GXM_BUFFERS;
#ifdef VITA_HW_ACCEL
    data->pool_index = 0;
    data->current_pool = (data->current_pool + 1) % 2;
#endif
}

void gxm_texture_set_filters(gxm_texture *texture, SceGxmTextureFilter min_filter, SceGxmTextureFilter mag_filter)
{
    sceGxmTextureSetMinFilter(&texture->gxm_tex, min_filter);
    sceGxmTextureSetMagFilter(&texture->gxm_tex, mag_filter);
}

void gxm_set_vblank_wait(int enable)
{
    data->displayData.vblank_wait = enable;
}

void gxm_set_finish_wait(int enable)
{
    gxm_finish_wait = enable;
}

void gxm_draw_screen_texture(gxm_texture *texture)
{
#ifdef VITA_HW_ACCEL
    set_alpha_blend(0);
#endif
    void *vertex_wvp_buffer;
    sceGxmSetVertexProgram(data->gxm_context, data->textureVertexProgram);
    sceGxmSetFragmentProgram(data->gxm_context, data->textureFragmentProgram);
    sceGxmSetVertexStream(data->gxm_context, 0, data->screenVertices);
    sceGxmReserveVertexDefaultUniformBuffer(data->gxm_context, &vertex_wvp_buffer);
    sceGxmSetUniformDataF(vertex_wvp_buffer, data->textureWvpParam, 0, 16, data->ortho_matrix);
    sceGxmSetFragmentTexture(data->gxm_context, 0, &texture->gxm_tex);
    sceGxmDraw(data->gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, data->linearIndices, 4);
}

void gxm_render_clear()
{
    void *color_buffer;
    float clear_color[4];

    clear_color[0] = 0;
    clear_color[1] = 0;
    clear_color[2] = 0;
    clear_color[3] = 1;

    // set clear shaders
    sceGxmSetVertexProgram(data->gxm_context, data->clearVertexProgram);
    sceGxmSetFragmentProgram(data->gxm_context, data->clearFragmentProgram);

    // set the clear color
    sceGxmReserveFragmentDefaultUniformBuffer(data->gxm_context, &color_buffer);
    sceGxmSetUniformDataF(color_buffer, data->clearClearColorParam, 0, 4, clear_color);

    // draw the clear triangle
    sceGxmSetVertexStream(data->gxm_context, 0, data->clearVertices);
    sceGxmDraw(data->gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, data->linearIndices, 3);
}

#ifdef VITA_HW_ACCEL
// what if locked texture is currently being read from..? probably still safer to use gxm_wait_rendering_done() for locking
// also there's a possibility of more that 1 job is queued that may result in fired up notification while there are still jobs left to do
// rework of notification system, sceGxmFinish or ensuring that jobs are finished before queuing new ones is probably a safer approach (and slower one)
void gxm_lock_texture(gxm_texture *texture)
{
    //if(*texture->fragment_notif.address != texture->fragment_notif.value)
    //    sceGxmFinish(data->gxm_context);
    while(*texture->fragment_notif.address != texture->fragment_notif.value) {};
}

void gxm_fill_rect(gxm_texture *dst, SDL_Rect dstrect, float r, float g, float b, float a)
{
    sceGxmBeginScene(
        data->gxm_context,
        0,
        dst->gxm_rendertarget,
        NULL,
        NULL,
        NULL,
        &dst->gxm_colorsurface,
        &dst->gxm_depthstencil
    );

    *dst->fragment_notif.address = 0;

    void *color_buffer;
    float fill_color[4];

    fill_color[0] = r;
    fill_color[1] = g;
    fill_color[2] = b;
    fill_color[3] = a;

    float tex_w = gxm_texture_get_width(dst);
    float tex_h = gxm_texture_get_height(dst);

    clear_vertex *fillVertices = pool_memalign(
        data,
        sizeof(clear_vertex) * 4,
        sizeof(clear_vertex)
    );

    fillVertices[0].x = (dstrect.x / tex_w) * 2.0f - 1.0f;
    fillVertices[0].y = ((dstrect.y / tex_h) * 2.0f - 1.0f) * -1.0f;

    fillVertices[1].x = ((dstrect.x + dstrect.w) / tex_w) * 2.0f - 1.0f;
    fillVertices[1].y = ((dstrect.y / tex_h) * 2.0f - 1.0f) * -1.0f;

    fillVertices[2].x = (dstrect.x / tex_w) * 2.0f - 1.0f;
    fillVertices[2].y = (((dstrect.y + dstrect.h) / tex_h) * 2.0f - 1.0f) * -1.0f;

    fillVertices[3].x = ((dstrect.x + dstrect.w) / tex_w) * 2.0f - 1.0f;
    fillVertices[3].y = (((dstrect.y + dstrect.h) / tex_h) * 2.0f - 1.0f) * -1.0f;

    sceGxmSetVertexProgram(data->gxm_context, data->clearVertexProgram);
    sceGxmSetFragmentProgram(data->gxm_context, data->clearFragmentProgram);
    sceGxmReserveFragmentDefaultUniformBuffer(data->gxm_context, &color_buffer);
    sceGxmSetUniformDataF(color_buffer, data->clearClearColorParam, 0, 4, fill_color);
    sceGxmSetVertexStream(data->gxm_context, 0, fillVertices);
    sceGxmDraw(data->gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, data->linearIndices, 4);
    sceGxmEndScene(data->gxm_context, NULL, &dst->fragment_notif);
    //sceGxmFinish(data->gxm_context);
}

void gxm_blit(gxm_texture *src, SDL_Rect srcrect, gxm_texture *dst, SDL_Rect dstrect, int alpha_blit)
{
    sceGxmBeginScene(
        data->gxm_context,
        0,
        dst->gxm_rendertarget,
        NULL,
        NULL,
        NULL,
        &dst->gxm_colorsurface,
        &dst->gxm_depthstencil
    );

    *dst->fragment_notif.address = 0;

    // works properly with centered screen surfaces ONLY
    float src_x = srcrect.x;
    float src_y = srcrect.y;
    float src_w = srcrect.w;
    float src_h = srcrect.h;
    float src_tex_w = gxm_texture_get_width(src);
    float src_tex_h = gxm_texture_get_height(src);
    float dst_x = dstrect.x;
    float dst_y = dstrect.y;
    float dst_tex_w = gxm_texture_get_width(dst);
    float dst_tex_h = gxm_texture_get_height(dst);

    texture_vertex *blitVertices = pool_memalign(
        data,
        4 * sizeof(texture_vertex),
        sizeof(texture_vertex)
    );

    blitVertices[0].x = (dst_x / dst_tex_w) * 2.0f - 1.0f;
    blitVertices[0].y = ((dst_y / dst_tex_h) * 2.0f - 1.0f) * -1.0f;
    blitVertices[0].z = +0.5f;
    blitVertices[0].u = src_x / src_tex_w;
    blitVertices[0].v = src_y / src_tex_h;

    blitVertices[1].x = ((dst_x + src_w) / dst_tex_w) * 2.0f - 1.0f;
    blitVertices[1].y = ((dst_y / dst_tex_h) * 2.0f - 1.0f) * -1.0f;
    blitVertices[1].z = +0.5f;
    blitVertices[1].u = (src_x + src_w) / src_tex_w;
    blitVertices[1].v = src_y / src_tex_h;

    blitVertices[2].x = (dst_x / dst_tex_w) * 2.0f - 1.0f;
    blitVertices[2].y = (((dst_y + src_h) / dst_tex_h) * 2.0f - 1.0f) * -1.0f;
    blitVertices[2].z = +0.5f;
    blitVertices[2].u = src_x / src_tex_w;
    blitVertices[2].v = (src_y + src_h) / src_tex_h;

    blitVertices[3].x = ((dst_x + src_w) / dst_tex_w) * 2.0f - 1.0f;
    blitVertices[3].y = (((dst_y + src_h) / dst_tex_h) * 2.0f - 1.0f) * -1.0f;
    blitVertices[3].z = +0.5f;
    blitVertices[3].u = (src_x + src_w) / src_tex_w;
    blitVertices[3].v = (src_y + src_h) / src_tex_h;

    set_alpha_blend(alpha_blit);

    void *vertex_wvp_buffer;
    sceGxmSetVertexProgram(data->gxm_context, data->textureVertexProgram);
    sceGxmSetFragmentProgram(data->gxm_context, data->textureFragmentProgram);

    sceGxmSetFragmentTexture(data->gxm_context, 0, &src->gxm_tex);
    sceGxmSetVertexStream(data->gxm_context, 0, blitVertices);
    sceGxmReserveVertexDefaultUniformBuffer(data->gxm_context, &vertex_wvp_buffer);

    sceGxmSetUniformDataF(vertex_wvp_buffer, data->textureWvpParam, 0, 16, data->ortho_matrix);
    sceGxmDraw(data->gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, data->linearIndices, 4);

    sceGxmEndScene(data->gxm_context, NULL, &dst->fragment_notif);
    //sceGxmFinish(data->gxm_context);
}
#endif

static unsigned int back_buffer_index_for_common_dialog = 0;
static unsigned int front_buffer_index_for_common_dialog = 0;

struct
{
    VITA_GXM_DisplayData displayData;
    SceGxmSyncObject* sync;
    SceGxmColorSurface surf;
    SceUID uid;
} buffer_for_common_dialog[VITA_GXM_BUFFERS];

void gxm_minimal_init_for_common_dialog(void)
{
    SceGxmInitializeParams initializeParams;
    SDL_memset(&initializeParams, 0, sizeof(initializeParams));
    initializeParams.flags                          = 0;
    initializeParams.displayQueueMaxPendingCount    = VITA_GXM_PENDING_SWAPS;
    initializeParams.displayQueueCallback           = display_callback;
    initializeParams.displayQueueCallbackDataSize   = sizeof(VITA_GXM_DisplayData);
    initializeParams.parameterBufferSize            = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    sceGxmInitialize(&initializeParams);
}

void gxm_minimal_term_for_common_dialog(void)
{
    sceGxmTerminate();
}

void gxm_init_for_common_dialog(void)
{
    for (int i = 0; i < VITA_GXM_BUFFERS; i += 1)
    {
        buffer_for_common_dialog[i].displayData.vblank_wait = SDL_TRUE;
        buffer_for_common_dialog[i].displayData.address = mem_gpu_alloc(
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            4 * VITA_GXM_SCREEN_STRIDE * VITA_GXM_SCREEN_HEIGHT,
            SCE_GXM_COLOR_SURFACE_ALIGNMENT,
            SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
            &buffer_for_common_dialog[i].uid);
        sceGxmColorSurfaceInit(
            &buffer_for_common_dialog[i].surf,
            VITA_GXM_PIXEL_FORMAT,
            SCE_GXM_COLOR_SURFACE_LINEAR,
            SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            VITA_GXM_SCREEN_WIDTH,
            VITA_GXM_SCREEN_HEIGHT,
            VITA_GXM_SCREEN_STRIDE,
            buffer_for_common_dialog[i].displayData.address
        );
        sceGxmSyncObjectCreate(&buffer_for_common_dialog[i].sync);
    }
    sceGxmDisplayQueueFinish();
}

void gxm_swap_for_common_dialog(void)
{
    SceCommonDialogUpdateParam updateParam;
    SDL_memset(&updateParam, 0, sizeof(updateParam));

    updateParam.renderTarget.colorFormat    = VITA_GXM_PIXEL_FORMAT;
    updateParam.renderTarget.surfaceType    = SCE_GXM_COLOR_SURFACE_LINEAR;
    updateParam.renderTarget.width          = VITA_GXM_SCREEN_WIDTH;
    updateParam.renderTarget.height         = VITA_GXM_SCREEN_HEIGHT;
    updateParam.renderTarget.strideInPixels = VITA_GXM_SCREEN_STRIDE;

    updateParam.renderTarget.colorSurfaceData = buffer_for_common_dialog[back_buffer_index_for_common_dialog].displayData.address;

    updateParam.displaySyncObject = buffer_for_common_dialog[back_buffer_index_for_common_dialog].sync;
    SDL_memset(buffer_for_common_dialog[back_buffer_index_for_common_dialog].displayData.address, 0, 4 * VITA_GXM_SCREEN_STRIDE * VITA_GXM_SCREEN_HEIGHT);
    sceCommonDialogUpdate(&updateParam);

    sceGxmDisplayQueueAddEntry(buffer_for_common_dialog[front_buffer_index_for_common_dialog].sync, buffer_for_common_dialog[back_buffer_index_for_common_dialog].sync, &buffer_for_common_dialog[back_buffer_index_for_common_dialog].displayData);
    front_buffer_index_for_common_dialog = back_buffer_index_for_common_dialog;
    back_buffer_index_for_common_dialog = (back_buffer_index_for_common_dialog + 1) % VITA_GXM_BUFFERS;
}

void gxm_term_for_common_dialog(void)
{
    sceGxmDisplayQueueFinish();
    for (int i = 0; i < VITA_GXM_BUFFERS; i += 1)
    {
        mem_gpu_free(buffer_for_common_dialog[i].uid);
        sceGxmSyncObjectDestroy(buffer_for_common_dialog[i].sync);
    }
}
