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

#include "AMF/components/VideoConverter.h"
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

static int amf_scale_query_formats(AVFilterContext *avctx)
{
    const enum AVPixelFormat *output_pix_fmts;
    AVFilterFormats *input_formats;
    int err;
    int i;
    static const enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_P010,
        AV_PIX_FMT_0RGB,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUYV422,
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
    AMFSize out_size;
    int err;
    AMF_RESULT res;
    enum AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM amf_color_profile;

    err = amf_init_scale_config(outlink);
    if (err < 0)
        return err;

    internal = (AVAMFDeviceContextInternal * )ctx->amf_device_ctx_internal->data;
    res = internal->factory->pVtbl->CreateComponent(internal->factory, internal->context, AMFVideoConverter, &ctx->scaler);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_FILTER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", AMFVideoConverter, res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int32)amf_av_to_amf_format(hwframes_out->sw_format));
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);

    out_size.width = outlink->w;
    out_size.height = outlink->h;
    AMF_ASSIGN_PROPERTY_SIZE(res, ctx->scaler, AMF_VIDEO_CONVERTER_OUTPUT_SIZE, out_size);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_SCALE, (amf_int32)ctx->scale_type);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-SetProperty() failed with error %d\n", res);

    amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;

    switch(ctx->color_profile) {
    case AMF_VIDEO_CONVERTER_COLOR_PROFILE_601:
        if (ctx->color_range == AMF_COLOR_RANGE_FULL) {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601;
        } else {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
        }
        break;
    case AMF_VIDEO_CONVERTER_COLOR_PROFILE_709:
        if (ctx->color_range == AMF_COLOR_RANGE_FULL) {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709;
        } else {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
        }
        break;
    case AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020:
        if (ctx->color_range == AMF_COLOR_RANGE_FULL) {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020;
        } else {
            amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        }
        break;
    default:
        amf_color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;
        break;
    }

    if (amf_color_profile != AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_COLOR_PROFILE, amf_color_profile);
    }

    if (ctx->color_range != AMF_COLOR_RANGE_UNDEFINED) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_OUTPUT_COLOR_RANGE, ctx->color_range);
    }

    if (ctx->primaries != AMF_COLOR_PRIMARIES_UNDEFINED) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_OUTPUT_COLOR_PRIMARIES, ctx->primaries);
    }

    if (ctx->trc != AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED) {
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->scaler, AMF_VIDEO_CONVERTER_OUTPUT_TRANSFER_CHARACTERISTIC, ctx->trc);
    }

    // FIXME: add support for other formats
    res = ctx->scaler->pVtbl->Init(ctx->scaler, AMF_SURFACE_NV12, inlink->w, inlink->h);
    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "AMFConverter-Init() failed with error %d\n", res);

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
    enum AVColorSpace out_colorspace;
    enum AVColorRange out_color_range;

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
    out_colorspace = AVCOL_SPC_UNSPECIFIED;

    if (ctx->color_profile != AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN) {
        switch(ctx->color_profile) {
        case AMF_VIDEO_CONVERTER_COLOR_PROFILE_601:
            out_colorspace = AVCOL_SPC_SMPTE170M;
        break;
        case AMF_VIDEO_CONVERTER_COLOR_PROFILE_709:
            out_colorspace = AVCOL_SPC_BT709;
        break;
        case AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020:
            out_colorspace = AVCOL_SPC_BT2020_NCL;
        break;
        case AMF_VIDEO_CONVERTER_COLOR_PROFILE_JPEG:
            out_colorspace = AVCOL_SPC_RGB;
        break;
        default:
            out_colorspace = AVCOL_SPC_UNSPECIFIED;
        break;
        }
        out->colorspace = out_colorspace;
    }

    out_color_range = AVCOL_RANGE_UNSPECIFIED;
    if (ctx->color_range == AMF_COLOR_RANGE_FULL)
        out_color_range = AVCOL_RANGE_JPEG;
    else if (ctx->color_range == AMF_COLOR_RANGE_STUDIO)
        out_color_range = AVCOL_RANGE_MPEG;

    if (ctx->color_range != AMF_COLOR_RANGE_UNDEFINED)
        out->color_range = out_color_range;

    if (ctx->primaries != AMF_COLOR_PRIMARIES_UNDEFINED)
        out->color_primaries = ctx->primaries;

    if (ctx->trc != AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED)
        out->color_trc = ctx->trc;


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
static const AVOption scale_amf_options[] = {
    { "w",              "Output video width",   OFFSET(w_expr),     AV_OPT_TYPE_STRING, { .str = "iw"   }, .flags = FLAGS },
    { "h",              "Output video height",  OFFSET(h_expr),     AV_OPT_TYPE_STRING, { .str = "ih"   }, .flags = FLAGS },
    { "format",         "Output pixel format",  OFFSET(format_str), AV_OPT_TYPE_STRING, { .str = "same" }, .flags = FLAGS },

    { "scale_type",     "Scale type",           OFFSET(scale_type),      AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR }, AMF_VIDEO_CONVERTER_SCALE_BILINEAR, AMF_VIDEO_CONVERTER_SCALE_BICUBIC, FLAGS, "scale_type" },
    { "bilinear",       "Bilinear",         0,  AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BILINEAR }, 0, 0, FLAGS, "scale_type" },
    { "bicubic",        "Bicubic",          0,  AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_SCALE_BICUBIC },  0, 0, FLAGS, "scale_type" },

    { "color_profile",  "Color profile",        OFFSET(color_profile), AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN }, AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN, AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020, FLAGS, "color_profile" },
    { "bt601",          "BT.601",           0,  AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601 }, 0, 0, FLAGS, "color_profile" },
    { "bt709",          "BT.709",           0,  AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709 },  0, 0, FLAGS, "color_profile" },
    { "bt2020",         "BT.2020",          0,  AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020 },  0, 0, FLAGS, "color_profile" },

    { "color_range",    "Color range",          OFFSET(color_range),      AV_OPT_TYPE_INT,   { .i64 = AMF_COLOR_RANGE_UNDEFINED }, AMF_COLOR_RANGE_UNDEFINED, AMF_COLOR_RANGE_FULL, FLAGS, "color_range" },
    { "studio",         "Studio",                   0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_RANGE_STUDIO }, 0, 0, FLAGS, "color_range" },
    { "full",           "Full",                     0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_RANGE_FULL }, 0, 0, FLAGS, "color_range" },

    { "primaries",      "Output color primaries",   OFFSET(primaries),  AV_OPT_TYPE_INT,   { .i64 = AMF_COLOR_PRIMARIES_UNDEFINED }, AMF_COLOR_PRIMARIES_UNDEFINED, AMF_COLOR_PRIMARIES_JEDEC_P22, FLAGS, "primaries" },
    { "bt709",          "BT.709",                   0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_BT709 }, 0, 0, FLAGS, "primaries" },
    { "bt470m",         "BT.470M",                  0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_BT470M }, 0, 0, FLAGS, "primaries" },
    { "bt470bg",        "BT.470BG",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_BT470BG }, 0, 0, FLAGS, "primaries" },
    { "smpte170m",      "SMPTE170M",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_SMPTE170M }, 0, 0, FLAGS, "primaries" },
    { "smpte240m",      "SMPTE240M",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_SMPTE240M }, 0, 0, FLAGS, "primaries" },
    { "film",           "FILM",                     0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_FILM }, 0, 0, FLAGS, "primaries" },
    { "bt2020",         "BT2020",                   0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_BT2020 }, 0, 0, FLAGS, "primaries" },
    { "smpte428",       "SMPTE428",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_SMPTE428 }, 0, 0, FLAGS, "primaries" },
    { "smpte431",       "SMPTE431",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_SMPTE431 }, 0, 0, FLAGS, "primaries" },
    { "smpte432",       "SMPTE432",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_SMPTE432 }, 0, 0, FLAGS, "primaries" },
    { "jedec-p22",      "JEDEC_P22",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_PRIMARIES_JEDEC_P22 }, 0, 0, FLAGS, "primaries" },

    { "trc",            "Output transfer characteristics",  OFFSET(trc),  AV_OPT_TYPE_INT,   { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED }, AMF_COLOR_TRANSFER_CHARACTERISTIC_UNDEFINED, AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67, FLAGS, "trc" },
    { "bt709",          "BT.709",                   0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709 }, 0, 0, FLAGS, "trc" },
    { "gamma22",        "GAMMA22",                  0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22 }, 0, 0, FLAGS, "trc" },
    { "gamma28",        "GAMMA28",                  0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA28 }, 0, 0, FLAGS, "trc" },
    { "smpte170m",      "SMPTE170M",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE170M }, 0, 0, FLAGS, "trc" },
    { "smpte240m",      "SMPTE240M",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE240M }, 0, 0, FLAGS, "trc" },
    { "linear",         "Linear",                   0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_LINEAR }, 0, 0, FLAGS, "trc" },
    { "log",            "LOG",                      0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_LOG }, 0, 0, FLAGS, "trc" },
    { "log-sqrt",       "LOG_SQRT",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_LOG_SQRT }, 0, 0, FLAGS, "trc" },
    { "iec61966-2-4",   "IEC61966_2_4",             0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_IEC61966_2_4 }, 0, 0, FLAGS, "trc" },
    { "bt1361-ecg",     "BT1361_ECG",               0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT1361_ECG }, 0, 0, FLAGS, "trc" },
    { "iec61966-2-1",   "IEC61966_2_1",             0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_IEC61966_2_1 }, 0, 0, FLAGS, "trc" },
    { "bt2020-10",      "BT.2020_10",               0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT2020_10 }, 0, 0, FLAGS, "trc" },
    { "bt2020-12",      "BT.2020-12",               0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT2020_12 }, 0, 0, FLAGS, "trc" },
    { "smpte2084",      "SMPTE2084",                0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084 }, 0, 0, FLAGS, "trc" },
    { "smpte428",       "SMPTE428",                 0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE428 }, 0, 0, FLAGS, "trc" },
    { "arib-std-b67",   "ARIB_STD_B67",             0,  AV_OPT_TYPE_CONST, { .i64 = AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67 }, 0, 0, FLAGS, "trc" },

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
    FILTER_QUERY_FUNC(&amf_scale_query_formats),

    .priv_size = sizeof(AMFScaleContext),
    .priv_class = &scale_amf_class,

    FILTER_INPUTS(amf_scale_inputs),
    FILTER_OUTPUTS(amf_scale_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_AMF),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_HWDEVICE,
};
