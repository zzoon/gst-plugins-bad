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

#include <unistd.h>
#include <stdlib.h>
#include <va/va.h>
#include "gstmsdkvideomemory.h"
#include "gstmsdkallocator.h"

#ifdef _WIN32
#define posix_memalign(d, a, s) ((*((void**)d) = _aligned_malloc(s, a)) ? 0 : -1)
#endif

#ifndef _WIN32
#define _aligned_free free
#endif

static gboolean
ensure_data (GstMsdkVideoMemory * mem, GstMsdkVideoAllocator * allocator)
{
  GstMsdkMemoryID *mem_id;
  GstMsdkAllocResponse *resp =
      gst_msdk_context_get_cached_alloc_responses (allocator->context);

  mem_id = (GstMsdkMemoryID *) resp->mem_ids[resp->num_used_memory++];
  mem->mem_id = mem_id;
  mem->surface->Data.MemId = mem_id->surface;

  return TRUE;
}

static mfxFrameSurface1 *
gst_msdk_video_allocator_create_surface (GstAllocator * allocator)
{
  mfxFrameInfo frame_info = { {0,}, 0, };
  mfxFrameSurface1 *surface;
  GstMsdkVideoAllocator *msdk_video_allocator =
      GST_MSDK_VIDEO_ALLOCATOR_CAST (allocator);

  surface = (mfxFrameSurface1 *) g_slice_new0 (mfxFrameSurface1);

  if (!surface) {
    GST_ERROR ("failed to allocate surface");
    return NULL;
  }

  gst_msdk_set_mfx_frame_info_from_video_info (&frame_info,
      &msdk_video_allocator->image_info);

  memcpy (&surface->Info, &frame_info, sizeof (mfxFrameInfo));

  return surface;
}

static GstMemory *
gst_msdk_video_memory_new (GstAllocator * base_allocator)
{
  GstMsdkVideoAllocator *allocator;
  GstVideoInfo *vip;
  GstMsdkVideoMemory *mem;

  g_return_val_if_fail (base_allocator, NULL);
  g_return_val_if_fail (GST_IS_MSDK_VIDEO_ALLOCATOR (base_allocator), NULL);

  allocator = GST_MSDK_VIDEO_ALLOCATOR_CAST (base_allocator);

  mem = g_slice_new0 (GstMsdkVideoMemory);
  if (!mem)
    return NULL;

  mem->surface_info = allocator->image_info;
  mem->surface = gst_msdk_video_allocator_create_surface (base_allocator);

  vip = &mem->surface_info;
  gst_memory_init (&mem->parent_instance, GST_MEMORY_FLAG_NO_SHARE,
      base_allocator, NULL, GST_VIDEO_INFO_SIZE (vip), 0, 0,
      GST_VIDEO_INFO_SIZE (vip));

  if (!ensure_data (mem, allocator))
    return FALSE;

  return GST_MEMORY_CAST (mem);
}

static gpointer
gst_msdk_video_memory_map_full (GstMemory * base_mem, GstMapInfo * info,
    gsize maxsize)
{
  GstMsdkVideoMemory *const mem = GST_MSDK_VIDEO_MEMORY_CAST (base_mem);
  GstAllocator *allocator = base_mem->allocator;
  GstMsdkVideoAllocator *msdk_video_allocator =
      GST_MSDK_VIDEO_ALLOCATOR_CAST (allocator);

  g_return_val_if_fail (mem, NULL);

  if (!mem->surface) {
    GST_WARNING ("The surface is not allocated");
    return FALSE;
  }

  if (mem->surface && mem->surface->Data.Locked) {
    GST_WARNING ("The surface in memory %p is not still avaliable", mem);
    return FALSE;
  }

  gst_msdk_frame_lock (msdk_video_allocator->context, mem->mem_id,
      &mem->surface->Data);
  return mem->surface->Data.Y;
}

static void
gst_msdk_video_memory_unmap (GstMemory * base_mem)
{
  GstMsdkVideoMemory *const mem = GST_MSDK_VIDEO_MEMORY_CAST (base_mem);
  GstAllocator *allocator = base_mem->allocator;
  GstMsdkVideoAllocator *msdk_video_allocator =
      GST_MSDK_VIDEO_ALLOCATOR_CAST (allocator);

  gst_msdk_frame_unlock (msdk_video_allocator->context, mem->mem_id,
      &mem->surface->Data);
}

/* GstMsdkVideoAllocator */
G_DEFINE_TYPE (GstMsdkVideoAllocator, gst_msdk_video_allocator,
    GST_TYPE_ALLOCATOR);

static GstMemory *
gst_msdk_video_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  return gst_msdk_video_memory_new (allocator);
}

static void
gst_msdk_video_allocator_finalize (GObject * object)
{
  GstMsdkVideoAllocator *allocator = GST_MSDK_VIDEO_ALLOCATOR_CAST (object);

  gst_object_unref (allocator->context);
  G_OBJECT_CLASS (gst_msdk_video_allocator_parent_class)->finalize (object);
}

static void
gst_msdk_video_allocator_class_init (GstMsdkVideoAllocatorClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass *const allocator_class = GST_ALLOCATOR_CLASS (klass);

  object_class->finalize = gst_msdk_video_allocator_finalize;

  allocator_class->alloc = gst_msdk_video_allocator_alloc;
}

static void
gst_msdk_video_allocator_init (GstMsdkVideoAllocator * allocator)
{
  GstAllocator *const base_allocator = GST_ALLOCATOR_CAST (allocator);

  base_allocator->mem_type = GST_MSDK_VIDEO_MEMORY_NAME;
  base_allocator->mem_map_full = gst_msdk_video_memory_map_full;
  base_allocator->mem_unmap = gst_msdk_video_memory_unmap;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

GstAllocator *
gst_msdk_video_allocator_new (GstMsdkContext * context,
    GstVideoInfo * image_info)
{
  GstMsdkVideoAllocator *allocator;

  g_return_val_if_fail (context != NULL, NULL);
  g_return_val_if_fail (image_info != NULL, NULL);

  allocator = g_object_new (GST_TYPE_MSDK_VIDEO_ALLOCATOR, NULL);
  if (!allocator)
    return NULL;

  allocator->context = gst_object_ref (context);
  allocator->image_info = *image_info;

  return GST_ALLOCATOR_CAST (allocator);
}
