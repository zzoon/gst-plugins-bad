/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
 * Copyright (C) 2013 Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 * Copyright (C) 2018 Centricular Ltd.
 *   Author: Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-wasapisink
 * @title: wasapisink
 *
 * Provides audio playback using the Windows Audio Session API available with
 * Vista and newer.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v audiotestsrc samplesperbuffer=160 ! wasapisink
 * ]| Generate 20 ms buffers and render to the default audio device.
 *
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstwasapisink.h"

#include <avrt.h>

GST_DEBUG_CATEGORY_STATIC (gst_wasapi_sink_debug);
#define GST_CAT_DEFAULT gst_wasapi_sink_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_WASAPI_STATIC_CAPS));

#define DEFAULT_ROLE          GST_WASAPI_DEVICE_ROLE_CONSOLE
#define DEFAULT_MUTE          FALSE
#define DEFAULT_EXCLUSIVE     FALSE
#define DEFAULT_LOW_LATENCY   FALSE

enum
{
  PROP_0,
  PROP_ROLE,
  PROP_MUTE,
  PROP_DEVICE,
  PROP_EXCLUSIVE,
  PROP_LOW_LATENCY
};

static void gst_wasapi_sink_dispose (GObject * object);
static void gst_wasapi_sink_finalize (GObject * object);
static void gst_wasapi_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_wasapi_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *gst_wasapi_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);

static gboolean gst_wasapi_sink_prepare (GstAudioSink * asink,
    GstAudioRingBufferSpec * spec);
static gboolean gst_wasapi_sink_unprepare (GstAudioSink * asink);
static gboolean gst_wasapi_sink_open (GstAudioSink * asink);
static gboolean gst_wasapi_sink_close (GstAudioSink * asink);
static gint gst_wasapi_sink_write (GstAudioSink * asink,
    gpointer data, guint length);
static guint gst_wasapi_sink_delay (GstAudioSink * asink);
static void gst_wasapi_sink_reset (GstAudioSink * asink);

#define gst_wasapi_sink_parent_class parent_class
G_DEFINE_TYPE (GstWasapiSink, gst_wasapi_sink, GST_TYPE_AUDIO_SINK);

