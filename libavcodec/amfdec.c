#include "amfdec.h"
#include <AMF/core/Variant.h>
#include <AMF/core/PropertyStorage.h>
#include <AMF/components/FFMPEGFileDemuxer.h>
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "codec_internal.h"
#include "hwconfig.h"
#include "amf.h"

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define propNotFound 0

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"

/*
FIXME: Uncomment when AMF hw_context is ready
static const AVCodecHWConfigInternal *const amf_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = // TODO: replace to AV_PIX_FMT_AMF,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                           AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_AMF,
        },
        .hwaccel = NULL,
    },
    NULL
};
*/

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

static void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    surface->pVtbl->Release(surface);
}

static int amf_load_library(AVCodecContext *avctx)
{
    AvAmfDecoderContext        *ctx = avctx->priv_data;
    AMFInit_Fn         init_fun;
    AMFQueryVersion_Fn version_fun;
    AMF_RESULT         res;

    ctx->library = dlopen(AMF_DLL_NAMEA, RTLD_NOW | RTLD_LOCAL);
    AMF_RETURN_IF_FALSE(ctx, ctx->library != NULL,
        AVERROR_UNKNOWN, "DLL %s failed to open\n", AMF_DLL_NAMEA);

    init_fun = (AMFInit_Fn)dlsym(ctx->library, AMF_INIT_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(ctx, init_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_INIT_FUNCTION_NAME);

    version_fun = (AMFQueryVersion_Fn)dlsym(ctx->library, AMF_QUERY_VERSION_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(ctx, version_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_QUERY_VERSION_FUNCTION_NAME);

    res = version_fun(&ctx->version);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_QUERY_VERSION_FUNCTION_NAME, res);
    res = init_fun(AMF_FULL_VERSION, &ctx->factory);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_INIT_FUNCTION_NAME, res);
    res = ctx->factory->pVtbl->GetTrace(ctx->factory, &ctx->trace);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetTrace() failed with error %d\n", res);
    res = ctx->factory->pVtbl->GetDebug(ctx->factory, &ctx->debug);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetDebug() failed with error %d\n", res);
    return 0;
}

static int amf_init_decoder(AVCodecContext *avctx)
{
    AvAmfDecoderContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AMF_SURFACE_FORMAT formatOut = amf_av_to_amf_format(avctx->pix_fmt);
    AMFBuffer * buffer;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoDecoderUVD_H264_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoDecoderHW_H265_HEVC;
            break;
        case AV_CODEC_ID_AV1:
            codec_id = AMFVideoDecoderHW_AV1;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);


    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->decoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_TIMESTAMP_MODE, AMF_TS_DECODE);// our sample H264 parser provides decode order timestamps - change this depend on demuxer

    if (avctx->extradata_size)
    { // set SPS/PPS extracted from stream or container; Alternatively can use parser->SetUseStartCodes(true)
        ctx->context->pVtbl->AllocBuffer(ctx->context, AMF_MEMORY_HOST, avctx->extradata_size, &buffer);

        memcpy(buffer->pVtbl->GetNative(buffer), avctx->extradata, avctx->extradata_size);
        AMF_ASSIGN_PROPERTY_INTERFACE(res,ctx->decoder, AMF_VIDEO_DECODER_EXTRADATA, buffer);
    }

    res = ctx->decoder->pVtbl->Init(ctx->decoder, formatOut, avctx->width, avctx->height);// parser->GetPictureWidth(), parser->GetPictureHeight()
    return 0;
}

