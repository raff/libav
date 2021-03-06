/*
 * Copyright (C) 2009 Justin Ruggles
 * Copyright (c) 2009 Xuggle Incorporated
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * libspeex Speex audio encoder
 *
 * Usage Guide
 * This explains the values that need to be set prior to initialization in
 * order to control various encoding parameters.
 *
 * Channels
 *     Speex only supports mono or stereo, so avctx->channels must be set to
 *     1 or 2.
 *
 * Sample Rate / Encoding Mode
 *     Speex has 3 modes, each of which uses a specific sample rate.
 *         narrowband     :  8 kHz
 *         wideband       : 16 kHz
 *         ultra-wideband : 32 kHz
 *     avctx->sample_rate must be set to one of these 3 values.  This will be
 *     used to set the encoding mode.
 *
 * Rate Control
 *     VBR mode is turned on by setting CODEC_FLAG_QSCALE in avctx->flags.
 *     avctx->global_quality is used to set the encoding quality.
 *     For CBR mode, avctx->bit_rate can be used to set the constant bitrate.
 *     Alternatively, the 'cbr_quality' option can be set from 0 to 10 to set
 *     a constant bitrate based on quality.
 *     For ABR mode, set avctx->bit_rate and set the 'abr' option to 1.
 *     Approx. Bitrate Range:
 *         narrowband     : 2400 - 25600 bps
 *         wideband       : 4000 - 43200 bps
 *         ultra-wideband : 4400 - 45200 bps
 *
 * Complexity
 *     Encoding complexity is controlled by setting avctx->compression_level.
 *     The valid range is 0 to 10.  A higher setting gives generally better
 *     quality at the expense of encoding speed.  This does not affect the
 *     bit rate.
 *
 * Frames-per-Packet
 *     The encoder defaults to using 1 frame-per-packet.  However, it is
 *     sometimes desirable to use multiple frames-per-packet to reduce the
 *     amount of container overhead.  This can be done by setting the
 *     'frames_per_packet' option to a value 1 to 8.
 */

#include <speex/speex.h>
#include <speex/speex_header.h>
#include <speex/speex_stereo.h>
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"

typedef struct {
    AVClass *class;             ///< AVClass for private options
    SpeexBits bits;             ///< libspeex bitwriter context
    SpeexHeader header;         ///< libspeex header struct
    void *enc_state;            ///< libspeex encoder state
    int frames_per_packet;      ///< number of frames to encode in each packet
    float vbr_quality;          ///< VBR quality 0.0 to 10.0
    int cbr_quality;            ///< CBR quality 0 to 10
    int abr;                    ///< flag to enable ABR
    int pkt_frame_count;        ///< frame count for the current packet
    int64_t next_pts;           ///< next pts, in sample_rate time base
    int pkt_sample_count;       ///< sample count in the current packet
} LibSpeexEncContext;

static av_cold void print_enc_params(AVCodecContext *avctx,
                                     LibSpeexEncContext *s)
{
    const char *mode_str = "unknown";

    av_log(avctx, AV_LOG_DEBUG, "channels: %d\n", avctx->channels);
    switch (s->header.mode) {
    case SPEEX_MODEID_NB:  mode_str = "narrowband";     break;
    case SPEEX_MODEID_WB:  mode_str = "wideband";       break;
    case SPEEX_MODEID_UWB: mode_str = "ultra-wideband"; break;
    }
    av_log(avctx, AV_LOG_DEBUG, "mode: %s\n", mode_str);
    if (s->header.vbr) {
        av_log(avctx, AV_LOG_DEBUG, "rate control: VBR\n");
        av_log(avctx, AV_LOG_DEBUG, "  quality: %f\n", s->vbr_quality);
    } else if (s->abr) {
        av_log(avctx, AV_LOG_DEBUG, "rate control: ABR\n");
        av_log(avctx, AV_LOG_DEBUG, "  bitrate: %d bps\n", avctx->bit_rate);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "rate control: CBR\n");
        av_log(avctx, AV_LOG_DEBUG, "  bitrate: %d bps\n", avctx->bit_rate);
    }
    av_log(avctx, AV_LOG_DEBUG, "complexity: %d\n",
           avctx->compression_level);
    av_log(avctx, AV_LOG_DEBUG, "frame size: %d samples\n",
           avctx->frame_size);
    av_log(avctx, AV_LOG_DEBUG, "frames per packet: %d\n",
           s->frames_per_packet);
    av_log(avctx, AV_LOG_DEBUG, "packet size: %d\n",
           avctx->frame_size * s->frames_per_packet);
}

