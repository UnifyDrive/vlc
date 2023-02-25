/*****************************************************************************
 * spdif.c: S/PDIF pass-though decoder
 *****************************************************************************
 * Copyright (C) 2016 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_codec.h>
#include <vlc_modules.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <vlc_avcodec.h>
#define BLOCK_FLAG_PRIVATE_REALLOCATED (1 << BLOCK_FLAG_PRIVATE_SHIFT)
static int OpenDecoder(vlc_object_t *);
static void EndTranscode(vlc_object_t *p_this);
static void Flush( decoder_t *p_dec );
static int process_decode_audio( decoder_t *p_dec, block_t *pp_block );
static int open_audio_codec(decoder_t *p_dec);

#if defined(__ANDROID__) || defined(__linux__)
/*****************************************************************************
 * Codec fourcc -> libavcodec Codec_id mapping
 * Sorted by AVCodecID enumeration order
 *****************************************************************************/
struct spdif_avcodec_fourcc
{
    vlc_fourcc_t i_fourcc;
    unsigned i_codec;
};
#endif

struct decoder_sys_t
{
    bool b_AC3_passthrough;
    AVCodecContext *p_context;
    AVCodecContext *ac3_CodecCtx;
    bool b_planar;
    SwrContext *swr_ctx;
    AVAudioFifo *fifo;
    int64_t pts_audio;

   
    /*
     * Output properties
     */
    audio_sample_format_t aout_format;
    date_t                end_date;

    /* */
    int     i_reject_count;

    /* */
    bool    b_extract;
    int     pi_extraction[AOUT_CHAN_MAX];
    int     i_previous_channels;
    uint64_t i_previous_layout;
};


vlc_module_begin()
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACODEC)
    set_description(N_("S/PDIF pass-through decoder"))
    set_capability("audio decoder", 120)
    set_callbacks(OpenDecoder, EndTranscode)
vlc_module_end()


#if defined(__ANDROID__) || defined(__linux__)

static int add_samples_to_fifo(decoder_t *p_dec,AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        msg_Dbg(p_dec,"tdx  Could not reallocate FIFO ");
        return -1;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        msg_Dbg(p_dec, "tdx  Could not write data to FIFO\n");
        return -1;
    }
    return 0;
}

static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;
    if (!(*frame = av_frame_alloc())) {
        return 0;
    }

    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        av_frame_free(frame);
        return 0;
    }

    return 1;
}

static int encode_write_frame(decoder_t *p_dec,AVFrame *filt_frame,block_t *p_block) {
    int ret = 1;
    int got_frame_local;
    AVPacket enc_pkt;
    decoder_sys_t *p_sys = p_dec->p_sys;
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);

    if (filt_frame) {
        filt_frame->pts = p_sys->pts_audio;
        p_sys->pts_audio += filt_frame->nb_samples;
    } 
   

    ret = avcodec_send_frame(p_sys->ac3_CodecCtx, filt_frame);
    if (ret < 0) {
        msg_Dbg(p_dec, "tdx  Error submitting the frame to the encoder, %s\n", av_err2str(ret));
        return ret;
    }

    while (1) {
        ret = avcodec_receive_packet(p_sys->ac3_CodecCtx, &enc_pkt);    
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          //  msg_Dbg(p_dec,  "tdx Error of EAGAIN or EOF\n");
            return 0;
        } else if (ret < 0){
            msg_Dbg(p_dec,  "tdx  Error during encoding, %s\n", av_err2str(ret));
            return ret; 
        } else {
            /* prepare packet for muxing */
            av_packet_rescale_ts(&enc_pkt, 
                p_sys->p_context->time_base, 
                p_sys->ac3_CodecCtx->time_base);
            block_t     *p_frame;
            p_frame = block_Alloc(enc_pkt.size);
            memcpy( p_frame->p_buffer, enc_pkt.data, enc_pkt.size);
            p_frame->i_pts = date_Get( &p_sys->end_date);
            p_frame->i_length = date_Increment( &p_sys->end_date,
                            p_sys->ac3_CodecCtx->frame_size) - p_frame->i_pts;
            p_frame->i_nb_samples =  p_sys->ac3_CodecCtx->frame_size;

            decoder_QueueAudio( p_dec, p_frame);
            av_packet_unref(&enc_pkt);
        }
    }
    return ret;
}

