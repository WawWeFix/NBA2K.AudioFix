#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <wchar.h>

#include "game_profile.h"

#define AUDIO_FIX_INI_NAME L"NBA2K.AudioFix.ini"

static const GameProfile k_game_profiles[] = {
    {
        GAME_PROFILE_NBA2K11,
        L"2K11",
        L"NBA 2K11",
        96694272u,
        0x0500BA60u,
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
