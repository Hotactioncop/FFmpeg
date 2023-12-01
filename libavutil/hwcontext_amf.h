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
#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/core/Trace.h>
#include "pixfmt.h"

#include "libavformat/avformat.h"
#include "libavutil/hwcontext.h"

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"

typedef struct AmfTraceWriter {
    AMFTraceWriterVtbl  *vtbl;
    void                *avctx;
    void                *avcl;
} AmfTraceWriter;

typedef struct AVAMFDeviceContextInternal {
    amf_handle          library; ///< handle to DLL library
    AMFFactory         *factory; ///< pointer to AMF factory
    AMFDebug           *debug;   ///< pointer to AMF debug interface
    AMFTrace           *trace;   ///< pointer to AMF trace interface

    amf_uint64          version; ///< version of AMF runtime
    AMFContext         *context; ///< AMF context
    AMF_MEMORY_TYPE     mem_type;
} AVAMFDeviceContextInternal;

/**
 * This struct is allocated as AVHWDeviceContext.hwctx
 */

typedef struct AVAMFDeviceContext {
    AVBufferRef        *internal;
} AVAMFDeviceContext;

typedef struct AMFFramesContext {
    AMFSurface * surfaces;
    int            nb_surfaces;
} AMFFramesContext;

/**
* Error handling helper
*/
#define AMF_RETURN_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
    if (!(exp)) { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return ret_value; \
    }

#define AMF_GOTO_FAIL_IF_FALSE(avctx, exp, ret_value, /*message,*/ ...) \
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
enum AMF_SURFACE_FORMAT av_amf_av_to_amf_format(enum AVPixelFormat fmt);
enum AVPixelFormat av_amf_to_av_format(enum AMF_SURFACE_FORMAT fmt);
extern AmfTraceWriter av_amf_trace_writer;

int av_amf_context_init(AVAMFDeviceContextInternal* internal, void* avcl);
int av_amf_load_library(AVAMFDeviceContextInternal* internal,  void* avcl);
int av_amf_create_context(  AVAMFDeviceContextInternal * internal,
                                void* avcl,
                                const char *device,
                                AVDictionary *opts, int flags);
int av_amf_context_internal_create(AVAMFDeviceContextInternal * internal,
                                void* avcl,
                                const char *device,
                                AVDictionary *opts, int flags);
int av_amf_context_internal_free(void *opaque, uint8_t *data);
int av_amf_context_derive(AVAMFDeviceContextInternal * internal,
                              AVHWDeviceContext *child_device_ctx, AVDictionary *opts,
                              int flags);

#endif /* AVUTIL_HWCONTEXT_AMF_H */