static int  process_ac3_encod(decoder_t *p_dec,AVFrame  *inputframe,block_t *p_block)
{
   decoder_sys_t *p_sys = p_dec->p_sys;
   int got_output;
   int ret;
  
   int  input_frame_size=0;
  
   if(!p_sys->ac3_CodecCtx)
        return 0;

    if(p_sys->swr_ctx == NULL && (p_sys->ac3_CodecCtx->sample_fmt != inputframe->format
            || p_sys->ac3_CodecCtx->channels != inputframe->channels
            || p_sys->ac3_CodecCtx->channel_layout != inputframe->channel_layout
            || p_sys->ac3_CodecCtx->sample_rate != inputframe->sample_rate)){
    //    msg_Dbg(p_dec,"tdx  dts layout  %lld  chanles  %d  samplerate  %d  format  %d  ",inputframe->channel_layout,inputframe->channels,inputframe->sample_rate,inputframe->format);
        p_sys->swr_ctx  = swr_alloc();
        p_sys->swr_ctx = swr_alloc_set_opts(
            p_sys->swr_ctx,
            p_sys->ac3_CodecCtx->channel_layout,
            p_sys->ac3_CodecCtx->sample_fmt,
            p_sys->ac3_CodecCtx->sample_rate,
           // av_get_default_channel_layout(p_sys->p_context->channels),
            inputframe->channel_layout,
            inputframe->format,
            inputframe->sample_rate,0,NULL);
        swr_init(p_sys->swr_ctx);
    }

   AVFrame *frame  = NULL;
   if(p_sys->swr_ctx != NULL){
        frame = av_frame_alloc();
        frame->channel_layout = p_sys->ac3_CodecCtx->channel_layout;
        frame->sample_rate = p_sys->ac3_CodecCtx->sample_rate;
        frame->format = p_sys->ac3_CodecCtx->sample_fmt;
        frame->channels = p_sys->ac3_CodecCtx->channels;
        ret = swr_convert_frame(p_sys->swr_ctx, frame, inputframe); 
        if(ret < 0){
            av_frame_free(&frame);
            msg_Dbg(p_dec,"   tdx  swr_convert_frame  failed ");
            return 0;
        }
   }else{
        frame = inputframe;
   }
   add_samples_to_fifo(p_dec,p_sys->fifo, frame->data, frame->nb_samples);
   if(frame != inputframe){
      av_frame_free(&frame);
   }
   
   int audio_fifo_size = av_audio_fifo_size(p_sys->fifo);

   if (audio_fifo_size < p_sys->ac3_CodecCtx->frame_size ) {
        return 0;
   }

    while (av_audio_fifo_size(p_sys->fifo) >= p_sys->ac3_CodecCtx->frame_size) {
        const int frame_size = p_sys->ac3_CodecCtx->frame_size;

        AVFrame *output_frame;
            
        if (init_output_frame(&output_frame, p_sys->ac3_CodecCtx, frame_size) < 0) {
            msg_Dbg(p_dec, "tdx  init_output_frame failed\n");
            return 0;
        }

        /* Read as many samples from the FIFO buffer as required to fill the frame.
          * The samples are stored in the frame temporarily. */
        if (av_audio_fifo_read(p_sys->fifo, (void **)output_frame->data, frame_size) < frame_size) {
            msg_Dbg(p_dec, "tdx  Could not read data from FIFO\n");
            av_frame_free(&output_frame);
            return 0;
        }

        ret = encode_write_frame(p_dec,output_frame,p_block);
        av_frame_free(&output_frame);
    } 
   return 1;
}
#endif

static int
DecodeBlock(decoder_t *p_dec, block_t *p_block)
{
#if defined(__ANDROID__) || defined(__linux__)
    if(p_dec->p_sys->b_AC3_passthrough == true){
        if(p_dec->fmt_in.i_codec == VLC_CODEC_A52){
            if(p_block != NULL)
                decoder_QueueAudio( p_dec, p_block);
        }else{
            if(p_block != NULL)
                process_decode_audio( p_dec, p_block);
        }
    }else
#endif
    {
        if(p_block != NULL){
            decoder_QueueAudio( p_dec, p_block);
        }
    }


#if defined( SPDIF_TDX )
    if(p_dec->fmt_in.i_codec == VLC_CODEC_A52)
    {
     //   msg_Dbg(p_dec,"     p_block  %lld  ",p_block->i_pts);
        if (p_block != NULL)
           decoder_QueueAudio( p_dec, p_block );
    }else
    {
        if(p_block != NULL)
            process_decode_audio(p_dec, p_block);
    }
    if (p_block != NULL)
           decoder_QueueAudio( p_dec, p_block );
#endif
}

#if defined(__ANDROID__) || defined(__linux__)
static void init_decoder_config(decoder_t *p_dec, AVCodecContext *p_context)
{
    if( p_dec->fmt_in.i_extra > 0 )
    {
        const uint8_t * const p_src = p_dec->fmt_in.p_extra;

        int i_offset = 0;
        int i_size = p_dec->fmt_in.i_extra;

        if( p_dec->fmt_in.i_codec == VLC_CODEC_ALAC )
        {
            static const uint8_t p_pattern[] = { 0, 0, 0, 36, 'a', 'l', 'a', 'c' };
            /* Find alac atom XXX it is a bit ugly */
            for( i_offset = 0; i_offset < i_size - (int)sizeof(p_pattern); i_offset++ )
            {
                if( !memcmp( &p_src[i_offset], p_pattern, sizeof(p_pattern) ) )
                    break;
            }
            i_size = __MIN( p_dec->fmt_in.i_extra - i_offset, 36 );
            if( i_size < 36 )
                i_size = 0;
        }

        if( i_size > 0 )
        {
            p_context->extradata =
                av_malloc( i_size + AV_INPUT_BUFFER_PADDING_SIZE );
            if( p_context->extradata )
            {
                uint8_t *p_dst = p_context->extradata;

                p_context->extradata_size = i_size;

                memcpy( &p_dst[0],            &p_src[i_offset], i_size );
                memset( &p_dst[i_size], 0, AV_INPUT_BUFFER_PADDING_SIZE );
            }
        }
    }
    else
    {
        p_context->extradata_size = 0;
        p_context->extradata = NULL;
    }
}

/*
 *  Audio Codecs
 */
