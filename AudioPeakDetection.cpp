/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2023 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/*                                                                 */
/*******************************************************************/

#include "AudioPeakDetection.h"

#include "AE_EffectVers.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <vector>

#define KISS_FFT_STATIC 1
extern "C" {
#include "kiss_fftr.h"
}

namespace {

constexpr PF_UFixed kPreferredSampleRate = 0xAC440000; // 44.1 kHz, 16.16 fixed
constexpr int kFFTSize = 2048;
constexpr int kHopSize = kFFTSize / 2;
constexpr int kThresholdWindow = 8;
constexpr PF_FpLong kLoudnessThreshold = AudioPeakDetection_LOUDNESS_THRESHOLD_PERCENT;
constexpr A_long kProgressMax = 100;

static AEGP_PluginID g_my_plugin_id = 0;

inline PF_Err ReportProgress(PF_InData* in_data, A_long current, A_long total)
{
	return (*(in_data->inter.progress))(in_data->effect_ref, current, total);
}

inline PF_Err AbortRequested(PF_InData* in_data)
{
	return (*(in_data->inter.abort))(in_data->effect_ref);
}

inline PF_Err CheckoutParam(PF_InData* in_data,
	A_long index,
	A_long time,
	A_long time_step,
	A_long time_scale,
	PF_ParamDef* paramP)
{
	return (*(in_data->inter.checkout_param))(
		in_data->effect_ref,
		index,
		time,
		time_step,
		time_scale,
		paramP);
}

inline PF_Err CheckinParam(PF_InData* in_data, PF_ParamDef* paramP)
{
	return (*(in_data->inter.checkin_param))(in_data->effect_ref, paramP);
}

inline PF_Err CheckoutLayerAudio(PF_InData* in_data,
	A_long index,
	A_long start_time,
	A_long duration,
	A_long time_scale,
	PF_UFixed sample_rate,
	PF_SoundSampleSize bytes_per_sample,
	PF_SoundChannels channels,
	PF_SoundFormat format,
	PF_LayerAudio* audioP)
{
	return (*(in_data->inter.checkout_layer_audio))(
		in_data->effect_ref,
		index,
		start_time,
		duration,
		time_scale,
		sample_rate,
		bytes_per_sample,
		channels,
		format,
		audioP);
}

inline PF_Err CheckinLayerAudio(PF_InData* in_data, PF_LayerAudio audio)
{
	return (*(in_data->inter.checkin_layer_audio))(in_data->effect_ref, audio);
}

inline PF_Err GetAudioData(PF_InData* in_data,
	PF_LayerAudio audio,
	PF_SndSamplePtr* dataP,
	A_long* sample_framesP,
	PF_UFixed* sample_rateP,
	A_long* bytes_per_sampleP,
	A_long* channel_countP,
	A_long* format_flagP)
{
	return (*(in_data->inter.get_audio_data))(
		in_data->effect_ref,
		audio,
		dataP,
		sample_framesP,
		sample_rateP,
		bytes_per_sampleP,
		channel_countP,
		format_flagP);
}

inline PF_Err AddParam(PF_InData* in_data, A_long index, PF_ParamDef* defP)
{
	return (*(in_data->inter.add_param))(in_data->effect_ref, index, defP);
}

template <typename T>
inline T ClampValue(const T& value, const T& minimum, const T& maximum)
{
	return std::min(std::max(value, minimum), maximum);
}

PF_Handle GetStateHandle(PF_InData* in_data, PF_OutData* out_data)
{
	if (in_data->sequence_data) {
		return reinterpret_cast<PF_Handle>(in_data->sequence_data);
	}
	return reinterpret_cast<PF_Handle>(out_data ? out_data->sequence_data : nullptr);
}

AnalysisState* GetState(PF_InData* in_data, PF_OutData* out_data)
{
	const PF_Handle handle = GetStateHandle(in_data, out_data);
	if (!handle) {
		return nullptr;
	}
	return reinterpret_cast<AnalysisState*>(*handle);
}

std::vector<float> CreateHannWindow()
{
	std::vector<float> window(kFFTSize);
	constexpr float two_pi = 6.283185307179586476925f;
	for (int n = 0; n < kFFTSize; ++n) {
		window[n] = 0.5f - 0.5f * std::cos(two_pi * static_cast<float>(n) / static_cast<float>(kFFTSize - 1));
	}
	return window;
}

void SmoothFlux(const std::vector<float>& in_flux,
	float smoothing_percent,
	std::vector<float>& out_flux)
{
	out_flux = in_flux;
	if (in_flux.empty()) {
		return;
	}

	const int max_radius = 10;
	const float clamped_percent = ClampValue(smoothing_percent, 0.0f, 100.0f);
	const int radius = ClampValue(static_cast<int>(std::round((clamped_percent / 100.0f) * static_cast<float>(max_radius))),
		0,
		max_radius);
	if (radius == 0) {
		return;
	}

	std::vector<float> smoothed(in_flux.size(), 0.0f);
	for (size_t i = 0; i < in_flux.size(); ++i) {
		const size_t start = (i <= static_cast<size_t>(radius)) ? 0 : i - static_cast<size_t>(radius);
		const size_t end = std::min(in_flux.size() - 1, i + static_cast<size_t>(radius));

		float sum = 0.0f;
		for (size_t j = start; j <= end; ++j) {
			sum += in_flux[j];
		}
		smoothed[i] = sum / static_cast<float>(end - start + 1);
	}

	out_flux.swap(smoothed);
}

} // namespace

