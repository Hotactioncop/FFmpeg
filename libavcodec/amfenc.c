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

#include "config.h"
#include "config_components.h"

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext.h"
#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif
#include "libavutil/hwcontext_amf.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "amfenc.h"
#include "encode.h"
#include "internal.h"
#include "libavutil/mastering_display_metadata.h"

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define PTS_PROP L"PtsProp"

static int amf_save_hdr_metadata(AVCodecContext *avctx, const AVFrame *frame, AMFHDRMetadata *hdrmeta)
{
    AVFrameSideData            *sd_display;
    AVFrameSideData            *sd_light;
    AVMasteringDisplayMetadata *display_meta;
    AVContentLightMetadata     *light_meta;

    sd_display = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd_display) {
        display_meta = (AVMasteringDisplayMetadata *)sd_display->data;
        if (display_meta->has_luminance) {
            const unsigned int luma_den = 10000;
            hdrmeta->maxMasteringLuminance =
                (amf_uint32)(luma_den * av_q2d(display_meta->max_luminance));
            hdrmeta->minMasteringLuminance =
                FFMIN((amf_uint32)(luma_den * av_q2d(display_meta->min_luminance)), hdrmeta->maxMasteringLuminance);
        }
        if (display_meta->has_primaries) {
            const unsigned int chroma_den = 50000;
            hdrmeta->redPrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[0][0])), chroma_den);
            hdrmeta->redPrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[0][1])), chroma_den);
            hdrmeta->greenPrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[1][0])), chroma_den);
            hdrmeta->greenPrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[1][1])), chroma_den);
            hdrmeta->bluePrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[2][0])), chroma_den);
            hdrmeta->bluePrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[2][1])), chroma_den);
            hdrmeta->whitePoint[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->white_point[0])), chroma_den);
            hdrmeta->whitePoint[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->white_point[1])), chroma_den);
        }

        sd_light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        if (sd_light) {
            light_meta = (AVContentLightMetadata *)sd_light->data;
            if (light_meta) {
                hdrmeta->maxContentLightLevel = (amf_uint16)light_meta->MaxCLL;
                hdrmeta->maxFrameAverageLightLevel = (amf_uint16)light_meta->MaxFALL;
            }
        }
        return 0;
    }
    return 1;
}

const enum AVPixelFormat ff_amf_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_AMF,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_NONE
};

static int amf_init_encoder(AVCodecContext *avctx)
{
    AmfContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AVPixelFormat pix_fmt;
    AVAMFDeviceContextInternal* internal = (AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoEncoderVCE_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoEncoder_HEVC;
            break;
        case AV_CODEC_ID_AV1 :
            codec_id = AMFVideoEncoder_AV1;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    if (ctx->hw_frames_ctx)
        pix_fmt = ((AVHWFramesContext*)ctx->hw_frames_ctx->data)->sw_format;
    else
        pix_fmt = avctx->pix_fmt;

    if (avctx->pix_fmt != AV_PIX_FMT_AMF)
        ctx->format = av_amf_av_to_amf_format(pix_fmt);
    else
        ctx->format = av_amf_av_to_amf_format(avctx->sw_pix_fmt);

    AMF_RETURN_IF_FALSE(ctx, ctx->format != AMF_SURFACE_UNKNOWN, AVERROR(EINVAL),
                    "Format %s is not supported\n", av_get_pix_fmt_name(pix_fmt));

    res = internal->factory->pVtbl->CreateComponent(internal->factory, internal->context, codec_id, &ctx->encoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);
    return 0;
}

int av_cold ff_amf_encode_close(AVCodecContext *avctx)
{
    AmfContext *ctx = avctx->priv_data;

    if (ctx->encoder) {
        ctx->encoder->pVtbl->Terminate(ctx->encoder);
        ctx->encoder->pVtbl->Release(ctx->encoder);
        ctx->encoder = NULL;
    }

    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);
    av_buffer_unref(&ctx->amf_device_ctx_internal);

    av_fifo_freep2(&ctx->timestamp_list);

    return 0;
}

