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
#include "AMF/components/ColorSpace.h"
#include "amf_common.h"

#include "avfilter.h"
#include "internal.h"
#include "formats.h"
#include "video.h"

#if CONFIG_DXVA2
#include <d3d9.h>
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif


static int amf_scale_query_formats(AVFilterContext *avctx)
{
    const enum AVPixelFormat *output_pix_fmts;
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

    return amf_setup_input_output_formats(avctx, input_pix_fmts, output_pix_fmts);
}

static int amf_scale_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink   *inlink = avctx->inputs[0];
    AMFScaleContext  *ctx = avctx->priv;
    AVAMFDeviceContextInternal * internal = NULL;
    AMFSize out_size;
    int err;
    AMF_RESULT res;
    enum AVPixelFormat in_format;

    err = amf_init_scale_config(outlink, &in_format);
    if (err < 0)
        return err;

    internal = (AVAMFDeviceContextInternal * )ctx->amf_device_ctx_internal->data;
    res = internal->factory->pVtbl->CreateComponent(internal->factory, internal->context, AMFHQScaler, &ctx->scaler);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_FILTER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFHQScaler, res);

    out_size.width = outlink->w;
    out_size.height = outlink->h;
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->scaler, AMF_HQ_SCALER_OUTPUT_SIZE, out_size);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFHQScaler-SetProperty() failed with error %d\n", res);

    if (ctx->algorithm != -1) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_HQ_SCALER_ALGORITHM, ctx->algorithm);
    }
    if (ctx->sharpness != -1) {
        AMF_ASSIGN_PROPERTY_DOUBLE(res, ctx->scaler, AMF_HQ_SCALER_SHARPNESS, ctx->sharpness);
    }
    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->scaler, AMF_HQ_SCALER_FILL, ctx->fill);
    AMF_ASSIGN_PROPERTY_BOOL(res, ctx->scaler, AMF_HQ_SCALER_KEEP_ASPECT_RATIO, ctx->keep_ratio);
    // Setup default options to skip color conversion
    ctx->color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;
    ctx->color_range = AMF_COLOR_RANGE_UNDEFINED;
    ctx->primaries = AMF_COLOR_PRIMARIES_UNDEFINED;
    ctx->trc = AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED;

    res = ctx->scaler->pVtbl->Init(ctx->scaler, av_amf_av_to_amf_format(in_format), inlink->w, inlink->h);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFHQScaler-Init() failed with error %d\n", res);

    return 0;
}

#define OFFSET(x) offsetof(AMFScaleContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption scale_amf_hq_options[] = {
    { "w",              "Output video width",   OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",              "Output video height",  OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },

    { "format",         "Output pixel format",  OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },
    { "sharpness",      "Sharpness",            OFFSET(sharpness),  AV_OPT_TYPE_FLOAT,  { .dbl = -1 }, -1, 2., FLAGS, "sharpness" },
    { "keep-ratio",     "Keep aspect ratio",    OFFSET(keep_ratio), AV_OPT_TYPE_BOOL, { .i64 = 0  },  0, 1, FLAGS, "keep_ration" },
    { "fill",           "Fill",                 OFFSET(fill),       AV_OPT_TYPE_BOOL, { .i64 = 0  },  0, 1, FLAGS, "fill" },

    { "algorithm",      "Scaling algorithm",    OFFSET(algorithm),      AV_OPT_TYPE_INT,   { .i64 = -1 }, -1, AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_1, FLAGS, "algorithm" },
    { "bilinear",       "Bilinear",             0,  AV_OPT_TYPE_CONST, { .i64 = AMF_HQ_SCALER_ALGORITHM_BILINEAR }, 0, 0, FLAGS, "algorithm" },
    { "bicubic",        "Bicubic",              0,  AV_OPT_TYPE_CONST, { .i64 = AMF_HQ_SCALER_ALGORITHM_BICUBIC }, 0, 0, FLAGS, "algorithm" },
    { "sr1-0",          "Video SR1.0",          0,  AV_OPT_TYPE_CONST, { .i64 = AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_0 }, 0, 0, FLAGS, "algorithm" },
    { "point",          "Point",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_HQ_SCALER_ALGORITHM_POINT }, 0, 0, FLAGS, "algorithm" },
    { "sr1-1",          "Video SR1.1",          0,  AV_OPT_TYPE_CONST, { .i64 = AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_1 }, 0, 0, FLAGS, "algorithm" },

    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
    { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
    { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 256, FLAGS },

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