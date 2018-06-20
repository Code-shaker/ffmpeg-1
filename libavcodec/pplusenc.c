/*
 * Perseus Plus encoding using the P.Plus Encoder IL.
 * Copyright (c) 2018 V-Nova International Ltd
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
#include "avcodec.h"
#include "internal.h"
#include "compat/w32dlfcn.h"
#include "libavutil/opt.h"

#include <ctype.h>
#include <float.h>
#include <stdint.h>
#include <PPlusEnc2_IL.h>

/* -----------------------------------------------------------------------------
 * IL lib handling.
 * -----------------------------------------------------------------------------*/

static const char* kILFilename =
#ifdef _WIN32
"PPlusEnc2IL.dll";
#else
"libPPlusEnc2IL.so";
#endif

typedef PPlusEnc2ILReturnCode (*ILOpenSettingsDefaultFn)(PPlusEnc2ILOpenSettings*);
typedef PPlusEnc2ILReturnCode (*ILInitSettingsDefaultFn)(PPlusEnc2ILInitSettings*);
typedef PPlusEnc2ILReturnCode (*ILOpen_0Fn)(PPlusEnc2ILOpenSettings*, PPlusEnc2ILContext**);
typedef PPlusEnc2ILReturnCode (*ILQueryMetadataFn)(PPlusEnc2ILContext*, const char*, PPlusEnc2ILProperty*);
typedef PPlusEnc2ILReturnCode (*ILInitialiseFn)(PPlusEnc2ILContext*, PPlusEnc2ILInitSettings*);
typedef void				  (*ILCloseFn)(PPlusEnc2ILContext*);
typedef PPlusEnc2ILReturnCode (*ILGetPictureFn)(PPlusEnc2ILContext*, PPlusEnc2ILFrameType, PPlusEnc2ILPicture**);
typedef PPlusEnc2ILReturnCode (*ILEncodeFn)(PPlusEnc2ILContext*, PPlusEnc2ILPicture*);
typedef PPlusEnc2ILReturnCode (*ILGetOutputFn)(PPlusEnc2ILContext*, PPlusEnc2ILOutput**);
typedef PPlusEnc2ILReturnCode (*ILReleaseOutputFn)(PPlusEnc2ILContext*, PPlusEnc2ILOutput*);

typedef struct PPlusEncILLib
{
	void*						handle;
	ILOpenSettingsDefaultFn		open_settings_default;
	ILInitSettingsDefaultFn		init_settings_default;
	ILOpen_0Fn					open;
	ILQueryMetadataFn			query_metadata;
	ILInitialiseFn				initialise;
	ILCloseFn					close;
	ILGetPictureFn				get_picture;
	ILEncodeFn					encode;
	ILGetOutputFn				get_output;
	ILReleaseOutputFn			release_output;
} PPlusEncILLib;

