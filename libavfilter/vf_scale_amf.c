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

/**
 * @file
 * scale video filter - AMF
 */

#include <stdio.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/hwcontext.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif

#include <AMF/core/Factory.h>
#include <AMF/components/VideoConverter.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "scale_eval.h"

#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define AMFAV_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }

#define AMFAV_GOTO_FAIL_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        ret = ret_value; \
        goto fail; \
    }

typedef struct FormatMap {
    enum AVPixelFormat       av_format;
    enum AMF_SURFACE_FORMAT  amf_format;
} FormatMap;

static const FormatMap format_map[] =
{
    { AV_PIX_FMT_NV12,       AMF_SURFACE_NV12 },

    { AV_PIX_FMT_BGR0,       AMF_SURFACE_BGRA },
    { AV_PIX_FMT_BGRA,       AMF_SURFACE_BGRA },

    { AV_PIX_FMT_RGB0,       AMF_SURFACE_RGBA },
    { AV_PIX_FMT_RGBA,       AMF_SURFACE_RGBA },

    { AV_PIX_FMT_0RGB,       AMF_SURFACE_ARGB },
    { AV_PIX_FMT_ARGB,       AMF_SURFACE_ARGB },

    { AV_PIX_FMT_GRAY8,      AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,    AMF_SURFACE_YUV420P },
    { AV_PIX_FMT_YUYV422,    AMF_SURFACE_YUY2 },
};

static enum AMF_SURFACE_FORMAT amf_av_to_amf_format(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].av_format == fmt) {
            return format_map[i].amf_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"

typedef struct AmfTraceWriter {
    AMFTraceWriterVtbl *vtbl;
    AVFilterContext     *avctx;
} AmfTraceWriter;

static void AMF_CDECL_CALL AMFTraceWriter_Write(AMFTraceWriter *pThis,
    const wchar_t *scope, const wchar_t *message)
{
    AmfTraceWriter *tracer = (AmfTraceWriter*)pThis;
    av_log(tracer->avctx, AV_LOG_DEBUG, "%ls: %ls", scope, message); // \n is provided from AMF
}

static void AMF_CDECL_CALL AMFTraceWriter_Flush(AMFTraceWriter *pThis)
{
}

static AMFTraceWriterVtbl tracer_vtbl =
{
    .Write = AMFTraceWriter_Write,
    .Flush = AMFTraceWriter_Flush,
};

typedef struct AMFScaleContext {
    const AVClass *class;

    int width, height;
    enum AVPixelFormat format;
    int scale_type;

    char *w_expr;
    char *h_expr;
    char *format_str;


    amf_handle          library; ///< handle to DLL library
    AMFDebug           *debug;   ///< pointer to AMF debug interface
    AMFTrace           *trace;   ///< pointer to AMF trace interface

    amf_uint64          version; ///< version of AMF runtime
    AmfTraceWriter      tracer;  ///< AMF writer registered with AMF
    AVBufferRef        *hw_device_ctx; ///< pointer to HW accelerator (decoder)
    AVBufferRef        *hw_frames_ctx; ///< pointer to HW accelerator (frame allocator)

    AMFComponent        *converter;
    AVBufferRef         *amf_device_ref;

    AVBufferRef         *hwframes_in_ref;
    AVBufferRef         *hwframes_out_ref;
    AVBufferRef         *hwdevice_ref;

    AMFContext          *context;
    AMFFactory          *factory;

} AMFScaleContext;


static int amf_copy_surface(AVFilterContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy(dst_data, dst_linesize,
        (const uint8_t**)frame->data, frame->linesize, frame->format,
        frame->width, frame->height);

    return 0;
}

static void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    surface->pVtbl->Release(surface);
}

static AVFrame *amf_amfsurface_to_avframe(AVFilterContext *avctx, AMFSurface* pSurface)
{
    AVFrame *frame = av_frame_alloc();

    if (!frame)
        return NULL;

    switch (pSurface->pVtbl->GetMemoryType(pSurface))
    {
#if CONFIG_D3D11VA
        case AMF_MEMORY_DX11:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[0] = plane0->pVtbl->GetNative(plane0);
            frame->data[1] = (uint8_t*)(intptr_t)0;

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
#if CONFIG_DXVA2
        case AMF_MEMORY_DX9:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[3] = plane0->pVtbl->GetNative(plane0);

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
    default:
        {
            av_assert0(0);//should not happen
        }
    }

    return frame;
}

