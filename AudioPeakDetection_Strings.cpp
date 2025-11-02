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
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

#include "AudioPeakDetection.h"

typedef struct {
	A_u_long	index;
	A_char		str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
	StrID_NONE,						"",
	StrID_Name,						"Audio Peak Detector",
	StrID_Description,				"KissFFT spectral-flux peak detection for After Effects.\rCopyright 2023-2025",
	StrID_Analyze_Button_Name,		"Analyze Audio",
	StrID_Create_Markers_Button_Name, "Create Markers",
	StrID_Detection_Group_Name,    "Detection Settings",
	StrID_Min_Gap_Slider_Name,     "Min Peak Separation (sec)",
	StrID_Threshold_Multiplier_Slider_Name, "Adaptive Threshold Multiplier",
	StrID_Smoothing_Slider_Name, "Smoothing (%)",
};

extern "C" {
	char* GetStringPtr(int strNum)
	{
		return g_strs[strNum].str;
	}
}
