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

#ifndef AVCODEC_AMFDEC_H
#define AVCODEC_AMFDEC_H

#include <AMF/core/Buffer.h>
#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/core/Surface.h>
#include <AMF/components/Component.h>
#include <AMF/components/VideoDecoderUVD.h>

#include "avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/fifo.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

/**
* AMF decoder context
*/

typedef struct AvAmfDecoderContext {
    AVClass            *avclass;

    AVBufferRef        *amf_device_ctx_internal;
    AVBufferRef        *amf_device_ctx;

    //decoder
    AMFComponent       *decoder; ///< AMF decoder object
    AMF_SURFACE_FORMAT  format;  ///< AMF surface format

    AVBufferRef        *hw_device_ctx; ///< pointer to HW accelerator (decoder)
    AVBufferRef        *hw_frames_ctx; ///< pointer to HW accelerator (frame allocator)

    AVBufferRef        *hw_device_ref;
    AVBufferRef        *hw_frames_ref;

    // shift dts back by max_b_frames in timing
    AVFifoBuffer       *timestamp_list;
    int64_t             dts_delay;

    amf_uint64          version; ///< version of AMF runtime
    // common encoder option options

    int                 log_to_dbg;
    // Static options, have to be set before Init() call
    int                 decoder_mode;
    int                 timestamp_mode;
    int                 surface_pool_size;
    int                 dpb_size;
    int                 lowlatency;
    int                 smart_access_video;
    int                 skip_transfer_sav;
    int                 drained;

} AvAmfDecoderContext;

#endif // AVCODEC_AMFDEC_H