static int amf_avframe_to_amfsurface(AVFilterContext *avctx, const AVFrame *frame, AMFSurface** ppSurface)
{
    AMFScaleContext *ctx = avctx->priv;
    AMFSurface *surface;
    AMF_RESULT  res;
    int hw_surface = 0;

    switch (frame->format) {
#if CONFIG_D3D11VA
    case AV_PIX_FMT_D3D11:
        {
            static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
            ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
            int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use
            texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

            res = ctx->context->pVtbl->CreateSurfaceFromDX11Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);
            hw_surface = 1;
        }
        break;
#endif
#if CONFIG_DXVA2
    case AV_PIX_FMT_DXVA2_VLD:
        {
            IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

            res = ctx->context->pVtbl->CreateSurfaceFromDX9Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);
            hw_surface = 1;
        }
        break;
#endif
    default:
        {
            AMF_SURFACE_FORMAT amf_fmt = amf_av_to_amf_format(frame->format);
            res = ctx->context->pVtbl->AllocSurface(ctx->context, AMF_MEMORY_HOST, amf_fmt, frame->width, frame->height, &surface);
            AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
            amf_copy_surface(avctx, frame, surface);
        }
        break;
    }

    if (hw_surface) {
        // input HW surfaces can be vertically aligned by 16; tell AMF the real size
        surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);
    }

    surface->pVtbl->SetPts(surface, frame->pts);
    *ppSurface = surface;
    return 0;
}


static int amf_load_library(AVFilterContext *avctx)
{
    AMFScaleContext    *ctx = avctx->priv;
    AMFInit_Fn         init_fun;
    AMFQueryVersion_Fn version_fun;
    AMF_RESULT         res;
    if (ctx->library != NULL)
        return 0;
    ctx->library = dlopen(AMF_DLL_NAMEA, RTLD_NOW | RTLD_LOCAL);
    AMFAV_RETURN_IF_FALSE(ctx, ctx->library != NULL,
        AVERROR_UNKNOWN, "DLL %s failed to open\n", AMF_DLL_NAMEA);

    init_fun = (AMFInit_Fn)dlsym(ctx->library, AMF_INIT_FUNCTION_NAME);
    AMFAV_RETURN_IF_FALSE(ctx, init_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_INIT_FUNCTION_NAME);

    version_fun = (AMFQueryVersion_Fn)dlsym(ctx->library, AMF_QUERY_VERSION_FUNCTION_NAME);
    AMFAV_RETURN_IF_FALSE(ctx, version_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_QUERY_VERSION_FUNCTION_NAME);

    res = version_fun(&ctx->version);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_QUERY_VERSION_FUNCTION_NAME, res);
    res = init_fun(AMF_FULL_VERSION, &ctx->factory);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_INIT_FUNCTION_NAME, res);
    res = ctx->factory->pVtbl->GetTrace(ctx->factory, &ctx->trace);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetTrace() failed with error %d\n", res);
    res = ctx->factory->pVtbl->GetDebug(ctx->factory, &ctx->debug);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetDebug() failed with error %d\n", res);
    return 0;
}

#if CONFIG_D3D11VA
static int amf_init_from_d3d11_device(AVFilterContext *avctx, AVD3D11VADeviceContext *hwctx)
{
    AMFScaleContext *ctx = avctx->priv;
    AMF_RESULT res;

    res = ctx->context->pVtbl->InitDX11(ctx->context, hwctx->device, AMF_DX11_1);
    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(avctx, AV_LOG_ERROR, "AMF via D3D11 is not supported on the given device.\n");
        else
            av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on the given D3D11 device: %d.\n", res);
        return AVERROR(ENODEV);
    }

    return 0;
}
#endif