static void
gst_wasapi_sink_class_init (GstWasapiSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
  GstAudioSinkClass *gstaudiosink_class = GST_AUDIO_SINK_CLASS (klass);

  gobject_class->dispose = gst_wasapi_sink_dispose;
  gobject_class->finalize = gst_wasapi_sink_finalize;
  gobject_class->set_property = gst_wasapi_sink_set_property;
  gobject_class->get_property = gst_wasapi_sink_get_property;

  g_object_class_install_property (gobject_class,
      PROP_ROLE,
      g_param_spec_enum ("role", "Role",
          "Role of the device: communications, multimedia, etc",
          GST_WASAPI_DEVICE_TYPE_ROLE, DEFAULT_ROLE, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_MUTE,
      g_param_spec_boolean ("mute", "Mute", "Mute state of this stream",
          DEFAULT_MUTE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING));

  g_object_class_install_property (gobject_class,
      PROP_DEVICE,
      g_param_spec_string ("device", "Device",
          "WASAPI playback device as a GUID string",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_EXCLUSIVE,
      g_param_spec_boolean ("exclusive", "Exclusive mode",
          "Open the device in exclusive mode",
          DEFAULT_EXCLUSIVE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_LOW_LATENCY,
      g_param_spec_boolean ("low-latency", "Low latency",
          "Optimize all settings for lowest latency",
          DEFAULT_LOW_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_static_metadata (gstelement_class, "WasapiSrc",
      "Sink/Audio",
      "Stream audio to an audio capture device through WASAPI",
      "Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>");

  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_wasapi_sink_get_caps);

  gstaudiosink_class->prepare = GST_DEBUG_FUNCPTR (gst_wasapi_sink_prepare);
  gstaudiosink_class->unprepare = GST_DEBUG_FUNCPTR (gst_wasapi_sink_unprepare);
  gstaudiosink_class->open = GST_DEBUG_FUNCPTR (gst_wasapi_sink_open);
  gstaudiosink_class->close = GST_DEBUG_FUNCPTR (gst_wasapi_sink_close);
  gstaudiosink_class->write = GST_DEBUG_FUNCPTR (gst_wasapi_sink_write);
  gstaudiosink_class->delay = GST_DEBUG_FUNCPTR (gst_wasapi_sink_delay);
  gstaudiosink_class->reset = GST_DEBUG_FUNCPTR (gst_wasapi_sink_reset);

  GST_DEBUG_CATEGORY_INIT (gst_wasapi_sink_debug, "wasapisink",
      0, "Windows audio session API sink");
}

static void
gst_wasapi_sink_init (GstWasapiSink * self)
{
  self->event_handle = CreateEvent (NULL, FALSE, FALSE, NULL);

  CoInitialize (NULL);
}

static void
gst_wasapi_sink_dispose (GObject * object)
{
  GstWasapiSink *self = GST_WASAPI_SINK (object);

  if (self->event_handle != NULL) {
    CloseHandle (self->event_handle);
    self->event_handle = NULL;
  }

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  if (self->render_client != NULL) {
    IUnknown_Release (self->render_client);
    self->render_client = NULL;
  }

  G_OBJECT_CLASS (gst_wasapi_sink_parent_class)->dispose (object);
}

static void
gst_wasapi_sink_finalize (GObject * object)
{
  GstWasapiSink *self = GST_WASAPI_SINK (object);

  g_clear_pointer (&self->mix_format, CoTaskMemFree);

  CoUninitialize ();

  if (self->cached_caps != NULL) {
    gst_caps_unref (self->cached_caps);
    self->cached_caps = NULL;
  }

  g_clear_pointer (&self->positions, g_free);
  g_clear_pointer (&self->device_strid, g_free);
  self->mute = FALSE;

  G_OBJECT_CLASS (gst_wasapi_sink_parent_class)->finalize (object);
}

static void
gst_wasapi_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWasapiSink *self = GST_WASAPI_SINK (object);

  switch (prop_id) {
    case PROP_ROLE:
      self->role = gst_wasapi_device_role_to_erole (g_value_get_enum (value));
      break;
    case PROP_MUTE:
      self->mute = g_value_get_boolean (value);
      break;
    case PROP_DEVICE:
    {
      const gchar *device = g_value_get_string (value);
      g_free (self->device_strid);
      self->device_strid =
          device ? g_utf8_to_utf16 (device, -1, NULL, NULL, NULL) : NULL;
      break;
    }
    case PROP_EXCLUSIVE:
      self->sharemode = g_value_get_boolean (value)
          ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
      break;
    case PROP_LOW_LATENCY:
      self->low_latency = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wasapi_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstWasapiSink *self = GST_WASAPI_SINK (object);

  switch (prop_id) {
    case PROP_ROLE:
      g_value_set_enum (value, gst_wasapi_erole_to_device_role (self->role));
      break;
    case PROP_MUTE:
      g_value_set_boolean (value, self->mute);
      break;
    case PROP_DEVICE:
      g_value_take_string (value, self->device_strid ?
          g_utf16_to_utf8 (self->device_strid, -1, NULL, NULL, NULL) : NULL);
      break;
    case PROP_EXCLUSIVE:
      g_value_set_boolean (value,
          self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE);
      break;
    case PROP_LOW_LATENCY:
      g_value_set_boolean (value, self->low_latency);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_wasapi_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstWasapiSink *self = GST_WASAPI_SINK (bsink);
  WAVEFORMATEX *format = NULL;
  GstCaps *caps = NULL;

  GST_DEBUG_OBJECT (self, "entering get caps");

  if (self->cached_caps) {
    caps = gst_caps_ref (self->cached_caps);
  } else {
    GstCaps *template_caps;
    gboolean ret;

    template_caps = gst_pad_get_pad_template_caps (bsink->sinkpad);

    if (!self->client)
      gst_wasapi_sink_open (GST_AUDIO_SINK (bsink));

    ret = gst_wasapi_util_get_device_format (GST_ELEMENT (self),
        self->sharemode, self->device, self->client, &format);
    if (!ret) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
          ("failed to detect format"));
      goto out;
    }

    gst_wasapi_util_parse_waveformatex ((WAVEFORMATEXTENSIBLE *) format,
        template_caps, &caps, &self->positions);
    if (caps == NULL) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unknown format"));
      goto out;
    }

    {
      gchar *pos_str = gst_audio_channel_positions_to_string (self->positions,
          format->nChannels);
      GST_INFO_OBJECT (self, "positions are: %s", pos_str);
      g_free (pos_str);
    }

    self->mix_format = format;
    gst_caps_replace (&self->cached_caps, caps);
    gst_caps_unref (template_caps);
  }

  if (filter) {
    GstCaps *filtered =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = filtered;
  }

  GST_DEBUG_OBJECT (self, "returning caps %" GST_PTR_FORMAT, caps);

out:
  return caps;
}

static gboolean
gst_wasapi_sink_open (GstAudioSink * asink)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);
  gboolean res = FALSE;
  IMMDevice *device = NULL;
  IAudioClient *client = NULL;

  GST_DEBUG_OBJECT (self, "opening device");

  if (self->client)
    return TRUE;

  /* FIXME: Switching the default device does not switch the stream to it,
   * even if the old device was unplugged. We need to handle this somehow.
   * For example, perhaps we should automatically switch to the new device if
   * the default device is changed and a device isn't explicitly selected. */
  if (!gst_wasapi_util_get_device_client (GST_ELEMENT (self), FALSE,
          self->role, self->device_strid, &device, &client)) {
    if (!self->device_strid)
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE, (NULL),
          ("Failed to get default device"));
    else
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE, (NULL),
          ("Failed to open device %S", self->device_strid));
    goto beach;
  }

  self->client = client;
  self->device = device;
  res = TRUE;