#if CONFIG_D3D11VA
static int amf_init_from_d3d11_device(AVCodecContext *avctx, AVD3D11VADeviceContext *hwctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
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
static int amf_init_from_dxva2_device(AVCodecContext *avctx, AVDXVA2DeviceContext *hwctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
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

static int amf_init_decoder_context(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMFContext1 *context1 = NULL;
    int ret;

    // configure AMF logger
    // the return of these functions indicates old state and do not affect behaviour
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, ctx->log_to_dbg != 0 );
    if (ctx->log_to_dbg)
        ctx->trace->pVtbl->SetWriterLevel(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, AMF_TRACE_TRACE);
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_CONSOLE, 0);
    ctx->trace->pVtbl->SetGlobalLevel(ctx->trace, AMF_TRACE_TRACE);

    // connect AMF logger to av_log
    ctx->tracer.vtbl = &tracer_vtbl;
    ctx->tracer.avctx = avctx;
    ctx->trace->pVtbl->RegisterWriter(ctx->trace, FFMPEG_AMF_WRITER_ID,(AMFTraceWriter*)&ctx->tracer, 1);
    ctx->trace->pVtbl->SetWriterLevel(ctx->trace, FFMPEG_AMF_WRITER_ID, AMF_TRACE_TRACE);

    ret = ctx->factory->pVtbl->CreateContext(ctx->factory, &ctx->context);
    AMF_RETURN_IF_FALSE(ctx, ret == AMF_OK, AVERROR_UNKNOWN, "CreateContext() failed with error %d\n", ret);

    if (avctx->hw_frames_ctx)
    {
         AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;

        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        switch (frames_ctx->device_ctx->type) {
#if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
            ret = amf_init_from_d3d11_device(avctx, frames_ctx->device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
#if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
            ret = amf_init_from_dxva2_device(avctx, frames_ctx->device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
        default:
            av_log(avctx, AV_LOG_ERROR, "AMF initialisation from a %s frames context is not supported.\n",
                   av_hwdevice_get_type_name(frames_ctx->device_ctx->type));
            return AVERROR(ENOSYS);
        }

        ctx->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    else if (avctx->hw_device_ctx) {
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
        ret = ctx->context->pVtbl->InitDX11(ctx->context, NULL, AMF_DX11_1);
        if (ret == AMF_OK) {
            av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D11.\n");
        } else {
            ret = ctx->context->pVtbl->InitDX9(ctx->context, NULL);
            if (ret == AMF_OK) {
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D9.\n");
            } else {
                AMFGuid guid = IID_AMFContext1();
                ret = ctx->context->pVtbl->QueryInterface(ctx->context, &guid, (void**)&context1);
                AMF_RETURN_IF_FALSE(ctx, ret == AMF_OK, AVERROR_UNKNOWN, "CreateContext1() failed with error %d\n", ret);

                ret = context1->pVtbl->InitVulkan(context1, NULL);
                context1->pVtbl->Release(context1);
                if (ret != AMF_OK) {
                    if (ret == AMF_NOT_SUPPORTED)
                        av_log(avctx, AV_LOG_ERROR, "AMF via Vulkan is not supported on the given device.\n");
                    else
                        av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on the given Vulkan device: %d.\n", ret);
                    return AVERROR(ENOSYS);
                }
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via Vulkan.\n");
            }
        }
    }

    return ret;
}

static int ff_amf_decode_close(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;

    if (ctx->decoder) {
        ctx->decoder->pVtbl->Terminate(ctx->decoder);
        ctx->decoder->pVtbl->Release(ctx->decoder);
        ctx->decoder = NULL;
    }

    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);

    ctx->factory = NULL;
    ctx->context = NULL;

    av_buffer_unref(&ctx->amf_device_ctx);

    return 0;

}

static int ff_amf_decode_init(AVCodecContext *avctx)
{
    int ret;

    if ((ret = amf_load_library(avctx)) == 0) {
        if ((ret = amf_init_decoder_context(avctx)) == 0) {
            if ((ret = amf_init_decoder(avctx)) == 0) {
                return 0;
            }
        }
    }
    ff_amf_decode_close(avctx);
    return ret;
}

static void dumpAvFrame(const char * path, const AVFrame *frame)
{
    FILE *fp;
    fp = fopen(path, "ab");
    if(!fp)
       return;
    fprintf(fp,"{\n");
    fprintf(fp, "    \"best_effort_timestamp\": %lld,\n", frame->best_effort_timestamp);
    fprintf(fp, "    \"crop_bottom\": %d,\n", (int)frame->crop_bottom);
    fprintf(fp, "    \"crop_left\": %d,\n", (int)frame->crop_left);
    fprintf(fp, "    \"crop_right\": %d,\n", (int)frame->crop_right);
    fprintf(fp, "    \"crop_top\": %d,\n", (int)frame->crop_top);
    fprintf(fp, "    \"decode_error_flags\": %d,\n", frame->decode_error_flags);
    fprintf(fp, "    \"flags\": %d,\n", frame->flags);
    fprintf(fp, "    \"format\": %d,\n", frame->format);
    fprintf(fp, "    \"height\": %d,\n", frame->height);
    fprintf(fp, "    \"interlaced_frame\": %d,\n", frame->interlaced_frame);
    fprintf(fp, "    \"key_frame\": %d,\n", frame->key_frame);
    fprintf(fp, "    \"nb_extended_buf\": %d,\n", frame->nb_extended_buf);
    fprintf(fp, "    \"nb_samples\": %d,\n", frame->nb_samples);
    fprintf(fp, "    \"nb_side_data\": %d,\n", frame->nb_side_data);
    fprintf(fp, "    \"palette_has_changed\": %d,\n", frame->palette_has_changed);
    fprintf(fp, "    \"pict_type\": %d,\n", (int)frame->pict_type);
    fprintf(fp, "    \"pkt_dts\": %lld,\n", frame->pkt_dts);
    fprintf(fp, "    \"duration\": %lld,\n", frame->duration);
#if FF_API_FRAME_PKT
FF_DISABLE_DEPRECATION_WARNINGS
    fprintf(fp, "    \"pkt_duration\": %lld,\n", frame->pkt_duration);
    fprintf(fp, "    \"pkt_pos\": %lld,\n", frame->pkt_pos);
    fprintf(fp, "    \"pkt_size\": %d,\n", frame->pkt_size);
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    fprintf(fp, "    \"pts\": %lld,\n", frame->pts);
    fprintf(fp, "    \"quality\": %d,\n", frame->quality);
    fprintf(fp, "    \"repeat_pict\": %d,\n", frame->repeat_pict);
    fprintf(fp, "    \"sample_rate\": %d,\n", frame->sample_rate);
    fprintf(fp, "    \"top_field_first\": %d,\n", frame->top_field_first);
    fprintf(fp, "    \"width\": %d,\n", frame->width);
    fprintf(fp, "    \"sample_aspect_ratio_den\": %d,\n", frame->sample_aspect_ratio.den);
    fprintf(fp, "    \"sample_aspect_ratio_num\": %d,\n", frame->sample_aspect_ratio.num);

    fprintf(fp, "    \"color_range\": %d,\n", (int)frame->color_range);
    fprintf(fp, "    \"color_trc\": %d,\n", (int)frame->color_trc);
    fprintf(fp, "    \"colorspace\": %d,\n", (int)frame->colorspace);
    fprintf(fp, "    \"pict_type\": %d,\n", (int)frame->pict_type);
    fprintf(fp,"}\n");

    fclose(fp);
}

static int amf_amfsurface_to_avframe(AVCodecContext *avctx, AMFSurface* pSurface, AVFrame *frame)
{
    AMFPlane *plane;
    AMFVariantStruct var = {0};
    int       i;
    AMF_RESULT  ret = AMF_OK;

    if (!frame)
        return AMF_INVALID_POINTER;

    switch (pSurface->pVtbl->GetMemoryType(pSurface))
        {
    #if CONFIG_D3D11VA
            case AMF_MEMORY_DX11:
            {
                AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
                frame->data[0] = plane0->pVtbl->GetNative(plane0);
                frame->linesize[0] = plane0->pVtbl->GetHPitch(plane0);
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
                ret = pSurface->pVtbl->Convert(pSurface, AMF_MEMORY_HOST);
                AMF_RETURN_IF_FALSE(avctx, ret == AMF_OK, AMF_UNEXPECTED, "Convert(amf::AMF_MEMORY_HOST) failed with error %d\n", ret);

                for (i = 0; i < pSurface->pVtbl->GetPlanesCount(pSurface); i++)
                {
                    plane = pSurface->pVtbl->GetPlaneAt(pSurface, i);
                    frame->data[i] = plane->pVtbl->GetNative(plane);
                    frame->linesize[i] = plane->pVtbl->GetHPitch(plane);
                }
                pSurface->pVtbl->Acquire(pSurface);
                frame->buf[0] = av_buffer_create(NULL,
                                                     0,
                                                     amf_free_amfsurface,
                                                     pSurface,
                                                     AV_BUFFER_FLAG_READONLY);
            }
        }

    frame->format = amf_to_av_format(pSurface->pVtbl->GetFormat(pSurface));
    frame->width  = avctx->width;
    frame->height = avctx->height;

    frame->pts = pSurface->pVtbl->GetPts(pSurface);

    pSurface->pVtbl->GetProperty(pSurface, L"FFMPEG:dts", &var);
    frame->pkt_dts = var.int64Value;
    pSurface->pVtbl->GetProperty(pSurface, L"FFMPEG:pts", &var);
    frame->pts = var.int64Value;
    pSurface->pVtbl->GetProperty(pSurface, L"FFMPEG:duration", &var);
    frame->duration = var.int64Value;

#if FF_API_FRAME_PKT
FF_DISABLE_DEPRECATION_WARNINGS
    pSurface->pVtbl->GetProperty(pSurface, L"FFMPEG:size", &var);
    frame->pkt_size = var.int64Value;
    pSurface->pVtbl->GetProperty(pSurface, L"FFMPEG:pos", &var);
    frame->pkt_pos = var.int64Value;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    dumpAvFrame("./amfdec.json", frame);
    return ret;
}

static AMF_RESULT ff_amf_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMF_RESULT  ret;
    AMFSurface *surface = NULL;
    AVFrame *data = NULL;
    AMFData *data_out = NULL;

    if (!ctx->decoder)
        return AVERROR(EINVAL);

    do {
        data_out = NULL;
        ret = ctx->decoder->pVtbl->QueryOutput(ctx->decoder, &data_out);
    } while (ret == AMF_REPEAT);

    if (data_out == NULL || ret == AMF_EOF)
    {
        av_log(avctx, AV_LOG_VERBOSE, "QueryOutput() returned empty data\n");
        return AMF_EOF;
    }

    AMFAV_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, ret, "QueryOutput() failed with error %d\n", ret);

    if (data_out)
    {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface); // query for buffer interface
        data_out->pVtbl->Release(data_out);
    }

    data = av_frame_alloc();
    ret = amf_amfsurface_to_avframe(avctx, surface, data);
    AMFAV_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, AVERROR_UNKNOWN, "Failed to convert AMFSurface to AVFrame");

    av_frame_move_ref(frame, data);
