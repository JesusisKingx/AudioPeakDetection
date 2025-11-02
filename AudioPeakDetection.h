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

#pragma once

#ifndef AUDIO_PEAK_DETECTION_H
#define AUDIO_PEAK_DETECTION_H

#include "AEConfig.h"

#ifdef AE_OS_WIN
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "Param_Utils.h"
#include "String_Utils.h"

#include "AudioPeakDetection_Strings.h"

#include <vector>

#ifndef DllExport
#define DllExport __declspec(dllexport)
#endif

/* Versioning information */
#define MAJOR_VERSION 1
#define MINOR_VERSION 3
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

/* Parameter defaults */
#define AudioPeakDetection_MIN_SEPARATION_MIN 0.05
#define AudioPeakDetection_MIN_SEPARATION_MAX 0.50
#define AudioPeakDetection_MIN_SEPARATION_DFLT 0.12

#define AudioPeakDetection_THRESHOLD_MULTIPLIER_MIN 1.0
#define AudioPeakDetection_THRESHOLD_MULTIPLIER_MAX 3.0
#define AudioPeakDetection_THRESHOLD_MULTIPLIER_DFLT 1.5

#define AudioPeakDetection_SMOOTHING_MIN 0.0
#define AudioPeakDetection_SMOOTHING_MAX 100.0
#define AudioPeakDetection_SMOOTHING_DFLT 30.0

#define AudioPeakDetection_LOUDNESS_THRESHOLD_PERCENT 75.0

enum {
    AudioPeakDetection_INPUT = 0,
    AudioPeakDetection_DETECTION_GROUP_START,
    AudioPeakDetection_MIN_SEPARATION,
    AudioPeakDetection_THRESHOLD_MULTIPLIER,
    AudioPeakDetection_SMOOTHING,
    AudioPeakDetection_DETECTION_GROUP_END,
    AudioPeakDetection_ANALYZE_BUTTON,
    AudioPeakDetection_CREATE_MARKERS_BUTTON,
    AudioPeakDetection_NUM_PARAMS
};

static_assert(AudioPeakDetection_CREATE_MARKERS_BUTTON + 1 == AudioPeakDetection_NUM_PARAMS,
        "Parameter enumeration and count are out of sync.");

enum {
    AUDIO_PEAK_DETECTOR_GROUP_START_DISK_ID = 1,
    AUDIO_PEAK_DETECTOR_MIN_SEPARATION_DISK_ID,
    AUDIO_PEAK_DETECTOR_THRESHOLD_MULTIPLIER_DISK_ID,
    AUDIO_PEAK_DETECTOR_SMOOTHING_DISK_ID,
    AUDIO_PEAK_DETECTOR_GROUP_END_DISK_ID,
    AUDIO_PEAK_DETECTOR_ANALYZE_BUTTON_DISK_ID,
    AUDIO_PEAK_DETECTOR_CREATE_MARKERS_BUTTON_DISK_ID
};

struct PeakMarker {
    A_Time time{};
    PF_FpShort amplitude = 0;
    A_Boolean is_loud = FALSE;
};

struct AnalysisState {
    PF_Boolean has_analyzed = FALSE;
    std::vector<PeakMarker> peaks;
};

extern "C" {

DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCBPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion);

DllExport PF_Err EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra);

} // extern "C"

#endif // AUDIO_PEAK_DETECTION_H
