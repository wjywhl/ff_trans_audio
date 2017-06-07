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

/**
 * @file
 * simple audio converter
 *
 * @example transcode_aac.c
 * Convert an input audio file to AAC in an MP4 container using FFmpeg.
 * @author Andreas Unterweger (dustsigns@gmail.com)
 */

#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "libavformat/avformat.h"
#include "libavformat/avio.h"

#include "libavcodec/avcodec.h"

#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

#include "libswresample/swresample.h"
#ifdef __cplusplus
}
#endif

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "postproc.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "swscale.lib")


/** The output bit rate in kbit/s */
#define OUTPUT_BIT_RATE 128000
/** The number of output channels */
#define OUTPUT_CHANNELS 2

static int g_audio_index = -1;
static int g_enc_frame_size = 0;
const int DEFAULT_FRAME_SIZE = 1024;
static int opkt_size = 0;
static int out_nb_frames = 0;
char *mux_fmt_name = "wav";

/** Open an input file and the required decoder. */
static int open_input_file(const char *filename,
	                       AVFormatContext **input_format_context,
	                       AVCodecContext **input_codec_context)
{
    AVCodecContext *avctx;
    AVCodec *input_codec;
    int error;

    /** Open the input file to read from it. */

	error = avformat_open_input(input_format_context, filename, NULL,NULL);
	if (error < 0)
	{
        fprintf(stderr, "Could not open input file '%s' (error '%s')\n",
                filename, av_err2str(error));
        *input_format_context = NULL;
        return error;
    }

    /** Get information on the input file (number of streams etc.). */
    if ((error = avformat_find_stream_info(*input_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not open find stream info (error '%s')\n",
                av_err2str(error));
        avformat_close_input(input_format_context);
        return error;
    }

    /** Find a decoder for the audio stream. */
	for (unsigned int i = 0; i < (*input_format_context)->nb_streams; i++) {
		if ((*input_format_context)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			g_audio_index = i;
			printf("Find Audio streams[%d]\n", i);
			break;

		}
	}
	if (g_audio_index == -1) {
		printf("No Audio Streams\n");
		avformat_close_input(input_format_context);
		return AVERROR_EXIT;

	}
    if (!(input_codec = avcodec_find_decoder((*input_format_context)->streams[g_audio_index]->codecpar->codec_id))) {
        fprintf(stderr, "Could not find input codec\n");
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }

    /** allocate a new decoding context */
    avctx = avcodec_alloc_context3(input_codec);
    if (!avctx) {
        fprintf(stderr, "Could not allocate a decoding context\n");
        avformat_close_input(input_format_context);
        return AVERROR(ENOMEM);
    }

    /** initialize the stream parameters with demuxer information */
    error = avcodec_parameters_to_context(avctx, (*input_format_context)->streams[g_audio_index]->codecpar);
    if (error < 0) {
        avformat_close_input(input_format_context);
        avcodec_free_context(&avctx);
        return error;
    }

    /** Open the decoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, input_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open input codec (error '%s')\n",
                av_err2str(error));
        avcodec_free_context(&avctx);
        avformat_close_input(input_format_context);
        return error;
    }

    /** Save the decoder context for easier access later. */
    *input_codec_context = avctx;
//    g_in_duration = (*input_format_context)->streams[g_audio_index]->duration;
//    g_in_time_base = (*input_format_context)->streams[g_audio_index]->time_base;

    return 0;
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 */
static int open_output_file(const char *filename,
                            AVCodecContext *input_codec_context,
                            AVFormatContext **output_format_context,
                            AVCodecContext **output_codec_context)
{
    AVCodecContext *avctx          = NULL;
    AVStream *stream               = NULL;
    AVCodec *output_codec          = NULL;
    int error;




	error = avformat_alloc_output_context2(output_format_context, NULL, mux_fmt_name, filename);
	if (error < 0) {
		fprintf(stderr, "Could not create output context-'%s' (error '%s')\n",
			filename, av_err2str(error));
		return error;
	}


    /** Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(*output_format_context, NULL))) {
        fprintf(stderr, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

	/** Find the encoder to be used by its name. */
	stream->codecpar->codec_id = av_guess_codec((*output_format_context)->oformat, NULL, (*output_format_context)->filename, NULL, AVMEDIA_TYPE_AUDIO);
	output_codec = avcodec_find_encoder(stream->codecpar->codec_id);
	if (!output_codec) {
		fprintf(stderr, "Could not find encoder.\n");
		goto cleanup;
	}


    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx) {
        fprintf(stderr, "Could not allocate an encoding context\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /**
     * Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion.
     */

    avctx->channels       = OUTPUT_CHANNELS;
    avctx->channel_layout = av_get_default_channel_layout(OUTPUT_CHANNELS);
    avctx->sample_rate    = 48000;
    avctx->sample_fmt     = output_codec->sample_fmts[0];
    avctx->bit_rate       = OUTPUT_BIT_RATE;	

    /** Allow the use of the experimental AAC encoder */
    avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /**
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, output_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open output codec (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

	g_enc_frame_size = (avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) ?
		DEFAULT_FRAME_SIZE : avctx->frame_size;

    error = avcodec_parameters_from_context(stream->codecpar, avctx);    
    if (error < 0) {
        fprintf(stderr, "Could not initialize stream parameters\n");
        goto cleanup;
    }


    *output_codec_context = avctx;

	if (!((*output_format_context)->oformat->flags &  AVFMT_NOFILE)) {
		error = avio_open(&(*output_format_context)->pb, filename, AVIO_FLAG_WRITE);
		if (error < 0) {
			fprintf(stderr, "Could not open output file '%s'", filename);
			goto cleanup;
		}
	}

    return 0;

cleanup:
    avcodec_free_context(&avctx);
    avio_closep(&(*output_format_context)->pb);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return error < 0 ? error : AVERROR_EXIT;
}

/** Initialize one data packet for reading or writing. */
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /** Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

/** Initialize one audio frame for reading from the input file */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize the audio resampler based on the input and output codec settings.
 * If the input and output sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 */
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
        int error;
		SwrContext *swr_ctx = NULL;
		swr_ctx = swr_alloc();
		if (swr_ctx == NULL) {
			fprintf(stderr, "Could not allocate resample context\n");
			return AVERROR(ENOMEM);
		}

        /**
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity (they are sometimes not detected
         * properly by the demuxer and/or decoder).
         */
         /************* update for ffmpeg3.3  20170417      ******/
/*        *resample_context = swr_alloc_set_opts(NULL,
                                              av_get_default_channel_layout(output_codec_context->channels),
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                              av_get_default_channel_layout(input_codec_context->channels),
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);

         swr_alloc_set_opts(swr_ctx,
                                              av_get_default_channel_layout(output_codec_context->channels),
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                              av_get_default_channel_layout(input_codec_context->channels),
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);

        if (swr_ctx == NULL) {
            fprintf(stderr, "Could not allocate resample context\n");
            return AVERROR(ENOMEM);
        }
*/
		if (av_opt_set_int(swr_ctx, "ocl", av_get_default_channel_layout(output_codec_context->channels), 0) < 0) {
			fprintf(stderr, "Could not set ocl\n");
			goto fail;
		}
		if (av_opt_set_int(swr_ctx, "osf", output_codec_context->sample_fmt, 0) < 0) {
			fprintf(stderr, "Could not set osf\n");
			goto fail;
		}
		if (av_opt_set_int(swr_ctx, "osr", output_codec_context->sample_rate, 0) < 0) {
			fprintf(stderr, "Could not set ocl\n");
			goto fail;
		}

		if (av_opt_set_int(swr_ctx, "icl", av_get_default_channel_layout(input_codec_context->channels), 0) < 0) {
			fprintf(stderr, "Could not set icl\n");
			goto fail;
		}
		if (av_opt_set_int(swr_ctx, "isf", input_codec_context->sample_fmt, 0) < 0) {
			fprintf(stderr, "Could not set isf\n");
			goto fail;
		}
		if (av_opt_set_int(swr_ctx, "isr", input_codec_context->sample_rate, 0) < 0) {
			fprintf(stderr, "Could not set isr\n");
			goto fail;
		}

		if (av_opt_set_int(swr_ctx, "ich", input_codec_context->channels, 0) < 0) {
			fprintf(stderr, "Could not set ich\n");
			goto fail;
		}
		if (av_opt_set_int(swr_ctx, "och", output_codec_context->channels, 0) < 0) {
			fprintf(stderr, "Could not set och\n");
			goto fail;
		}
        /**
        * Perform a sanity check so that the number of converted samples is
        * not greater than the number of samples to be converted.
        * If the sample rates differ, this case has to be handled differently
        */
 //       av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

        /** Open the resampler with the specified parameters. */
        if ((error = swr_init(swr_ctx)) < 0) {
            fprintf(stderr, "Could not open resample context\n");
            swr_free(&swr_ctx);
            return error;
        }

		*resample_context = swr_ctx;
		return 0;
	fail:
		swr_free(&swr_ctx);
		return -1;
}

/** Initialize a FIFO buffer for the audio samples to be encoded. */
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/** Write the header of the output file container. */
static int write_output_file_header(AVFormatContext *output_format_context)
{
    int error;
   
    if ((error = avformat_write_header(output_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not write output file header (error '%s')\n",
                av_err2str(error));
        return error;
    }
    return 0;
}


/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int nb_samples)
{
    int error;

    /**
     * Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
	int line_size = -1;
//	error = av_samples_alloc(converted_input_samples, &line_size, output_codec_context->channels,
//		nb_samples, output_codec_context->sample_fmt, 1);

	error = av_samples_alloc_array_and_samples(converted_input_samples, &line_size, output_codec_context->channels,
	nb_samples, output_codec_context->sample_fmt, 1);
	if (error < 0) {
		fprintf(stderr,
			"Could not allocate converted input samples (error '%s-%d')\n",
			av_err2str(error), error);
		fprintf(stderr,"channels = %d, frame_size = %d, sample_fmt = %d, line_size = %d\n",
			output_codec_context->channels, nb_samples, output_codec_context->sample_fmt, line_size);
		return error;
	}

    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is specified
 * by frame_size.
 */
static int convert_samples(SwrContext *resample_context,
						   uint8_t **converted_data,	int out_samples,
						   const uint8_t **input_data,	int in_samples                           
                           )
{
    int error;

    /** Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, out_samples,
                             input_data    , in_samples)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }

    return 0;
}

/** Add converted input audio samples to the FIFO buffer for later processing. */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int nb_samples)
{
    int error;

    /**
     * Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples.
     */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + nb_samples)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /** Store the new samples in the FIFO buffer. */
	error = av_audio_fifo_write(fifo, (void **)converted_input_samples, nb_samples);
	if (error < nb_samples) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}


/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /**
     * Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /**
     * Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified.
     */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_err2str(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}

/** Global timestamp for the audio frames */
static int64_t pts = 0;

/** Encode one frame worth of audio to the output file. */
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    int error;
	*data_present = 0;


    init_packet(&output_packet);
    output_packet.stream_index = 0;

    /** Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
		printf("frame.pts = %lld\n", frame->pts);
        pts += frame->nb_samples;
    }

    /**
     * Encode the audio frame and store it in the temporary packet.
     * The output audio stream encoder is used to do this.
     */
/*   update for ffmpeg3.3 20170417    */
    error = avcodec_send_frame(output_codec_context, frame);
    if (error == AVERROR_EOF) {
        fprintf(stderr, "(error '%s')\n",
                av_err2str(error));
       av_packet_unref(&output_packet);
       return 0;
    }

    if (error < 0) {
       fprintf(stderr, "Could not avcodec_send_frame (error '%s')\n",
                av_err2str(error));
       av_packet_unref(&output_packet);
       return error;
    }

    out_nb_frames++;
	while (1) {
		error = avcodec_receive_packet(output_codec_context, &output_packet);
		if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
			return 0;
		}
		else if (error < 0) {
			printf("avcodec_receive_packet error = %d\n", error);
			return error;
		}
		

		output_packet.stream_index = 0;
//		av_packet_rescale_ts(&output_packet, output_codec_context->time_base, output_format_context->streams[0]->time_base);
//		output_packet.dts = output_packet.pts;
//		output_packet.duration = av_rescale_q(output_packet.duration, output_codec_context->time_base, output_format_context->streams[0]->time_base);


		error = av_write_frame(output_format_context, &output_packet);
		if (error < 0) {
			fprintf(stderr, "Could not write frame (error '%s')\n",
				av_err2str(error));
			av_packet_unref(&output_packet);
			return error;
		}
		*data_present = 1;
		opkt_size += output_packet.size;

	}
	return 0;
}

/**
 * Load one audio frame from the FIFO buffer, encode and write it to the
 * output file.
 */
static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /**
     * Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size
     */
    
	const int frame_size = FFMIN(av_audio_fifo_size(fifo),
		g_enc_frame_size);

    int data_written;

    /** Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /**
     * Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily.
     */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        fprintf(stderr, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /** Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

static void flush_encoders(AVFormatContext *output_format_context, AVCodecContext *output_codec_context)
{
    AVPacket output_packet;
    int error;

    init_packet(&output_packet);

    avcodec_send_frame(output_codec_context, NULL);
    while(1) {
        error = avcodec_receive_packet(output_codec_context, &output_packet);
        if (error != 0) {
            printf("not found last packet\n");
            return;
        }

        error = av_write_frame(output_format_context, &output_packet);
        if (error == 0 ) {

            opkt_size += output_packet.size;
            printf("write end packet ok,packet.size = %d\n", output_packet.size);

        }
    }
}

/** Write the trailer of the output file container. */
static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    int error;

    if ((error = av_write_trailer(output_format_context)) < 0) {
        fprintf(stderr, "Could not write output file trailer (error '%s')\n",
                av_err2str(error));
        return error;
    }
    return 0;
}

static int convert_frame_to_fifo(AVAudioFifo *fifo, AVFrame *input_frame, AVCodecContext *output_codec_context, SwrContext *resample_context)
{
	int ret;
	uint8_t **converted_input_samples = NULL;
	int src_nb_samples = input_frame->nb_samples;	
//	int dst_nb_samples = av_rescale_rnd(src_nb_samples, output_codec_context->sample_rate, input_frame->sample_rate, AV_ROUND_UP);
	int dst_nb_samples = src_nb_samples * output_codec_context->sample_rate / input_frame->sample_rate;
	ret = init_converted_samples(&converted_input_samples, output_codec_context, dst_nb_samples);
	if (ret < 0) {
		goto clean;
	}

	/**
	* Convert the input samples to the desired output sample format.
	* This requires a temporary storage provided by converted_input_samples.
	*/
	ret = convert_samples(resample_context, converted_input_samples, dst_nb_samples,(const uint8_t**)input_frame->extended_data, src_nb_samples);
		
	if (ret != 0) {
		goto clean;
	}

	/** Add the converted input samples to the FIFO buffer for later processing. */
	ret = add_samples_to_fifo(fifo, converted_input_samples,
		dst_nb_samples);
	if (ret != 0) {
		goto clean;
	}
clean:
	if (converted_input_samples) {
		av_freep(&converted_input_samples[0]);
		av_freep(&converted_input_samples);
	}
	return ret;
}




/** Convert an audio file to an AAC file in an MP4 container. */
int main(int argc, char **argv)
{
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVCodecContext *input_codec_context = NULL, *output_codec_context = NULL;
    SwrContext *resample_context = NULL;
    AVAudioFifo *fifo = NULL;
    int ret = AVERROR_EXIT;
	const char *iurl = "D:\\app\\dev\\ffmpeg-vs\\ff_trans_audio\\x64\\Debug\\1.flac";
	const char *ourl = "D:\\app\\dev\\ffmpeg-vs\\ff_trans_audio\\x64\\Debug\\1flac.wav";
/*
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(1);
    }
*/
    /** Register all codecs and formats so that they can be used. */
    av_register_all();

    /** Open the input file for reading. */
    if (open_input_file(iurl, &input_format_context,
                        &input_codec_context))
        goto cleanup;
    /** Open the output file for writing. */
    if (open_output_file(ourl, input_codec_context,
                         &output_format_context, &output_codec_context))
        goto cleanup;
    /** Initialize the resampler to be able to convert audio sample formats. */
    if (init_resampler(input_codec_context, output_codec_context,
                       &resample_context))
        goto cleanup;
    /** Initialize the FIFO buffer to store audio samples to be encoded. */
    if (init_fifo(&fifo, output_codec_context))
        goto cleanup;
    /** Write the header of the output file container. */
    if (write_output_file_header(output_format_context))
        goto cleanup;

	AVFrame *input_frame = NULL;
	if (init_input_frame(&input_frame) != 0)
		goto cleanup;
    /**
     * Loop as long as we have input samples to read or output samples
     * to write; abort as soon as we have neither.
     */


    while (1) {
        /** Use the encoder's desired frame size for processing. */
        const int output_frame_size = output_codec_context->frame_size;
        int finished                = 0;

        /**
         * Make sure that there is one frame worth of samples in the FIFO
         * buffer so that the encoder can do its work.
         * Since the decoder's and the encoder's frame size may differ, we
         * need to FIFO buffer to store as many frames worth of input samples
         * that they make up at least one frame worth of output samples.
         */
		for (;;) {

			int ret;
			AVPacket in_pkt;
			init_packet(&in_pkt);

			ret = av_read_frame(input_format_context, &in_pkt);
			if (ret < 0) {
				finished = 1;
				break;
			}

			if (in_pkt.size <= 0 || in_pkt.stream_index != g_audio_index) {
				av_packet_unref(&in_pkt);
				continue;
			}

			ret = avcodec_send_packet(input_codec_context, &in_pkt);
			if (ret == AVERROR_EOF) {
				av_packet_unref(&in_pkt);
				continue;
			}
			if (ret < 0) {
				av_packet_unref(&in_pkt);
				finished = 1;
				break;
			}


			while (1) {
				ret = avcodec_receive_frame(input_codec_context, input_frame);
				if (ret < 0)
					break;

				ret = convert_frame_to_fifo(fifo, input_frame, output_codec_context, resample_context);
				if (ret < 0)
					break;

			}

			av_packet_unref(&in_pkt);
			if (av_audio_fifo_size(fifo) >= g_enc_frame_size) {
				break;
			}
		}

        /**
         * If we have enough samples for the encoder, we encode them.
         * At the end of the file, we pass the remaining samples to
         * the encoder.
         */
        while (av_audio_fifo_size(fifo) >= g_enc_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0))
            /**
             * Take one frame worth of audio samples from the FIFO buffer,
             * encode it and write it to the output file.
             */
            if (load_encode_and_write(fifo, output_format_context,
                                      output_codec_context))
                goto cleanup;

        /**
         * If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish.
         */
        if (finished) {
            printf("finish = %d\n", finished);
            flush_encoders(output_format_context, output_codec_context);
            break;
        }

    }

    /** Write the trailer of the output file container. */

    if (write_output_file_trailer(output_format_context))
        goto cleanup;
    ret = 0;
    printf("output size = %d\n",opkt_size);

    goto cleanup;


cleanup:
	av_frame_free(&input_frame);
	if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    if (output_codec_context)
        avcodec_free_context(&output_codec_context);
    if (output_format_context) {
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
        avcodec_free_context(&input_codec_context);
    if (input_format_context)
        avformat_close_input(&input_format_context);
	getchar();
    return ret;
}