fail:
    if (data) {
        av_frame_free(&data);
    }
    if (surface){
        surface->pVtbl->Release(surface);
    }
    return ret;
}

static void AMF_STD_CALL  update_buffer_video_duration(AVCodecContext *avctx, AMFBuffer* pBuffer, const AVPacket* pPacket)
{
    if (pPacket->duration != 0)
    {
        amf_int64 durationByFFMPEG    = av_rescale_q(pPacket->duration, avctx->time_base, AMF_TIME_BASE_Q);
        amf_int64 durationByFrameRate = (amf_int64)((amf_double)AMF_SECOND / ((amf_double)avctx->framerate.num / (amf_double)avctx->framerate.den));
        if (abs(durationByFrameRate - durationByFFMPEG) > AMF_MIN(durationByFrameRate, durationByFFMPEG) / 2)
        {
            durationByFFMPEG = durationByFrameRate;
        }

        pBuffer->pVtbl->SetDuration(pBuffer, durationByFFMPEG);
    }
}

static AMF_RESULT amf_update_buffer_properties(AVCodecContext *avctx, AMFBuffer* pBuffer, const AVPacket* pPacket)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMFContext *ctxt = ctx->context;
    AMF_RESULT res;
    amf_int64 pts = 0;
    int m_ptsInitialMinPosition = propNotFound;
    int64_t vst = propNotFound;//avctx->vst
    int condition1 = propNotFound;//(m_iVideoStreamIndex == -1 || pPacket->stream_index == m_iVideoStreamIndex) && m_ptsSeekPos != -1

    AMF_RETURN_IF_FALSE(ctxt, pBuffer != NULL, AMF_INVALID_ARG, "UpdateBufferProperties() - buffer not passed in");
    AMF_RETURN_IF_FALSE(ctxt, pPacket != NULL, AMF_INVALID_ARG, "UpdateBufferProperties() - packet not passed in");
    pts = av_rescale_q(pPacket->dts, avctx->time_base, AMF_TIME_BASE_Q);
    pBuffer->pVtbl->SetPts(pBuffer, pts);// - GetMinPosition());
    if (pPacket != NULL)
    {
        //AMFVariantStruct var;
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:pts", pPacket->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:dts", pPacket->dts);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:stream_index", pPacket->stream_index);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:duration", pPacket->duration);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:size", pPacket->size);
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:pos", pPacket->pos);
    }
    AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:FirstPtsOffset", m_ptsInitialMinPosition);

    if (vst != AV_NOPTS_VALUE)
    {
        int start_time = propNotFound;
        AMF_ASSIGN_PROPERTY_INT64(res, pBuffer, L"FFMPEG:start_time", start_time);
    }

    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, L"FFMPEG:time_base_den", avctx->time_base.den);
    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, L"FFMPEG:time_base_num", avctx->time_base.num);

    if (condition1)
    {
        int m_ptsSeekPos = propNotFound;
        int m_ptsPosition = propNotFound;
        if (pts < m_ptsSeekPos)
        {
            AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"Seeking", true);
        }
        if (m_ptsSeekPos <= m_ptsPosition)
        {
            AVFormatContext * m_pInputContext = propNotFound;
            int default_stream_index = av_find_default_stream_index(m_pInputContext);
            AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"EndSeeking", true);
            if (pPacket->stream_index == default_stream_index)
            {
                m_ptsSeekPos = -1;
            }
        }
        else
        {
            if (pPacket->flags & AV_PKT_FLAG_KEY)
            {
                AMF_ASSIGN_PROPERTY_BOOL(res, pBuffer, L"BeginSeeking", true);
            }
        }
    }
    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, FFMPEG_DEMUXER_BUFFER_TYPE, AMF_STREAM_VIDEO);
    update_buffer_video_duration(avctx, pBuffer, pPacket);

    AMF_ASSIGN_PROPERTY_DATA(res, Int64, pBuffer, FFMPEG_DEMUXER_BUFFER_STREAM_INDEX, pPacket->stream_index);
    AMF_RETURN_IF_FALSE(ctxt, res == AMF_OK, res, "Failed to set property");
    return AMF_OK;
}

