/* GStreamer
 * Copyright (C) 2008 David Schleef <ds@schleef.org>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _GST_BASE_VIDEO_DECODER_H_
#define _GST_BASE_VIDEO_DECODER_H_

#ifndef GST_USE_UNSTABLE_API
#warning "GstBaseVideoDecoder is unstable API and may change in future."
#warning "You can define GST_USE_UNSTABLE_API to avoid this warning."
#endif

#include <gst/video/gstbasevideocodec.h>

G_BEGIN_DECLS

#define GST_TYPE_BASE_VIDEO_DECODER \
  (gst_base_video_decoder_get_type())
#define GST_BASE_VIDEO_DECODER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BASE_VIDEO_DECODER,GstBaseVideoDecoder))
#define GST_BASE_VIDEO_DECODER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BASE_VIDEO_DECODER,GstBaseVideoDecoderClass))
#define GST_BASE_VIDEO_DECODER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_BASE_VIDEO_DECODER,GstBaseVideoDecoderClass))
#define GST_IS_BASE_VIDEO_DECODER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BASE_VIDEO_DECODER))
#define GST_IS_BASE_VIDEO_DECODER_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BASE_VIDEO_DECODER))

/**
 * GST_BASE_VIDEO_DECODER_SINK_NAME:
 *
 * The name of the templates for the sink pad.
 */
#define GST_BASE_VIDEO_DECODER_SINK_NAME    "sink"
/**
 * GST_BASE_VIDEO_DECODER_SRC_NAME:
 *
 * The name of the templates for the source pad.
 */
#define GST_BASE_VIDEO_DECODER_SRC_NAME     "src"

/**
 * GST_BASE_VIDEO_DECODER_FLOW_NEED_DATA:
 *
 * Returned while parsing to indicate more data is needed.
 **/
#define GST_BASE_VIDEO_DECODER_FLOW_NEED_DATA GST_FLOW_CUSTOM_SUCCESS


typedef struct _GstBaseVideoDecoder GstBaseVideoDecoder;
typedef struct _GstBaseVideoDecoderClass GstBaseVideoDecoderClass;

struct _GstBaseVideoDecoder
{
  GstBaseVideoCodec base_video_codec;

  /*< protected >*/
  gboolean          sink_clipping;
  gboolean          do_byte_time;
  gboolean          packetized;

  /* parse tracking */
  /* input data */
  GstAdapter       *input_adapter;
  /* assembles current frame */
  GstAdapter       *output_adapter;

  /*< private >*/
  /* FIXME move to real private part ?
   * (and introduce a context ?) */
  /* ... being tracked here;
   * only available during parsing */
  /* FIXME remove and add parameter to method */
  GstVideoFrame    *current_frame;
  /* relative offset of input data */
  guint64           input_offset;
  /* relative offset of frame */
  guint64           frame_offset;
  /* tracking ts and offsets */
  GList            *timestamps;
  /* whether parsing is in sync */
  gboolean          have_sync;

  /* maybe sort-of protected ? */

  /* need mark discont */
  gboolean          discont;
  /* src caps set */
  gboolean          have_src_caps;

  /* combine to yield (presentation) ts */
  GstClockTime      timestamp_offset;
  int               field_index;

  /* last outgoing ts */
  GstClockTime      last_timestamp;

  /* no comment ... */
  guint64           base_picture_number;
  int               reorder_depth;
  int               distance_from_sync;

  /* FIXME before moving to base */
  void             *padding[GST_PADDING_LARGE];
};

struct _GstBaseVideoDecoderClass
{
  GstBaseVideoCodecClass base_video_codec_class;

  gboolean (*set_format) (GstBaseVideoDecoder *coder, GstVideoState * state);
  gboolean (*start) (GstBaseVideoDecoder *coder);
  gboolean (*stop) (GstBaseVideoDecoder *coder);
  gboolean (*reset) (GstBaseVideoDecoder *coder);
  int (*scan_for_sync) (GstBaseVideoDecoder *decoder, gboolean at_eos,
      int offset, int n);
  GstFlowReturn (*parse_data) (GstBaseVideoDecoder *decoder, gboolean at_eos);
  GstFlowReturn (*finish) (GstBaseVideoDecoder *coder);
  GstFlowReturn (*handle_frame) (GstBaseVideoDecoder *coder, GstVideoFrame *frame);
  GstFlowReturn (*shape_output) (GstBaseVideoDecoder *coder, GstVideoFrame *frame);
  GstCaps *(*get_caps) (GstBaseVideoDecoder *coder);

  guint32 capture_mask;
  guint32 capture_pattern;

  /* FIXME before moving to base */
  void *padding[GST_PADDING_LARGE];
};

GType gst_base_video_decoder_get_type (void);

void gst_base_video_decoder_class_set_capture_pattern (GstBaseVideoDecoderClass *klass,
    guint32 mask, guint32 pattern);

GstVideoFrame *gst_base_video_decoder_get_frame (GstBaseVideoDecoder *coder,
    int frame_number);
GstVideoFrame *gst_base_video_decoder_get_oldest_frame (GstBaseVideoDecoder *coder);
void gst_base_video_decoder_add_to_frame (GstBaseVideoDecoder *base_video_decoder,
    int n_bytes);
GstFlowReturn gst_base_video_decoder_finish_frame (GstBaseVideoDecoder *base_video_decoder,
    GstVideoFrame *frame);
GstFlowReturn
gst_base_video_decoder_have_frame (GstBaseVideoDecoder *base_video_decoder);
GstVideoState * gst_base_video_decoder_get_state (GstBaseVideoDecoder *base_video_decoder);
void gst_base_video_decoder_set_state (GstBaseVideoDecoder *base_video_decoder,
    GstVideoState *state);
void gst_base_video_decoder_lost_sync (GstBaseVideoDecoder *base_video_decoder);
void gst_base_video_decoder_set_sync_point (GstBaseVideoDecoder *base_video_decoder);

void gst_base_video_decoder_set_src_caps (GstBaseVideoDecoder *base_video_decoder);

GstBuffer * gst_base_video_decoder_alloc_src_buffer (GstBaseVideoDecoder *
    base_video_decoder);
GstFlowReturn gst_base_video_decoder_alloc_src_frame (GstBaseVideoDecoder *base_video_decoder,
    GstVideoFrame *frame);

GstClockTimeDiff gst_base_video_decoder_get_max_decode_time (
    GstBaseVideoDecoder *base_video_decoder, GstVideoFrame *frame);

G_END_DECLS

#endif