static int amf_init_encoder_context(AVCodecContext *avctx)
{
    AmfContext *ctx = avctx->priv_data;
    AMFContext1 *context1 = NULL;
    int ret;

    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        ret = av_amf_context_derive((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, frames_ctx->device_ctx, NULL, 0);
        if (ret < 0)
            return ret;
        ctx->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    else if (avctx->hw_device_ctx) {
        AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
        ret = av_amf_context_derive((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, device_ctx, NULL, 0);
        if (ret < 0)
            return ret;
        ctx->hw_device_ctx = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hw_device_ctx)
            return AVERROR(ENOMEM);

    } else {
        ret = av_amf_context_init((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, avctx);
        if (ret != 0) {
            return ret;
        }
    }


    return ret;
}

static int amf_copy_surface(AVCodecContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4] = {0};
    int       dst_linesize[4] = {0};
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy2(dst_data, dst_linesize,
                   frame->data, frame->linesize, frame->format,
                   avctx->width, avctx->height);

    return 0;
}

static int amf_copy_buffer(AVCodecContext *avctx, AVPacket *pkt, AMFBuffer *buffer)
{
    AmfContext      *ctx = avctx->priv_data;
    int              ret;
    AMFVariantStruct var = {0};
    int64_t          timestamp = AV_NOPTS_VALUE;
    int64_t          size = buffer->pVtbl->GetSize(buffer);

    if ((ret = ff_get_encode_buffer(avctx, pkt, size, 0)) < 0) {
        return ret;
    }
    memcpy(pkt->data, buffer->pVtbl->GetNative(buffer), size);

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &var);
            if(var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        case AV_CODEC_ID_HEVC:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &var);
            if (var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        case AV_CODEC_ID_AV1:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &var);
            if (var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
        default:
            break;
    }

    buffer->pVtbl->GetProperty(buffer, PTS_PROP, &var);

    pkt->pts = var.int64Value; // original pts


    AMF_RETURN_IF_FALSE(ctx, av_fifo_read(ctx->timestamp_list, &timestamp, 1) >= 0,
                        AVERROR_UNKNOWN, "timestamp_list is empty\n");

    // calc dts shift if max_b_frames > 0
    if ((ctx->max_b_frames > 0 || ((ctx->pa_adaptive_mini_gop == 1) ? true : false)) && ctx->dts_delay == 0) {
        int64_t timestamp_last = AV_NOPTS_VALUE;
        size_t can_read = av_fifo_can_read(ctx->timestamp_list);

        AMF_RETURN_IF_FALSE(ctx, can_read > 0, AVERROR_UNKNOWN,
            "timestamp_list is empty while max_b_frames = %d\n", avctx->max_b_frames);
        av_fifo_peek(ctx->timestamp_list, &timestamp_last, 1, can_read - 1);
        if (timestamp < 0 || timestamp_last < AV_NOPTS_VALUE) {
            return AVERROR(ERANGE);
        }
        ctx->dts_delay = timestamp_last - timestamp;
    }
    pkt->dts = timestamp - ctx->dts_delay;
    return 0;
}

// amfenc API implementation
int ff_amf_encode_init(AVCodecContext *avctx)
{
    int ret;
    AmfContext *ctx = avctx->priv_data;
    AVHWDeviceContext   *hwdev_ctx = NULL;
    if (avctx->hw_device_ctx) {
        hwdev_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
    } else if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        hwdev_ctx = (AVHWDeviceContext*)frames_ctx->device_ctx;
    }
    if (av_amf_trace_writer.avctx == NULL)
        av_amf_trace_writer.avctx = avctx;
    // hardcoded to current HW queue size - will auto-realloc if too small
    ctx->timestamp_list = av_fifo_alloc2(avctx->max_b_frames + 16, sizeof(int64_t),
                                         AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->timestamp_list) {
        return AVERROR(ENOMEM);
    }
    ctx->dts_delay = 0;

    ctx->hwsurfaces_in_queue = 0;
    ctx->hwsurfaces_in_queue_max = 16;

    if (avctx->hw_frames_ctx && hwdev_ctx && hwdev_ctx->type == AV_HWDEVICE_TYPE_AMF) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        AVAMFDeviceContext * amf_ctx =  hwdev_ctx->hwctx;
        ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
    }
    else if (avctx->hw_device_ctx && hwdev_ctx && hwdev_ctx->type == AV_HWDEVICE_TYPE_AMF) {
        AVAMFDeviceContext * amf_ctx =  hwdev_ctx->hwctx;
        ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
    } else {
        AVAMFDeviceContextInternal *wrapped = av_mallocz(sizeof(*wrapped));
        ctx->amf_device_ctx_internal = av_buffer_create((uint8_t *)wrapped, sizeof(*wrapped),
                                                av_amf_context_internal_free, NULL, 0);
        if ((ret = av_amf_context_internal_create((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, avctx, "", NULL, 0)) != 0) {
            ff_amf_encode_close(avctx);
            return ret;
        }
        if ((ret = amf_init_encoder_context(avctx)) != 0) {
            ff_amf_encode_close(avctx);
            return ret;
        }
    }
    if ((ret = amf_init_encoder(avctx)) == 0) {
        return 0;
    }

    ff_amf_encode_close(avctx);
    return ret;
}