beach:

  return res;
}

static gboolean
gst_wasapi_sink_close (GstAudioSink * asink)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);

  if (self->device != NULL) {
    IUnknown_Release (self->device);
    self->device = NULL;
  }

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  return TRUE;
}

/* Get the empty space in the buffer that we have to write to */
static gint
gst_wasapi_sink_get_can_frames (GstWasapiSink * self)
{
  HRESULT hr;
  guint n_frames_padding;

  /* There is no padding in exclusive mode since there is no ringbuffer */
  if (self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
    GST_DEBUG_OBJECT (self, "exclusive mode, can write: %i",
        self->buffer_frame_count);
    return self->buffer_frame_count;
  }

  /* Frames the card hasn't rendered yet */
  hr = IAudioClient_GetCurrentPadding (self->client, &n_frames_padding);
  if (hr != S_OK) {
    gchar *msg = gst_wasapi_util_hresult_to_string (hr);
    GST_ERROR_OBJECT (self, "IAudioClient::GetCurrentPadding failed: %s", msg);
    g_free (msg);
    return -1;
  }

  GST_DEBUG_OBJECT (self, "%i unread frames (padding)", n_frames_padding);

  /* We can write out these many frames */
  return self->buffer_frame_count - n_frames_padding;
}

static gboolean
gst_wasapi_sink_prepare (GstAudioSink * asink, GstAudioRingBufferSpec * spec)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);
  gboolean res = FALSE;
  REFERENCE_TIME latency_rt;
  REFERENCE_TIME default_period, min_period;
  REFERENCE_TIME device_period, device_buffer_duration;
  guint bpf, rate;
  HRESULT hr;

  hr = IAudioClient_GetDevicePeriod (self->client, &default_period,
      &min_period);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::GetDevicePeriod failed");
    return FALSE;
  }

  GST_INFO_OBJECT (self, "wasapi default period: %" G_GINT64_FORMAT
      ", min period: %" G_GINT64_FORMAT, default_period, min_period);

  bpf = GST_AUDIO_INFO_BPF (&spec->info);
  rate = GST_AUDIO_INFO_RATE (&spec->info);

  if (self->low_latency) {
    if (self->sharemode == AUDCLNT_SHAREMODE_SHARED) {
      device_period = default_period;
      device_buffer_duration = 0;
    } else {
      device_period = min_period;
      device_buffer_duration = min_period;
    }
  } else {
    /* Clamp values to integral multiples of an appropriate period */
    gst_wasapi_util_get_best_buffer_sizes (spec,
        self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE, default_period,
        min_period, &device_period, &device_buffer_duration);
  }

  /* For some reason, we need to call this a second time for exclusive mode */
  if (self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE)
    CoInitialize (NULL);

  hr = IAudioClient_Initialize (self->client, self->sharemode,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, device_buffer_duration,
      /* This must always be 0 in shared mode */
      self->sharemode == AUDCLNT_SHAREMODE_SHARED ? 0 : device_period,
      self->mix_format, NULL);

  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED &&
      self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
    guint32 n_frames;

    GST_WARNING_OBJECT (self, "initialize failed due to unaligned period %i",
        (int) device_period);

    /* Calculate a new aligned period. First get the aligned buffer size. */
    hr = IAudioClient_GetBufferSize (self->client, &n_frames);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE, (NULL),
          ("IAudioClient::GetBufferSize() failed: %s", msg));
      g_free (msg);
      goto beach;
    }

    device_period = (GST_SECOND / 100) * n_frames / rate;

    GST_WARNING_OBJECT (self, "trying to re-initialize with period %i "
        "(%i frames, %i rate)", (int) device_period, n_frames, rate);

    hr = IAudioClient_Initialize (self->client, self->sharemode,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, device_period,
        device_period, self->mix_format, NULL);
  }
  if (hr != S_OK) {
    gchar *msg = gst_wasapi_util_hresult_to_string (hr);
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE, (NULL),
        ("IAudioClient::Initialize () failed: %s", msg));
    g_free (msg);
    goto beach;
  }

  /* Total size of the allocated buffer that we will write to */
  hr = IAudioClient_GetBufferSize (self->client, &self->buffer_frame_count);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::GetBufferSize failed");
    goto beach;
  }
  GST_INFO_OBJECT (self, "buffer size is %i frames, bpf is %i bytes, "
      "rate is %i Hz", self->buffer_frame_count, bpf, rate);

  /* Actual latency-time/buffer-time are different now */
  spec->segsize = gst_util_uint64_scale_int_round (rate * bpf,
      device_period * 100, GST_SECOND);

  /* We need a minimum of 2 segments to ensure glitch-free playback */
  spec->segtotal = MAX (self->buffer_frame_count * bpf / spec->segsize, 2);

  GST_INFO_OBJECT (self, "segsize is %i, segtotal is %i", spec->segsize,
      spec->segtotal);

  /* Get latency for logging */
  hr = IAudioClient_GetStreamLatency (self->client, &latency_rt);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::GetStreamLatency failed");
    goto beach;
  }
  GST_INFO_OBJECT (self, "wasapi stream latency: %" G_GINT64_FORMAT " (%"
      G_GINT64_FORMAT "ms)", latency_rt, latency_rt / 10000);

  /* Set the event handler which will trigger writes */
  hr = IAudioClient_SetEventHandle (self->client, self->event_handle);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::SetEventHandle failed");
    goto beach;
  }

  /* Get render sink client and start it up */
  if (!gst_wasapi_util_get_render_client (GST_ELEMENT (self), self->client,
          &self->render_client)) {
    goto beach;
  }

  GST_INFO_OBJECT (self, "got render client");

  /* To avoid start-up glitches, before starting the streaming, we fill the
   * buffer with silence as recommended by the documentation:
   * https://msdn.microsoft.com/en-us/library/windows/desktop/dd370879%28v=vs.85%29.aspx */
  {
    gint n_frames, len;
    gint16 *dst = NULL;

    n_frames = gst_wasapi_sink_get_can_frames (self);
    if (n_frames < 1) {
      GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
          ("should have more than %i frames to write", n_frames));
      goto beach;
    }

    len = n_frames * self->mix_format->nBlockAlign;

    hr = IAudioRenderClient_GetBuffer (self->render_client, n_frames,
        (BYTE **) & dst);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
          ("IAudioRenderClient::GetBuffer failed: %s", msg));
      g_free (msg);
      goto beach;
    }

    GST_DEBUG_OBJECT (self, "pre-wrote %i bytes of silence", len);

    hr = IAudioRenderClient_ReleaseBuffer (self->render_client, n_frames,
        AUDCLNT_BUFFERFLAGS_SILENT);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ERROR_OBJECT (self, "IAudioRenderClient::ReleaseBuffer failed: %s",
          msg);
      g_free (msg);
      goto beach;
    }
  }

  hr = IAudioClient_Start (self->client);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "IAudioClient::Start failed");
    goto beach;
  }

  gst_audio_ring_buffer_set_channel_positions (GST_AUDIO_BASE_SINK
      (self)->ringbuffer, self->positions);