#if CONFIG_DXVA2
static int amf_init_from_dxva2_device(AVFilterContext *avctx, AVDXVA2DeviceContext *hwctx)
{
    AMFScaleContext *ctx = avctx->priv;
    HANDLE device_handle;
    IDirect3DDevice9 *device;
    HRESULT hr;
    AMF_RESULT res;
    int ret;

    hr = IDirect3DDeviceManager9_OpenDeviceHandle(hwctx->devmgr, &device_handle);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    hr = IDirect3DDeviceManager9_LockDevice(hwctx->devmgr, device_handle, &device, FALSE);
    if (SUCCEEDED(hr)) {
        IDirect3DDeviceManager9_UnlockDevice(hwctx->devmgr, device_handle, FALSE);
        ret = 0;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Failed to lock device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
    }

    IDirect3DDeviceManager9_CloseDeviceHandle(hwctx->devmgr, device_handle);

    if (ret < 0)
        return ret;

    res = ctx->context->pVtbl->InitDX9(ctx->context, device);

    IDirect3DDevice9_Release(device);

    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(avctx, AV_LOG_ERROR, "AMF via D3D9 is not supported on the given device.\n");
        else
            av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on given D3D9 device: %d.\n", res);
        return AVERROR(ENODEV);
    }

    return 0;
}
#endif

static int amf_init_context(AVFilterContext *avctx)
{
    AMFScaleContext *ctx = avctx->priv;
    AMFContext1 *context1 = NULL;
    AMF_RESULT  res;
    av_unused int ret;

    if (ctx->context != NULL)
        return 0;

    // configure AMF logger
    // the return of these functions indicates old state and do not affect behaviour
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, TRUE );
    ctx->trace->pVtbl->SetWriterLevel(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, AMF_TRACE_TRACE);
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_CONSOLE, TRUE);
    ctx->trace->pVtbl->SetGlobalLevel(ctx->trace, AMF_TRACE_TRACE);

    // connect AMF logger to av_log
    ctx->tracer.vtbl = &tracer_vtbl;
    ctx->tracer.avctx = avctx;
    ctx->trace->pVtbl->RegisterWriter(ctx->trace, FFMPEG_AMF_WRITER_ID,(AMFTraceWriter*)&ctx->tracer, 1);
    ctx->trace->pVtbl->SetWriterLevel(ctx->trace, FFMPEG_AMF_WRITER_ID, AMF_TRACE_TRACE);

    res = ctx->factory->pVtbl->CreateContext(ctx->factory, &ctx->context);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext() failed with error %d\n", res);

    if (avctx->hw_device_ctx) {
        AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;

        switch (device_ctx->type) {
#if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
            ret = amf_init_from_d3d11_device(avctx, device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
#if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
            ret = amf_init_from_dxva2_device(avctx, device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
        default:
            av_log(avctx, AV_LOG_ERROR, "AMF initialisation from a %s device is not supported.\n",
                   av_hwdevice_get_type_name(device_ctx->type));
            return AVERROR(ENOSYS);
        }

        ctx->hw_device_ctx = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hw_device_ctx)
            return AVERROR(ENOMEM);

    } else {
        res = ctx->context->pVtbl->InitDX11(ctx->context, NULL, AMF_DX11_1);
        if (res == AMF_OK) {
            av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D11.\n");
        } else {
            res = ctx->context->pVtbl->InitDX9(ctx->context, NULL);
            if (res == AMF_OK) {
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D9.\n");
            } else {
                AMFGuid guid = IID_AMFContext1();
                res = ctx->context->pVtbl->QueryInterface(ctx->context, &guid, (void**)&context1);
                AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext1() failed with error %d\n", res);

                res = context1->pVtbl->InitVulkan(context1, NULL);
                context1->pVtbl->Release(context1);
                if (res != AMF_OK) {
                    if (res == AMF_NOT_SUPPORTED)
                        av_log(avctx, AV_LOG_ERROR, "AMF via Vulkan is not supported on the given device.\n");
                    else
                        av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on the given Vulkan device: %d.\n", res);
                    return AVERROR(ENOSYS);
                }
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via Vulkan.\n");
            }
        }
    }
    return 0;
}