static const struct spdif_avcodec_fourcc audio_codecs[] =
{
    /* PCM */
    { VLC_CODEC_S16L, AV_CODEC_ID_PCM_S16LE },
    { VLC_CODEC_S16B, AV_CODEC_ID_PCM_S16BE },
    { VLC_CODEC_U16L, AV_CODEC_ID_PCM_U16LE },
    { VLC_CODEC_U16B, AV_CODEC_ID_PCM_U16BE },
    { VLC_CODEC_S8, AV_CODEC_ID_PCM_S8 },
    { VLC_CODEC_U8, AV_CODEC_ID_PCM_U8 },
    { VLC_CODEC_MULAW, AV_CODEC_ID_PCM_MULAW },
    { VLC_CODEC_ALAW, AV_CODEC_ID_PCM_ALAW },
    { VLC_CODEC_S32L, AV_CODEC_ID_PCM_S32LE },
    { VLC_CODEC_S32B, AV_CODEC_ID_PCM_S32BE },
    { VLC_CODEC_U32L, AV_CODEC_ID_PCM_U32LE },
    { VLC_CODEC_U32B, AV_CODEC_ID_PCM_U32BE },
    { VLC_CODEC_S24L, AV_CODEC_ID_PCM_S24LE },
    { VLC_CODEC_S24B, AV_CODEC_ID_PCM_S24BE },
    { VLC_CODEC_U24L, AV_CODEC_ID_PCM_U24LE },
    { VLC_CODEC_U24B, AV_CODEC_ID_PCM_U24BE },
    { VLC_CODEC_S24DAUD, AV_CODEC_ID_PCM_S24DAUD },
    /* AV_CODEC_ID_PCM_ZORK */
    { VLC_CODEC_S16L_PLANAR, AV_CODEC_ID_PCM_S16LE_PLANAR },
    /* AV_CODEC_ID_PCM_DVD */
    { VLC_CODEC_F32B, AV_CODEC_ID_PCM_F32BE },
    { VLC_CODEC_F32L, AV_CODEC_ID_PCM_F32LE },
    { VLC_CODEC_F64B, AV_CODEC_ID_PCM_F64BE },
    { VLC_CODEC_F64L, AV_CODEC_ID_PCM_F64LE },
    { VLC_CODEC_BD_LPCM, AV_CODEC_ID_PCM_BLURAY },
    /* AV_CODEC_ID_PCM_LXF */
    /* AV_CODEC_ID_S302M */
    { VLC_CODEC_PCM_S8_PLANAR, AV_CODEC_ID_PCM_S8_PLANAR},
    { VLC_CODEC_PCM_S24LE_PLANAR, AV_CODEC_ID_PCM_S24LE_PLANAR},
    { VLC_CODEC_PCM_S32LE_PLANAR, AV_CODEC_ID_PCM_S32LE_PLANAR},
    { VLC_CODEC_PCM_S16BE_PLANAR, AV_CODEC_ID_PCM_S16BE_PLANAR},
    /* AV_CODEC_ID_PCM_S24LE_PLANAR */
    /* AV_CODEC_ID_PCM_S32LE_PLANAR */
    /* ffmpeg only: AV_CODEC_ID_PCM_S16BE_PLANAR */

    /* ADPCM */
    { VLC_CODEC_ADPCM_IMA_QT, AV_CODEC_ID_ADPCM_IMA_QT },
    { VLC_CODEC_ADPCM_IMA_WAV, AV_CODEC_ID_ADPCM_IMA_WAV },
    /* AV_CODEC_ID_ADPCM_IMA_DK3 */
    /* AV_CODEC_ID_ADPCM_IMA_DK4 */
    { VLC_CODEC_ADPCM_IMA_WS, AV_CODEC_ID_ADPCM_IMA_WS },
    /* AV_CODEC_ID_ADPCM_IMA_SMJPEG */
    { VLC_CODEC_ADPCM_MS, AV_CODEC_ID_ADPCM_MS },
    { VLC_CODEC_ADPCM_4XM, AV_CODEC_ID_ADPCM_4XM },
    { VLC_CODEC_ADPCM_XA, AV_CODEC_ID_ADPCM_XA },
    { VLC_CODEC_ADPCM_ADX, AV_CODEC_ID_ADPCM_ADX },
    { VLC_CODEC_ADPCM_EA, AV_CODEC_ID_ADPCM_EA },
    { VLC_CODEC_ADPCM_G726, AV_CODEC_ID_ADPCM_G726 },
    { VLC_CODEC_ADPCM_CREATIVE, AV_CODEC_ID_ADPCM_CT },
    { VLC_CODEC_ADPCM_SWF, AV_CODEC_ID_ADPCM_SWF },
    { VLC_CODEC_ADPCM_YAMAHA, AV_CODEC_ID_ADPCM_YAMAHA },
    { VLC_CODEC_ADPCM_SBPRO_4, AV_CODEC_ID_ADPCM_SBPRO_4 },
    { VLC_CODEC_ADPCM_SBPRO_3, AV_CODEC_ID_ADPCM_SBPRO_3 },
    { VLC_CODEC_ADPCM_SBPRO_2, AV_CODEC_ID_ADPCM_SBPRO_2 },
    { VLC_CODEC_ADPCM_THP, AV_CODEC_ID_ADPCM_THP },
    { VLC_CODEC_ADPCM_IMA_AMV, AV_CODEC_ID_ADPCM_IMA_AMV },
    { VLC_CODEC_ADPCM_EA_R1, AV_CODEC_ID_ADPCM_EA_R1 },
    /* AV_CODEC_ID_ADPCM_EA_R3 */
    /* AV_CODEC_ID_ADPCM_EA_R2 */
    { VLC_CODEC_ADPCM_IMA_EA_SEAD, AV_CODEC_ID_ADPCM_IMA_EA_SEAD },
    /* AV_CODEC_ID_ADPCM_IMA_EA_EACS */
    /* AV_CODEC_ID_ADPCM_EA_XAS */
    /* AV_CODEC_ID_ADPCM_EA_MAXIS_XA */
    /* AV_CODEC_ID_ADPCM_IMA_ISS */
    { VLC_CODEC_ADPCM_G722, AV_CODEC_ID_ADPCM_G722 },
    { VLC_CODEC_ADPCM_IMA_APC, AV_CODEC_ID_ADPCM_IMA_APC },
    /* ffmpeg only: AV_CODEC_ID_VIMA */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_AFC */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_IMA_OKI */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_DTK */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_IMA_RAD */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_G726LE */

    /* AMR */
    { VLC_CODEC_AMR_NB, AV_CODEC_ID_AMR_NB },
    { VLC_CODEC_AMR_WB, AV_CODEC_ID_AMR_WB },

    /* RealAudio */
    { VLC_CODEC_RA_144, AV_CODEC_ID_RA_144 },
    { VLC_CODEC_RA_288, AV_CODEC_ID_RA_288 },

    /* DPCM */
    { VLC_CODEC_ROQ_DPCM, AV_CODEC_ID_ROQ_DPCM },
    { VLC_CODEC_INTERPLAY_DPCM, AV_CODEC_ID_INTERPLAY_DPCM },
    /* AV_CODEC_ID_XAN_DPCM */
    /* AV_CODEC_ID_SOL_DPCM */

    /* audio codecs */
    { VLC_CODEC_MPGA, AV_CODEC_ID_MP2 },
    { VLC_CODEC_MP2, AV_CODEC_ID_MP2 },
    { VLC_CODEC_MP3, AV_CODEC_ID_MP3 },
    { VLC_CODEC_MP4A, AV_CODEC_ID_AAC },
    { VLC_CODEC_A52, AV_CODEC_ID_AC3 },
    { VLC_CODEC_DTS, AV_CODEC_ID_DTS },
    { VLC_CODEC_VORBIS, AV_CODEC_ID_VORBIS },
    { VLC_CODEC_DVAUDIO, AV_CODEC_ID_DVAUDIO },
    { VLC_CODEC_WMA1, AV_CODEC_ID_WMAV1 },
    { VLC_CODEC_WMA2, AV_CODEC_ID_WMAV2 },
    { VLC_CODEC_MACE3, AV_CODEC_ID_MACE3 },
    { VLC_CODEC_MACE6, AV_CODEC_ID_MACE6 },
    { VLC_CODEC_VMDAUDIO, AV_CODEC_ID_VMDAUDIO },
    { VLC_CODEC_FLAC, AV_CODEC_ID_FLAC },
    /* AV_CODEC_ID_MP3ADU */
    /* AV_CODEC_ID_MP3ON4 */
    { VLC_CODEC_SHORTEN, AV_CODEC_ID_SHORTEN },
    { VLC_CODEC_ALAC, AV_CODEC_ID_ALAC },
    /* AV_CODEC_ID_WESTWOOD_SND1 */
    { VLC_CODEC_GSM, AV_CODEC_ID_GSM },
    { VLC_CODEC_QDM2, AV_CODEC_ID_QDM2 },
    { VLC_CODEC_COOK, AV_CODEC_ID_COOK },
    { VLC_CODEC_TRUESPEECH, AV_CODEC_ID_TRUESPEECH },
    { VLC_CODEC_TTA, AV_CODEC_ID_TTA },
    { VLC_CODEC_SMACKAUDIO, AV_CODEC_ID_SMACKAUDIO },
    { VLC_CODEC_QCELP, AV_CODEC_ID_QCELP },
    { VLC_CODEC_WAVPACK, AV_CODEC_ID_WAVPACK },
    { VLC_CODEC_DSICINAUDIO, AV_CODEC_ID_DSICINAUDIO },
    { VLC_CODEC_IMC, AV_CODEC_ID_IMC },
    { VLC_CODEC_MUSEPACK7, AV_CODEC_ID_MUSEPACK7 },
    { VLC_CODEC_MLP, AV_CODEC_ID_MLP },
    { VLC_CODEC_GSM_MS, AV_CODEC_ID_GSM_MS },
    { VLC_CODEC_ATRAC3, AV_CODEC_ID_ATRAC3 },
    { VLC_CODEC_APE, AV_CODEC_ID_APE },
    { VLC_CODEC_NELLYMOSER, AV_CODEC_ID_NELLYMOSER },
    { VLC_CODEC_MUSEPACK8, AV_CODEC_ID_MUSEPACK8 },
    { VLC_CODEC_SPEEX, AV_CODEC_ID_SPEEX },
    { VLC_CODEC_WMAS, AV_CODEC_ID_WMAVOICE },
    { VLC_CODEC_WMAP, AV_CODEC_ID_WMAPRO },
    { VLC_CODEC_WMAL, AV_CODEC_ID_WMALOSSLESS },
    { VLC_CODEC_ATRAC3P, AV_CODEC_ID_ATRAC3P },
    { VLC_CODEC_EAC3, AV_CODEC_ID_EAC3 },
    { VLC_CODEC_SIPR, AV_CODEC_ID_SIPR },
    /* AV_CODEC_ID_MP1 */
    { VLC_CODEC_TWINVQ, AV_CODEC_ID_TWINVQ },
    { VLC_CODEC_TRUEHD, AV_CODEC_ID_TRUEHD },
    { VLC_CODEC_ALS, AV_CODEC_ID_MP4ALS },
    { VLC_CODEC_ATRAC1, AV_CODEC_ID_ATRAC1 },
    { VLC_CODEC_BINKAUDIO_RDFT, AV_CODEC_ID_BINKAUDIO_RDFT },
    { VLC_CODEC_BINKAUDIO_DCT, AV_CODEC_ID_BINKAUDIO_DCT },
    { VLC_CODEC_MP4A, AV_CODEC_ID_AAC_LATM },
    /* AV_CODEC_ID_QDMC */
    /* AV_CODEC_ID_CELT */
    { VLC_CODEC_G723_1, AV_CODEC_ID_G723_1 },
    /* AV_CODEC_ID_G729 */
    /* AV_CODEC_ID_8SVX_EXP */
    /* AV_CODEC_ID_8SVX_FIB */
    { VLC_CODEC_BMVAUDIO, AV_CODEC_ID_BMV_AUDIO },
    { VLC_CODEC_RALF, AV_CODEC_ID_RALF },
    { VLC_CODEC_INDEO_AUDIO, AV_CODEC_ID_IAC },
    /* AV_CODEC_ID_ILBC */
    { VLC_CODEC_OPUS, AV_CODEC_ID_OPUS },
    /* AV_CODEC_ID_COMFORT_NOISE */
    { VLC_CODEC_TAK, AV_CODEC_ID_TAK },
    { VLC_CODEC_METASOUND, AV_CODEC_ID_METASOUND },
    /* AV_CODEC_ID_PAF_AUDIO */
    { VLC_CODEC_ON2AVC, AV_CODEC_ID_ON2AVC },

    /* ffmpeg only: AV_CODEC_ID_FFWAVESYNTH */
    /* ffmpeg only: AV_CODEC_ID_SONIC */
    /* ffmpeg only: AV_CODEC_ID_SONIC_LS */
    /* ffmpeg only: AV_CODEC_ID_PAF_AUDIO */
    /* ffmpeg only: AV_CODEC_ID_EVRC */
    /* ffmpeg only: AV_CODEC_ID_SMV */

    { VLC_CODEC_DSD_LSBF, AV_CODEC_ID_DSD_LSBF },
    { VLC_CODEC_DSD_MSBF, AV_CODEC_ID_DSD_MSBF },
    { VLC_CODEC_DSD_LSBF_PLANAR, AV_CODEC_ID_DSD_LSBF_PLANAR },
    { VLC_CODEC_DSD_MSBF_PLANAR, AV_CODEC_ID_DSD_MSBF_PLANAR },
};