static AMF_RESULT amf_buffer_from_packet(AVCodecContext *avctx, const AVPacket* pPacket, AMFBuffer** ppBuffer)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMFContext *ctxt = ctx->context;
    void *pMem;
    AMF_RESULT err;
    AMFBuffer* pBuffer = NULL;

    AMF_RETURN_IF_FALSE(ctxt, pPacket != NULL, AMF_INVALID_ARG, "BufferFromPacket() - packet not passed in");
    AMF_RETURN_IF_FALSE(ctxt, ppBuffer != NULL, AMF_INVALID_ARG, "BufferFromPacket() - buffer pointer not passed in");


    // Reproduce FFMPEG packet allocate logic (file libavcodec/avpacket.c function av_packet_duplicate)
    // ...
    //    data = av_malloc(pkt->size + FF_INPUT_BUFFER_PADDING_SIZE);
    // ...
    //MM this causes problems because there is no way to set real buffer size. Allocation has 32 byte alignment - should be enough.
    err = ctxt->pVtbl->AllocBuffer(ctxt, AMF_MEMORY_HOST, pPacket->size + AV_INPUT_BUFFER_PADDING_SIZE, ppBuffer);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, "BufferFromPacket() - AllocBuffer failed");
    pBuffer = *ppBuffer;
    err = pBuffer->pVtbl->SetSize(pBuffer, pPacket->size);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, "BufferFromPacket() - SetSize failed");
    // get the memory location and check the buffer was indeed allocated
    pMem = pBuffer->pVtbl->GetNative(pBuffer);
    AMF_RETURN_IF_FALSE(ctxt, pMem != NULL, AMF_INVALID_POINTER, "BufferFromPacket() - GetMemory failed");

    // copy the packet memory and don't forget to
    // clear data padding like it is done by FFMPEG
    memcpy(pMem, pPacket->data, pPacket->size);
    memset((amf_int8*)(pMem)+pPacket->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    // now that we created the buffer, it's time to update
    // it's properties from the packet information...
    return amf_update_buffer_properties(avctx, pBuffer, pPacket);
}