static int amf_scale_init(AVFilterContext *avctx)
{
    AMFScaleContext     *ctx = avctx->priv;
    int ret = 0;
    if (!strcmp(ctx->format_str, "same")) {
        ctx->format = AV_PIX_FMT_NONE;
    } else {
        ctx->format = av_get_pix_fmt(ctx->format_str);
        if (ctx->format == AV_PIX_FMT_NONE) {
            av_log(avctx, AV_LOG_ERROR, "Unrecognized pixel format: %s\n", ctx->format_str);
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

static void amf_scale_uninit(AVFilterContext *avctx)
{
    AMFScaleContext *ctx = avctx->priv;

    if (ctx->converter) {
        ctx->converter->pVtbl->Terminate(ctx->converter);
        ctx->converter->pVtbl->Release(ctx->converter);
        ctx->converter = NULL;
    }

    av_buffer_unref(&ctx->amf_device_ref);
    av_buffer_unref(&ctx->hwdevice_ref);
    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);
    
    if (ctx->context) {
        ctx->context->pVtbl->Terminate(ctx->context);
        ctx->context->pVtbl->Release(ctx->context);
        ctx->context = NULL;
    }

}

static int amf_scale_query_formats(AVFilterContext *avctx)
{
    const enum AVPixelFormat *output_pix_fmts;
    AVFilterFormats *input_formats;
    int err;
    int i;
    static const enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_0RGB,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_NONE,
    };
    static const enum AVPixelFormat output_pix_fmts_default[] = {
        AV_PIX_FMT_D3D11,
        AV_PIX_FMT_DXVA2_VLD,
        AV_PIX_FMT_NONE,
    };
    output_pix_fmts = output_pix_fmts_default;

    //in case if hw_device_ctx is set to DXVA2 we change order of pixel formats to set DXVA2 be choosen by default
    //The order is ignored if hw_frames_ctx is not NULL on the config_output stage
    if (avctx->hw_device_ctx) {
        AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;

        switch (device_ctx->type) {
    #if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
            {
                static const enum AVPixelFormat output_pix_fmts_d3d11[] = {
                    AV_PIX_FMT_D3D11,
                    AV_PIX_FMT_NONE,
                };
                output_pix_fmts = output_pix_fmts_d3d11;
            }
            break;
    #endif
    #if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
            {
                static const enum AVPixelFormat output_pix_fmts_dxva2[] = {
                    AV_PIX_FMT_DXVA2_VLD,
                    AV_PIX_FMT_NONE,
                };
                output_pix_fmts = output_pix_fmts_dxva2;
            }
            break;
    #endif
        default:
            {
                av_log(avctx, AV_LOG_ERROR, "Unsupported device : %s\n", av_hwdevice_get_type_name(device_ctx->type));
                return AVERROR(EINVAL);
            }
            break;
        }
    }

    input_formats = ff_make_format_list(output_pix_fmts);
    if (!input_formats) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; input_pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        err = ff_add_format(&input_formats, input_pix_fmts[i]);
        if (err < 0)
            return err;
    }

    if ((err = ff_formats_ref(input_formats, &avctx->inputs[0]->outcfg.formats)) < 0 ||
        (err = ff_formats_ref(ff_make_format_list(output_pix_fmts),
                              &avctx->outputs[0]->incfg.formats)) < 0)
        return err;

    return 0;
}

static int amf_scale_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink   *inlink = avctx->inputs[0];
    AMFScaleContext  *ctx = avctx->priv;
    //AVAMFDeviceContext *amf_ctx;
    AVHWFramesContext *hwframes_out;
    enum AVPixelFormat pix_fmt_in;
    AMFSize out_size;
    int err;
    AMF_RESULT res;

    if ((err = ff_scale_eval_dimensions(avctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &ctx->width, &ctx->height)) < 0)
        return err;

    av_buffer_unref(&ctx->amf_device_ref);
    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)inlink->hw_frames_ctx->data;

        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        //err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
        if (err < 0)
            return err;

        ctx->hwframes_in_ref = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hwframes_in_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(frames_ctx->device_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = frames_ctx->sw_format;
        pix_fmt_in = frames_ctx->sw_format;

    } else if (avctx->hw_device_ctx) {
        //err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
        if (err < 0)
            return err;

        ctx->hwdevice_ref = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hwdevice_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(ctx->hwdevice_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = inlink->format;
        pix_fmt_in = inlink->format;

    } else {
        //av_log(ctx, AV_LOG_ERROR, "A hardware device reference is required to init hwcontext_amf.\n");
        //return AVERROR(EINVAL);
    }

    if (ctx->format != AV_PIX_FMT_NONE) {
        hwframes_out->sw_format = ctx->format;
    }

    outlink->w = ctx->width;
    outlink->h = ctx->height;

    hwframes_out->width = outlink->w;
    hwframes_out->height = outlink->h;

    err = av_hwframe_ctx_init(ctx->hwframes_out_ref);
    if (err < 0)
        return err;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
    if (!outlink->hw_frames_ctx) {
        return AVERROR(ENOMEM);
    }

    if ((res = amf_load_library(avctx)) == 0) {
        if ((res = amf_init_context(avctx)) == 0) {
            return 0;
        }
    }
    //amf_ctx = ((AVHWDeviceContext*)ctx->amf_device_ref->data)->hwctx;
    //ctx->context = amf_ctx->context;
    //ctx->factory = amf_ctx->factory;


    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, AMFVideoConverter, &ctx->converter);
    AMFAV_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_FILTER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFVideoConverter, res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int32)amf_av_to_amf_format(hwframes_out->sw_format));
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);

    out_size.width = outlink->w;
    out_size.height = outlink->h;
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->converter, AMF_VIDEO_CONVERTER_OUTPUT_SIZE, out_size);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_SCALE, (amf_int32)ctx->scale_type);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);


    res = ctx->converter->pVtbl->Init(ctx->converter, amf_av_to_amf_format(pix_fmt_in), inlink->w, inlink->h);
    AMFAV_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-Init() failed with error %d\n", res);

    return 0;
}

