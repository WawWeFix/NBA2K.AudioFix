#ifndef NBA2K_AUDIO_DECODER_H
#define NBA2K_AUDIO_DECODER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "game_profile.h"

void audio_decoder_set_module(HMODULE module);
void audio_decoder_set_game_profile(GameProfileId profile);
int audio_decoder_decode_track(
    const WCHAR *asset_name, int track_index,
    int loop_requested, HANDLE output);

#endif