static AMF_RESULT amf_set_property_buffer(AMFSurface *object, const wchar_t *name, AMFBuffer *val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        AMFGuid guid_AMFInterface = IID_AMFInterface();
        AMFInterface *amf_interface;
        res = val->pVtbl->QueryInterface(val, &guid_AMFInterface, (void**)&amf_interface);

        if (res == AMF_OK) {
            res = AMFVariantAssignInterface(&var, amf_interface);
            amf_interface->pVtbl->Release(amf_interface);
        }
        if (res == AMF_OK) {
            res = object->pVtbl->SetProperty(object, name, var);
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMF_RESULT amf_get_property_buffer(AMFData *object, const wchar_t *name, AMFBuffer **val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        res = object->pVtbl->GetProperty(object, name, &var);
        if (res == AMF_OK) {
            if (var.type == AMF_VARIANT_INTERFACE) {
                AMFGuid guid_AMFBuffer = IID_AMFBuffer();
                AMFInterface *amf_interface = AMFVariantInterface(&var);
                res = amf_interface->pVtbl->QueryInterface(amf_interface, &guid_AMFBuffer, (void**)val);
            } else {
                res = AMF_INVALID_DATA_TYPE;
            }
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMFBuffer *amf_create_buffer_with_frame_ref(const AVFrame *frame, AMFContext *context)
{
    AVFrame *frame_ref;
    AMFBuffer *frame_ref_storage_buffer = NULL;
    AMF_RESULT res;

    res = context->pVtbl->AllocBuffer(context, AMF_MEMORY_HOST, sizeof(frame_ref), &frame_ref_storage_buffer);
    if (res == AMF_OK) {
        frame_ref = av_frame_clone(frame);
        if (frame_ref) {
            memcpy(frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), &frame_ref, sizeof(frame_ref));
        } else {
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
            frame_ref_storage_buffer = NULL;
        }
    }
    return frame_ref_storage_buffer;
}

static void amf_release_buffer_with_frame_ref(AMFBuffer *frame_ref_storage_buffer)
{
    AVFrame *frame_ref;
    memcpy(&frame_ref, frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), sizeof(frame_ref));
    av_frame_free(&frame_ref);
    frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
}

static int fill_packet(AVCodecContext *avctx, AMFData *data, AVPacket *avpkt)
{
    AmfContext *ctx = avctx->priv_data;
    AMFBuffer* buffer = NULL;
    AMF_RESULT  res;
    int         ret;
    // copy data to packet
    AMFGuid guid = IID_AMFBuffer();
    data->pVtbl->QueryInterface(data, &guid, (void **)&buffer); // query for buffer interface
    ret = amf_copy_buffer(avctx, avpkt, buffer);

    buffer->pVtbl->Release(buffer);

    if (data->pVtbl->HasProperty(data, L"av_frame_ref")) {
        AMFBuffer *frame_ref_storage_buffer;
        res = amf_get_property_buffer(data, L"av_frame_ref", &frame_ref_storage_buffer);
        AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetProperty failed for \"av_frame_ref\" with error %d\n", res);
        amf_release_buffer_with_frame_ref(frame_ref_storage_buffer);
        ctx->hwsurfaces_in_queue--;
    }
    data->pVtbl->Release(data);

    AMF_RETURN_IF_FALSE(ctx, ret >= 0, ret, "amf_copy_buffer() failed with error %d\n", ret);

    return ret;
}

int ff_amf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    AmfContext *ctx = avctx->priv_data;
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data;
    AMFSurface *surface;
    AMF_RESULT  res;
    int         ret;
    AMF_RESULT  res_query;
    AMFData    *data = NULL;
    AVFrame    *frame = av_frame_alloc();
    int         block_and_wait;
    int         query_output_data_flag = 0;
    int64_t     pts = 0;
    int         count = 0;
    if (!ctx->encoder)
        return AVERROR(EINVAL);
    res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
    if (data) {
        ret = fill_packet(avctx, data, avpkt);
        goto end;
    }

    ret = ff_encode_get_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;

    if (!frame->buf[0]) { // submit drain
        if (!ctx->eof) { // submit drain one time only
            res = ctx->encoder->pVtbl->Drain(ctx->encoder);
            if (res == AMF_INPUT_FULL) {
                do {
                    res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
                    if (data) {
                        ret = fill_packet(avctx, data, avpkt);
                        goto end;
                    }
                    av_log(avctx, AV_LOG_ERROR, "Retry QueryOutput() %d\n");
                    //av_usleep(500);
                } while (data == NULL);
            } else if (res == AMF_OK) {
                ctx->eof = 1; // drain started
            } else {
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Drain() failed with error %d\n", res);
            }
        }
    } else { // submit frame
        int hw_surface = 0;

        // prepare surface from frame
        switch (frame->format) {
#if CONFIG_D3D11VA
        case AV_PIX_FMT_D3D11:
            {
                static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
                ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
                int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use

                av_assert0(frame->hw_frames_ctx       && ctx->hw_frames_ctx &&
                           frame->hw_frames_ctx->data == ctx->hw_frames_ctx->data);

                texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

                res = internal->context->pVtbl->CreateSurfaceFromDX11Native(internal->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
#if CONFIG_DXVA2
        case AV_PIX_FMT_DXVA2_VLD:
            {
                IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

                res = internal->context->pVtbl->CreateSurfaceFromDX9Native(internal->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
        case AV_PIX_FMT_AMF:
            {
                surface = (AMFSurface*)frame->data[3];
                surface->pVtbl->Acquire(surface);
                hw_surface = 1;
            }
            break;
        default:
            {
                res = internal->context->pVtbl->AllocSurface(internal->context, AMF_MEMORY_HOST, ctx->format, avctx->width, avctx->height, &surface);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
                amf_copy_surface(avctx, frame, surface);
            }
            break;
        }

        if (hw_surface) {
            AMFBuffer *frame_ref_storage_buffer;

            // input HW surfaces can be vertically aligned by 16; tell AMF the real size
            surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);

            frame_ref_storage_buffer = amf_create_buffer_with_frame_ref(frame, internal->context);
            AMF_RETURN_IF_FALSE(ctx, frame_ref_storage_buffer != NULL, AVERROR(ENOMEM), "create_buffer_with_frame_ref() returned NULL\n");

            res = amf_set_property_buffer(surface, L"av_frame_ref", frame_ref_storage_buffer);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_ref\" with error %d\n", res);
            ctx->hwsurfaces_in_queue++;
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
        }


        // HDR10 metadata
        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            AMFBuffer * hdrmeta_buffer = NULL;
            res = internal->context->pVtbl->AllocBuffer(internal->context, AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &hdrmeta_buffer);
            if (res == AMF_OK) {
                AMFHDRMetadata * hdrmeta = (AMFHDRMetadata*)hdrmeta_buffer->pVtbl->GetNative(hdrmeta_buffer);
                if (amf_save_hdr_metadata(avctx, frame, hdrmeta) == 0) {
                    switch (avctx->codec->id) {
                    case AV_CODEC_ID_H264:
                        AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                    case AV_CODEC_ID_HEVC:
                        AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                    }
                    res = amf_set_property_buffer(surface, L"av_frame_hdrmeta", hdrmeta_buffer);
                    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_hdrmeta\" with error %d\n", res);
                }
                hdrmeta_buffer->pVtbl->Release(hdrmeta_buffer);
            }
        }
        surface->pVtbl->SetPts(surface, frame->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, PTS_PROP, frame->pts);

        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_AUD, !!ctx->aud);
            break;
        case AV_CODEC_ID_HEVC:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, !!ctx->aud);
            break;
        //case AV_CODEC_ID_AV1 not supported
        default:
            break;
        }

        // submit surface
        res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
        if (res == AMF_INPUT_FULL) { // handle full queue
            do {
                res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
                if (data) {
                    ret = fill_packet(avctx, data, avpkt);
                }
                //av_usleep(500);
            } while (data == NULL);
            res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
            pts = frame->pts;
            surface->pVtbl->Release(surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);
            av_frame_unref(frame);
            ret = av_fifo_write(ctx->timestamp_list, &pts, 1);
            if (ret < 0)
                return ret;
        } else {
            pts = frame->pts;
            surface->pVtbl->Release(surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);
            av_frame_unref(frame);
            ret = av_fifo_write(ctx->timestamp_list, &pts, 1);
            if (ret < 0)
                return ret;
            res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
            if (data) {
                ret = fill_packet(avctx, data, avpkt);
                goto end;
            }
        }
    }

end:
    if (res_query == AMF_EOF) {
        ret = AVERROR_EOF;
    } else if (data == NULL && ctx->eof != 1) {
        ret = AVERROR(EAGAIN);
    } else {
        ret = 0;
    }
    return ret;
}

const AVCodecHWConfigInternal *const ff_amfenc_hw_configs[] = {
#if CONFIG_D3D11VA
    HW_CONFIG_ENCODER_FRAMES(D3D11, D3D11VA),
    HW_CONFIG_ENCODER_DEVICE(NONE,  D3D11VA),
#endif
#if CONFIG_DXVA2
    HW_CONFIG_ENCODER_FRAMES(DXVA2_VLD, DXVA2),
    HW_CONFIG_ENCODER_DEVICE(NONE,      DXVA2),
#endif
    HW_CONFIG_ENCODER_FRAMES(AMF,       AMF),
    HW_CONFIG_ENCODER_DEVICE(NONE,      AMF),
    NULL,
};
