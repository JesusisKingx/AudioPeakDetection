# Audio Peak Detector Notes

The plug-in now performs KissFFT-based spectral-flux onset detection. Audio is converted to mono, analyzed with 2048-sample Hann windows at 50% overlap, and peaks are selected where the flux rises above an adaptive threshold. Detection controls appear alongside the effect: **Min Separation (sec)** enforces minimum spacing between peaks, **Threshold Multiplier** adjusts the adaptive gate, and **Smoothing (%)** blends the flux curve before thresholding. High-energy hits normalised above 75% receive blue "AudioPeak" markers, otherwise markers are purple so quieter beats remain distinguishable. No external DLLs are required; KissFFT sources are compiled directly into the effect.

## Building

1. Launch Visual Studio from the After Effects 25.5 SDK command prompt so the environment variables (e.g. `AE_PLUGIN_BUILD_DIR`) are populated.
2. Open `Win/AudioPeakDetection.sln` and select either **Debug** or **Release** with the **x64** platform. 32-bit builds are not supported by the AE 25.5 host.
3. Build the `AudioPeakDetection` project. The post-build step drops `AudioPeakDetection.aex` into `$(AE_PLUGIN_BUILD_DIR)`.
4. Copy the generated `.aex` into the After Effects Plug-ins folder (e.g. `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\Audio`), or point `AE_PLUGIN_BUILD_DIR` at that directory before building.
5. If After Effects has cached an older build, start AE while holding **Ctrl+Alt+Shift** (Windows) to clear preferences, then allow it to rebuild the plug-in cache on launch.

## Verifying in After Effects

1. Launch After Effects 25.5 and create a composition containing an audio layer.
2. Apply **Audio Peak Detector** to a solid or adjustment layer and assign the **Audio Source** parameter to the audio layer.
3. Click **Analyze Audio**. The Info panel reports progress and the return message confirms how many transients were found.
4. Click **Create Markers** to inject markers on the analyzed layer; louder hits are labelled blue, quieter hits purple, and each marker carries an "AudioPeak" comment with the normalized amplitude.