bool spdifGetFfmpegCodec( enum es_format_category_e cat, vlc_fourcc_t i_fourcc,
                     unsigned *pi_ffmpeg_codec, const char **ppsz_name )
{
    const struct spdif_avcodec_fourcc *base;
    size_t count;

    switch( cat )
    {
        case AUDIO_ES:
            base = audio_codecs;
            count = ARRAY_SIZE(audio_codecs);
            break;
        default:
            base = NULL;
            count = 0;
    }

    i_fourcc = vlc_fourcc_GetCodec( cat, i_fourcc );

    for( size_t i = 0; i < count; i++ )
    {
        if( base[i].i_fourcc == i_fourcc )
        {
            if( pi_ffmpeg_codec != NULL )
                *pi_ffmpeg_codec = base[i].i_codec;
            if( ppsz_name )
                *ppsz_name = vlc_fourcc_GetDescription( cat, i_fourcc );
            return true;
        }
    }
    return false;
}

AVCodecContext *spdif_ffmpeg_AllocContext( decoder_t *p_dec,
                                     const AVCodec **restrict codecp )
{
    unsigned i_codec_id;
    const char *psz_namecodec;
    const AVCodec *p_codec = NULL;

    /* *** determine codec type *** */
    if( !spdifGetFfmpegCodec( p_dec->fmt_in.i_cat, p_dec->fmt_in.i_codec,
                         &i_codec_id, &psz_namecodec ) )
         return NULL;

    /* *** ask ffmpeg for a decoder *** */
    char *psz_decoder = var_InheritString( p_dec, "avcodec-codec" );
    if( psz_decoder != NULL )
    {
        p_codec = avcodec_find_decoder_by_name( psz_decoder );
        if( !p_codec )
            msg_Err( p_dec, "Decoder `%s' not found", psz_decoder );
        else if( p_codec->id != i_codec_id )
        {
            msg_Err( p_dec, "Decoder `%s' can't handle %4.4s",
                    psz_decoder, (char*)&p_dec->fmt_in.i_codec );
            p_codec = NULL;
        }
        free( psz_decoder );
    }
    if( !p_codec )
        p_codec = avcodec_find_decoder( i_codec_id );
    if( !p_codec )
    {
        msg_Dbg( p_dec, " codec not found (%s)", psz_namecodec );
        return NULL;
    }

    *codecp = p_codec;

    /* *** get a p_context *** */
    AVCodecContext *avctx = avcodec_alloc_context3(p_codec);
    if( unlikely(avctx == NULL) )
        return NULL;

    avctx->debug = var_InheritInteger( p_dec, "avcodec-debug" );
    avctx->opaque = p_dec;
    return avctx;
}


