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
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_amf.h"

#include "AMF/components/HQScaler.h"
#include "amf_common.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "scale_eval.h"

#if CONFIG_DXVA2
#include <d3d9.h>
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

static int amf_scale_init(AVFilterContext *avctx)
{
    AMFScaleContext     *ctx = avctx->priv;

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

    if (ctx->scaler) {
        ctx->scaler->pVtbl->Terminate(ctx->scaler);
        ctx->scaler->pVtbl->Release(ctx->scaler);
        ctx->scaler = NULL;
    }

    av_buffer_unref(&ctx->amf_device_ref);
    av_buffer_unref(&ctx->hwdevice_ref);
    av_buffer_unref(&ctx->hwframes_in_ref);
    av_buffer_unref(&ctx->hwframes_out_ref);
}

static int amf_scale_query_formats(AVFilterContext *avctx)
{
    const enum AVPixelFormat *output_pix_fmts;
    AVFilterFormats *input_formats;
    int err;
    int i;
    static const enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_AMF,
        AV_PIX_FMT_NONE,
    };
    static const enum AVPixelFormat output_pix_fmts_default[] = {
        AV_PIX_FMT_AMF,
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
    AVAMFDeviceContextInternal * internal = NULL;
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
        if (frames_ctx->device_ctx->type == AV_HWDEVICE_TYPE_AMF) {
            AVAMFDeviceContext * amf_ctx =  frames_ctx->device_ctx->hwctx;
            ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
        }
        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, frames_ctx->device_ref, 0);
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
        AVHWDeviceContext   *hwdev_ctx;
        err = av_hwdevice_ctx_create_derived(&ctx->amf_device_ref, AV_HWDEVICE_TYPE_AMF, avctx->hw_device_ctx, 0);
        if (err < 0)
            return err;
        hwdev_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
        if (hwdev_ctx->type == AV_HWDEVICE_TYPE_AMF)
        {
            AVAMFDeviceContext * amf_ctx =  hwdev_ctx->hwctx;
            ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
        }
        ctx->hwdevice_ref = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hwdevice_ref)
            return AVERROR(ENOMEM);

        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(ctx->hwdevice_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = inlink->format;
        //FIXME:  use hw_frames_ctx->format instead of inlink->format when it will be available
        if (inlink->format != AV_PIX_FMT_AMF)
            pix_fmt_in = inlink->format;

    } else {
        AVAMFDeviceContextInternal *wrapped = av_mallocz(sizeof(*wrapped));
        ctx->amf_device_ctx_internal = av_buffer_create((uint8_t *)wrapped, sizeof(*wrapped),
                                                amf_context_internal_free, NULL, 0);
        if ((res == amf_context_internal_create((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, avctx, "", NULL, 0)) != 0) {
            return res;
        }
        ctx->hwframes_out_ref = av_hwframe_ctx_alloc(ctx->amf_device_ref);
        if (!ctx->hwframes_out_ref)
            return AVERROR(ENOMEM);

        hwframes_out = (AVHWFramesContext*)ctx->hwframes_out_ref->data;
        hwframes_out->format    = outlink->format;
        hwframes_out->sw_format = inlink->format;
        pix_fmt_in = inlink->format;
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
    internal = (AVAMFDeviceContextInternal * )ctx->amf_device_ctx_internal->data;
    res = internal->factory->pVtbl->CreateComponent(internal->factory, internal->context, AMFHQScaler, &ctx->scaler);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_FILTER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFHQScaler, res);

    out_size.width = outlink->w;
    out_size.height = outlink->h;
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->scaler, AMF_HQ_SCALER_OUTPUT_SIZE, out_size);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFHQScaler-SetProperty() failed with error %d\n", res);

    // FIXME: add support for other formats
    res = ctx->scaler->pVtbl->Init(ctx->scaler, AMF_SURFACE_NV12, inlink->w, inlink->h);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFHQScaler-Init() failed with error %d\n", res);

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
    AMFData *data_out = NULL;

    AVFrame *out = NULL;
    int ret = 0;

    if (!ctx->scaler)
        return AVERROR(EINVAL);

    ret = amf_avframe_to_amfsurface(avctx, in, &surface_in);
    if (ret < 0)
        goto fail;
    res = ctx->scaler->pVtbl->SubmitInput(ctx->scaler, (AMFData*)surface_in);
    AMF_GOTO_FAIL_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

    res = ctx->scaler->pVtbl->QueryOutput(ctx->scaler, &data_out);
    AMF_GOTO_FAIL_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "QueryOutput() failed with error %d\n", res);

    if (data_out) {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface_out); // query for buffer interface
        data_out->pVtbl->Release(data_out);
    }

    out = amf_amfsurface_to_avframe(avctx, surface_out);

    ret = av_frame_copy_props(out, in);

    if (ret < 0)
        goto fail;

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
static const AVOption scale_amf_hq_options[] = {
    { "w",              "Output video width",   OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",              "Output video height",  OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "format",         "Output pixel format",  OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

    { NULL },
};


AVFILTER_DEFINE_CLASS(scale_amf_hq);

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

AVFilter ff_vf_scale_amf_hq = {
    .name      = "scale_amf_hq",
    .description = NULL_IF_CONFIG_SMALL("AMF HQ video upscaling"),

    .init          = amf_scale_init,
    .uninit        = amf_scale_uninit,
    FILTER_QUERY_FUNC(&amf_scale_query_formats),

    .priv_size = sizeof(AMFScaleContext),
    .priv_class = &scale_amf_hq_class,

    FILTER_INPUTS(amf_scale_inputs),
    FILTER_OUTPUTS(amf_scale_outputs),

    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_AMF),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_HWDEVICE,
};