static av_cold int encode_init(AVCodecContext *avctx)
{
    LibSpeexEncContext *s = avctx->priv_data;
    const SpeexMode *mode;
    uint8_t *header_data;
    int header_size;
    int32_t complexity;

    /* channels */
    if (avctx->channels < 1 || avctx->channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "Invalid channels (%d). Only stereo and "
               "mono are supported\n", avctx->channels);
        return AVERROR(EINVAL);
    }

    /* sample rate and encoding mode */
    switch (avctx->sample_rate) {
    case  8000: mode = &speex_nb_mode;  break;
    case 16000: mode = &speex_wb_mode;  break;
    case 32000: mode = &speex_uwb_mode; break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Sample rate of %d Hz is not supported. "
               "Resample to 8, 16, or 32 kHz.\n", avctx->sample_rate);
        return AVERROR(EINVAL);
    }

    /* initialize libspeex */
    s->enc_state = speex_encoder_init(mode);
    if (!s->enc_state) {
        av_log(avctx, AV_LOG_ERROR, "Error initializing libspeex\n");
        return -1;
    }
    speex_init_header(&s->header, avctx->sample_rate, avctx->channels, mode);

    /* rate control method and parameters */
    if (avctx->flags & CODEC_FLAG_QSCALE) {
        /* VBR */
        s->header.vbr = 1;
        speex_encoder_ctl(s->enc_state, SPEEX_SET_VBR, &s->header.vbr);
        s->vbr_quality = av_clipf(avctx->global_quality / (float)FF_QP2LAMBDA,
                                  0.0f, 10.0f);
        speex_encoder_ctl(s->enc_state, SPEEX_SET_VBR_QUALITY, &s->vbr_quality);
    } else {
        s->header.bitrate = avctx->bit_rate;
        if (avctx->bit_rate > 0) {
            /* CBR or ABR by bitrate */
            if (s->abr) {
                speex_encoder_ctl(s->enc_state, SPEEX_SET_ABR,
                                  &s->header.bitrate);
                speex_encoder_ctl(s->enc_state, SPEEX_GET_ABR,
                                  &s->header.bitrate);
            } else {
                speex_encoder_ctl(s->enc_state, SPEEX_SET_BITRATE,
                                  &s->header.bitrate);
                speex_encoder_ctl(s->enc_state, SPEEX_GET_BITRATE,
                                  &s->header.bitrate);
            }
        } else {
            /* CBR by quality */
            speex_encoder_ctl(s->enc_state, SPEEX_SET_QUALITY,
                              &s->cbr_quality);
            speex_encoder_ctl(s->enc_state, SPEEX_GET_BITRATE,
                              &s->header.bitrate);
        }
        /* stereo side information adds about 800 bps to the base bitrate */
        /* TODO: this should be calculated exactly */
        avctx->bit_rate = s->header.bitrate + (avctx->channels == 2 ? 800 : 0);
    }

    /* set encoding complexity */
    if (avctx->compression_level > FF_COMPRESSION_DEFAULT) {
        complexity = av_clip(avctx->compression_level, 0, 10);
        speex_encoder_ctl(s->enc_state, SPEEX_SET_COMPLEXITY, &complexity);
    }
    speex_encoder_ctl(s->enc_state, SPEEX_GET_COMPLEXITY, &complexity);
    avctx->compression_level = complexity;

    /* set packet size */
    avctx->frame_size = s->header.frame_size;
    s->header.frames_per_packet = s->frames_per_packet;

    /* set encoding delay */
    speex_encoder_ctl(s->enc_state, SPEEX_GET_LOOKAHEAD, &avctx->delay);

    /* create header packet bytes from header struct */
    /* note: libspeex allocates the memory for header_data, which is freed
             below with speex_header_free() */
    header_data = speex_header_to_packet(&s->header, &header_size);

    /* allocate extradata and coded_frame */
    avctx->extradata   = av_malloc(header_size + FF_INPUT_BUFFER_PADDING_SIZE);
    avctx->coded_frame = avcodec_alloc_frame();
    if (!avctx->extradata || !avctx->coded_frame) {
        speex_header_free(header_data);
        speex_encoder_destroy(s->enc_state);
        av_log(avctx, AV_LOG_ERROR, "memory allocation error\n");
        return AVERROR(ENOMEM);
    }

    /* copy header packet to extradata */
    memcpy(avctx->extradata, header_data, header_size);
    avctx->extradata_size = header_size;
    speex_header_free(header_data);

    /* init libspeex bitwriter */
    speex_bits_init(&s->bits);

    print_enc_params(avctx, s);
    return 0;
}

