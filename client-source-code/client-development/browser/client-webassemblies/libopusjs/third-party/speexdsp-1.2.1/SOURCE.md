# Source provenance

Official SpeexDSP 1.2.1 release:
https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz

SHA-256: `8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`

Retained unchanged: COPYING, AUTHORS, libspeexdsp/resample.c,
libspeexdsp/arch.h and include/speex/speex_resampler.h. The OUTSIDE_SPEEX,
FLOATING_POINT and RANDOM_PREFIX=speex configuration makes this a standalone
resampler. Other DSP components and build systems are not required.