static int amf_decode_frame(AVCodecContext *avctx, AVFrame *data,
                       int *got_frame, AVPacket *avpkt)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;

    AMFBuffer * buf;
    AMF_RESULT res;
    AVFrame *frame = data;

    if (!avpkt->size)
    {
        return 0;
    }

    res = amf_buffer_from_packet(avctx, avpkt, &buf);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, 0, "Cannot convert AVPacket to AMFbuffer");

    while(1)
    {
        res = ctx->decoder->pVtbl->SubmitInput(ctx->decoder, (AMFData*) buf);
        if (res == AMF_OK || res == AMF_NEED_MORE_INPUT)
        {
            *got_frame = 0;
        }
        else if (res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES)
        {
            res = ff_amf_receive_frame(avctx, frame);
            if (res == AMF_OK)
            {
                AMF_RETURN_IF_FALSE(avctx, !*got_frame, avpkt->size, "frame already got");
                *got_frame = 1;
                break;
            } else if (res == AMF_EOF)
            {
                break;
            }
        }
        else
        {
            AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, 0, "SubmitInput to decoder failed");
        }
    }
    ctx->decoder->pVtbl->Drain(ctx->decoder);
    return avpkt->size;
}

static void amf_decode_flush(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    ctx->decoder->pVtbl->Flush(ctx->decoder);
}

