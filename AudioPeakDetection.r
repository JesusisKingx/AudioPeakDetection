/* AudioPeakDetectionPiPL.r  –  single-source PiPL, ID 16000 */

#include "AEConfig.h"

/* Literal flag values mirrored from AE_Effect.h so the resource
   script remains self-contained and does not require heavyweight headers. */
#define PF_OutFlag_WIDE_TIME_INPUT                  (1L << 1)
#define PF_OutFlag_I_USE_AUDIO                      (1L << 20)
#define PF_OutFlag_AUDIO_FLOAT_ONLY                 (1L << 27)
#define PF_OutFlag_AUDIO_EFFECT_TOO                 (1L << 30)
#define PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG (1L << 3)

resource 'PiPL' (16000)
{
    {
        /* [1] kind */
        Kind { AEEffect },

        /* [2] UI name */
        Name { "Audio Peak Detector" },

        /* [3] category */
        Category { "Audio" },

#ifdef AE_OS_WIN
    #ifdef AE_PROC_INTELx64
        CodeWin64X86 { "EffectMain" },
    #endif
#else
    #ifdef AE_OS_MAC
        CodeMacIntel64 { "EffectMain" },
        CodeMacARM64  { "EffectMain" },
    #endif
#endif

        /* [6] PiPL format version */
        AE_PiPL_Version { 2, 0 },

        /* [7] effect‑spec version — MUST equal code (1.3) */
        AE_Effect_Spec_Version { 1, 3 },

        /* [8] effect version word (1 << 16 | 3) */
        AE_Effect_Version { 65539 },

        /* [9] info flags */
        AE_Effect_Info_Flags { 0 },

        /* [10] global out‑flags: audio effect with float audio + wide time */
        AE_Effect_Global_OutFlags {
            PF_OutFlag_AUDIO_EFFECT_TOO |
            PF_OutFlag_AUDIO_FLOAT_ONLY |
            PF_OutFlag_WIDE_TIME_INPUT |
            PF_OutFlag_I_USE_AUDIO
        },
        AE_Effect_Global_OutFlags_2 { PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG },

        /* [11] match‑name */
        AE_Effect_Match_Name { "ADBE AudioPeakDetector" },

        /* [12] reserved */
        AE_Reserved_Info { 0 },

        /* [13] support URL */
        AE_Effect_Support_URL { "https://www.adobe.com" }
    }
};
