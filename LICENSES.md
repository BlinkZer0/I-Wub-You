# Licences and credits

**I Wub You** (this repository): no licence file has been added yet, so the usual default applies: the author
keeps all rights to the plugin's own code, UI, presets and documentation. Add a `LICENSE` file here to change that.

The binaries are built with and contain:

- **JUCE 8.0.17** (juce.com): the plugin framework, the VST3 wrapper and the Standalone host. JUCE 8 is available
  under its own licence terms (the JUCE End User Licence Agreement, with an AGPLv3 option for open-source
  projects); the terms that apply to a distribution of this plugin are JUCE's.
- **VST3 SDK** (Steinberg Media Technologies GmbH): through JUCE's VST3 wrapper. VST is a trademark of Steinberg
  Media Technologies GmbH.
- **Half-band filter design**: the polyphase IIR coefficients come from the Valenzuela-Constantinides design as
  published in Laurent de Soras' HIIR library (WTFPL), re-implemented in `vst/Source/dsp/Oversampler.h`.
- **WebView2** (Microsoft): the plugin's UI runs in the system's WebView2 runtime, which stays Microsoft's.

Everything else in the signal chain (the zero-delay-feedback state-variable filter, the Linkwitz-Riley split,
the YIN pitch tracker, the PolyBLEP oscillators, the true-peak limiter) is implemented from the published
techniques in this repository's own code.
