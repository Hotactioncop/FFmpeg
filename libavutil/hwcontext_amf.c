/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_amf.h"
#include "hwcontext_internal.h"
#if CONFIG_VULKAN
#include "hwcontext_vulkan.h"
#endif
#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"
#include "imgutils.h"
#include "libavutil/avassert.h"
#include <AMF/core/Surface.h>
#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

static const AVClass amflib_class = {
    .class_name = "amf",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

typedef struct AMFLibraryContext {
    const AVClass      *avclass;
} AMFLibraryContext;

static AMFLibraryContext amflib_context =
{
    .avclass = &amflib_class,
};


static void AMF_CDECL_CALL AMFTraceWriter_Write(AMFTraceWriter *pThis,
    const wchar_t *scope, const wchar_t *message)
{
    AmfTraceWriter *tracer = (AmfTraceWriter*)pThis;
    av_log(tracer->avcl, AV_LOG_DEBUG, "%ls: %ls", scope, message); // \n is provided from AMF
}

static void AMF_CDECL_CALL AMFTraceWriter_Flush(AMFTraceWriter *pThis)
{
}

static AMFTraceWriterVtbl tracer_vtbl =
{
    .Write = AMFTraceWriter_Write,
    .Flush = AMFTraceWriter_Flush,
};

AmfTraceWriter av_amf_trace_writer =
{
    .vtbl = &tracer_vtbl,
    .avcl = &amflib_context,
    .avctx = NULL
};

const FormatMap format_map[] =
{
    { AV_PIX_FMT_NONE,          AMF_SURFACE_UNKNOWN },
    { AV_PIX_FMT_NV12,          AMF_SURFACE_NV12 },
    { AV_PIX_FMT_BGR0,          AMF_SURFACE_BGRA },
    { AV_PIX_FMT_RGB0,          AMF_SURFACE_RGBA },
    { AV_PIX_FMT_BGRA,          AMF_SURFACE_BGRA },
    { AV_PIX_FMT_ARGB,          AMF_SURFACE_ARGB },
    { AV_PIX_FMT_RGBA,          AMF_SURFACE_RGBA },
    { AV_PIX_FMT_GRAY8,         AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,       AMF_SURFACE_NV12 }, // FIXME change this when hw_scaler will process format properly
    { AV_PIX_FMT_YUYV422,       AMF_SURFACE_YUY2 },
    { AV_PIX_FMT_P010,          AMF_SURFACE_P010 },
    { AV_PIX_FMT_YUV420P10,     AMF_SURFACE_P010 },
    { AV_PIX_FMT_YUV420P12,     AMF_SURFACE_P012 },
    { AV_PIX_FMT_YUV420P12,     AMF_SURFACE_P012 },
    { AV_PIX_FMT_YUV420P16,     AMF_SURFACE_P016 },
    { AV_PIX_FMT_YUV422P10LE,   AMF_SURFACE_Y210 },
    { AV_PIX_FMT_YUV444P10LE,   AMF_SURFACE_Y416 },
};

enum AMF_SURFACE_FORMAT av_amf_av_to_amf_format(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].av_format == fmt) {
            return format_map[i].amf_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

enum AVPixelFormat av_amf_to_av_format(enum AMF_SURFACE_FORMAT fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].amf_format == fmt) {
            return format_map[i].av_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_BGRA,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_YUV420P10,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_AMF
};

static int amf_frames_get_constraints(AVHWDeviceContext *ctx,
                                       const void *hwconfig,
                                       AVHWFramesConstraints *constraints)
{
    int i;

    constraints->valid_sw_formats = av_malloc_array(FF_ARRAY_ELEMS(supported_formats) + 1,
                                                    sizeof(*constraints->valid_sw_formats));
    if (!constraints->valid_sw_formats)
        return AVERROR(ENOMEM);

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        constraints->valid_sw_formats[i] = supported_formats[i];
    constraints->valid_sw_formats[FF_ARRAY_ELEMS(supported_formats)] = AV_PIX_FMT_NONE;

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(*constraints->valid_hw_formats));
    if (!constraints->valid_hw_formats)
        return AVERROR(ENOMEM);

    constraints->valid_hw_formats[0] = AV_PIX_FMT_AMF;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    return 0;
}

static void amf_dummy_free(void *opaque, uint8_t *data)
{

}

static AVBufferRef *amf_pool_alloc(void *opaque, size_t size)
{
    AVHWFramesContext *hwfc = (AVHWFramesContext *)opaque;
    AVBufferRef *buf;

    buf = av_buffer_create(NULL, NULL, amf_dummy_free, hwfc, AV_BUFFER_FLAG_READONLY);
    if (!buf) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create buffer for AMF context.\n");
        return NULL;
    }
    return buf;
}

