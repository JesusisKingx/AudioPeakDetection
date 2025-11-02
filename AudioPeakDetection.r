/* AudioPeakDetectionPiPL.r  –  single‑source PiPL, ID 16000 */

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

        /* [10] global out‑flags: deep‑color + audio */
        AE_Effect_Global_OutFlags { 0x42000000 },
        AE_Effect_Global_OutFlags_2 { 0 },

        /* [11] match‑name */
        AE_Effect_Match_Name { "ADBE AudioPeakDetector" },

        /* [12] reserved */
        AE_Reserved_Info { 0 },

        /* [13] support URL */
        AE_Effect_Support_URL { "https://www.adobe.com" }
    }
};