static int encode_frame(AVCodecContext *avctx, uint8_t *frame, int buf_size,
                        void *data)
{
    LibSpeexEncContext *s = avctx->priv_data;
    int16_t *samples      = data;

    if (data) {
        /* encode Speex frame */
        if (avctx->channels == 2)
            speex_encode_stereo_int(samples, s->header.frame_size, &s->bits);
        speex_encode_int(s->enc_state, samples, &s->bits);
        s->pkt_frame_count++;
        s->pkt_sample_count += avctx->frame_size;
    } else {
        /* handle end-of-stream */
        if (!s->pkt_frame_count)
            return 0;
        /* add extra terminator codes for unused frames in last packet */
        while (s->pkt_frame_count < s->frames_per_packet) {
            speex_bits_pack(&s->bits, 15, 5);
            s->pkt_frame_count++;
        }
    }

    /* write output if all frames for the packet have been encoded */
    if (s->pkt_frame_count == s->frames_per_packet) {
        s->pkt_frame_count = 0;
        avctx->coded_frame->pts = ff_samples_to_time_base(avctx, s->next_pts -
                                                          avctx->delay);
        s->next_pts += s->pkt_sample_count;
        s->pkt_sample_count = 0;
        if (buf_size > speex_bits_nbytes(&s->bits)) {
            int ret = speex_bits_write(&s->bits, frame, buf_size);
            speex_bits_reset(&s->bits);
            return ret;
        } else {
            av_log(avctx, AV_LOG_ERROR, "output buffer too small");
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

static av_cold int encode_close(AVCodecContext *avctx)
{
    LibSpeexEncContext *s = avctx->priv_data;

    speex_bits_destroy(&s->bits);
    speex_encoder_destroy(s->enc_state);

    av_freep(&avctx->coded_frame);
    av_freep(&avctx->extradata);

    return 0;
}

#define OFFSET(x) offsetof(LibSpeexEncContext, x)
#define AE AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "abr",               "Use average bit rate",                      OFFSET(abr),               AV_OPT_TYPE_INT, { 0 }, 0,   1, AE },
    { "cbr_quality",       "Set quality value (0 to 10) for CBR",       OFFSET(cbr_quality),       AV_OPT_TYPE_INT, { 8 }, 0,  10, AE },
    { "frames_per_packet", "Number of frames to encode in each packet", OFFSET(frames_per_packet), AV_OPT_TYPE_INT, { 1 }, 1,   8, AE },
    { NULL },
};

static const AVClass class = {
    .class_name = "libspeex",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault defaults[] = {
    { "b",                 "0" },
    { "compression_level", "3" },
    { NULL },
};

AVCodec ff_libspeex_encoder = {
    .name           = "libspeex",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = CODEC_ID_SPEEX,
    .priv_data_size = sizeof(LibSpeexEncContext),
    .init           = encode_init,
    .encode         = encode_frame,
    .close          = encode_close,
    .capabilities   = CODEC_CAP_DELAY,
    .sample_fmts    = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE },
    .long_name      = NULL_IF_CONFIG_SMALL("libspeex Speex"),
    .priv_class     = &class,
    .defaults       = defaults,
};