static int amf_frames_init(AVHWFramesContext *ctx)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (ctx->sw_format == supported_formats[i])
            break;
    }
    if (i == FF_ARRAY_ELEMS(supported_formats)) {
        av_log(ctx, AV_LOG_ERROR, "Pixel format '%s' is not supported\n",
               av_get_pix_fmt_name(ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    ctx->internal->pool_internal =
            av_buffer_pool_init2(sizeof(AMFSurface), ctx,
                                 &amf_pool_alloc, NULL);


    return 0;
}

static int amf_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
    frame->buf[0] = av_buffer_pool_get(ctx->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->data[3] = frame->buf[0]->data;
    frame->format  = AV_PIX_FMT_AMF;
    frame->width   = ctx->width;
    frame->height  = ctx->height;
    return 0;
}

static int amf_transfer_get_formats(AVHWFramesContext *ctx,
                                     enum AVHWFrameTransferDirection dir,
                                     enum AVPixelFormat **formats)
{
    enum AVPixelFormat *fmts;

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts)
        return AVERROR(ENOMEM);

    fmts[0] = ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;

    return 0;
}

static int amf_transfer_data_to(AVHWFramesContext *ctx, AVFrame *dst,
                                 const AVFrame *src)
{
    AMFSurface* surface = (AMFSurface*)dst->data[3];
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;
    int w = FFMIN(dst->width,  src->width);
    int h = FFMIN(dst->height, src->height);

    planes = (int)surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy(dst_data, dst_linesize,
        (const uint8_t**)src->data, src->linesize, src->format,
        w, h);

    return 0;
}
static int amf_transfer_data_from(AVHWFramesContext *ctx, AVFrame *dst,
                                    const AVFrame *src)
{
    AMFSurface* surface = (AMFSurface*)src->data[3];
    AMFPlane *plane;
    uint8_t  *src_data[4];
    int       src_linesize[4];
    int       planes;
    int       i;
    int w = FFMIN(dst->width,  src->width);
    int h = FFMIN(dst->height, src->height);


    planes = (int)surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(src_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        src_data[i] = plane->pVtbl->GetNative(plane);
        src_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy(dst->data, dst->linesize,
                  (const uint8_t **)src_data, src_linesize, dst->format,
                  w, h);

    return 0;
}


static void amf_device_uninit(AVHWDeviceContext *device_ctx)
{
    AVAMFDeviceContext      *amf_ctx = device_ctx->hwctx;
    av_buffer_unref(&amf_ctx->internal);
}

static int amf_device_init(AVHWDeviceContext *ctx)
{
    AVAMFDeviceContext *amf_ctx = ctx->hwctx;
    return av_amf_context_init((AVAMFDeviceContextInternal * )amf_ctx->internal->data, ctx);
}

static int amf_device_create(AVHWDeviceContext *device_ctx,
                              const char *device,
                              AVDictionary *opts, int flags)
{
    AVAMFDeviceContext        *ctx = device_ctx->hwctx;
    AVAMFDeviceContextInternal *wrapped = av_mallocz(sizeof(*wrapped));
    ctx->internal = av_buffer_create((uint8_t *)wrapped, sizeof(*wrapped),
                                                    av_amf_context_internal_free, NULL, 0);
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal * )ctx->internal->data;
    int ret;
    if ((ret = av_amf_load_library(internal, device_ctx)) == 0) {
        if ((ret = av_amf_create_context(internal, device_ctx, "", opts, flags)) == 0){
            return 0;
        }
    }
    amf_device_uninit(device_ctx);
    return ret;
}

static int amf_device_derive(AVHWDeviceContext *device_ctx,
                              AVHWDeviceContext *child_device_ctx, AVDictionary *opts,
                              int flags)
{
    AVAMFDeviceContext        *ctx = device_ctx->hwctx;
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal * )ctx->internal->data;
    int ret;

    ret = amf_device_create(device_ctx, "", opts, flags);
    if(ret < 0)
        return ret;

    return av_amf_context_derive(internal, child_device_ctx, opts, flags);
}

