/* GStreamer Intel MSDK plugin
 * Copyright (c) 2017, Intel Corporation
 * Copyright (c) 2017, Igalia S.L.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGDECE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <va/va.h>
#include "gstmsdkallocator.h"
#include "msdk_libva.h"

mfxStatus
gst_msdk_frame_alloc (mfxHDL pthis, mfxFrameAllocRequest * req,
    mfxFrameAllocResponse * resp)
{
  VAStatus va_status;
  mfxStatus status;
  gint i;
  guint format;
  guint va_fourcc = 0;
  VASurfaceID *surfaces = NULL;
  VASurfaceAttrib attrib;
  mfxMemId *mids = NULL;
  GstMsdkContext *context = (GstMsdkContext *) pthis;
  GstMsdkMemoryID *va_mids = NULL;
  GstMsdkAllocResponse *va_resp = NULL;
  mfxU32 fourcc = req->Info.FourCC;
  mfxU16 surfaces_num = req->NumFrameSuggested;

  if (!(req->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET |
              MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET)))
    return MFX_ERR_UNSUPPORTED;

  va_fourcc = gst_msdk_get_va_fourcc_from_mfx_fourcc (fourcc);

  attrib.type = VASurfaceAttribPixelFormat;
  attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrib.value.type = VAGenericValueTypeInteger;
  attrib.value.value.i = va_fourcc;
  format = va_fourcc;

  va_mids =
      (GstMsdkMemoryID *) g_slice_alloc0 (surfaces_num *
      sizeof (GstMsdkMemoryID));
  mids = (mfxMemId *) g_slice_alloc0 (surfaces_num * sizeof (mfxMemId));
  surfaces =
      (VASurfaceID *) g_slice_alloc0 (surfaces_num * sizeof (VASurfaceID));
  va_resp =
      (GstMsdkAllocResponse *) g_slice_alloc0 (sizeof (GstMsdkAllocResponse));

  if (va_fourcc == VA_FOURCC_NV12 || va_fourcc == VA_FOURCC_YV12) {
    format = VA_RT_FORMAT_YUV420;
  } else if ((va_fourcc == VA_FOURCC_UYVY) || (va_fourcc == VA_FOURCC_YUY2)) {
    format = VA_RT_FORMAT_YUV422;
  }

  va_status = vaCreateSurfaces (gst_msdk_context_get_va_display (context),
      format,
      req->Info.Width, req->Info.Height, surfaces, surfaces_num, &attrib, 1);

  status = gst_msdk_get_mfx_status_from_va_status (va_status);
  if (status != MFX_ERR_NONE) {
    GST_WARNING ("failed to create VA surface");
    return status;
  }

  for (i = 0; i < surfaces_num; i++) {
    va_mids[i].surface = &surfaces[i];
    mids[i] = (mfxMemId *) & va_mids[i];
  }

  resp->mids = mids;
  resp->NumFrameActual = surfaces_num;

  va_resp->response = resp;
  va_resp->mem_ids = mids;

  gst_msdk_context_add_alloc_response (context, va_resp);

  return status;
}

mfxStatus
gst_msdk_frame_free (mfxHDL pthis, mfxFrameAllocResponse * resp)
{
  GstMsdkContext *context = (GstMsdkContext *) pthis;
  VAStatus va_status;
  mfxStatus status;
  GstMsdkMemoryID *mem_id;

  mem_id = resp->mids[0];

  va_status =
      vaDestroySurfaces (gst_msdk_context_get_va_display (context),
      (VASurfaceID *) mem_id->surface, resp->NumFrameActual);

  status = gst_msdk_get_mfx_status_from_va_status (va_status);
  return status;
}

mfxStatus
gst_msdk_frame_lock (mfxHDL pthis, mfxMemId mid, mfxFrameData * data)
{
  GstMsdkContext *context = (GstMsdkContext *) pthis;
  VAStatus va_status;
  mfxStatus status;
  mfxU8 *buf = NULL;
  VASurfaceID *va_surface;
  VADisplay dpy;
  GstMsdkMemoryID *mem_id;

  mem_id = (GstMsdkMemoryID *) mid;
  va_surface = mem_id->surface;
  dpy = gst_msdk_context_get_va_display (context);

  va_status = vaDeriveImage (dpy, *va_surface, &mem_id->image);
  status = gst_msdk_get_mfx_status_from_va_status (va_status);

  if (status != MFX_ERR_NONE) {
    GST_WARNING ("failed to derive image");
    return status;
  }

  va_status = vaMapBuffer (dpy, mem_id->image.buf, (void **) &buf);
  status = gst_msdk_get_mfx_status_from_va_status (va_status);

  if (status != MFX_ERR_NONE) {
    GST_WARNING ("failed to map");
    return status;
  }

  switch (mem_id->image.format.fourcc) {
    case VA_FOURCC_NV12:
      data->Pitch = mem_id->image.pitches[0];
      data->Y = buf + mem_id->image.offsets[0];
      data->UV = buf + mem_id->image.offsets[1];
      break;
    case VA_FOURCC_YV12:
      data->Pitch = mem_id->image.pitches[0];
      data->Y = buf + mem_id->image.offsets[0];
      data->U = buf + mem_id->image.offsets[2];
      data->V = buf + mem_id->image.offsets[1];
      break;
    case VA_FOURCC_YUY2:
      data->Pitch = mem_id->image.pitches[0];
      data->Y = buf + mem_id->image.offsets[0];
      data->U = data->Y + 1;
      data->V = data->Y + 3;
      break;
    case VA_FOURCC_UYVY:
      data->Pitch = mem_id->image.pitches[0];
      data->U = buf + mem_id->image.offsets[0];
      data->Y = data->U + 1;
      data->V = data->U + 2;
      break;
    case VA_FOURCC_ARGB:
      data->Pitch = mem_id->image.pitches[0];
      data->R = buf + mem_id->image.offsets[0];
      data->G = data->R + 1;
      data->B = data->R + 2;
      data->A = data->R + 3;
      break;
  }

  return MFX_ERR_NONE;
}

mfxStatus
gst_msdk_frame_unlock (mfxHDL pthis, mfxMemId mid, mfxFrameData * ptr)
{
  GstMsdkContext *context = (GstMsdkContext *) pthis;
  VAStatus va_status;
  mfxStatus status;
  VADisplay dpy;
  GstMsdkMemoryID *mem_id;

  mem_id = (GstMsdkMemoryID *) mid;
  dpy = gst_msdk_context_get_va_display (context);

  vaUnmapBuffer (dpy, mem_id->image.buf);
  va_status = vaDestroyImage (dpy, mem_id->image.image_id);

  status = gst_msdk_get_mfx_status_from_va_status (va_status);

  return status;
}

mfxStatus
gst_msdk_frame_get_hdl (mfxHDL pthis, mfxMemId mid, mfxHDL * hdl)
{
  GstMsdkMemoryID *mem_id;

  if (!hdl || !mid)
    return MFX_ERR_INVALID_HANDLE;

  mem_id = mid;
  *hdl = mem_id->surface;

  return MFX_ERR_NONE;
}

void
gst_msdk_set_frame_allocator (GstMsdkContext * context)
{
  mfxFrameAllocator gst_msdk_frame_allocator = {
    .pthis = context,
    .Alloc = gst_msdk_frame_alloc,
    .Lock = gst_msdk_frame_lock,
    .Unlock = gst_msdk_frame_unlock,
    .GetHDL = gst_msdk_frame_get_hdl,
    .Free = gst_msdk_frame_free,
  };

  MFXVideoCORE_SetFrameAllocator (gst_msdk_context_get_session (context),
      &gst_msdk_frame_allocator);
}