/* ------------------------------------------------------------- About */
static PF_Err About(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	suites.ANSICallbacksSuite1()->sprintf(
		out_data->return_msg,
		"%s v%d.%d\r%s",
		STR(StrID_Name),
		MAJOR_VERSION,
		MINOR_VERSION,
		STR(StrID_Description));

	return PF_Err_NONE;
}

/* ------------------------------------------------------ GlobalSetup */
static PF_Err GlobalSetup(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	PF_Err err = PF_Err_NONE;

	out_data->my_version = PF_VERSION(
		MAJOR_VERSION,
		MINOR_VERSION,
		BUG_VERSION,
		STAGE_VERSION,
		BUILD_VERSION);

	out_data->out_flags = PF_OutFlag_WIDE_TIME_INPUT |
		PF_OutFlag_I_USE_AUDIO |
		PF_OutFlag_AUDIO_EFFECT_TOO |
		PF_OutFlag_AUDIO_FLOAT_ONLY;

	out_data->out_flags2 |= PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG;

	g_my_plugin_id = 0;

	return err;
}

/* ----------------------------------------------------- ParamsSetup */
static PF_Err ParamsSetup(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	PF_Err err = PF_Err_NONE;
	PF_ParamDef def{};

	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("Audio Source", PF_LayerDefault_MYSELF, 0);

	AEFX_CLR_STRUCT(def);
	def.param_type = PF_Param_GROUP_START;
	PF_STRNNCPY(def.name, STR(StrID_Detection_Group_Name), sizeof(def.name));
	def.flags = PF_ParamFlag_COLLAPSE_TWIRLY | PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_SUPERVISE;
	def.uu.id = AUDIO_PEAK_DETECTOR_GROUP_START_DISK_ID;
	err = AddParam(in_data, -1, &def);
	if (err != PF_Err_NONE) {
		return err;
	}

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(STR(StrID_Min_Gap_Slider_Name),
		AudioPeakDetection_MIN_SEPARATION_MIN,
		AudioPeakDetection_MIN_SEPARATION_MAX,
		AudioPeakDetection_MIN_SEPARATION_MIN,
		AudioPeakDetection_MIN_SEPARATION_MAX,
		AudioPeakDetection_MIN_SEPARATION_DFLT,
		PF_Precision_HUNDREDTHS,
		0,
		PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_SUPERVISE,
		AUDIO_PEAK_DETECTOR_MIN_SEPARATION_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(STR(StrID_Threshold_Multiplier_Slider_Name),
		AudioPeakDetection_THRESHOLD_MULTIPLIER_MIN,
		AudioPeakDetection_THRESHOLD_MULTIPLIER_MAX,
		AudioPeakDetection_THRESHOLD_MULTIPLIER_MIN,
		AudioPeakDetection_THRESHOLD_MULTIPLIER_MAX,
		AudioPeakDetection_THRESHOLD_MULTIPLIER_DFLT,
		PF_Precision_HUNDREDTHS,
		0,
		PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_SUPERVISE,
		AUDIO_PEAK_DETECTOR_THRESHOLD_MULTIPLIER_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(STR(StrID_Smoothing_Slider_Name),
		AudioPeakDetection_SMOOTHING_MIN,
		AudioPeakDetection_SMOOTHING_MAX,
		AudioPeakDetection_SMOOTHING_MIN,
		AudioPeakDetection_SMOOTHING_MAX,
		static_cast<PF_FpShort>(AudioPeakDetection_SMOOTHING_DFLT),
	PF_Precision_TENTHS,
	PF_ValueDisplayFlag_PERCENT,
	PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_SUPERVISE,
	AUDIO_PEAK_DETECTOR_SMOOTHING_DISK_ID);

	AEFX_CLR_STRUCT(def);
	def.param_type = PF_Param_GROUP_END;
	PF_STRNNCPY(def.name, STR(StrID_Detection_Group_Name), sizeof(def.name));
	def.flags = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_SUPERVISE;
	def.uu.id = AUDIO_PEAK_DETECTOR_GROUP_END_DISK_ID;
	err = AddParam(in_data, -1, &def);
	if (err != PF_Err_NONE) {
		return err;
	}

	AEFX_CLR_STRUCT(def);
	PF_ADD_BUTTON(STR(StrID_Analyze_Button_Name),
		STR(StrID_Analyze_Button_Name),
		0,
		PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY,
		AUDIO_PEAK_DETECTOR_ANALYZE_BUTTON_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_BUTTON(STR(StrID_Create_Markers_Button_Name),
		STR(StrID_Create_Markers_Button_Name),
		0,
		PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY,
		AUDIO_PEAK_DETECTOR_CREATE_MARKERS_BUTTON_DISK_ID);

	out_data->num_params = AudioPeakDetection_NUM_PARAMS;
	return err;
}

/* ---------------------------------------------------- SequenceSetup */
static PF_Err SequenceSetup(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	PF_Handle state_handle = PF_NEW_HANDLE(sizeof(AnalysisState));
	if (!state_handle) {
		return PF_Err_OUT_OF_MEMORY;
	}

	void* memory = *state_handle;
	new (memory) AnalysisState();
	out_data->sequence_data = state_handle;
	return PF_Err_NONE;
}

static PF_Err SequenceResetup(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	if (!in_data->sequence_data) {
		return SequenceSetup(in_data, out_data, params, output);
	}
	return PF_Err_NONE;
}

static PF_Err SequenceSetdown(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output)
{
	PF_Handle state_handle = reinterpret_cast<PF_Handle>(in_data->sequence_data);
	if (state_handle) {
		AnalysisState* state = reinterpret_cast<AnalysisState*>(*state_handle);
		state->~AnalysisState();
		(*in_data->utils->host_dispose_handle)(state_handle);
		in_data->sequence_data = nullptr;
	}
	return PF_Err_NONE;
}

static PF_Err SequenceFlatten(PF_InData* /*in_data*/,
	PF_OutData* /*out_data*/,
	PF_ParamDef* /*params*/[],
	PF_LayerDef* /*output*/)
{
	return PF_Err_NONE;
}

/* ----------------------------------------------------------- Render */
static PF_Err Render(PF_InData* /*in_data*/,
	PF_OutData* /*out_data*/,
	PF_ParamDef* /*params*/[],
	PF_LayerDef* /*output*/)
{
	// Visual render path is a no-op.
	return PF_Err_NONE;
}

/* -------------------------------------------------------- Audio path */
static PF_Err AudioSetup(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* /*params*/[],
	PF_LayerDef* /*output*/)
{
	out_data->start_sampL = 0;
	out_data->dur_sampL = in_data->total_sampL;
	return PF_Err_NONE;
}

static PF_Err AudioRender(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* /*params*/[],
	PF_LayerDef* /*output*/)
{
	// Pass-through to keep host audio intact.
	out_data->dest_snd = in_data->src_snd;
	out_data->start_sampL = in_data->start_sampL;
	out_data->dur_sampL = in_data->dur_sampL;
	return PF_Err_NONE;
}

static PF_Err AudioSetdown(PF_InData* /*in_data*/,
	PF_OutData* /*out_data*/,
	PF_ParamDef* /*params*/[],
	PF_LayerDef* /*output*/)
{
	return PF_Err_NONE;
}

/* ------------------------------------------------------- AnalyzeAudio */
static PF_Err AnalyzeAudio(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[])
{
	AnalysisState* state = GetState(in_data, out_data);
	if (!state) {
		return PF_Err_INTERNAL_STRUCT_DAMAGED;
	}

	state->peaks.clear();
	state->has_analyzed = FALSE;

	PF_Err err = ReportProgress(in_data, 0, kProgressMax);
	if (err != PF_Err_NONE) {
		return err;
	}

	PF_ParamDef audio_layer_param{};
	err = CheckoutParam(in_data,
		AudioPeakDetection_INPUT,
		in_data->current_time,
		in_data->time_step,
		in_data->time_scale,
		&audio_layer_param);
	if (err != PF_Err_NONE) {
		return err;
	}

	const PF_Boolean has_audio_layer = (audio_layer_param.u.ld.data != nullptr);
	err = CheckinParam(in_data, &audio_layer_param);
	if (err != PF_Err_NONE) {
		return err;
	}

	if (!has_audio_layer) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Please choose an audio layer.");
		}
		return PF_Err_NONE;
	}

	PF_LayerAudio audio = nullptr;
	PF_SndSamplePtr audio_data = nullptr;
	A_long sample_frames = 0;
	PF_UFixed sample_rate_fixed = 0;
	A_long bytes_per_sample = 0;
	A_long channel_count = 0;
	A_long format_flag = 0;

	const A_long start_timeL = 0;
	A_long durationL = in_data->total_time;
	if (durationL <= 0) {
		durationL = (in_data->time_step > 0) ? in_data->time_step : in_data->time_scale;
	}
	if (durationL <= 0) {
		durationL = in_data->time_scale;
	}

	err = CheckoutLayerAudio(in_data,
		AudioPeakDetection_INPUT,
		start_timeL,
		durationL,
		in_data->time_scale,
		kPreferredSampleRate,
		PF_SSS_4,
		PF_Channels_STEREO,
		PF_SIGNED_FLOAT,
		&audio);
	if (err != PF_Err_NONE) {
		return err;
	}

	auto cleanup_audio = [&](PF_Err status) -> PF_Err {
		if (audio) {
			const PF_Err checkin_err = CheckinLayerAudio(in_data, audio);
			audio = nullptr;
			if (status == PF_Err_NONE && checkin_err != PF_Err_NONE) {
				status = checkin_err;
			}
		}
		return status;
	};

	err = GetAudioData(in_data,
		audio,
		&audio_data,
		&sample_frames,
		&sample_rate_fixed,
		&bytes_per_sample,
		&channel_count,
		&format_flag);
	if (err != PF_Err_NONE) {
		return cleanup_audio(err);
	}

	if (!audio_data || sample_frames <= 0 || channel_count <= 0) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Unable to access audio samples.");
		}
		PF_Err progress_err = ReportProgress(in_data, kProgressMax, kProgressMax);
		if (progress_err != PF_Err_NONE) {
			return cleanup_audio(progress_err);
		}
		return cleanup_audio(PF_Err_NONE);
	}

	if (sample_frames > 0) {
		--sample_frames;
	}

	double sample_rate = sample_rate_fixed ? static_cast<double>(sample_rate_fixed) / 65536.0 : 44100.0;
	if (sample_rate <= 0.0) {
		sample_rate = 44100.0;
	}

	const float min_separation_seconds = static_cast<float>(params[AudioPeakDetection_MIN_SEPARATION]->u.fs_d.value);
	const float threshold_multiplier = static_cast<float>(params[AudioPeakDetection_THRESHOLD_MULTIPLIER]->u.fs_d.value);
	const float smoothing_percent = static_cast<float>(params[AudioPeakDetection_SMOOTHING]->u.fs_d.value);

	const bool data_is_float = (format_flag == PF_SIGNED_FLOAT) && (bytes_per_sample == PF_SSS_4);
	const bool data_is_int16 = (format_flag == PF_SIGNED_PCM) && (bytes_per_sample == PF_SSS_2);
	const bool data_is_int8 = (format_flag == PF_SIGNED_PCM) && (bytes_per_sample == PF_SSS_1);

	const float* samples_f32 = data_is_float ? reinterpret_cast<const float*>(audio_data) : nullptr;
	const int16_t* samples_i16 = data_is_int16 ? reinterpret_cast<const int16_t*>(audio_data) : nullptr;
	const int8_t* samples_i8 = data_is_int8 ? reinterpret_cast<const int8_t*>(audio_data) : nullptr;

	const size_t frame_count = static_cast<size_t>(sample_frames);
	std::vector<float> mono(frame_count, 0.0f);

	for (A_long i = 0; i < sample_frames; ++i) {
		float sum = 0.0f;
		for (A_long ch = 0; ch < channel_count; ++ch) {
			const size_t idx = static_cast<size_t>(i) * static_cast<size_t>(channel_count) + static_cast<size_t>(ch);
			float value = 0.0f;
			if (samples_f32) {
				value = samples_f32[idx];
			}
			else if (samples_i16) {
				value = static_cast<float>(samples_i16[idx]) / 32768.0f;
			}
			else if (samples_i8) {
				value = static_cast<float>(samples_i8[idx]) / 128.0f;
			}
			sum += value;
		}
		mono[static_cast<size_t>(i)] = sum / static_cast<float>(channel_count);

		if ((i & 0x3FFF) == 0) {
			err = AbortRequested(in_data);
			if (err != PF_Err_NONE) {
				return cleanup_audio(err);
			}
		}
	}

	if (mono.size() < static_cast<size_t>(kFFTSize)) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Audio layer is too short to analyze.");
		}
		PF_Err progress_err = ReportProgress(in_data, kProgressMax, kProgressMax);
		if (progress_err != PF_Err_NONE) {
			return cleanup_audio(progress_err);
		}
		return cleanup_audio(PF_Err_NONE);
	}

	const size_t num_frames = 1 + (mono.size() - kFFTSize) / kHopSize;
	if (num_frames == 0) {
		PF_Err progress_err = ReportProgress(in_data, kProgressMax, kProgressMax);
		if (progress_err != PF_Err_NONE) {
			return cleanup_audio(progress_err);
		}
		return cleanup_audio(PF_Err_NONE);
	}

	std::vector<float> flux(num_frames, 0.0f);

	using KissFftCfgPtr = std::unique_ptr<void, decltype(&free)>;
	KissFftCfgPtr fft_cfg(kiss_fftr_alloc(kFFTSize, 0, nullptr, nullptr), &free);
	if (!fft_cfg) {
		return cleanup_audio(PF_Err_OUT_OF_MEMORY);
	}
	kiss_fftr_cfg cfg = reinterpret_cast<kiss_fftr_cfg>(fft_cfg.get());

	std::vector<float> window = CreateHannWindow();
	std::vector<kiss_fft_scalar> fft_in(static_cast<size_t>(kFFTSize), 0.0f);
	std::vector<kiss_fft_cpx> fft_out(static_cast<size_t>(kFFTSize / 2 + 1));
	std::vector<float> prev_magnitude(static_cast<size_t>(kFFTSize / 2 + 1), 0.0f);
	std::vector<float> curr_magnitude(static_cast<size_t>(kFFTSize / 2 + 1), 0.0f);

	for (size_t frame = 0; frame < num_frames; ++frame) {
		const size_t start = frame * static_cast<size_t>(kHopSize);
		for (int n = 0; n < kFFTSize; ++n) {
			fft_in[static_cast<size_t>(n)] = static_cast<kiss_fft_scalar>(mono[start + static_cast<size_t>(n)] * window[static_cast<size_t>(n)]);
		}

		kiss_fftr(cfg, fft_in.data(), fft_out.data());

		float frame_flux = 0.0f;
		for (int bin = 0; bin <= kFFTSize / 2; ++bin) {
			const float re = fft_out[static_cast<size_t>(bin)].r;
			const float im = fft_out[static_cast<size_t>(bin)].i;
			curr_magnitude[static_cast<size_t>(bin)] = std::sqrt(re * re + im * im);
			const float diff = curr_magnitude[static_cast<size_t>(bin)] - prev_magnitude[static_cast<size_t>(bin)];
			if (diff > 0.0f) {
				frame_flux += diff;
			}
			prev_magnitude[static_cast<size_t>(bin)] = curr_magnitude[static_cast<size_t>(bin)];
		}

		flux[frame] = frame_flux;

		if ((frame & 0x3F) == 0) {
			err = AbortRequested(in_data);
			if (err != PF_Err_NONE) {
				return cleanup_audio(err);
			}
			const A_long progress = static_cast<A_long>(10 + (frame * 70) / std::max<size_t>(1, num_frames));
			ReportProgress(in_data, progress, kProgressMax);
		}
	}

	std::vector<float> smoothed_flux;
	SmoothFlux(flux, smoothing_percent, smoothed_flux);

	const auto max_it = std::max_element(smoothed_flux.begin(), smoothed_flux.end());
	if (max_it == smoothed_flux.end() || *max_it <= 0.0f) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: No usable transients were detected.");
		}
		PF_Err progress_err = ReportProgress(in_data, kProgressMax, kProgressMax);
		if (progress_err != PF_Err_NONE) {
			return cleanup_audio(progress_err);
		}
		return cleanup_audio(PF_Err_NONE);
	}
	const float max_flux = *max_it;

	const double frames_per_second = sample_rate / static_cast<double>(kHopSize);
	const size_t min_separation_frames = std::max<size_t>(1,
		static_cast<size_t>(std::ceil(min_separation_seconds * frames_per_second)));

	struct CandidatePeak {
		size_t frame_index = 0;
		float flux_value = 0.0f;
	};

	std::vector<CandidatePeak> candidates;
	candidates.reserve(smoothed_flux.size() / 4);

	double last_peak_frame = -static_cast<double>(min_separation_frames);
	for (size_t i = 0; i < smoothed_flux.size(); ++i) {
		const size_t window_start = (i <= static_cast<size_t>(kThresholdWindow)) ? 0 : i - static_cast<size_t>(kThresholdWindow);
		float mean = 0.0f;
		size_t count = 0;
		for (size_t j = window_start; j < i; ++j) {
			mean += smoothed_flux[j];
			++count;
		}
		if (count == 0) {
			continue;
		}
		mean /= static_cast<float>(count);
		const float adaptive_threshold = mean * threshold_multiplier;

		const bool is_local_max =
			(i == 0 || smoothed_flux[i] > smoothed_flux[i - 1]) &&
			(i + 1 == smoothed_flux.size() || smoothed_flux[i] >= smoothed_flux[i + 1]);

		if (!is_local_max || smoothed_flux[i] <= adaptive_threshold) {
			continue;
		}

		const double frame_index = static_cast<double>(i);
		const double frame_gap = frame_index - last_peak_frame;
		const float flux_value = smoothed_flux[i];

		if (frame_gap < static_cast<double>(min_separation_frames)) {
			if (!candidates.empty() && flux_value > candidates.back().flux_value) {
				candidates.back().frame_index = i;
				candidates.back().flux_value = flux_value;
				last_peak_frame = frame_index;
			}
			continue;
		}

		candidates.push_back({ i, flux_value });
		last_peak_frame = frame_index;
	}

	state->peaks.clear();
	state->peaks.reserve(candidates.size());
	for (const auto& candidate : candidates) {
		const double frame_time = static_cast<double>(candidate.frame_index * kHopSize) / sample_rate;
		const double amplitude_percent = ClampValue((candidate.flux_value / max_flux) * 100.0, 0.0, 100.0);

		PeakMarker marker;
		marker.time.scale = in_data->time_scale;
		marker.time.value = static_cast<A_long>(std::llround(frame_time * static_cast<double>(in_data->time_scale)));
		marker.amplitude = static_cast<PF_FpShort>(amplitude_percent);
		marker.is_loud = (amplitude_percent >= kLoudnessThreshold) ? TRUE : FALSE;
		state->peaks.push_back(marker);
	}

	state->has_analyzed = TRUE;

	err = ReportProgress(in_data, kProgressMax, kProgressMax);
	if (err != PF_Err_NONE) {
		return cleanup_audio(err);
	}

	if (in_data->utils) {
		in_data->utils->ansi.sprintf(out_data->return_msg,
			"AudioPeakDetector: Found %d peaks.",
			static_cast<int>(state->peaks.size()));
	}

	return cleanup_audio(PF_Err_NONE);
}