static int amf_scale_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext             *avctx = inlink->dst;
    AMFScaleContext             *ctx = avctx->priv;
    AVFilterLink                *outlink = avctx->outputs[0];
    AMF_RESULT  res;
    AMFSurface *surface_in;
    AMFSurface *surface_out;
    AMFData *data_out;
    enum AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;

    AVFrame *out = NULL;
    int ret = 0;

    if (!ctx->converter)
        return AVERROR(EINVAL);

    ret = amf_avframe_to_amfsurface(avctx, in, &surface_in);
    if (ret < 0)
        goto fail;

    res = ctx->converter->pVtbl->SubmitInput(ctx->converter, (AMFData*)surface_in);
    AMFAV_GOTO_FAIL_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

    res = ctx->converter->pVtbl->QueryOutput(ctx->converter, &data_out);
    AMFAV_GOTO_FAIL_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "QueryOutput() failed with error %d\n", res);

    if (data_out) {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface_out); // query for buffer interface
        data_out->pVtbl->Release(data_out);
    }

    out = amf_amfsurface_to_avframe(avctx, surface_out);

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    switch(in->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
        break;
    case AVCOL_SPC_BT709:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        break;
    case AVCOL_SPC_RGB:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_JPEG;
        break;
    default:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;
        break;
    }

    if (amf_color_profile != AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->converter, AMF_VIDEO_CONVERTER_COLOR_PROFILE, amf_color_profile);
    }

    out->format = outlink->format;
    out->width  = outlink->w;
    out->height = outlink->h;

    out->hw_frames_ctx = av_buffer_ref(ctx->hwframes_out_ref);
    if (!out->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    surface_in->pVtbl->Release(surface_in);
    surface_out->pVtbl->Release(surface_out);

    if (inlink->sample_aspect_ratio.num) {
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

#define OFFSET(x) offsetof(AMFScaleContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption scale_amf_options[] = {
    { "w",      "Output video width",               OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",      "Output video height",              OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "format", "Output pixel format",              OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

    { "scale_type",    "Scale type",                OFFSET(scale_type),      AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR },
        AMF_VIDEO_CONVERTER_SCALE_BILINEAR, AMF_VIDEO_CONVERTER_SCALE_BICUBIC, FLAGS, "scale_type" },
    { "bilinear",      "Bilinear",      0,                       AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR }, 0, 0, FLAGS, "scale_type" },
    { "bicubic",       "Bicubic",       0,                       AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BICUBIC },  0, 0, FLAGS, "scale_type" },

    { NULL },
};


AVFILTER_DEFINE_CLASS(scale_amf);

static const AVFilterPad amf_scale_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = amf_scale_filter_frame,
    }
};

static const AVFilterPad amf_scale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = amf_scale_config_output,
    }
};

AVFilter ff_vf_scale_amf = {
    .name      = "scale_amf",
    .description = NULL_IF_CONFIG_SMALL("AMF video scaling and format conversion"),

    .init          = amf_scale_init,
    .uninit        = amf_scale_uninit,
    //.query_formats = amf_scale_query_formats,

    .priv_size = sizeof(AMFScaleContext),
    .priv_class = &scale_amf_class,

    FILTER_INPUTS(amf_scale_inputs),
    FILTER_OUTPUTS(amf_scale_outputs),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};