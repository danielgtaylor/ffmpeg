/*
 * copyright (c) 2007 Bobby Bingham
 *
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

/**
 * @file
 * filter to overlay one video on top of another
 */

#include "avfilter.h"
#include "libavcore/imgutils.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"

static const char *var_names[] = {
    "main_w",    ///< width of the main video
    "main_h",    ///< height of the main video
    "overlay_w", ///< width of the overlay video
    "overlay_h", ///< height of the overlay video
    NULL
};

enum var_name {
    MAIN_W,
    MAIN_H,
    OVERLAY_W,
    OVERLAY_H,
    VARS_NB
};

typedef struct {
    int x, y;                   //< position of subpicture

    /** pics[0][0..1] are pictures for the main image.
     *  pics[1][0..1] are pictures for the sub image.
     *  pics[x][0]    are previously outputted images.
     *  pics[x][1]    are queued, yet unused frames for each input. */
    AVFilterBufferRef *pics[2][2];

    int bpp;                    //< bytes per pixel
    int hsub, vsub;             //< chroma subsampling

    char x_expr[256], y_expr[256];
} OverlayContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    OverlayContext *over = ctx->priv;

    av_strlcpy(over->x_expr, "0", sizeof(over->x_expr));
    av_strlcpy(over->y_expr, "0", sizeof(over->y_expr));

    if (args)
        sscanf(args, "%255[^:]:%255[^:]", over->x_expr, over->y_expr);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;
    int i, j;

    for(i = 0; i < 2; i ++)
        for(j = 0; j < 2; j ++)
            if(over->pics[i][j])
                avfilter_unref_buffer(over->pics[i][j]);
}

static int query_formats(AVFilterContext *ctx)
{
  //const enum PixelFormat inout_pix_fmts[] = { PIX_FMT_BGR24, PIX_FMT_NONE };
  //const enum PixelFormat blend_pix_fmts[] = { PIX_FMT_BGRA, PIX_FMT_NONE };
    const enum PixelFormat inout_pix_fmts[] = { PIX_FMT_YUV420P, PIX_FMT_NONE };
    const enum PixelFormat blend_pix_fmts[] = { PIX_FMT_YUVA420P, PIX_FMT_NONE };
    AVFilterFormats *inout_formats = avfilter_make_format_list(inout_pix_fmts);
    AVFilterFormats *blend_formats = avfilter_make_format_list(blend_pix_fmts);

    avfilter_formats_ref(inout_formats, &ctx->inputs [0]->out_formats);
    avfilter_formats_ref(blend_formats, &ctx->inputs [1]->out_formats);
    avfilter_formats_ref(inout_formats, &ctx->outputs[0]->in_formats );

    return 0;
}

static int config_input_main(AVFilterLink *link)
{
    OverlayContext *over = link->dst->priv;

    switch(link->format) {
    case PIX_FMT_RGB32:
    case PIX_FMT_BGR32:
        over->bpp = 4;
        break;
    case PIX_FMT_RGB24:
    case PIX_FMT_BGR24:
        over->bpp = 3;
        break;
    case PIX_FMT_RGB565:
    case PIX_FMT_RGB555:
    case PIX_FMT_BGR565:
    case PIX_FMT_BGR555:
    case PIX_FMT_GRAY16BE:
    case PIX_FMT_GRAY16LE:
        over->bpp = 2;
        break;
    default:
        over->bpp = 1;
    }

    avcodec_get_chroma_sub_sample(link->format, &over->hsub, &over->vsub);

    return 0;
}

