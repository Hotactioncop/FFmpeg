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

#ifndef AV_AMF_H
#define AV_AMF_H
#include <AMF/core/Surface.h>
#include "libavformat/avformat.h"

/**
* Error handling helper
*/
#define AMF_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
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

#define AMF_TIME_BASE_Q          (AVRational){1, AMF_SECOND}

typedef struct FormatMap {
    enum AVPixelFormat       av_format;
    enum AMF_SURFACE_FORMAT  amf_format;
} FormatMap;

extern const FormatMap format_map[];
enum AMF_SURFACE_FORMAT amf_av_to_amf_format(enum AVPixelFormat fmt);
enum AVPixelFormat amf_to_av_format(enum AMF_SURFACE_FORMAT fmt);

#endif // AV_AMF_H