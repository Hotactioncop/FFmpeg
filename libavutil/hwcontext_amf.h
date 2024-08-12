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


#ifndef AVUTIL_HWCONTEXT_AMF_H
#define AVUTIL_HWCONTEXT_AMF_H

#include "pixfmt.h"

#include "libavformat/avformat.h"
#include "libavutil/hwcontext.h"

typedef struct AVAMFDeviceContextInternal AVAMFDeviceContextInternal;

/**
 * This struct is allocated as AVHWDeviceContext.hwctx
 */

typedef struct AVAMFDeviceContext {
    AVBufferRef        *internal;
} AVAMFDeviceContext;

enum AMF_SURFACE_FORMAT av_amf_av_to_amf_format(enum AVPixelFormat fmt);
enum AVPixelFormat av_amf_to_av_format(enum AMF_SURFACE_FORMAT fmt);

#endif /* AVUTIL_HWCONTEXT_AMF_H */