#if defined(_MSC_VER) || defined(GST_FORCE_WIN_AVRT)
  /* Increase the thread priority to reduce glitches */
  {
    DWORD taskIndex = 0;
    self->thread_priority_handle =
        AvSetMmThreadCharacteristics (TEXT ("Pro Audio"), &taskIndex);
  }
#endif

  res = TRUE;

beach:
  /* unprepare() is not called if prepare() fails, but we want it to be, so call
   * it manually when needed */
  if (!res)
    gst_wasapi_sink_unprepare (asink);

  return res;
}

static gboolean
gst_wasapi_sink_unprepare (GstAudioSink * asink)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);

  if (self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE)
    CoUninitialize ();

#if defined(_MSC_VER) || defined(GST_FORCE_WIN_AVRT)
  if (self->thread_priority_handle != NULL) {
    AvRevertMmThreadCharacteristics (self->thread_priority_handle);
    self->thread_priority_handle = NULL;
  }
#endif

  if (self->client != NULL) {
    IAudioClient_Stop (self->client);
  }

  if (self->render_client != NULL) {
    IUnknown_Release (self->render_client);
    self->render_client = NULL;
  }

  return TRUE;
}

static gint
gst_wasapi_sink_write (GstAudioSink * asink, gpointer data, guint length)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);
  HRESULT hr;
  gint16 *dst = NULL;
  guint pending = length;

  while (pending > 0) {
    guint can_frames, have_frames, n_frames, write_len;

    WaitForSingleObject (self->event_handle, INFINITE);

    /* We have N frames to be written out */
    have_frames = pending / (self->mix_format->nBlockAlign);
    /* We have can_frames space in the output buffer */
    can_frames = gst_wasapi_sink_get_can_frames (self);
    /* We will write out these many frames, and this much length */
    n_frames = MIN (can_frames, have_frames);
    write_len = n_frames * self->mix_format->nBlockAlign;

    GST_DEBUG_OBJECT (self, "total: %i, have_frames: %i (%i bytes), "
        "can_frames: %i, will write: %i (%i bytes)", self->buffer_frame_count,
        have_frames, pending, can_frames, n_frames, write_len);

    hr = IAudioRenderClient_GetBuffer (self->render_client, n_frames,
        (BYTE **) & dst);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
          ("IAudioRenderClient::GetBuffer failed: %s", msg));
      g_free (msg);
      length = 0;
      goto beach;
    }

    memcpy (dst, data, write_len);

    hr = IAudioRenderClient_ReleaseBuffer (self->render_client, n_frames,
        self->mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ERROR_OBJECT (self, "IAudioRenderClient::ReleaseBuffer failed: %s",
          msg);
      g_free (msg);
      length = 0;
      goto beach;
    }

    pending -= write_len;
  }

beach:

  return length;
}

static guint
gst_wasapi_sink_delay (GstAudioSink * asink)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);
  guint delay = 0;
  HRESULT hr;

  hr = IAudioClient_GetCurrentPadding (self->client, &delay);
  if (hr != S_OK) {
    gchar *msg = gst_wasapi_util_hresult_to_string (hr);
    GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
        ("IAudioClient::GetCurrentPadding failed %s", msg));
    g_free (msg);
  }

  return delay;
}

static void
gst_wasapi_sink_reset (GstAudioSink * asink)
{
  GstWasapiSink *self = GST_WASAPI_SINK (asink);
  HRESULT hr;

  if (self->client) {
    hr = IAudioClient_Stop (self->client);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ERROR_OBJECT (self, "IAudioClient::Stop () failed: %s", msg);
      g_free (msg);
      return;
    }

    hr = IAudioClient_Reset (self->client);
    if (hr != S_OK) {
      gchar *msg = gst_wasapi_util_hresult_to_string (hr);
      GST_ERROR_OBJECT (self, "IAudioClient::Reset () failed: %s", msg);
      g_free (msg);
      return;
    }
  }
}