#define OFFSET(x) offsetof(AvAmfDecoderContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    //Decoder mode
    { "decoder_mode",          "Decoder mode",        OFFSET(decoder_mode),  AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_DECODER_MODE_REGULAR      }, AMF_VIDEO_DECODER_MODE_REGULAR, AMF_VIDEO_DECODER_MODE_LOW_LATENCY, VD, "decoder_mode" },
    { "timestamp_mode",        "Timestamp mode",        OFFSET(timestamp_mode),  AV_OPT_TYPE_INT,   { .i64 = AMF_TS_PRESENTATION      }, AMF_TS_PRESENTATION, AMF_TS_DECODE, VD, "timestamp_mode" },
    { NULL }
};

static const AVClass amf_decode_class = {
    .class_name = "amf",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

FFCodec ff_h264_amf_decoder = {
    .p.name         = "h264_amf",
    .p.long_name    = "AMD AMF decoder",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(AvAmfDecoderContext),
    .p.priv_class   = &amf_decode_class,
    .init           = ff_amf_decode_init,
    FF_CODEC_DECODE_CB(amf_decode_frame),
    .flush          = amf_decode_flush,
    .close          = ff_amf_decode_close,
    .p.pix_fmts     = ff_amf_pix_fmts,
    .bsfs           = "h264_mp4toannexb",
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .p.wrapper_name = "amf",
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,
    //.hw_configs     = amf_hw_configs,
};

FFCodec ff_hevc_amf_decoder = {
    .p.name         = "hevc_amf",
    .p.long_name    = "AMD AMF decoder",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HEVC,
    .priv_data_size = sizeof(AvAmfDecoderContext),
    .p.priv_class   = &amf_decode_class,
    .init           = ff_amf_decode_init,
    FF_CODEC_DECODE_CB(amf_decode_frame),
    .flush          = amf_decode_flush,
    .close          = ff_amf_decode_close,
    .p.pix_fmts     = ff_amf_pix_fmts,
    .bsfs           = "hevc_mp4toannexb",
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .p.wrapper_name = "amf",
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,
    //.hw_configs     = amf_hw_configs,
};

FFCodec ff_av1_amf_decoder = {
    .p.name         = "av1_amf",
    .p.long_name    = "AMD AMF decoder",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AV1,
    .priv_data_size = sizeof(AvAmfDecoderContext),
    .p.priv_class   = &amf_decode_class,
    .init           = ff_amf_decode_init,
    FF_CODEC_DECODE_CB(amf_decode_frame),
    .flush          = amf_decode_flush,
    .close          = ff_amf_decode_close,
    .p.pix_fmts     = ff_amf_pix_fmts,
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .p.wrapper_name = "amf",
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,
    //.hw_configs     = amf_hw_configs,
};