/* -------------------------------------------------------- CreateMarkers */
static PF_Err CreateMarkers(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[])
{
	AnalysisState* state = GetState(in_data, out_data);
	if (!state || !state->has_analyzed) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Run Analyze Audio before creating markers.");
		}
		return PF_Err_NONE;
	}

	if (state->peaks.empty()) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: No peaks available. Re-run analysis with different settings.");
		}
		return PF_Err_NONE;
	}

	if (g_my_plugin_id == 0) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Marker creation unavailable in this build.");
		}
		return PF_Err_NONE;
	}

	AEGP_SuiteHandler suites(in_data->pica_basicP);

	AEGP_LayerH layerH = nullptr;
	A_Err ae_err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
	if (ae_err != A_Err_NONE || !layerH) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Unable to access effect layer.");
		}
		return ae_err;
	}

	AEGP_StreamRefH marker_streamH = nullptr;
	ae_err = suites.StreamSuite6()->AEGP_GetNewLayerStream(
		g_my_plugin_id,
		layerH,
		AEGP_LayerStream_MARKER,
		&marker_streamH);
	if (ae_err != A_Err_NONE || !marker_streamH) {
		if (in_data->utils) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Layer does not support markers.");
		}
		if (marker_streamH) {
			suites.StreamSuite6()->AEGP_DisposeStream(marker_streamH);
		}
		return ae_err;
	}

	int marker_count = 0;
	int loud_count = 0;
	int quiet_count = 0;

	for (const PeakMarker& peak : state->peaks) {
		AEGP_MarkerValP markerP = nullptr;
		ae_err = suites.MarkerSuite3()->AEGP_NewMarker(&markerP);
		if (ae_err != A_Err_NONE || !markerP) {
			continue;
		}

		ae_err = suites.MarkerSuite3()->AEGP_SetMarkerLabel(markerP, peak.is_loud ? 1 : 4);
		if (ae_err == A_Err_NONE) {
			if (peak.is_loud) {
				++loud_count;
			}
			else {
				++quiet_count;
			}
		}

		char comment[128];
		std::snprintf(comment, sizeof(comment), "AudioPeak: %.1f", static_cast<double>(peak.amplitude));

			A_u_short unicode_comment[128] = {};
		const size_t length = std::min(sizeof(unicode_comment) / sizeof(unicode_comment[0]) - 1, std::strlen(comment));
		for (size_t i = 0; i < length; ++i) {
			unicode_comment[i] = static_cast<A_u_short>(comment[i]);
		}
		unicode_comment[length] = 0;

		suites.MarkerSuite3()->AEGP_SetMarkerString(
			markerP,
			AEGP_MarkerString_COMMENT,
			unicode_comment,
			static_cast<A_long>(length));

		A_long keyframe_index = 0;
		ae_err = suites.KeyframeSuite5()->AEGP_InsertKeyframe(
			marker_streamH,
			AEGP_LTimeMode_LayerTime,
			&peak.time,
			&keyframe_index);
		if (ae_err != A_Err_NONE) {
			suites.MarkerSuite3()->AEGP_DisposeMarker(markerP);
			continue;
		}

		AEGP_StreamValue2 stream_value{};
		stream_value.streamH = marker_streamH;
		stream_value.val.markerP = markerP;

		ae_err = suites.KeyframeSuite5()->AEGP_SetKeyframeValue(marker_streamH, keyframe_index, &stream_value);
		suites.MarkerSuite3()->AEGP_DisposeMarker(markerP);
		if (ae_err == A_Err_NONE) {
			++marker_count;
		}
	}

	suites.StreamSuite6()->AEGP_DisposeStream(marker_streamH);

	if (in_data->utils) {
		if (marker_count > 0) {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: Created %d markers (%d loud, %d quiet).",
				marker_count,
				loud_count,
				quiet_count);
		}
		else {
			in_data->utils->ansi.sprintf(out_data->return_msg,
				"AudioPeakDetector: No markers were created.");
		}
	}

	return PF_Err_NONE;
}

