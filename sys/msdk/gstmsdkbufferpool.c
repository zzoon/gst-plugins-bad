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

#include "gstmsdkbufferpool.h"
#include "gstmsdksystemmemory.h"
#include "gstmsdkvideomemory.h"

GST_DEBUG_CATEGORY_STATIC (gst_debug_msdkbufferpool);
#define GST_CAT_DEFAULT gst_debug_msdkbufferpool

#define gst_msdk_buffer_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstMsdkBufferPool, gst_msdk_buffer_pool,
    GST_TYPE_VIDEO_BUFFER_POOL,
    GST_DEBUG_CATEGORY_INIT (gst_debug_msdkbufferpool, "msdkbufferpool", 0,
        "MSDK Buffer Pool"));

static gboolean
gst_msdk_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstCaps *caps = NULL;
  GstAllocator *allocator = NULL;
  GstVideoInfo video_info;
  guint size, min_buffers, max_buffers;
  gboolean add_videometa;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers))
    goto error_invalid_config;

  if (!caps)
    goto error_no_caps;

  if (!gst_video_info_from_caps (&video_info, caps))
    goto error_invalid_caps;

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, NULL))
    goto error_invalid_allocator;

  if (allocator
      && (g_strcmp0 (allocator->mem_type, GST_MSDK_SYSTEM_MEMORY_NAME) != 0
          && g_strcmp0 (allocator->mem_type,
              GST_MSDK_VIDEO_MEMORY_NAME) != 0)) {
    GST_INFO_OBJECT (pool,
        "This is not MSDK allocator. So this will be ignored");
    gst_object_unref (allocator);
    allocator = NULL;
  }

  add_videometa = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (add_videometa && gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
    GstVideoAlignment alignment;

    gst_msdk_set_video_alignment (&video_info, &alignment);
    gst_video_info_align (&video_info, &alignment);
    gst_buffer_pool_config_set_video_alignment (config, &alignment);
  }

  /* create a new allocator if needed */
  if (!allocator) {
    GstAllocationParams params = { 0, 31, 0, 0, };

    /* FIXME: it should choose proper allocator */
    allocator = gst_msdk_system_allocator_new (&video_info);

    if (!allocator)
      goto error_no_allocator;

    GST_INFO_OBJECT (pool, "created new allocator %" GST_PTR_FORMAT, allocator);

    gst_buffer_pool_config_set_allocator (config, allocator, &params);
    gst_object_unref (allocator);
  }

  return GST_BUFFER_POOL_CLASS
      (gst_msdk_buffer_pool_parent_class)->set_config (pool, config);

error_invalid_config:
  {
    GST_ERROR_OBJECT (pool, "invalid config");
    return FALSE;
  }
error_no_caps:
  {
    GST_ERROR_OBJECT (pool, "no caps in config");
    return FALSE;
  }
error_invalid_caps:
  {
    GST_ERROR_OBJECT (pool, "invalid caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
error_invalid_allocator:
  {
    GST_ERROR_OBJECT (pool, "no allocator in config");
    return FALSE;
  }
error_no_allocator:
  {
    GST_ERROR_OBJECT (pool, "no allocator defined");
    return FALSE;
  }
}

static void
gst_msdk_buffer_pool_init (GstMsdkBufferPool * pool)
{
}

static void
gst_msdk_buffer_pool_class_init (GstMsdkBufferPoolClass * klass)
{
  GstBufferPoolClass *const pool_class = GST_BUFFER_POOL_CLASS (klass);

  pool_class->set_config = gst_msdk_buffer_pool_set_config;
}

GstBufferPool *
gst_msdk_buffer_pool_new (void)
{
  return g_object_new (GST_TYPE_MSDK_BUFFER_POOL, NULL);
}