static int32_t pplus_enc_init_lib(AVCodecContext* avctx, PPlusEncILLib* lib)
{
	void* h = dlopen(kILFilename, RTLD_NOW|RTLD_LOCAL);

	if(h == NULL)
	{
#ifdef _WIN32
		av_log(avctx, AV_LOG_ERROR, "Failed to open PPlusEnc2IL shared library (%s).\n", kILFilename);
#else
		av_log(avctx, AV_LOG_ERROR, "Failed to open PPlusEnc2IL shared library (%s), Error: %s.\n", kILFilename, dlerror());
#endif
		return -1;
	}

	memset(lib, 0, sizeof(PPlusEncILLib));

	#define pplus_stringify(x) #x
	#define pplus_get_fn(dest, name) dest = (IL##name##Fn)dlsym(h, pplus_stringify(PPlusEnc2IL_ ## name)); \
									 if(dest == NULL) { \
										av_log(avctx, AV_LOG_ERROR, "Failed to load function '%s' from PPlusEnc2IL shared library.\n", "" #name); return -1; }
	pplus_get_fn(lib->open_settings_default, OpenSettingsDefault);
	pplus_get_fn(lib->init_settings_default, InitSettingsDefault);
	pplus_get_fn(lib->open, Open_0);
	pplus_get_fn(lib->query_metadata, QueryMetadata);
	pplus_get_fn(lib->initialise, Initialise);
	pplus_get_fn(lib->close, Close);
	pplus_get_fn(lib->get_picture, GetPicture);
	pplus_get_fn(lib->encode, Encode);
	pplus_get_fn(lib->get_output, GetOutput);
	pplus_get_fn(lib->release_output, ReleaseOutput);
	#undef pplus_get_fn
	#undef pplus_stringify

	lib->handle = h;
	return 0;
}

static void pplus_enc_close_lib(PPlusEncILLib* lib)
{
	if(lib->handle)
	{
		dlclose(lib->handle);
		lib->handle = NULL;
	}
}

/* -----------------------------------------------------------------------------
 * Context
 * -----------------------------------------------------------------------------*/

typedef struct PPlusEncContext
{
	AVClass*				avclass;
	PPlusEncILLib			lib;
	PPlusEnc2ILContext*		ilctx;
	int32_t					draining;

	char*					json_buffer;
	char					json_format_buffer[16384];
	uint32_t				json_buffer_length;
	int32_t					json_first_value;

	/* IL parameters */
	struct
	{
		char*				base_encoder;
		char*				base_resolution_mode;
		char*				eil_args;
		char*				epi_args;
		char*				downsample_luma;
		char*				downsample_chroma;
		int32_t	 			temporal;
		char*				debanding_type;
		int32_t				debanding_strength;
		int32_t				double_upsampling;
		char*				transform_type;
		float				average_base_proportion;
		float				maximum_base_proportion;
		char*				upsample;
		int32_t				top_field_first;
		int32_t				output_filler;
		int32_t				pass_count;
		char*				db_path;
		char*				genre;
		char*				perseus_tune;
		int32_t				db_warn_on_fail;
	} il;

	/* x264 parameters */
	struct
	{
		char*				preset;
		char*				tune;
		char*				profile;
		char*				level;
		int32_t				fastfirstpass;
		char*				base_me_method;
		char*				base_psy_rd;
		int32_t				base_dct_8x8;
		int32_t				weightp;
		int32_t				weightb;
		int32_t				psy;
		int32_t				chroma_qp_offset;
		int32_t				noise_reduction;
		char*				b_pyramid;
		int32_t				b_frame_strategy;
		int32_t				intra_refresh;
		int32_t				scenechange_threshold;
		int32_t				deadzone_intra;
		int32_t				deadzone_inter;
		int32_t				dct_decimate;
		int32_t				aq_mode;
		float				aq_strength;
		int32_t				rc_lookahead;
		char*				direct_pred;
		float				cplxblur;
		char*				deblock;
		int32_t				fast_pskip;
		int32_t				b_bias;
		char*				nal_hrd;
		int32_t				mixed_refs;
		int32_t				mbtree;
		int32_t				avcintra_class;
		int32_t				deterministic;
		float				crf;
	} x264;
} PPlusEncContext;

/* -----------------------------------------------------------------------------
 * JSON & Misc.
 * -----------------------------------------------------------------------------*/

static char* pplus_enc_config_expand_buffer(PPlusEncContext* ctx, uint32_t amount)
{
	uint32_t offset = ctx->json_buffer_length;
	ctx->json_buffer = (char*)av_realloc(ctx->json_buffer, offset + amount);
	memset(ctx->json_buffer + offset, 0, amount);
	ctx->json_buffer_length = offset + amount;
	return (ctx->json_buffer + offset);
}

static void pplus_enc_config_begin(PPlusEncContext* ctx)
{
	static const char* kOpen = "{\n";
	const uint32_t openLength = (uint32_t)strlen(kOpen);
	char* pos = pplus_enc_config_expand_buffer(ctx, openLength);
	memcpy(pos, kOpen, openLength);
	ctx->json_first_value = 0;
}

static void pplus_enc_config_close(PPlusEncContext* ctx)
{
	static const char* kClose = "\n}";
	const uint32_t closeLength = (uint32_t)strlen(kClose);
	char* pos = pplus_enc_config_expand_buffer(ctx, closeLength + 1);	/* Zero-terminate string. */
	memcpy(pos, kClose, closeLength);
}

static void pplus_enc_config_cap_last(PPlusEncContext* ctx)
{
	if(ctx->json_first_value)
	{
		static const char* kCap = ",\n";
		const uint32_t capLength = (uint32_t)strlen(kCap);
		char* pos = pplus_enc_config_expand_buffer(ctx, capLength);
		memcpy(pos, kCap, capLength);
	}

	ctx->json_first_value = 1;
}

static void pplus_enc_config_add_i_unchecked(PPlusEncContext* ctx, const char* field, int i)
{
	int count;
	char* pos;

	pplus_enc_config_cap_last(ctx);
	count	= sprintf(ctx->json_format_buffer, "\t\"%s\" : %d", field, i);
	pos		= pplus_enc_config_expand_buffer(ctx, count);
	memcpy(pos, ctx->json_format_buffer, count);
}

static void pplus_enc_config_add_i(PPlusEncContext* ctx, const char* field, int i)
{
	if(i < 0)
		return;

	pplus_enc_config_add_i_unchecked(ctx, field, i);
}

static void pplus_enc_config_add_f(PPlusEncContext* ctx, const char* field, float f)
{
	int count;
	char* pos;

	if(f < 0.0f)
		return;

	pplus_enc_config_cap_last(ctx);
	count	= sprintf(ctx->json_format_buffer, "\t\"%s\" : %f", field, f);
	pos		= pplus_enc_config_expand_buffer(ctx, count);
	memcpy(pos, ctx->json_format_buffer, count);
}

static void pplus_enc_config_add_s(PPlusEncContext* ctx, const char* field, const char* s)
{
	int count;
	char* pos;

	if(s == NULL)
		return;

	pplus_enc_config_cap_last(ctx);
	count	= sprintf(ctx->json_format_buffer, "\t\"%s\" : \"%s\"", field, s);
	pos		= pplus_enc_config_expand_buffer(ctx, count);
	memcpy(pos, ctx->json_format_buffer, count);
}

static void pplus_enc_config_process_args(PPlusEncContext* ctx, char* args)
{
	char* pair = NULL;
	char  name[256];
	char  value[256];
	float f;

	if(args == NULL)
		return;

	pair = strtok(args, ";");

	while(pair != NULL)
	{
		char* equals = strchr(pair, '=');

		/* A pair can either be just a key, where it is interpreted as key=1, or
		   a key=value pair. */
		if(equals)
		{
			int32_t i = 0, isstr = 0;
			uint32_t namelen = equals - pair;
			uint32_t valuelen = (uint32_t)strlen(pair) - (namelen + 1);

			/* Cap, copy, terminate. */
			namelen = namelen < 256 ? namelen : 255;
			valuelen = valuelen < 256 ? valuelen : 255;

			memcpy(name, pair, namelen);
			memcpy(value, equals + 1, valuelen);

			name[namelen] = 0;
			value[valuelen] = 0;

			/* Basic check for value as a number. */
			while(i < valuelen)
			{
				char c = value[i];
				i += 1;

				if(!isdigit(c) && (c != '.') && (c != '+') && (c != '-'))
				{
					isstr = 1;
					break;
				}
			}

			if(isstr)
			{
				pplus_enc_config_add_s(ctx, name, value);
			}
			else
			{
				f = atof(value);
				if(f == (float)((int32_t)f))
					pplus_enc_config_add_i(ctx, name, (int32_t)f);
				else
					pplus_enc_config_add_f(ctx, name, f);
			}
		}
		else
		{
			pplus_enc_config_add_i(ctx, pair, 1);
		}

		pair = strtok(NULL, ";");
	}
}

static int32_t pplus_enc_get_encoding_pass(int32_t flags)
{
	if(flags & AV_CODEC_FLAG_PASS1)
		return 1;
	else if(flags & AV_CODEC_FLAG_PASS2)
		return 2;

	return 0;
}

static void pplus_enc_log(void* userdata, int level, const char* msg)
{
	static const int level_map[] =
	{
		AV_LOG_DEBUG,
		AV_LOG_INFO,
		AV_LOG_WARNING,
		AV_LOG_ERROR,
		AV_LOG_DEBUG
	};

	if (level < 0 || level > 4)
	{
		return;
	}

	av_log((AVCodecContext*)userdata, level_map[level], "%s", msg);
}

/* -----------------------------------------------------------------------------
 * Initialisation
 * -----------------------------------------------------------------------------*/

typedef int32_t (*BaseConfigFn)(AVCodecContext* avctx);
typedef int32_t (*BaseInitFn)(AVCodecContext* avctx);

static int32_t pplus_enc_init_common(AVCodecContext* avctx, const char* basename, BaseConfigFn configfn, BaseInitFn initfn, int32_t generic)
{
	PPlusEncContext*		pplusctx			= avctx->priv_data;
	PPlusEnc2ILOpenSettings	openset				= {0};
	PPlusEnc2ILInitSettings	initset				= {0};
	uint32_t				rcbucketduration	= 0;
	float					maxbaseprop			= -1.0f;
	AVCPBProperties*		cpbprops;

	if(avctx->bit_rate == 0)
	{
		av_log(avctx, AV_LOG_ERROR, "Constant quality is not currently supported in this implementation\n");
		return AVERROR_EXTERNAL;
	}

	if(avctx->rc_max_rate > 0 && pplusctx->il.maximum_base_proportion >= 0.0f)
	{
		av_log(avctx, AV_LOG_ERROR, "Both maxrate and bitate_max_base_prop are supplied, both can't be used together they conflict with no priority given to either\n");
		return AVERROR_EXTERNAL;
	}

	if((avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) != 0)
	{
		av_log(avctx, AV_LOG_ERROR, "Interlaced encoding is not currently supported in this implementation\n");
		return AVERROR_EXTERNAL;
	}

	if(avctx->pix_fmt != AV_PIX_FMT_YUV420P)
	{
		av_log(avctx, AV_LOG_ERROR, "YUV420P is the only supported format in this implementation\n");
		return AVERROR_EXTERNAL;
	}

	/* Open lib. */
	if(pplus_enc_init_lib(avctx, &pplusctx->lib) != 0)
	{
		return AVERROR_EXTERNAL;
	}

	/* Open context */
	if(pplusctx->lib.open_settings_default(&openset) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Failed to initialise P.Plus Encoder IL open config\n");
		return AVERROR_EXTERNAL;
	}

	if(generic)
	{
		if (pplusctx->il.base_encoder == NULL)
		{
			av_log(avctx, AV_LOG_ERROR, "Base encoder must be specified when utilising a generic P.Plus encoder\n");
			return AVERROR_UNKNOWN;
		}

		openset.base_encoder = pplusctx->il.base_encoder;
	}
	else
	{
		openset.base_encoder = basename;
	}

	openset.log_callback	= pplus_enc_log;
	openset.log_userdata	= avctx;
	openset.db_filepath		= pplusctx->il.db_path ? pplusctx->il.db_path : 0;

	if(pplusctx->lib.open(&openset, &pplusctx->ilctx) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Failed to open P.Plus Encoder IL\n");
		return AVERROR_EXTERNAL;
	}

	/* Init context */
	if(pplusctx->lib.init_settings_default(&initset) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Failed to initialise P.Plus Encoder IL init config\n");
		return AVERROR_EXTERNAL;
	}

	/* IL init config */
	initset.width			= avctx->width;
	initset.height			= avctx->height;
	initset.fps_num			= avctx->time_base.den;
	initset.fps_denom		= avctx->time_base.num * avctx->ticks_per_frame;
	initset.timebase_num	= avctx->time_base.num;
	initset.timebase_denom	= avctx->time_base.den;

	rcbucketduration = (((float)avctx->gop_size * initset.fps_denom) / initset.fps_num) * 1000.0f;
	maxbaseprop = (pplusctx->il.maximum_base_proportion >= 0.0f) ? pplusctx->il.maximum_base_proportion						:	/* Use max base prop if specified */
				  (avctx->rc_max_rate > 0)						 ? (float)((double)avctx->rc_max_rate / avctx->bit_rate)	:	/* Else calculate max base prop if max rate specified. */
																  -1.0f;														/* Else disable it. */

	/* Generate JSON config */
	pplus_enc_config_begin(pplusctx);

	/* Load up base config settings, this can manipulate the Perseus options if needed. */
	if (configfn)
	{
		int32_t r = configfn(avctx);
		if (r)
		{
			av_log(avctx, AV_LOG_ERROR, "Failed to initialise base encoder config\n");
			return r;
		}
	}

	/* Load up IL config settings. */
	pplus_enc_config_add_i(pplusctx, "pass", pplus_enc_get_encoding_pass(avctx->flags));
	pplus_enc_config_add_s(pplusctx, "perseus_mode", pplusctx->il.base_resolution_mode);
	pplus_enc_config_add_s(pplusctx, "encoding_downsample_luma", pplusctx->il.downsample_luma);
	pplus_enc_config_add_s(pplusctx, "encoding_downsample_chroma", pplusctx->il.downsample_chroma);
	pplus_enc_config_add_s(pplusctx, "encoding_upsample", pplusctx->il.upsample);
	pplus_enc_config_add_i(pplusctx, "dc_double_upsampling_enabled", pplusctx->il.double_upsampling);
	pplus_enc_config_add_s(pplusctx, "dc_dithering_type", pplusctx->il.debanding_type);
	pplus_enc_config_add_i(pplusctx, "dc_dithering_strength", pplusctx->il.debanding_strength);
	pplus_enc_config_add_i(pplusctx, "temporal_enabled", pplusctx->il.temporal);
	pplus_enc_config_add_s(pplusctx, "encoding_transform_type", pplusctx->il.transform_type);
	pplus_enc_config_add_i(pplusctx, "encoding_top_field_first", pplusctx->il.top_field_first);
	pplus_enc_config_add_i(pplusctx, "output_filler_enabled", pplusctx->il.output_filler);
	pplus_enc_config_add_i(pplusctx, "pass_count", pplusctx->il.pass_count);
	pplus_enc_config_add_s(pplusctx, "epi_params", pplusctx->il.epi_args);
	pplus_enc_config_add_i(pplusctx, "bitrate", avctx->bit_rate / 1000);
	pplus_enc_config_add_f(pplusctx, "bitrate_base_prop", pplusctx->il.average_base_proportion);
	pplus_enc_config_add_f(pplusctx, "bitrate_max_base_prop", maxbaseprop);
	pplus_enc_config_add_f(pplusctx, "rc_bucket_duration_ms", rcbucketduration);

	if (generic)
	{
		pplus_enc_config_add_i(pplusctx, "db_bypass", 1);
		pplus_enc_config_add_i(pplusctx, "debug_warn_on_property_error", 1);
	}
	else
	{
		pplus_enc_config_add_s(pplusctx, "db_genre", pplusctx->il.genre);
		pplus_enc_config_add_s(pplusctx, "db_tune", pplusctx->il.perseus_tune);
		pplus_enc_config_add_i(pplusctx, "db_warn_on_fail", pplusctx->il.db_warn_on_fail);
	}

	pplus_enc_config_process_args(pplusctx, pplusctx->il.eil_args);

	pplus_enc_config_close(pplusctx);

	/* Intialise the P.Plus IL encoder */
	initset.properties_json	= pplusctx->json_buffer;

	if (pplusctx->lib.initialise(pplusctx->ilctx, &initset) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Failed to initialise P.Plus Encoder IL\n");
		return AVERROR_EXTERNAL;
	}

	/* Post initialisation step. */
	if(initfn)
	{
		int32_t r = initfn(avctx);
		if (r)
		{
			av_log(avctx, AV_LOG_ERROR, "Failed to initialise base encoder\n");
			return r;
		}
	}

	/* Feedback modified properties */
	avctx->width		= initset.width;
	avctx->height		= initset.height;

	cpbprops = ff_add_cpb_side_data(avctx);
	if(!cpbprops)
		return AVERROR(ENOMEM);

	cpbprops->avg_bitrate = avctx->bit_rate;
	cpbprops->max_bitrate = avctx->bit_rate;
	cpbprops->buffer_size = ((avctx->bit_rate * rcbucketduration) / 1000);

	return 0;
}

/* -----------------------------------------------------------------------------
 * x264 config
 * -----------------------------------------------------------------------------*/

static int32_t pplus_enc_base_config_x264(AVCodecContext* avctx)
{
	PPlusEncContext* pplusctx = avctx->priv_data;

	pplus_enc_config_add_s(pplusctx, "preset", pplusctx->x264.preset);
	pplus_enc_config_add_s(pplusctx, "tune", pplusctx->x264.tune);
	pplus_enc_config_add_i(pplusctx, "fastfirstpass", pplusctx->x264.fastfirstpass);
	pplus_enc_config_add_s(pplusctx, "deblock", pplusctx->x264.deblock);
	pplus_enc_config_add_i(pplusctx, "b-bias", pplusctx->x264.b_bias);
	pplus_enc_config_add_s(pplusctx, "nal-hrd", pplusctx->x264.nal_hrd);
	pplus_enc_config_add_i(pplusctx, "mixed-refs", pplusctx->x264.mixed_refs);
	pplus_enc_config_add_i(pplusctx, "mbtree", pplusctx->x264.mbtree);
	pplus_enc_config_add_i(pplusctx, "avcintra-class", pplusctx->x264.avcintra_class);
	pplus_enc_config_add_i(pplusctx, "dct-decimate", pplusctx->x264.dct_decimate);
	pplus_enc_config_add_i(pplusctx, "bframes", avctx->max_b_frames);
	pplus_enc_config_add_i(pplusctx, "qp-min", avctx->qmin);
	pplus_enc_config_add_i(pplusctx, "qp-max", avctx->qmax);
	pplus_enc_config_add_i(pplusctx, "qp-step", avctx->max_qdiff);
	pplus_enc_config_add_i(pplusctx, "ref", avctx->refs);
	pplus_enc_config_add_f(pplusctx, "i-quant-factor", avctx->i_quant_factor);
	pplus_enc_config_add_f(pplusctx, "b-quant-factor", avctx->b_quant_factor);
	pplus_enc_config_add_f(pplusctx, "qblur", avctx->qblur);
	pplus_enc_config_add_i(pplusctx, "keyint", avctx->gop_size);
	pplus_enc_config_add_i(pplusctx, "min-keyint", avctx->keyint_min);
	pplus_enc_config_add_i(pplusctx, "chroma-me", ((avctx->me_cmp >= 0) && (avctx->me_cmp & FF_CMP_CHROMA)) ? 1 : -1);
	pplus_enc_config_add_s(pplusctx, "me", pplusctx->x264.base_me_method);
	pplus_enc_config_add_i(pplusctx, "psy", pplusctx->x264.psy);
	pplus_enc_config_add_s(pplusctx, "psy-rd", pplusctx->x264.base_psy_rd);
	pplus_enc_config_add_i(pplusctx, "8x8dct", pplusctx->x264.base_dct_8x8);
	pplus_enc_config_add_f(pplusctx, "cplxblur", pplusctx->x264.cplxblur);
	pplus_enc_config_add_i(pplusctx, "weightp", pplusctx->x264.weightp);
	pplus_enc_config_add_i(pplusctx, "weightb", pplusctx->x264.weightb);
	pplus_enc_config_add_i(pplusctx, "nr", pplusctx->x264.noise_reduction);
	pplus_enc_config_add_s(pplusctx, "b-pyramid", pplusctx->x264.b_pyramid);
	pplus_enc_config_add_i(pplusctx, "b-adapt", pplusctx->x264.b_frame_strategy);
	pplus_enc_config_add_i(pplusctx, "intra-refresh", pplusctx->x264.intra_refresh);
	pplus_enc_config_add_i(pplusctx, "scenecut", pplusctx->x264.scenechange_threshold);
	pplus_enc_config_add_i(pplusctx, "deadzone-intra", pplusctx->x264.deadzone_intra);
	pplus_enc_config_add_i(pplusctx, "deadzone-inter", pplusctx->x264.deadzone_inter);
	pplus_enc_config_add_f(pplusctx, "aq-strength", pplusctx->x264.aq_strength);
	pplus_enc_config_add_i(pplusctx, "rc-lookahead", pplusctx->x264.rc_lookahead);
	pplus_enc_config_add_i(pplusctx, "aq-mode", pplusctx->x264.aq_mode);
	pplus_enc_config_add_f(pplusctx, "qcompress", avctx->qcompress);
	pplus_enc_config_add_s(pplusctx, "direct-pred", pplusctx->x264.direct_pred);
	pplus_enc_config_add_i(pplusctx, "trellis", avctx->trellis);
	pplus_enc_config_add_i(pplusctx, "fast-pskip", pplusctx->x264.fast_pskip);
	pplus_enc_config_add_i(pplusctx, "me-range", avctx->me_range);
	pplus_enc_config_add_i(pplusctx, "subme", avctx->me_subpel_quality);
	pplus_enc_config_add_i(pplusctx, "global-header", (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : -1);
	pplus_enc_config_add_i(pplusctx, "aud", (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 0 : 1);
	pplus_enc_config_add_i(pplusctx, "deterministic", pplusctx->x264.deterministic);
	pplus_enc_config_add_f(pplusctx, "crf", pplusctx->x264.crf);

	if(avctx->thread_count > 0)
		pplus_enc_config_add_i(pplusctx, "threads", avctx->thread_count);

	if(pplusctx->x264.chroma_qp_offset >= -12 && pplusctx->x264.chroma_qp_offset <= 12)
		pplus_enc_config_add_i_unchecked(pplusctx, "chroma-qp-offset", pplusctx->x264.chroma_qp_offset);

	if(!pplusctx->x264.profile)
	{
		const char* p = 0;
		switch (avctx->profile)
		{
			case FF_PROFILE_H264_BASELINE:	p = "baseline"; break;
			case FF_PROFILE_H264_HIGH:		p = "high"; break;
			case FF_PROFILE_H264_HIGH_10:	p = "high10"; break;
			case FF_PROFILE_H264_HIGH_422:	p = "high422"; break;
			case FF_PROFILE_H264_HIGH_444:	p = "high444"; break;
			case FF_PROFILE_H264_MAIN:		p = "main"; break;
			default: break;
		}

		if(p)
			pplus_enc_config_add_s(pplusctx, "profile", p);
	}
	else
	{
		pplus_enc_config_add_s(pplusctx, "profile", pplusctx->x264.profile);
	}

	if(pplusctx->x264.level)
	{
		int level = 0;
		const char* value = pplusctx->x264.level;

		if (!strcmp(value, "1b"))
			level = 9;
		else if (atof(value) < 7)
			level = (int)(10 * atof(value) + .5);
		else
			level = atoi(value);

		pplus_enc_config_add_i(pplusctx, "level", level);
	}
	else
	{
		pplus_enc_config_add_i(pplusctx, "level", avctx->level);
	}

	if (avctx->rc_buffer_size)
	{
		av_log(avctx, AV_LOG_WARNING, "Ignoring rc buffer size setting from command line (%d), instead it is calculated from GOP size and bitrate\n", (int32_t)avctx->rc_buffer_size);
	}

	return 0;
}

static int32_t pplus_enc_base_init_x264(AVCodecContext* avctx)
{
	PPlusEncContext* pplusctx = avctx->priv_data;

	/* Update AVCodecContext with parameters. */
	if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
	{
		PPlusEnc2ILProperty headerData;
		if (pplusctx->lib.query_metadata(pplusctx->ilctx, "h264_header_data", &headerData) != PPE2_RC_Success)
		{
			av_log(avctx, AV_LOG_ERROR, "Failed to get global header data from the IL\n");
			return AVERROR_EXTERNAL;
		}

		avctx->extradata = av_mallocz(headerData.blob_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!avctx->extradata)
			return AVERROR(ENOMEM);

		memcpy(avctx->extradata, headerData.blob, headerData.blob_size);
		avctx->extradata_size = headerData.blob_size;
	}

	avctx->max_b_frames = (avctx->max_b_frames < 0) ? 0 : avctx->max_b_frames;
	avctx->has_b_frames = avctx->max_b_frames ? (pplusctx->x264.b_pyramid > 0) ? 2 : 1 : 0;

	return 0;
}

static av_cold int pplus_enc_init_x264(AVCodecContext* avctx)
{
	return pplus_enc_init_common(avctx, "x264", pplus_enc_base_config_x264, pplus_enc_base_init_x264, 0);
}

/* -----------------------------------------------------------------------------
 * Generic config
 * -----------------------------------------------------------------------------*/

static av_cold int pplus_enc_init_generic_base(AVCodecContext* avctx)
{
	return pplus_enc_init_common(avctx, NULL, NULL, NULL, 1);
}

/* -----------------------------------------------------------------------------
 * Encoding
 * -----------------------------------------------------------------------------*/

static int32_t pplus_enc_process_output(AVCodecContext* avctx, AVPacket* pkt, PPlusEnc2ILOutput* output)
{
	uint32_t			data_length		= output->data_length;
	uint8_t*			data_ptr;
	int32_t				ret;
	int32_t				out_pic_type;

	if(!output->data)
	{
		return 0;
	}

	if((ret = ff_alloc_packet2(avctx, pkt, data_length, 0)) < 0)
	{
		return ret;
	}

	data_ptr = pkt->data;
	memcpy(data_ptr, output->data, output->data_length);

	switch(output->base_type)
	{
		case PPE2_BT_Keyframe:
		case PPE2_BT_IDR:
		case PPE2_BT_SI:
		case PPE2_BT_I:		out_pic_type = AV_PICTURE_TYPE_I;		break;
		case PPE2_BT_SP:
		case PPE2_BT_P:		out_pic_type = AV_PICTURE_TYPE_P;		break;
		case PPE2_BT_BRef:
		case PPE2_BT_B:		out_pic_type = AV_PICTURE_TYPE_B;		break;
		default:			out_pic_type = AV_PICTURE_TYPE_NONE;	break;
	}

	ff_side_data_set_encoder_stats(pkt, output->qp * FF_QP2LAMBDA, NULL, 0, out_pic_type);

	pkt->pts = output->pts;
	pkt->dts = output->dts;
	pkt->flags |= (output->keyframe) ? AV_PKT_FLAG_KEY : 0;

	return 0;
}

static int pplus_enc_send_frame(AVCodecContext* avctx, const AVFrame* frame)
{
	PPlusEncContext* pplusctx	= avctx->priv_data;
	PPlusEnc2ILPicture* picture = NULL;

	if(frame)
	{
		int i, j, h, w;

		if (pplusctx->lib.get_picture(pplusctx->ilctx, PPE2_FrameType_Progressive, &picture) != PPE2_RC_Success)
		{
			av_log(avctx, AV_LOG_ERROR, "Error whilst obtaining P.Plus input picture\n");
			return AVERROR_EXTERNAL;
		}

		/* Load up frame data into picture.*/
		for(i = 0; i < 3; i++)
		{
			h = (i == 0) ? frame->height : (frame->height >> 1);
			w = picture->stride[i];

			if(w == frame->linesize[i])
			{
				memcpy(picture->plane[i], frame->data[i], w * h);
			}
			else if(frame->linesize[i] != w)
			{
				uint8_t* dst = picture->plane[i];
				const uint8_t* src = frame->data[i];
				const uint32_t amount = w < frame->linesize[i] ? w : frame->linesize[i];

				for(j = 0; j < h; j++)
				{
					memcpy(dst, src, amount);
					dst += w;
					src += frame->linesize[i];
				}
			}
		}

		picture->pts 			= frame->pts;
		picture->field_type 	= PPE2_FieldType_None;

		switch(frame->pict_type)
		{
			case AV_PICTURE_TYPE_I: picture->base_type = PPE2_BT_Keyframe;	break;	/* @todo: Optionally control as IDR instead */
			case AV_PICTURE_TYPE_P: picture->base_type = PPE2_BT_P;			break;
			case AV_PICTURE_TYPE_B: picture->base_type = PPE2_BT_B;			break;
			default: 				picture->base_type = PPE2_BT_Unknown;	break;
		}
	}

	pplusctx->draining = (frame == NULL) ? 1 : 0;

	if(pplusctx->lib.encode(pplusctx->ilctx, picture) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Error whilst encoding P.Plus input picture\n");
		return AVERROR_EXTERNAL;
	}

	return 0;
}

static int pplus_enc_receive_packet(AVCodecContext* avctx, AVPacket* pkt)
{
	PPlusEncContext*		pplusctx	= avctx->priv_data;
	PPlusEnc2ILOutput*		output		= NULL;
	PPlusEnc2ILReturnCode	rc			= pplusctx->lib.get_output(pplusctx->ilctx, &output);

	/* No output */
	if(rc == PPE2_RC_Finished)
	{
		/* Finished flushing */
		if(pplusctx->draining == 1)
			return AVERROR_EOF;

		/* Building delay */
		return AVERROR(EAGAIN);
	}
	else if(rc != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Error whilst obtaining P.Plus output data\n");
		return AVERROR_EXTERNAL;
	}

	/* Process packet */
	if(pplus_enc_process_output(avctx, pkt, output) != 0)
	{
		av_log(avctx, AV_LOG_ERROR, "Error whilst processing P.Plus output data\n");
	}

	/* Release data. */
	if(pplusctx->lib.release_output(pplusctx->ilctx, output) != PPE2_RC_Success)
	{
		av_log(avctx, AV_LOG_ERROR, "Error whilst releasing P.Plus output data\n");
		return AVERROR_EXTERNAL;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Shutdown
 * -----------------------------------------------------------------------------*/

static av_cold int pplus_enc_close(AVCodecContext *avctx)
{
	PPlusEncContext* pplusctx = avctx->priv_data;

	av_freep(&avctx->extradata);

	if(pplusctx->ilctx)
	{
		av_realloc(pplusctx->json_buffer, 0);
		pplusctx->json_buffer			= NULL;
		pplusctx->json_buffer_length	= 0;
		pplusctx->json_first_value		= 0;

		pplusctx->lib.close(pplusctx->ilctx);
		pplusctx->ilctx = NULL;

		pplus_enc_close_lib(&pplusctx->lib);
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------------*/

static const enum AVPixelFormat pplusenc_pix_fmts[] =
{
	AV_PIX_FMT_YUV420P,
	AV_PIX_FMT_NONE
};

static const AVCodecDefault pplusenc_defaults[] =
{
	{ "refs",		"-1"		},
	{ "flags2",		 "0"		},
	{ "bf",			"-1"		},
	{ "g",			"250"		},
	{ "i_qfactor",	"-1"		},
	{ "b_qfactor",	"-1"		},
	{ "qmin",		"-1"		},
	{ "qmax",		"-1"		},
	{ "qdiff",		"-1"		},
	{ "qblur",		"-1"		},
	{ "qcomp",		"-1"		},
	{ "trellis",	"-1"		},
	{ "me_range",	"-1"		},
	{ "subq",		"-1"		},
	{ "keyint_min",	"-1"		},
	{ "cmp",		"-1"		},
	{ "flags",		"+cgop"		},
	{ "threads",	"0"			},
	{ NULL },
};

#define OFFSET(x) offsetof(PPlusEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define ILOPTIONS \
	{ "perseus_mode",					"Determine the base resolution mode (robust, sharp, native)",										OFFSET(il.base_resolution_mode),		AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "db_path",						"Supply the path to the database", 																	OFFSET(il.db_path),						AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "db_genre",						"Supply the content genre to P.Plus", 																OFFSET(il.genre),						AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "db_tune",						"Supply the tune parameter to P.Plus (metrics, subjective, speed)",									OFFSET(il.perseus_tune),				AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "db_warn_on_fail",				"Indicate the P.Plus database should only warn when failing to find an operating point",			OFFSET(il.db_warn_on_fail),				AV_OPT_TYPE_INT,		{ -1 },			-1,	1,			VE },	\
	{ "eil_params",						"Supply additional EIL configuration properties as a ; separated list of key=value parameters",		OFFSET(il.eil_args),					AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "epi_params",						"Supply additional EPI configuration properties as a ; separated list of key=value parameters",		OFFSET(il.epi_args),					AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "encoding_downsample_luma",		"Set downsample_luma type (area, lanczos)",															OFFSET(il.downsample_luma),				AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "encoding_downsample_chroma",		"Set downsample_chroma type (area, lanczos)",														OFFSET(il.downsample_chroma),			AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "encoding_upsample",				"Set Perseus upsample type (cubic, linear)",														OFFSET(il.upsample),					AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "temporal_enabled",				"Enable Temporal for Perseus",																		OFFSET(il.temporal),					AV_OPT_TYPE_INT,		{ -1 },			-1,	1,			VE },	\
	{ "dc_double_upsampling_enabled",	"Run with double upsampling",																		OFFSET(il.double_upsampling),			AV_OPT_TYPE_INT,		{ -1 },			-1,	1,			VE },	\
	{ "dc_dithering_type",				"Supply dithering type for Perseus encoder (none, uniform)",										OFFSET(il.debanding_type),				AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "dc_dithering_strength",			"set Perseus debanding strength",																	OFFSET(il.debanding_strength),			AV_OPT_TYPE_INT,		{ -1 },			-1,	31,			VE },	\
	{ "encoding_transform_type",		"Specify the transform type to use for encoding (dd, dds)",											OFFSET(il.transform_type),				AV_OPT_TYPE_STRING,		{ 0 },			0,	0,			VE },	\
	{ "bitrate_base_prop",				"Set the average base proportion for Perseus",														OFFSET(il.average_base_proportion),		AV_OPT_TYPE_FLOAT,		{ .dbl=-1 },	-1,	1,			VE },	\
	{ "bitrate_max_base_prop",			"Set the maximum base proportion for Perseus",														OFFSET(il.maximum_base_proportion),		AV_OPT_TYPE_FLOAT,		{ .dbl=-1 },	-1,	1,			VE },	\
	{ "encoding_top_field_first",		"Specify top field first encoding, or not",															OFFSET(il.top_field_first),				AV_OPT_TYPE_INT,		{ -1 },			-1,	1,			VE },	\
	{ "output_filler_enabled",			"Specify filler should be generated for the output",												OFFSET(il.output_filler),				AV_OPT_TYPE_INT,		{ -1 },			-1,	1,			VE },	\
	{ "pass_count",						"Specify the intended number of encoding passes",													OFFSET(il.pass_count),					AV_OPT_TYPE_INT,		{ -1 },			-1,	INT_MAX,	VE },

#if CONFIG_PPLUSENC_X264_ENCODER

static const AVOption options_x264[] =
{
	ILOPTIONS

	{ "preset",							"'preset' parameter for x264 (veryslow, slow, ...)",												OFFSET(x264.preset),					AV_OPT_TYPE_STRING,		{ 0 },			0,	0,	VE },
	{ "tune",							"'tune' parameter for x264 (ssim, film, ...)",														OFFSET(x264.tune),						AV_OPT_TYPE_STRING,		{ 0 },			0,	0,	VE },
	{ "profile",						"'profile' parameter for x264",																		OFFSET(x264.profile),					AV_OPT_TYPE_STRING,		{ 0 },			0,	0,	VE },
	{ "fastfirstpass",					"Use fast x264 settings when encoding first pass",													OFFSET(x264.fastfirstpass),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE},
	{ "level",							"Specify level (as defined by Annex A)",															OFFSET(x264.level),						AV_OPT_TYPE_STRING,		{.str = NULL },	0,	0, VE},
	{ "motion-est",						"Set X264 Motion Estimation method (umh, dia, hex, tes)",											OFFSET(x264.base_me_method),			AV_OPT_TYPE_STRING,		{ 0 },			0,	0,	VE },
	{ "weightb",						"Weighted prediction for B-frames.",																OFFSET(x264.weightb),					AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "weightp",						"Weighted prediction for P-frames.",																OFFSET(x264.weightp),					AV_OPT_TYPE_INT,		{ -1 },			-1, 3,  VE },
	{ "psy",							"Use psychovisual optimizations.",																	OFFSET(x264.psy),						AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "psy-rd",							"Strength of psychovisual optimization, in <psy-rd>:<psy-trellis> format.",							OFFSET(x264.base_psy_rd),				AV_OPT_TYPE_STRING,		{ 0 },			0, 0, VE },
	{ "chromaoffset",					"QP difference between chroma and luma",															OFFSET(x264.chroma_qp_offset),			AV_OPT_TYPE_INT,		{ INT_MAX },	INT_MIN, INT_MAX, VE },
	{ "8x8dct",							"High profile 8x8 transform.",																		OFFSET(x264.base_dct_8x8),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "cplxblur",						"Reduce fluctuations in QP (before curve compression)",												OFFSET(x264.cplxblur),					AV_OPT_TYPE_FLOAT,		{.dbl = -1 },	-1, FLT_MAX, VE },
	{ "noise-reduction",				"Noise reduction",																					OFFSET(x264.noise_reduction),			AV_OPT_TYPE_INT,		{ -1 },			INT_MIN, INT_MAX, VE },
	{ "b-pyramid",						"Keep some B-frames as references.",																OFFSET(x264.b_pyramid),					AV_OPT_TYPE_STRING,		{ 0 },			0, 0,  VE },
	{ "b-adapt",						"Strategy to choose between I/P/B-frames",															OFFSET(x264.b_frame_strategy),			AV_OPT_TYPE_INT,		{ -1 },			-1, 2, VE },
	{ "intra-refresh",					"Use Periodic Intra Refresh instead of IDR frames.",												OFFSET(x264.intra_refresh),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "sc-threshold",					"Scene change threshold",																			OFFSET(x264.scenechange_threshold),		AV_OPT_TYPE_INT,		{ -1 },			INT_MIN, INT_MAX, VE },
	{ "aq-mode",						"AQ method (0=none, 1=variance, 2=autovariance, 3=autovariance_biased)",							OFFSET(x264.aq_mode),					AV_OPT_TYPE_INT,		{ -1 },			-1, 3 ,VE },
	{ "aq-strength",					"AQ strength. Reduces blocking and blurring in flat and textured areas.",							OFFSET(x264.aq_strength),				AV_OPT_TYPE_FLOAT,		{.dbl = -1 },	-1, FLT_MAX, VE },
	{ "rc-lookahead",					"Number of frames to look ahead for frametype and ratecontrol",										OFFSET(x264.rc_lookahead),				AV_OPT_TYPE_INT,		{ -1 },			-1, INT_MAX, VE },
	{ "direct-pred",					"Direct MV prediction mode (none, spatial, temporal, auto)",										OFFSET(x264.direct_pred),				AV_OPT_TYPE_STRING,		{ 0 },			0, 0,  VE, },
	{ "fast-pskip",						NULL,																								OFFSET(x264.fast_pskip),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "b-bias",							"Influences how often B-frames are used",															OFFSET(x264.b_bias),					AV_OPT_TYPE_INT,		{ -1 },			INT_MIN, INT_MAX, VE },
	{ "nal-hrd",						"Signal HRD information ""cbr not allowed in .mp4)(none, vbr,cbr)",									OFFSET(x264.nal_hrd),					AV_OPT_TYPE_STRING,		{ 0 },			0, 0, VE },
	{ "mixed-refs",						"One reference per partition, as opposed to one reference per macroblock",							OFFSET(x264.mixed_refs),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "deblock",						"Loop filter parameters, in <alpha:beta> form.",													OFFSET(x264.deblock),					AV_OPT_TYPE_STRING,		{ 0 },			0, 0, VE },
	{ "mbtree",							"Use macroblock tree ratecontrol.",																	OFFSET(x264.mbtree),					AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "avcintra-class",					"AVC-Intra class 50/100/200",																		OFFSET(x264.avcintra_class),			AV_OPT_TYPE_INT,		{ -1 },			-1, 200 , VE },
	{ "deterministic",					"Operate in base encoder in deterministic mode",													OFFSET(x264.deterministic),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "crf",							"Set x264 crf",																						OFFSET(x264.crf),						AV_OPT_TYPE_FLOAT,		{.dbl = -1 },	-1, FLT_MAX, VE },
	{ "deadzone-intra",					"Set deadzone parameters for DCT coefficient rounding format for intra",							OFFSET(x264.deadzone_intra),			AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "deadzone-inter",					"Set deadzone parameters for DCT coefficient rounding format for inter",							OFFSET(x264.deadzone_inter),			AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ "dct-decimate",					"decimate P-blocks that are extremely close to empty of coefficents.",								OFFSET(x264.dct_decimate),				AV_OPT_TYPE_INT,		{ -1 },			-1, 1, VE },
	{ NULL },
};

static const AVClass pplusenc_x264_class =
{
	.class_name		= "pplusenc_x264",
	.option			= options_x264,
	.item_name		= av_default_item_name,
	.version		= LIBAVUTIL_VERSION_INT,
};

AVCodec ff_pplusenc_x264_encoder =
{
	.name				= "pplusenc_x264",
	.long_name			= NULL_IF_CONFIG_SMALL("P.Plus Encoder / x264 Base Encoder"),
	.type				= AVMEDIA_TYPE_VIDEO,
	.id					= AV_CODEC_ID_H264,		/* Registered as H.264 so that other components treat the output as such. */
	.init				= pplus_enc_init_x264,
	.send_frame 		= pplus_enc_send_frame,
	.receive_packet 	= pplus_enc_receive_packet,
	.close				= pplus_enc_close,
	.priv_data_size		= sizeof(PPlusEncContext),
	.priv_class			= &pplusenc_x264_class,
	.defaults			= pplusenc_defaults,
	.capabilities		= AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
	.caps_internal		= FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
	.pix_fmts			= pplusenc_pix_fmts,
};

#endif

#if CONFIG_PPLUSENC_H264_ENCODER

static const AVOption options_h264[] =
{
	ILOPTIONS

	{ "base_encoder", "Specify the base encoder the IL should use", OFFSET(il.base_encoder), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
	{ NULL },
};

static const AVClass pplusenc_h264_class =
{
	.class_name = "pplusenc_h264",
	.option = options_h264,
	.item_name = av_default_item_name,
	.version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_pplusenc_h264_encoder =
{
	.name				= "pplusenc_h264",
	.long_name			= NULL_IF_CONFIG_SMALL("P.Plus Encoder / H.264 Generic Base Encoder"),
	.type				= AVMEDIA_TYPE_VIDEO,
	.id					= AV_CODEC_ID_H264,		/* Registered as H.264 so that other components treat the output as such. */
	.init				= pplus_enc_init_generic_base,
	.send_frame			= pplus_enc_send_frame,
	.receive_packet		= pplus_enc_receive_packet,
	.close				= pplus_enc_close,
	.priv_data_size		= sizeof(PPlusEncContext),
	.priv_class			= &pplusenc_h264_class,
	.defaults			= pplusenc_defaults,
	.capabilities		= AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
	.caps_internal		= FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
	.pix_fmts			= pplusenc_pix_fmts,
};

#endif

#if CONFIG_PPLUSENC_HEVC_ENCODER

static const AVOption options_hevc[] =
{
	ILOPTIONS

	{ "base_encoder", "Specify the base encoder the IL should user", OFFSET(il.base_encoder), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
	{ NULL },
};

static const AVClass pplusenc_hevc_class =
{
	.class_name = "pplusenc_hevc",
	.option = options_hevc,
	.item_name = av_default_item_name,
	.version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_pplusenc_hevc_encoder =
{
	.name				= "pplusenc_hevc",
	.long_name			= NULL_IF_CONFIG_SMALL("P.Plus Encoder / HEVC Generic Base Encoder"),
	.type				= AVMEDIA_TYPE_VIDEO,
	.id					= AV_CODEC_ID_HEVC,		/* Registered as H.264 so that other components treat the output as such. */
	.init				= pplus_enc_init_generic_base,
	.send_frame			= pplus_enc_send_frame,
	.receive_packet		= pplus_enc_receive_packet,
	.close				= pplus_enc_close,
	.priv_data_size		= sizeof(PPlusEncContext),
	.priv_class			= &pplusenc_hevc_class,
	.defaults			= pplusenc_defaults,
	.capabilities		= AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
	.caps_internal		= FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
	.pix_fmts			= pplusenc_pix_fmts,
};

#endif