static int config_input_overlay(AVFilterLink *link)
{
    AVFilterContext *ctx  = link->dst;
    OverlayContext  *over = link->dst->priv;
    char *expr;
    double var_values[VARS_NB], res;
    int ret;

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    var_values[MAIN_W]    = ctx->inputs[0]->w;
    var_values[MAIN_H]    = ctx->inputs[0]->h;
    var_values[OVERLAY_W] = ctx->inputs[1]->w;
    var_values[OVERLAY_H] = ctx->inputs[1]->h;

    if ((ret = av_parse_and_eval_expr(&res, (expr = over->x_expr), var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    over->x = res;
    if ((ret = av_parse_and_eval_expr(&res, (expr = over->y_expr), var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)))
        goto fail;
    over->y = res;
    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'\n", expr);
    return ret;
}

static void shift_input(OverlayContext *over, int idx)
{
    assert(over->pics[idx][0]);
    assert(over->pics[idx][1]);
    avfilter_unref_buffer(over->pics[idx][0]);
    over->pics[idx][0] = over->pics[idx][1];
    over->pics[idx][1] = NULL;
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    OverlayContext *over = link->dst->priv;
    /* There shouldn't be any previous queued frame in this queue */
    assert(!over->pics[link->dstpad - link->dst->input_pads][1]);
    if (over->pics[link->dstpad - link->dst->input_pads][0]) {
        /* Queue the new frame */
        over->pics[link->dstpad - link->dst->input_pads][1] = picref;
    } else {
        /* No previous unused frame, take this one into use directly */
        over->pics[link->dstpad - link->dst->input_pads][0] = picref;
    }
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
}

static void end_frame(AVFilterLink *link)
{
}

static int lower_timestamp(OverlayContext *over)
{
    if(!over->pics[0][0] &&
       !over->pics[1][0]) return 2;
    if(!over->pics[0][1]) return 0;
    if(!over->pics[1][1]) return 1;

    if(over->pics[0][1]->pts == over->pics[1][1]->pts) return 2;
    return (over->pics[0][1]->pts > over->pics[1][1]->pts);
}

static void copy_image_rgb(AVFilterBufferRef *dst, int x, int y,
                           AVFilterBufferRef *src, int w, int h, int bpp)
{
    dst->data[0] += x * bpp;
    dst->data[0] += y * dst->linesize[0];

    if (src->format == PIX_FMT_BGRA) {
        for (y = 0; y < h; y++) {
                  uint8_t *optr = dst->data[0] + y * dst->linesize[0];
            const uint8_t *iptr = src->data[0] + y * src->linesize[0];
            for (x = 0; x < w; x++) {
                uint8_t a = iptr[3];
                optr[0] = (optr[0] * (0xff - a) + iptr[0] * a + 128) >> 8;
                optr[1] = (optr[1] * (0xff - a) + iptr[1] * a + 128) >> 8;
                optr[2] = (optr[2] * (0xff - a) + iptr[2] * a + 128) >> 8;
                iptr += bpp+1;
                optr += bpp;
            }
        }
    } else {
        av_image_copy(dst->data, dst->linesize,
                      src->data, src->linesize,
                      dst->format, w, h);
    }
}

static void copy_blended(uint8_t* out, int out_linesize,
    const uint8_t* in, int in_linesize,
    const uint8_t* alpha, int alpha_linesize,
    int w, int h, int hsub, int vsub)
{
    int y;
    for (y = 0; y < h; y++) {
        int x;
              uint8_t *optr = out   + y         * out_linesize;
        const uint8_t *iptr = in    + y         * in_linesize;
        const uint8_t *aptr = alpha + (y<<vsub) * alpha_linesize;
        for (x = 0; x < w; x++) {
            uint8_t a = *aptr;
            *optr = (*optr * (0xff - a) + *iptr * a + 128) >> 8;
            optr++;
            iptr++;
            aptr += 1 << hsub;
        }
    }
}

static void copy_image_yuv(AVFilterBufferRef *dst, int x, int y,
                           AVFilterBufferRef *src, int w, int h,
                           int bpp, int hsub, int vsub)
{
    int i;
    uint8_t *dst_data[4];

    for(i = 0; i < 4; i ++) {
        if (dst->data[i]) {
            int x_off = x;
            int y_off = y;
            if (i == 1 || i == 2) {
                x_off >>= hsub;
                y_off >>= vsub;
            }
            dst_data[i] = dst->data[i] + x_off * bpp + y_off * dst->linesize[i];
        }
    }

    if (src->format == PIX_FMT_YUVA420P) {
        int chroma_w = w>>hsub;
        int chroma_h = h>>vsub;
        assert(dst->pic->format == PIX_FMT_YUV420P);
        copy_blended(dst_data[0], dst->linesize[0], src->data[0], src->linesize[0], src->data[3], src->linesize[3], w, h, 0, 0);
        copy_blended(dst_data[1], dst->linesize[1], src->data[1], src->linesize[1], src->data[3], src->linesize[3], chroma_w, chroma_h, hsub, vsub);
        copy_blended(dst_data[2], dst->linesize[2], src->data[2], src->linesize[2], src->data[3], src->linesize[3], chroma_w, chroma_h, hsub, vsub);
    } else {
        av_image_copy(dst_data, dst->linesize, src->data, src->linesize, dst->format, w, h);
    }
}

static void copy_image(AVFilterBufferRef *dst, int x, int y,
                       AVFilterBufferRef *src, int w, int h,
                       int bpp, int hsub, int vsub)
{
    if (dst->format == PIX_FMT_YUV420P)
        return copy_image_yuv(dst, x, y, src, w, h, bpp, hsub, vsub);
    else
        return copy_image_rgb(dst, x, y, src, w, h, bpp);
}

static int request_frame(AVFilterLink *link)
{
    AVFilterBufferRef *pic;
    OverlayContext *over = link->src->priv;
    int idx;
    int x, y, w, h;

    if (!over->pics[0][0] || !over->pics[1][0]) {
        /* No frame output yet, we need one frame from each input */
        if (!over->pics[0][0] && avfilter_request_frame(link->src->inputs[0]))
            return AVERROR_EOF;
        if (!over->pics[1][0] && avfilter_request_frame(link->src->inputs[1]))
            return AVERROR_EOF;
    } else {
        int eof = 0;

        /* Try pulling a new candidate from each input unless we already
           have one */
        for (idx = 0; idx < 2; idx++) {
            if (!over->pics[idx][1] &&
                 avfilter_request_frame(link->src->inputs[idx]))
                eof++;
        }
        if (eof == 2)
            return AVERROR_EOF; /* No new candidates in any input; EOF */

        /* At least one new frame */
        assert(over->pics[0][1] || over->pics[1][1]);

        if (over->pics[0][1] && over->pics[1][1]) {
            /* Neither one of the inputs has finished */
            if ((idx = lower_timestamp(over)) == 2) {
                shift_input(over, 0);
                shift_input(over, 1);
            } else
                shift_input(over, idx);
        } else if (over->pics[0][1]) {
            /* Use the single new input frame */
            shift_input(over, 0);
        } else {
            assert(over->pics[1][1]);
            shift_input(over, 1);
        }
    }

    /* we draw the output frame */
    pic = avfilter_get_video_buffer(link, AV_PERM_WRITE, link->w, link->h);
    if(over->pics[0][0]) {
        pic->video->pixel_aspect = over->pics[0][0]->video->pixel_aspect;
        copy_image(pic, 0, 0, over->pics[0][0], link->w, link->h,
                   over->bpp, over->hsub, over->vsub);
    }
    x = FFMIN(over->x, link->w-1);
    y = FFMIN(over->y, link->h-1);
    w = FFMIN(link->w-x, over->pics[1][0]->video->w);
    h = FFMIN(link->h-y, over->pics[1][0]->video->h);
    if(over->pics[1][0])
        copy_image(pic, x, y, over->pics[1][0], w, h,
                   over->bpp, over->hsub, over->vsub);

    /* we give the output frame the higher of the two current pts values */
    pic->pts = FFMAX(over->pics[0][0]->pts, over->pics[1][0]->pts);

    /* and send it to the next filter */
    avfilter_start_frame(link, avfilter_ref_buffer(pic, ~0));
    avfilter_draw_slice (link, 0, pic->video->h, 1);
    avfilter_end_frame  (link);
    avfilter_unref_buffer(pic);

    return 0;
}

AVFilter avfilter_vf_overlay =
{
    .name      = "overlay",
    .description = "Overlay a video source on top of the input.",

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(OverlayContext),

    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame,
                                    .config_props    = config_input_main,
                                    .draw_slice      = draw_slice,
                                    .end_frame       = end_frame,
                                    .min_perms       = AV_PERM_READ,
                                    .rej_perms       = AV_PERM_REUSE2, },
                                  { .name            = "sub",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame,
                                    .config_props    = config_input_overlay,
                                    .draw_slice      = draw_slice,
                                    .end_frame       = end_frame,
                                    .min_perms       = AV_PERM_READ,
                                    .rej_perms       = AV_PERM_REUSE2, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .request_frame   = request_frame, },
                                  { .name = NULL}},
};

