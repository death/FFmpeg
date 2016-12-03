/*
 * Copyright (c) #x7E0 death
 *
 * This file is not part of FFmpeg.
 *
 * Borrowed some code from f_zmq and vf_hflip.
 */

/**
 * @file
 * filter video frames through ZMQ pipe
 */

#include <zmq.h>
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

typedef struct {
  const AVClass *class;
  void *zmq;
  void *sock;
  char *conn_address;
  int max_step[4];
  int planewidth[4];
  int planeheight[4];
} ZMQContext;

#define OFFSET(x) offsetof(ZMQContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
  {"conn_address", "set connection address", OFFSET(conn_address), AV_OPT_TYPE_STRING, {.str = "tcp://127.0.0.1:5556"}, 0, 0, FLAGS},
  {"c",            "set connection address", OFFSET(conn_address), AV_OPT_TYPE_STRING, {.str = "tcp://127.0.0.1:5556"}, 0, 0, FLAGS},
  {NULL}
};

static av_cold int init(AVFilterContext *ctx)
{
  ZMQContext *zmq = ctx->priv;

  zmq->zmq = zmq_ctx_new();
  if (!zmq->zmq) {
    av_log(ctx, AV_LOG_ERROR,
           "Could not create ZMQ context: %s\n", zmq_strerror(errno));
    return AVERROR_EXTERNAL;
  }

  zmq->sock = zmq_socket(zmq->zmq, ZMQ_REQ);
  if (!zmq->sock) {
    av_log(ctx, AV_LOG_ERROR,
           "Could not create ZMQ socket: %s\n", zmq_strerror(errno));
    return AVERROR_EXTERNAL;
  }

  if (zmq_connect(zmq->sock, zmq->conn_address) == -1) {
    av_log(ctx, AV_LOG_ERROR,
           "Could not connect ZMQ socket to address '%s': %s\n",
           zmq->conn_address, zmq_strerror(errno));
    return AVERROR_EXTERNAL;
  }

  return 0;
}

static void av_cold uninit(AVFilterContext *ctx)
{
  ZMQContext *zmq = ctx->priv;

  zmq_close(zmq->sock);
  zmq_ctx_destroy(zmq->zmq);
}

static int query_formats(AVFilterContext *ctx)
{
  AVFilterFormats *pix_fmts = NULL;
  int fmt, ret;

  for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL ||
          desc->flags & AV_PIX_FMT_FLAG_BITSTREAM ||
          (desc->log2_chroma_w != desc->log2_chroma_h &&
           desc->comp[0].plane == desc->comp[1].plane)) &&
        (ret = ff_add_format(&pix_fmts, fmt)) < 0)
      return ret;
  }

  return ff_set_common_formats(ctx, pix_fmts);
}