#if CONFIG_DXVA2
static int amf_init_from_dxva2_device(AVAMFDeviceContextInternal * internal, AVDXVA2DeviceContext *hwctx)
{
    IDirect3DDevice9    *device;
    HANDLE              device_handle;
    HRESULT             hr;
    AMF_RESULT          res;
    int ret;

    hr = IDirect3DDeviceManager9_OpenDeviceHandle(hwctx->devmgr, &device_handle);
    if (FAILED(hr)) {
        av_log(hwctx, AV_LOG_ERROR, "Failed to open device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    hr = IDirect3DDeviceManager9_LockDevice(hwctx->devmgr, device_handle, &device, FALSE);
    if (SUCCEEDED(hr)) {
        IDirect3DDeviceManager9_UnlockDevice(hwctx->devmgr, device_handle, FALSE);
        ret = 0;
    } else {
        av_log(hwctx, AV_LOG_ERROR, "Failed to lock device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
    }


    IDirect3DDeviceManager9_CloseDeviceHandle(hwctx->devmgr, device_handle);

    if (ret < 0)
        return ret;

    res = internal->context->pVtbl->InitDX9(internal->context, device);

    IDirect3DDevice9_Release(device);

    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(hwctx, AV_LOG_ERROR, "AMF via D3D9 is not supported on the given device.\n");
        else
            av_log(hwctx, AV_LOG_ERROR, "AMF failed to initialise on given D3D9 device: %d.\n", res);
        return AVERROR(ENODEV);
    }
    internal->mem_type = AMF_MEMORY_DX9;
    return 0;
}
#endif

#if CONFIG_D3D11VA
static int amf_init_from_d3d11_device(AVAMFDeviceContextInternal* internal, AVD3D11VADeviceContext *hwctx)
{
    AMF_RESULT res;
    res = internal->context->pVtbl->InitDX11(internal->context, hwctx->device, AMF_DX11_1);
    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(hwctx, AV_LOG_ERROR, "AMF via D3D11 is not supported on the given device.\n");
        else
            av_log(hwctx, AV_LOG_ERROR, "AMF failed to initialise on the given D3D11 device: %d.\n", res);
        return AVERROR(ENODEV);
    }
    internal->mem_type = AMF_MEMORY_DX11;
    return 0;
}
#endif

int av_amf_context_init(AVAMFDeviceContextInternal* internal, void* avcl)
{
     AMFContext1 *context1 = NULL;
     AMF_RESULT res;

    res = internal->context->pVtbl->InitDX11(internal->context, NULL, AMF_DX11_1);
    if (res == AMF_OK) {
        internal->mem_type = AMF_MEMORY_DX11;
        av_log(avcl, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D11.\n");
    } else {
        res = internal->context->pVtbl->InitDX9(internal->context, NULL);
        if (res == AMF_OK) {
            internal->mem_type = AMF_MEMORY_DX9;
            av_log(avcl, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D9.\n");
        } else {
            AMFGuid guid = IID_AMFContext1();
            res = internal->context->pVtbl->QueryInterface(internal->context, &guid, (void**)&context1);
            AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext1() failed with error %d\n", res);

            res = context1->pVtbl->InitVulkan(context1, NULL);
            context1->pVtbl->Release(context1);
            if (res != AMF_OK) {
                if (res == AMF_NOT_SUPPORTED)
                    av_log(avcl, AV_LOG_ERROR, "AMF via Vulkan is not supported on the given device.\n");
                 else
                    av_log(avcl, AV_LOG_ERROR, "AMF failed to initialise on the given Vulkan device: %d.\n", res);
                 return AVERROR(ENOSYS);
            }
            internal->mem_type = AMF_MEMORY_VULKAN;
            av_log(avcl, AV_LOG_VERBOSE, "AMF initialisation succeeded via Vulkan.\n");
         }
     }
     return 0;
}
int av_amf_load_library(AVAMFDeviceContextInternal* internal,  void* avcl)
{
    AMFInit_Fn         init_fun;
    AMFQueryVersion_Fn version_fun;
    AMF_RESULT         res;

    internal->library = dlopen(AMF_DLL_NAMEA, RTLD_NOW | RTLD_LOCAL);
    AMF_RETURN_IF_FALSE(avcl, internal->library != NULL,
        AVERROR_UNKNOWN, "DLL %s failed to open\n", AMF_DLL_NAMEA);

    init_fun = (AMFInit_Fn)dlsym(internal->library, AMF_INIT_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(avcl, init_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_INIT_FUNCTION_NAME);

    version_fun = (AMFQueryVersion_Fn)dlsym(internal->library, AMF_QUERY_VERSION_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(avcl, version_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_QUERY_VERSION_FUNCTION_NAME);

    res = version_fun(&internal->version);
    AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_QUERY_VERSION_FUNCTION_NAME, res);
    res = init_fun(AMF_FULL_VERSION, &internal->factory);
    AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_INIT_FUNCTION_NAME, res);
    res = internal->factory->pVtbl->GetTrace(internal->factory, &internal->trace);
    AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "GetTrace() failed with error %d\n", res);
    res = internal->factory->pVtbl->GetDebug(internal->factory, &internal->debug);
    AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "GetDebug() failed with error %d\n", res);
    return 0;
}

int av_amf_create_context(  AVAMFDeviceContextInternal * internal,
                                void* avcl,
                                const char *device,
                                AVDictionary *opts, int flags)
{
    AMF_RESULT         res;

    internal->trace->pVtbl->EnableWriter(internal->trace, AMF_TRACE_WRITER_CONSOLE, 0);
    internal->trace->pVtbl->SetGlobalLevel(internal->trace, AMF_TRACE_TRACE);

     // connect AMF logger to av_log
    av_amf_trace_writer.avctx = avcl;
    internal->trace->pVtbl->RegisterWriter(internal->trace, FFMPEG_AMF_WRITER_ID, (AMFTraceWriter*)&av_amf_trace_writer, 1);
    internal->trace->pVtbl->SetWriterLevel(internal->trace, FFMPEG_AMF_WRITER_ID, AMF_TRACE_TRACE);

    res = internal->factory->pVtbl->CreateContext(internal->factory, &internal->context);
    AMF_RETURN_IF_FALSE(avcl, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext() failed with error %d\n", res);

    return 0;
}

int av_amf_context_internal_create(AVAMFDeviceContextInternal * internal,
                                void* avcl,
                                const char *device,
                                AVDictionary *opts, int flags)
{
    int ret;
    if ((ret = av_amf_load_library(internal, avcl)) == 0) {
        if ((ret = av_amf_create_context(internal, avcl, "", opts, flags)) == 0){
            return 0;
        }
    }
    av_amf_context_internal_free(0, (uint8_t *)internal);
    return ret;
}

int av_amf_context_internal_free(void *opaque, uint8_t *data)
{
    AVAMFDeviceContextInternal *amf_ctx = (AVAMFDeviceContextInternal *)data;
    if (amf_ctx->context) {
        amf_ctx->context->pVtbl->Terminate(amf_ctx->context);
        amf_ctx->context->pVtbl->Release(amf_ctx->context);
        amf_ctx->context = NULL;
    }

    if (amf_ctx->trace) {
        amf_ctx->trace->pVtbl->UnregisterWriter(amf_ctx->trace, FFMPEG_AMF_WRITER_ID);
    }

    if(amf_ctx->library) {
        dlclose(amf_ctx->library);
        amf_ctx->library = NULL;
    }

    amf_ctx->debug = NULL;
    amf_ctx->version = 0;
    av_free(amf_ctx);
    return 0;
}

int av_amf_context_derive(AVAMFDeviceContextInternal * internal,
                               AVHWDeviceContext *child_device_ctx, AVDictionary *opts,
                               int flags)
{

    switch (child_device_ctx->type) {

#if CONFIG_DXVA2
    case AV_HWDEVICE_TYPE_DXVA2:
        {
            AVDXVA2DeviceContext *child_device_hwctx = child_device_ctx->hwctx;
            return amf_init_from_dxva2_device(internal, child_device_hwctx);
        }
        break;
#endif

#if CONFIG_D3D11VA
    case AV_HWDEVICE_TYPE_D3D11VA:
        {
            AVD3D11VADeviceContext *child_device_hwctx = child_device_ctx->hwctx;
            return amf_init_from_d3d11_device(internal, child_device_hwctx);
        }
        break;
#endif
    default:
        {
            av_log(child_device_ctx, AV_LOG_ERROR, "AMF initialisation from a %s device is not supported.\n",
                av_hwdevice_get_type_name(child_device_ctx->type));
            return AVERROR(ENOSYS);
        }
    }
    return 0;
}

const HWContextType ff_hwcontext_type_amf = {
    .type                 = AV_HWDEVICE_TYPE_AMF,
    .name                 = "AMF",

    .device_hwctx_size    = sizeof(AVAMFDeviceContext),
    .frames_priv_size     = sizeof(AMFFramesContext),

    .device_create        = amf_device_create,
    .device_derive        = amf_device_derive,
    .device_init          = amf_device_init,
    .device_uninit        = amf_device_uninit,
    .frames_get_constraints = amf_frames_get_constraints,
    .frames_init          = amf_frames_init,
    .frames_get_buffer    = amf_get_buffer,
    .transfer_get_formats = amf_transfer_get_formats,
    .transfer_data_to     = amf_transfer_data_to,
    .transfer_data_from   = amf_transfer_data_from,

    .pix_fmts             = (const enum AVPixelFormat[]){ AV_PIX_FMT_AMF, AV_PIX_FMT_NONE },
};