static int  init_decoder(decoder_t *p_dec)
{
    const AVCodec *codec;
    decoder_sys_t *p_sys = p_dec->p_sys;
    p_sys->ac3_CodecCtx = NULL;
    p_sys->p_context = NULL;
    p_sys->swr_ctx = NULL;
    p_sys->fifo = NULL;
    avcodec_register_all();
    AVCodecContext *avctx = spdif_ffmpeg_AllocContext( p_dec, &codec );
    if( avctx == NULL ){
        msg_Dbg(p_dec," tdx   alloce  context  failed   ");
        return false;
    }

    /* Allocate the memory needed to store the decoder's structure */
    p_sys->p_context = avctx;

     // Initialize decoder extradata
    init_decoder_config(p_dec, avctx);
    /* ***** Open the codec ***** */
    if(open_audio_codec( p_dec ) < 0 )
    {
        msg_Dbg(p_dec,"tdx   Open Audio  Codec   failed    ");
        return false;
    }
    msg_Dbg(p_dec,"tdx    OpenAudioCodec  success ");
    return true;
}

static int spdif_ffmpeg_OpenCodec( decoder_t *p_dec, AVCodecContext *ctx,
                      const AVCodec *codec )
{
    int ret;
    vlc_avcodec_lock();
    ret = avcodec_open2( ctx, codec,  NULL );
    vlc_avcodec_unlock();
    if( ret < 0 )
    {
        msg_Err( p_dec, "cannot start codec (%s)", codec->name );
        return VLC_EGENERIC;
    }

    msg_Dbg( p_dec, "codec (%s) started", codec->name );
    return VLC_SUCCESS;
}

