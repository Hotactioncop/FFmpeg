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

#include "amf_common.h"

#include "libavutil/avassert.h"
#include "avfilter.h"
#include "formats.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext_amf.h"

#if CONFIG_DXVA2
#include <d3d9.h>
#endif

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

int amf_copy_surface(AVFilterContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy(dst_data, dst_linesize,
        (const uint8_t**)frame->data, frame->linesize, frame->format,
        frame->width, frame->height);

    return 0;
}

void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    // FIXME: release surface properly
    //surface->pVtbl->Release(surface);
}

AVFrame *amf_amfsurface_to_avframe(AVFilterContext *avctx, AMFSurface* pSurface)
{
    AVFrame *frame = av_frame_alloc();

    if (!frame)
        return NULL;
    /*
    switch (pSurface->pVtbl->GetMemoryType(pSurface))
    {
#if CONFIG_D3D11VA
        case AMF_MEMORY_DX11:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[0] = plane0->pVtbl->GetNative(plane0);
            frame->data[1] = (uint8_t*)(intptr_t)0;

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
#if CONFIG_DXVA2
        case AMF_MEMORY_DX9:
        {
            AMFPlane *plane0 = pSurface->pVtbl->GetPlaneAt(pSurface, 0);
            frame->data[3] = plane0->pVtbl->GetNative(plane0);

            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
        break;
#endif
    default: */
        // FIXME: add support for other memory types
        {
            frame->data[3] = pSurface;
            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     pSurface,
                                     AV_BUFFER_FLAG_READONLY);
            pSurface->pVtbl->Acquire(pSurface);
        }
    //}

    return frame;
}

int amf_avframe_to_amfsurface(AVFilterContext *avctx, const AVFrame *frame, AMFSurface** ppSurface)
{
    AMFScaleContext *ctx = avctx->priv;
    AVAMFDeviceContextInternal* internal = (AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data;
    AMFSurface *surface;
    AMF_RESULT  res;
    int hw_surface = 0;

    switch (frame->format) {
#if CONFIG_D3D11VA
    case AV_PIX_FMT_D3D11:
        {
            static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
            ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
            int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use
            texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

            res = internal->context->pVtbl->CreateSurfaceFromDX11Native(internal->context, texture, &surface, NULL); // wrap to AMF surface
            AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);
            hw_surface = 1;
        }
        break;
#endif
    // FIXME: need to use hw_frames_ctx to get texture
    case AV_PIX_FMT_AMF:
        {
            surface = (AMFSurface*)frame->data[3]; // actual surface
            hw_surface = 1;
        }
        break;

#if CONFIG_DXVA2
    case AV_PIX_FMT_DXVA2_VLD:
        {
            IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

            res = internal->context->pVtbl->CreateSurfaceFromDX9Native(internal->context, texture, &surface, NULL); // wrap to AMF surface
            AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);
            hw_surface = 1;
        }
        break;
#endif
    default:
        {
            AMF_SURFACE_FORMAT amf_fmt = amf_av_to_amf_format(frame->format);
            res = internal->context->pVtbl->AllocSurface(internal->context, AMF_MEMORY_HOST, amf_fmt, frame->width, frame->height, &surface);
            AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
            amf_copy_surface(avctx, frame, surface);
        }
        break;
    }

    if (hw_surface) {
        // input HW surfaces can be vertically aligned by 16; tell AMF the real size
        surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);
    }

    surface->pVtbl->SetPts(surface, frame->pts);
    *ppSurface = surface;
    return 0;
}