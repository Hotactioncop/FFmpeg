#include "amf.h"

const enum AVPixelFormat ff_amf_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_NONE
};

const AVCodecHWConfigInternal *const ff_amfenc_hw_configs[] = {
#if CONFIG_D3D11VA
    HW_CONFIG_ENCODER_FRAMES(D3D11, D3D11VA),
    HW_CONFIG_ENCODER_DEVICE(NONE,  D3D11VA),
#endif
#if CONFIG_DXVA2
    HW_CONFIG_ENCODER_FRAMES(DXVA2_VLD, DXVA2),
    HW_CONFIG_ENCODER_DEVICE(NONE,      DXVA2),
#endif
    NULL,
};

const FormatMap format_map[] =
{
    { AV_PIX_FMT_NONE,       AMF_SURFACE_UNKNOWN },
    { AV_PIX_FMT_NV12,       AMF_SURFACE_NV12 },
    { AV_PIX_FMT_BGR0,       AMF_SURFACE_BGRA },
    { AV_PIX_FMT_RGB0,       AMF_SURFACE_RGBA },
    { AV_PIX_FMT_GRAY8,      AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,    AMF_SURFACE_YUV420P },
    { AV_PIX_FMT_YUYV422,    AMF_SURFACE_YUY2 },
};

enum AMF_SURFACE_FORMAT amf_av_to_amf_format(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].av_format == fmt) {
            return format_map[i].amf_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

 enum AVPixelFormat amf_to_av_format(enum AMF_SURFACE_FORMAT fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].amf_format == fmt) {
            return format_map[i].av_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}