static int open_audio_codec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
    const AVCodec *codec = ctx->codec;

    if( ctx->extradata_size <= 0 )
    {
        if( codec->id == AV_CODEC_ID_VORBIS ||
            ( codec->id == AV_CODEC_ID_AAC &&
              !p_dec->fmt_in.b_packetized ) )
        {
            msg_Warn( p_dec, "waiting for extra data for codec %s",
                      codec->name );
            return 1;
        }
    }

    ctx->sample_rate = p_dec->fmt_in.audio.i_rate;
    ctx->channels = p_dec->fmt_in.audio.i_channels;
    ctx->block_align = p_dec->fmt_in.audio.i_blockalign;
    ctx->bit_rate = p_dec->fmt_in.i_bitrate;
    ctx->bits_per_coded_sample = p_dec->fmt_in.audio.i_bitspersample;

    if( codec->id == AV_CODEC_ID_ADPCM_G726 &&
        ctx->bit_rate > 0 &&
        ctx->sample_rate >  0)
        ctx->bits_per_coded_sample = ctx->bit_rate / ctx->sample_rate;

    return spdif_ffmpeg_OpenCodec( p_dec, ctx, codec );
}

static int
process_decode_audio( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
    AVFrame *frame = NULL;
    
    bool b_error = false;
    int ret;

    if( p_block->i_flags & BLOCK_FLAG_CORRUPTED )
    {
        Flush( p_dec );
        goto drop;
    }

    if( p_block->i_flags & BLOCK_FLAG_DISCONTINUITY )
    {
        date_Set( &p_sys->end_date, VLC_TS_INVALID );
    }

    /* We've just started the stream, wait for the first PTS. */
    if( !date_Get( &p_sys->end_date ) && p_block->i_pts <= VLC_TS_INVALID )
        goto drop;

    if( p_block->i_buffer <= 0 )
        goto drop;

    if( (p_block->i_flags & BLOCK_FLAG_PRIVATE_REALLOCATED) == 0 )
    {
        p_block = block_Realloc( p_block, 0, p_block->i_buffer + AV_INPUT_BUFFER_PADDING_SIZE );
        if( !p_block )
            goto end;
        p_block->i_buffer -= AV_INPUT_BUFFER_PADDING_SIZE;
        memset( &p_block->p_buffer[p_block->i_buffer], 0, AV_INPUT_BUFFER_PADDING_SIZE );

        p_block->i_flags |= BLOCK_FLAG_PRIVATE_REALLOCATED;
    }




    frame = av_frame_alloc();
    if (unlikely(frame == NULL))
        goto end;

    /* Feed in the loop as buffer could have been full on first iterations */
    AVPacket *pkt ;
    pkt = av_packet_alloc();
    if (unlikely(frame == NULL))
        goto end;
    pkt->data = p_block->p_buffer;
    pkt->size = p_block->i_buffer;
    ret = avcodec_send_packet( ctx, pkt );
    
    if( ret >= 0 ) /* Block has been consumed */
    {
        if( date_Get( &p_sys->end_date ) == 0)
        {
            date_Set( &p_sys->end_date, p_block->i_pts );
        }
    }
    else if ( ret != AVERROR(EAGAIN) ) /* Errors other than buffer full */
    {
        msg_Dbg(p_dec,"tdx    avcodec_send_packet error  ret    %d",ret);
        if( ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL) )
            goto end;
        else
            goto drop;
    }

    /* Try to read one or multiple frames */
    ret = avcodec_receive_frame( ctx, frame );
    if( ret >= 0 )
    {       
     //  msg_Dbg(p_dec,"tdx  avcodec_receive_frame  ret   ret  %d",ret);
        process_ac3_encod(p_dec,frame,p_block); 
    }
    else
    {
        msg_Dbg(p_dec,"tdx  avcodec_receive_frame  error   ret  %d",ret);
        /* After draining, we need to reset decoder with a flush */
        if( ret == AVERROR_EOF )
            avcodec_flush_buffers( p_sys->p_context );
       // av_frame_free( frame );
    }
    if(pkt != NULL){
        pkt->data = NULL;
        pkt->size = 0; 
        av_packet_free(&pkt);
    }
    if( p_block != NULL )
        block_Release(p_block);
    if( frame != NULL ){
        av_frame_free(&frame );
    }
    return VLCDEC_SUCCESS;

