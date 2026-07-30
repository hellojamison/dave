// SPDX-License-Identifier: GPL-3.0-or-later
// dr_wav's implementation lives in AudioClipNode.cpp in the app build. The
// tests link Edit.cpp (which decodes WAVs) but not AudioClipNode.cpp, so the
// definitions are emitted here instead. Pulling in AudioClipNode.cpp would drag
// the node graph and miniaudio's CoreAudio backend into a headless test binary.
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