static int config_props(AVFilterLink *inlink)
{
  ZMQContext *zmq = inlink->dst->priv;
  const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);
  const int hsub = pix_desc->log2_chroma_w;
  const int vsub = pix_desc->log2_chroma_h;

  av_image_fill_max_pixsteps(zmq->max_step, NULL, pix_desc);
  zmq->planewidth[0]  = zmq->planewidth[3]  = inlink->w;
  zmq->planewidth[1]  = zmq->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, hsub);
  zmq->planeheight[0] = zmq->planeheight[3] = inlink->h;
  zmq->planeheight[1] = zmq->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, vsub);

  return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
  AVFilterContext *ctx = inlink->dst;
  ZMQContext *zmq = ctx->priv;
  AVFilterLink *outlink = ctx->outputs[0];
  AVFrame *ioframe;
  int plane;
  int nplanes;
  char buf[256];                /* hopefully enough, no bound checks!... */
  int bufp;
  int ret;

  /* Initialize our IO frame. */
  ioframe = ff_get_video_buffer(outlink, in->width, in->height);
  if (!ioframe) {
    return AVERROR(ENOMEM);
  }
  av_frame_copy_props(ioframe, in);
  av_frame_copy(ioframe, in);

  for (nplanes = 0; nplanes < 4 && ioframe->data[nplanes] && ioframe->linesize[nplanes]; nplanes++)
    ;

  /* Send information about the frame. */
  bufp = sprintf(buf, "(");
  for (plane = 0; plane < nplanes; plane++) {
    int w = zmq->planewidth[plane];
    int h = zmq->planeheight[plane];
    int step = zmq->max_step[plane];
    int linesize = ioframe->linesize[plane];
    bufp += sprintf(&buf[bufp], "(%d %d %d %d)", w, h, step, linesize);
  }
  bufp += sprintf(&buf[bufp], ")");

  if (zmq_send(zmq->sock, buf, strlen(buf), ZMQ_SNDMORE) == -1) {
    av_log(ctx, AV_LOG_ERROR,
           "Failed to send meta message part: %s\n",
           zmq_strerror(errno));
    ret = AVERROR_EXTERNAL;
    goto err;
  }

  /* Send the frame data. */
  for (plane = 0; plane < nplanes; plane++) {
    int h = zmq->planeheight[plane];
    int linesize = ioframe->linesize[plane];
    size_t datasize = h * linesize;
    int flags = plane == nplanes - 1 ? 0 : ZMQ_SNDMORE;
    if (zmq_send(zmq->sock, ioframe->data[plane], datasize, flags) != datasize) {
      av_log(ctx, AV_LOG_ERROR,
             "Failed to send plane %d message part: %s\n",
             plane, zmq_strerror(errno));
      ret = AVERROR_EXTERNAL;
      goto err;
    }
  }

  /* Receive the new frame data; it should be a multi-part message
   * with the same format as the input. */
  for (plane = 0; plane < nplanes; plane++) {
    int needmore = plane != nplanes - 1;
    int h = zmq->planeheight[plane];
    int linesize = ioframe->linesize[plane];
    size_t datasize = h * linesize;
    int hasmore;
    size_t hasmorelen = sizeof(int);
    if (zmq_recv(zmq->sock, ioframe->data[plane], datasize, 0) != datasize) {
      av_log(ctx, AV_LOG_ERROR,
             "Failed to receive plane %d message part: %s\n",
             plane, zmq_strerror(errno));
      ret = AVERROR_EXTERNAL;
      goto err;
    }
    if (zmq_getsockopt(zmq->sock, ZMQ_RCVMORE, &hasmore, &hasmorelen) == -1) {
      av_log(ctx, AV_LOG_ERROR,
             "Failed to get socket option for plane %d message part: %s\n",
             plane, zmq_strerror(errno));
      ret = AVERROR_EXTERNAL;
      goto err;
    }
    if (hasmore != needmore) {
      av_log(ctx, AV_LOG_ERROR,
             "Mismatch in number of message parts: hasmore(%d) != needmore(%d)\n",
             hasmore, needmore);
      ret = AVERROR_EXTERNAL;
      goto err;
    }
  }

  av_frame_free(&in);
  return ff_filter_frame(outlink, ioframe);

 err:
  av_frame_free(&ioframe);
  av_frame_free(&in);
  return ret;
}

#if CONFIG_ZMQIMG_FILTER

#define zmqimg_options options
AVFILTER_DEFINE_CLASS(zmqimg);

static const AVFilterPad avfilter_vf_zmqimg_in[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .filter_frame = filter_frame,
    .config_props = config_props,
  },
  {
    NULL
  }
};

static const AVFilterPad avfilter_vf_zmqimg_out[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
  },
  {
    NULL
  }
};

AVFilter ff_vf_zmqimg = {
  .name = "zmqimg",
  .description = NULL_IF_CONFIG_SMALL("Filter video frames through ZMQ pipe."),
  .priv_size = sizeof(ZMQContext),
  .priv_class = &zmqimg_class,
  .init = init,
  .uninit = uninit,
  .query_formats = query_formats,
  .inputs = avfilter_vf_zmqimg_in,
  .outputs = avfilter_vf_zmqimg_out,
};

#endif // CONFIG_ZMQIMG_FILTER