end:
    b_error = true;
drop:
    if(pkt != NULL){
        pkt->data = NULL;
        pkt->size = 0; 
        av_packet_free(&pkt);
    }
    if( p_block != NULL )
        block_Release(p_block);
    if( frame != NULL )
        av_frame_free( frame );
    return VLC_EGENERIC;
}

static int open_ac3_encoder(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodec *codec = NULL;
    int  ret ;
    codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
    /* check we got the codec */
    if (!codec)
        return false;
    p_sys->ac3_CodecCtx = avcodec_alloc_context3(codec);
    if (!p_sys->ac3_CodecCtx)
        return false;
    if(!p_sys->p_context)
         return false;
    p_sys->ac3_CodecCtx->bit_rate =  640000;
    p_sys->ac3_CodecCtx->sample_fmt =  AV_SAMPLE_FMT_FLTP;
    p_sys->ac3_CodecCtx->sample_rate = 48000;
    p_sys->ac3_CodecCtx->channel_layout =   AV_CH_LAYOUT_5POINT1_BACK;
    p_sys->ac3_CodecCtx->channels = av_get_channel_layout_nb_channels(p_sys->ac3_CodecCtx->channel_layout);

    date_Set( &p_sys->end_date, VLC_TS_INVALID );
    date_Init( &p_sys->end_date, p_sys->ac3_CodecCtx->sample_rate, 1 );

  /* open the codec */
  ret = avcodec_open2( p_sys->ac3_CodecCtx, codec,  NULL );
  if (ret < 0)
  {
    msg_Dbg(p_dec,"tdx      avcodec_open2   failed   %d ",ret);
    avcodec_free_context(&p_sys->ac3_CodecCtx);
    return false;
  }
  p_sys->fifo = av_audio_fifo_alloc(p_sys->ac3_CodecCtx->sample_fmt, p_sys->ac3_CodecCtx->channels, p_sys->ac3_CodecCtx->frame_size);
  p_sys->pts_audio = 0;
  return true;
}

#endif
/*****************************************************************************
 * Flush:
 *****************************************************************************/
static void Flush( decoder_t *p_dec )
{
#if defined(__ANDROID__) || defined(__linux__)
    if(p_dec->p_sys->b_AC3_passthrough == true){
        if(p_dec->fmt_in.i_codec != VLC_CODEC_A52){
            decoder_sys_t *p_sys = p_dec->p_sys;
            date_Set( &p_sys->end_date, VLC_TS_INVALID );
            AVCodecContext *ctx = p_sys->p_context;
            AVCodecContext *ac3ctx = p_sys->ac3_CodecCtx;
            if( avcodec_is_open( ctx ) )
                avcodec_flush_buffers( ctx );
            if (avcodec_is_open(ac3ctx))
                avcodec_flush_buffers(ac3ctx);
            if(p_sys->fifo != NULL){
                int frame_size;
                while(frame_size = av_audio_fifo_size(p_sys->fifo) > 0){
                    AVFrame *output_frame;
                    init_output_frame(&output_frame, p_sys->ac3_CodecCtx, frame_size);
                    av_audio_fifo_read(p_sys->fifo, (void **)output_frame->data, frame_size);
                    av_frame_free(&output_frame);
                }
            }
            if(p_sys->swr_ctx != NULL){
                int  fifo_size = swr_get_out_samples(p_sys->swr_ctx,0);
                if(fifo_size > 0){
                    AVFrame *frame  = NULL;
                    frame = av_frame_alloc();
                    frame->channel_layout = p_sys->ac3_CodecCtx->channel_layout;
                    frame->sample_rate = p_sys->ac3_CodecCtx->sample_rate;
                    frame->format = p_sys->ac3_CodecCtx->sample_fmt;
                    frame->channels = p_sys->ac3_CodecCtx->channels;
                    swr_convert_frame(p_sys->swr_ctx, frame, NULL);
                    av_frame_free(&frame);
                }
            }
        }
    }
#endif
#if defined( SPDIF_TDX )
    if(p_dec->fmt_in.i_codec != VLC_CODEC_A52)
    {
        decoder_sys_t *p_sys = p_dec->p_sys;
        AVCodecContext *ctx = p_sys->p_context;
        AVCodecContext *ac3ctx = p_sys->ac3_CodecCtx;
        if( avcodec_is_open( ctx ) )
            avcodec_flush_buffers( ctx );
        if (avcodec_is_open(ac3ctx))
            avcodec_flush_buffers(ac3ctx);
        if(p_sys->fifo != NULL){
            int frame_size;
            while(frame_size = av_audio_fifo_size(p_sys->fifo) > 0){
                AVFrame *output_frame;
                init_output_frame(&output_frame, p_sys->ac3_CodecCtx, frame_size);
                av_audio_fifo_read(p_sys->fifo, (void **)output_frame->data, frame_size);
                av_frame_free(&output_frame);
            }
        }
        if(p_sys->swr_ctx != NULL){
            int  fifo_size = swr_get_out_samples(p_sys->swr_ctx,0);
            if(fifo_size > 0){
                AVFrame *frame  = NULL;
                frame = av_frame_alloc();
                frame->channel_layout = p_sys->ac3_CodecCtx->channel_layout;
                frame->sample_rate = p_sys->ac3_CodecCtx->sample_rate;
                frame->format = p_sys->ac3_CodecCtx->sample_fmt;
                frame->channels = p_sys->ac3_CodecCtx->channels;
                swr_convert_frame(p_sys->swr_ctx, frame, NULL);
                av_frame_free(&frame);
            }
        }
    }
#endif

}

