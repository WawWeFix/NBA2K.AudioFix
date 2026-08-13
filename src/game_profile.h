#ifndef NBA2K_AUDIO_FIX_GAME_PROFILE_H
#define NBA2K_AUDIO_FIX_GAME_PROFILE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

#include "pcm_asset.h"

typedef enum GameProfileId {
    GAME_PROFILE_NONE = 0,
    GAME_PROFILE_NBA2K11 = 1,
} GameProfileId;

typedef struct GameProfile {
    GameProfileId id;
    const WCHAR *name;
    const WCHAR *display_name;
    DWORD expected_image_size;
    DWORD xaudio2_global_rva;
    const float *pcm_gains;
    size_t pcm_gain_count;
} GameProfile;

const GameProfile *game_profile_load(HMODULE plugin_module);
const GameProfile *game_profile_active(void);
float game_profile_pcm_gain(PcmAssetKind asset);

#endif