/* --------------------------------------------------- UserChangedParam */
static PF_Err UserChangedParam(PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output,
	PF_UserChangedParamExtra* extra)
{
	PF_Err err = PF_Err_NONE;

	switch (extra->param_index) {
	case AudioPeakDetection_ANALYZE_BUTTON:
		err = AnalyzeAudio(in_data, out_data, params);
		out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
		break;
	case AudioPeakDetection_CREATE_MARKERS_BUTTON:
		err = CreateMarkers(in_data, out_data, params);
		out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
		break;
	default:
		break;
	}

	return err;
}

/* ------------------------------------------------- PluginEntryPoints */
extern "C" DllExport PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCBPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;

	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCBPtr,
		"Audio Peak Detector",
		"ADBE AudioPeakDetector",
		"Audio",
		AE_RESERVED_INFO,
		"EffectMain",
		"https://www.adobe.com");

	return result;
}

extern "C" DllExport PF_Err EffectMain(
	PF_Cmd cmd,
	PF_InData* in_data,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output,
	void* extra)
{
	PF_Err err = PF_Err_NONE;

	try {
		switch (cmd) {
		case PF_Cmd_ABOUT:
			err = About(in_data, out_data, params, output);
			break;
		case PF_Cmd_GLOBAL_SETUP:
			err = GlobalSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_PARAMS_SETUP:
			err = ParamsSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_SEQUENCE_SETUP:
			err = SequenceSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_SEQUENCE_RESETUP:
			err = SequenceResetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_SEQUENCE_FLATTEN:
			err = SequenceFlatten(in_data, out_data, params, output);
			break;
		case PF_Cmd_SEQUENCE_SETDOWN:
			err = SequenceSetdown(in_data, out_data, params, output);
			break;
		case PF_Cmd_RENDER:
			err = Render(in_data, out_data, params, output);
			break;
		case PF_Cmd_AUDIO_SETUP:
			err = AudioSetup(in_data, out_data, params, output);
			break;
		case PF_Cmd_AUDIO_RENDER:
			err = AudioRender(in_data, out_data, params, output);
			break;
		case PF_Cmd_AUDIO_SETDOWN:
			err = AudioSetdown(in_data, out_data, params, output);
			break;
		case PF_Cmd_USER_CHANGED_PARAM:
			err = UserChangedParam(in_data, out_data, params, output, reinterpret_cast<PF_UserChangedParamExtra*>(extra));
			break;
		default:
			break;
		}
	}
	catch (...) {
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
	}

	return err;
}
