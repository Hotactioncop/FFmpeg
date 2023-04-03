#include "amf.h"

const FormatMap format_map[] =
{
    { AV_PIX_FMT_NONE,          AMF_SURFACE_UNKNOWN },
    { AV_PIX_FMT_NV12,          AMF_SURFACE_NV12 },
    { AV_PIX_FMT_BGR0,          AMF_SURFACE_BGRA },
    { AV_PIX_FMT_RGB0,          AMF_SURFACE_RGBA },
    { AV_PIX_FMT_BGRA,          AMF_SURFACE_BGRA},
    { AV_PIX_FMT_ARGB,          AMF_SURFACE_ARGB},
    { AV_PIX_FMT_RGBA,          AMF_SURFACE_RGBA},
    { AV_PIX_FMT_GRAY8,         AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,       AMF_SURFACE_YUV420P },
    { AV_PIX_FMT_YUYV422,       AMF_SURFACE_YUY2 },
    { AV_PIX_FMT_P010,          AMF_SURFACE_P010},
    { AV_PIX_FMT_YUV420P10,     AMF_SURFACE_P010},
    { AV_PIX_FMT_YUV420P12,     AMF_SURFACE_P012},
    { AV_PIX_FMT_YUV420P12,     AMF_SURFACE_P012},
    { AV_PIX_FMT_YUV420P16,     AMF_SURFACE_P016},
    { AV_PIX_FMT_YUV422P10LE,   AMF_SURFACE_Y210},
    { AV_PIX_FMT_YUV444P10LE,   AMF_SURFACE_Y416},
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