static void 
EndTranscode(vlc_object_t *p_this)
{
#if defined(__ANDROID__) || defined(__linux__)
    decoder_t *p_dec = (decoder_t*)p_this;
    if(p_dec->p_sys->b_AC3_passthrough == true){
        if(p_dec->fmt_in.i_codec != VLC_CODEC_A52){
            if(p_dec->p_sys != NULL){
                decoder_sys_t *p_sys = p_dec->p_sys;
                if(p_sys->p_context != NULL){
                    avcodec_close(p_sys->p_context);
                    avcodec_free_context(&(p_sys->p_context));
                }
                if(p_sys->ac3_CodecCtx != NULL){
                    avcodec_close(p_sys->ac3_CodecCtx);
                    avcodec_free_context(&(p_sys->ac3_CodecCtx));
                }
                if(p_sys->swr_ctx != NULL){
                    swr_free(&(p_sys->swr_ctx));
                }
                if(p_sys->fifo != NULL)
                    av_audio_fifo_free(p_sys->fifo);
                free( p_sys );
            }
        }
    }
#endif

#if defined( SPDIF_TDX )

    if(p_dec->fmt_in.i_codec != VLC_CODEC_A52)
    {
        if(p_dec->p_sys != NULL){
            decoder_sys_t *p_sys = p_dec->p_sys;
            if(p_sys->p_context != NULL){
                avcodec_close(p_sys->p_context);
                avcodec_free_context(&(p_sys->p_context));
            }
            if(p_sys->ac3_CodecCtx != NULL){
                avcodec_close(p_sys->ac3_CodecCtx);
                avcodec_free_context(&(p_sys->ac3_CodecCtx));
                
            }
            if(p_sys->swr_ctx != NULL){
                swr_free(&(p_sys->swr_ctx));
            }
            if(p_sys->fifo != NULL)
                av_audio_fifo_free(p_sys->fifo);
            free( p_sys );
        }
    }
#endif
}

static int
OpenDecoder(vlc_object_t *p_this)
{
    decoder_t *p_dec = (decoder_t*)p_this;
    switch (p_dec->fmt_in.i_codec)
    {
    case VLC_CODEC_MPGA:
    case VLC_CODEC_MP3:
        /* Disabled by default */
        if (!p_dec->obj.force)
            return VLC_EGENERIC;
        break;
    case VLC_CODEC_A52:
    case VLC_CODEC_EAC3:
    case VLC_CODEC_MLP:
    case VLC_CODEC_TRUEHD:
    case VLC_CODEC_DTS:
    case VLC_CODEC_SPDIFL:
    case VLC_CODEC_SPDIFB:
        /* Enabled by default */
        break;
    default:
        return VLC_EGENERIC;
    }

    /* Set output properties */
    p_dec->fmt_out.i_codec = p_dec->fmt_in.i_codec;
    p_dec->fmt_out.audio = p_dec->fmt_in.audio;
    p_dec->fmt_out.i_profile = p_dec->fmt_in.i_profile;
    p_dec->fmt_out.audio.i_format = p_dec->fmt_out.i_codec;
    p_dec->p_sys  = malloc(sizeof(struct decoder_sys_t));
    if( unlikely(p_dec->p_sys == NULL) )
    {
        return VLC_EGENERIC;
    }
#if defined(__ANDROID__) || defined(__linux__)
    p_dec->p_sys->b_AC3_passthrough = var_InheritBool(p_dec, "spdif-ac3");
    if(p_dec->p_sys->b_AC3_passthrough == true){
        if(p_dec->fmt_in.i_codec != VLC_CODEC_A52){
            p_dec->fmt_out.i_codec = VLC_CODEC_A52;
            p_dec->fmt_out.audio = p_dec->fmt_in.audio;
            p_dec->fmt_out.i_profile = p_dec->fmt_in.i_profile;
            p_dec->fmt_out.audio.i_format = p_dec->fmt_out.i_codec;
            p_dec->fmt_out.audio.i_channels = 6;
            p_dec->fmt_out.audio.i_rate = 48000;
            p_dec->fmt_out.audio.i_frame_length = 1536;
            p_dec->fmt_out.audio.i_bytes_per_frame = 2560;
            if(!init_decoder(p_dec))
                return VLC_EGENERIC;
            if(!open_ac3_encoder(p_dec))
                return VLC_EGENERIC;
        }
    }
#endif

#if defined( SPDIF_TDX )
    if(p_dec->fmt_in.i_codec != VLC_CODEC_A52)
    {
        if(!init_decoder(p_dec))
            return VLC_EGENERIC;
        if(!open_ac3_encoder(p_dec))
            return VLC_EGENERIC;
    }
#endif
    if (decoder_UpdateAudioFormat(p_dec)){
        return VLC_EGENERIC;
    }

    p_dec->pf_decode = DecodeBlock;
    p_dec->pf_flush  = Flush;
    return VLC_SUCCESS;
}
