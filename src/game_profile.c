#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <wchar.h>

#include "game_profile.h"

#define AUDIO_FIX_INI_NAME L"NBA2K.AudioFix.ini"

static const float k_nba2k11_pcm_gains[PCM_ASSET_COUNT] = {
    [PCM_ASSET_LOADING_SEQUENCE] = 0.916088882f,
    [PCM_ASSET_MENTOR] = 0.907440869f,
    [PCM_ASSET_MENTOR_TS] = 0.911649232f,
    [PCM_ASSET_LOADING_VO] = 0.939140139f,
    [PCM_ASSET_LOADING_VO_PS] = 0.944077615f,
    [PCM_ASSET_LOADING_VO_TS] = 0.941141552f,
    [PCM_ASSET_LINES_CS] = 0.897606612f,
    [PCM_ASSET_ENV_AMB] = 0.964303473f,
    [PCM_ASSET_OVERLAY_AUDIO] = 0.852330315f,
    [PCM_ASSET_CAIRBALL] = 0.882375583f,
    [PCM_ASSET_CWD_LOOP] = 0.903490817f,
    [PCM_ASSET_COACHES] = 0.905904179f,
    [PCM_ASSET_TEAMS] = 0.899913465f,
    [PCM_ASSET_PLAYERS] = 0.909203233f,
    [PCM_ASSET_PRESS_CONF] = 0.886124992f,
    [PCM_ASSET_PA_PLAYERS] = 0.891741633f,
    [PCM_ASSET_LINES] = 0.897362970f,
    [PCM_ASSET_LINES_PS] = 0.902000121f,
    [PCM_ASSET_LINES_TS] = 0.903751050f,
    [PCM_ASSET_PA_LINES] = 0.888244485f,
    [PCM_ASSET_CWD_STR_LOOP_INSIDE] = 0.945994120f,
    [PCM_ASSET_EVENT_MUSIC] = 0.930355215f,
    [PCM_ASSET_STREAMED_CHATTER] = 0.967423729f,
    [PCM_ASSET_STREAMED_PLAYER_CHATTER] = 0.950653344f,
};

static const GameProfile k_game_profiles[] = {
    {
        GAME_PROFILE_NBA2K11,
        L"2K11",
        L"NBA 2K11",
        96694272u,
        0x0500BA60u,
        k_nba2k11_pcm_gains,
        ARRAYSIZE(k_nba2k11_pcm_gains),
    },
};

static const GameProfile *g_active_profile;

static int sibling_path(
    HMODULE module, WCHAR *output, DWORD capacity,
    const WCHAR *relative_path) {
    WCHAR *slash;
    DWORD length = GetModuleFileNameW(module, output, capacity);
    if (!length || length >= capacity) return 0;
    slash = output + length;
    while (slash > output && slash[-1] != L'\\' && slash[-1] != L'/') {
        --slash;
    }
    if ((size_t)(slash - output) + wcslen(relative_path) + 1 > capacity) {
        return 0;
    }
    wcscpy(slash, relative_path);
    return 1;
}

const GameProfile *game_profile_load(HMODULE plugin_module) {
    WCHAR ini_path[MAX_PATH * 2];
    WCHAR selected_game[32];
    size_t index;

    g_active_profile = NULL;
    if (!sibling_path(
            plugin_module, ini_path, ARRAYSIZE(ini_path),
            AUDIO_FIX_INI_NAME)) {
        return NULL;
    }
    if (!GetPrivateProfileStringW(
            L"AudioFix", L"Game", L"", selected_game,
            ARRAYSIZE(selected_game), ini_path)) {
        return NULL;
    }
    for (index = 0; index < ARRAYSIZE(k_game_profiles); ++index) {
        if (_wcsicmp(selected_game, k_game_profiles[index].name) == 0) {
            g_active_profile = &k_game_profiles[index];
            return g_active_profile;
        }
    }
    return NULL;
}

const GameProfile *game_profile_active(void) {
    return g_active_profile;
}

float game_profile_pcm_gain(PcmAssetKind asset) {
    float gain;
    if (!g_active_profile || !g_active_profile->pcm_gains ||
        asset <= PCM_ASSET_NONE ||
        (size_t)asset >= g_active_profile->pcm_gain_count) {
        return 1.0f;
    }
    gain = g_active_profile->pcm_gains[asset];
    return gain > 0.0f ? gain : 1.0f;
}
