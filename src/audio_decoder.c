/*
 * NBA 2K audio decoder core.
 *
 * Decodes one requested console audio cue to 16-bit PCM.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "audio_decoder.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libswresample/swresample.h"

typedef struct XboxTrack {
    int index;
    uint32_t start_offset;
    uint32_t data_size;
    uint32_t samples;
    uint32_t average_bitrate_bps;
} XboxTrack;

typedef struct XboxBank {
    const WCHAR *name;
    const WCHAR *relative_path;
    const XboxTrack *tracks;
    size_t track_count;
    uint64_t expected_size;
    int kind;
    uint32_t sample_rate;
    uint16_t channels;
} XboxBank;

#define XBOX_BANK_XMA2 1
#define XBOX_BANK_WMA_FIXED_GRID 2
#define PS3_BANK_ATRAC3 3
#define XBOX_BANK_XWMA_FILE 4
#define PS3_ATRAC3_SOUND_UNIT 152u
#define PS3_ATRAC3_SAMPLES_PER_FRAME 1024u
#define PS3_MAP_XBOX_FALLBACK 0xFFFFFFFFu
#define JUKEBOX_TRACK_COUNT 29
#define LOADING_SEQUENCE_TRACK_COUNT 6
#define LOADING_SEQUENCE_JM_TRACK_COUNT 10
#define MENTOR_TRACK_COUNT 611
#define MENTOR_TS_TRACK_COUNT 306
#define LOADING_VO_TRACK_COUNT 833
#define LOADING_VO_PS_TRACK_COUNT 1464
#define LOADING_VO_TS_TRACK_COUNT 918
#define LINES_CS_TRACK_COUNT 907
#define ENV_AMB_TRACK_COUNT 135
#define OVERLAY_AUDIO_TRACK_COUNT 2
#define CAIRBALL_TRACK_COUNT 4
#define AI_STREET_TRACK_COUNT 1
#define CWD_LOOP_TRACK_COUNT 1
#define AO_STREET_TRACK_COUNT 1
#define AS_STREET_TRACK_COUNT 2
#define DUNK_SFX_TRACK_COUNT 39
#define COACHES_TRACK_COUNT 403
#define TEAMS_TRACK_COUNT 942
#define PLAYERS_TRACK_COUNT 1737
#define PRESS_CONF_TRACK_COUNT 1532
#define PA_PLAYERS_TRACK_COUNT 5432
#define CACHED_LINES_TRACK_COUNT 85
#define LINES_TRACK_COUNT 29005
#define LINES_PS_TRACK_COUNT 29832
#define LINES_TS_TRACK_COUNT 10139
#define PA_LINES_TRACK_COUNT 5280
#define CWD_STR_LOOP_DUNK_CONTEST_TRACK_COUNT 2
#define CWD_STR_LOOP_GYM_TRACK_COUNT 4
#define CWD_STR_LOOP_INSIDE_TRACK_COUNT 6
#define CWD_STR_SFX_DUNK_CONTEST_TRACK_COUNT 17
#define CWD_STR_SFX_INSIDE_TRACK_COUNT 445
#define EVENT_MUSIC_TRACK_COUNT 1534
#define LOAD_M_TRACK_COUNT 1
#define STREAMED_CHATTER_TRACK_COUNT 14778
#define STREAMED_PLAYER_CHATTER_TRACK_COUNT 12804
#define XBOX_WMA_CHUNK_SIZE 0x1800u
#define XBOX_WMA_HEADER_SIZE 0x38u
#define XBOX_WMA_MARKER 0xD2BEA169u

typedef struct XboxWmaClip {
    uint32_t start_chunk;
    uint32_t chunk_count;
    uint32_t packet_count;
    uint32_t payload_size;
    uint32_t decoded_pcm_bytes;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t average_bytes_per_second;
    uint32_t block_align;
} XboxWmaClip;

static const XboxBank k_banks[] = {
    {
        L"jukebox", L"data\\jukeboxmusic.bin",
        NULL, JUKEBOX_TRACK_COUNT, 87972736ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"loadingsequence", L"data\\loadingsequence.bin",
        NULL, LOADING_SEQUENCE_TRACK_COUNT, 1630656ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"loadingsequencejm", L"data\\loadingsequencejm.bin",
        NULL, LOADING_SEQUENCE_JM_TRACK_COUNT, 2660000ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"mentor", L"data\\mentor.bin",
        NULL, MENTOR_TRACK_COUNT, 31081568ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"mentorts", L"data\\mentor_ts.bin",
        NULL, MENTOR_TS_TRACK_COUNT, 7685120ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"loadingvo", L"data\\loadingvo.bin",
        NULL, LOADING_VO_TRACK_COUNT, 17345328ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"loadingvops", L"data\\loadingvo_ps.bin",
        NULL, LOADING_VO_PS_TRACK_COUNT, 44135024ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"loadingvots", L"data\\loadingvo_ts.bin",
        NULL, LOADING_VO_TS_TRACK_COUNT, 9547728ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"linescs", L"data\\lines_cs.bin",
        NULL, LINES_CS_TRACK_COUNT, 27861904ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"envamb", L"data\\env_amb.bin",
        NULL, ENV_AMB_TRACK_COUNT, 39409344ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"overlayaudio", L"data\\overlayaudio.bin",
        NULL, OVERLAY_AUDIO_TRACK_COUNT, 99408ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"cairball", L"data\\cairball.bin",
        NULL, CAIRBALL_TRACK_COUNT, 227392ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"aistreet", L"data\\aistreet.bin",
        NULL, AI_STREET_TRACK_COUNT, 798912ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdloop", L"data\\cwdloop.bin",
        NULL, CWD_LOOP_TRACK_COUNT, 1380768ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"aostreet", L"data\\aostreet.bin",
        NULL, AO_STREET_TRACK_COUNT, 1569248ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"asstreet", L"data\\asstreet.bin",
        NULL, AS_STREET_TRACK_COUNT, 2048960ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"dunksfx", L"data\\dunksfx.bin",
        NULL, DUNK_SFX_TRACK_COUNT, 1833728ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"coaches", L"data\\coaches.bin",
        NULL, COACHES_TRACK_COUNT, 3112352ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"teams", L"data\\teams.bin",
        NULL, TEAMS_TRACK_COUNT, 8592560ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"players", L"data\\players.bin",
        NULL, PLAYERS_TRACK_COUNT, 9153440ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"pressconf", L"data\\pressconf.bin",
        NULL, PRESS_CONF_TRACK_COUNT, 82464560ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"paplayers", L"data\\paplayers.bin",
        NULL, PA_PLAYERS_TRACK_COUNT, 162902656ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cachedlines", L"data\\cachedlines.spc",
        NULL, CACHED_LINES_TRACK_COUNT, 1445216ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"lines", L"data\\lines.bin",
        NULL, LINES_TRACK_COUNT, 1006328768ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"linesps", L"data\\lines_ps.bin",
        NULL, LINES_PS_TRACK_COUNT, 467476608ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"linests", L"data\\lines_ts.bin",
        NULL, LINES_TS_TRACK_COUNT, 262650528ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"palines", L"data\\palines.bin",
        NULL, PA_LINES_TRACK_COUNT, 242296512ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdstrloopdunkcontest",
        L"data\\cwdstrloop_dunkcontest.bin",
        NULL, CWD_STR_LOOP_DUNK_CONTEST_TRACK_COUNT, 1270720ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdstrloopgym", L"data\\cwdstrloop_gym.bin",
        NULL, CWD_STR_LOOP_GYM_TRACK_COUNT, 2711680ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdstrloopinside", L"data\\cwdstrloop_inside.bin",
        NULL, CWD_STR_LOOP_INSIDE_TRACK_COUNT, 12051776ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdstrsfxdunkcontest",
        L"data\\cwdstrsfx_dunkcontest.bin",
        NULL, CWD_STR_SFX_DUNK_CONTEST_TRACK_COUNT, 4166624ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"cwdstrsfxinside", L"data\\cwdstrsfx_inside.bin",
        NULL, CWD_STR_SFX_INSIDE_TRACK_COUNT, 88404416ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"eventmusic", L"data\\eventmusic.bin",
        NULL, EVENT_MUSIC_TRACK_COUNT, 663688544ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"loadm", L"data\\loadm.bin",
        NULL, LOAD_M_TRACK_COUNT, 342304ull,
        PS3_BANK_ATRAC3, 48000u, 2u,
    },
    {
        L"streamedchatter", L"data\\streamedchatter.bin",
        NULL, STREAMED_CHATTER_TRACK_COUNT, 267760768ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
    {
        L"streamedplayerchatter",
        L"data\\streamedplayerchatter.bin",
        NULL, STREAMED_PLAYER_CHATTER_TRACK_COUNT, 146560832ull,
        PS3_BANK_ATRAC3, 48000u, 1u,
    },
};

static const XboxBank k_lines_ps_xbox_fallback = {
    L"linesps-xbox-fallback", NULL,
    NULL, LINES_PS_TRACK_COUNT, 0,
    XBOX_BANK_XWMA_FILE, 44100u, 1u,
};

static HMODULE g_audio_decoder_module;
static GameProfileId g_audio_decoder_profile;

void audio_decoder_set_module(HMODULE module) {
    g_audio_decoder_module = module;
}

void audio_decoder_set_game_profile(GameProfileId profile) {
    g_audio_decoder_profile = profile;
}

static int sibling_path(
    WCHAR *output, DWORD capacity, const WCHAR *relative_path) {
    WCHAR *slash;
    DWORD length = GetModuleFileNameW(
        g_audio_decoder_module, output, capacity);
    if (!length || length >= capacity) return 0;
    slash = output + length;
    while (slash > output && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
    if ((size_t)(slash - output) + wcslen(relative_path) + 1 > capacity) {
        return 0;
    }
    wcscpy(slash, relative_path);
    return 1;
}

static const XboxBank *find_bank(const WCHAR *name) {
    size_t i;
    if (g_audio_decoder_profile != GAME_PROFILE_NBA2K11) return NULL;
    for (i = 0; i < ARRAYSIZE(k_banks); ++i) {
        if (_wcsicmp(k_banks[i].name, name) == 0) return &k_banks[i];
    }
    return NULL;
}

static const XboxTrack *find_track(const XboxBank *bank, int index) {
    size_t i;
    if (!bank || !bank->tracks) return NULL;
    for (i = 0; i < bank->track_count; ++i) {
        if (bank->tracks[i].index == index) return &bank->tracks[i];
    }
    return NULL;
}

static int write_all(HANDLE output, const void *data, DWORD size) {
    const BYTE *cursor = (const BYTE *)data;
    while (size) {
        DWORD written = 0;
        if (!WriteFile(output, cursor, size, &written, NULL) || !written) {
            return 0;
        }
        cursor += written;
        size -= written;
    }
    return 1;
}

static int copy_file_to_output(HANDLE input, HANDLE output) {
    BYTE *buffer;
    LARGE_INTEGER start;
    int ok = 1;
    start.QuadPart = 0;
    if (!SetFilePointerEx(input, start, NULL, FILE_BEGIN)) return 0;
    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 256 * 1024);
    if (!buffer) return 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer, 256 * 1024, &read, NULL)) {
            ok = 0;
            break;
        }
        if (!read) break;
        if (!write_all(output, buffer, read)) {
            ok = 0;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return ok;
}

static int write_u16(HANDLE output, uint16_t value) {
    BYTE data[2] = {
        (BYTE)(value & 0xFF),
        (BYTE)((value >> 8) & 0xFF),
    };
    return write_all(output, data, sizeof(data));
}

static int write_u32(HANDLE output, uint32_t value) {
    BYTE data[4] = {
        (BYTE)(value & 0xFF),
        (BYTE)((value >> 8) & 0xFF),
        (BYTE)((value >> 16) & 0xFF),
        (BYTE)((value >> 24) & 0xFF),
    };
    return write_all(output, data, sizeof(data));
}

static uint32_t read_be32(const BYTE *data) {
    return ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        (uint32_t)data[3];
}

static uint32_t read_le32(const BYTE *data) {
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static int load_wma_start_chunk(
    const XboxBank *bank_spec, int track_index,
    uint32_t *start_chunk) {
    WCHAR relative_path[MAX_PATH * 2];
    WCHAR map_path[MAX_PATH * 2];
    WCHAR *extension;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    LARGE_INTEGER position;
    BYTE header[16];
    BYTE value[4];
    DWORD read = 0;
    uint32_t version;
    uint32_t count;
    uint32_t entry_size;
    uint32_t result;
    uint32_t total_chunks;
    int ok = 0;
    size_t length;

    if (!bank_spec || !start_chunk || track_index < 0 ||
        track_index >= (int)bank_spec->track_count ||
        !bank_spec->relative_path) {
        return 0;
    }
    length = wcslen(bank_spec->relative_path);
    if (length + wcslen(L"_pcm_map.bin") + 1 >
        ARRAYSIZE(relative_path)) {
        return 0;
    }
    wcscpy(relative_path, bank_spec->relative_path);
    extension = wcsrchr(relative_path, L'.');
    if (!extension) return 0;
    wcscpy(extension, L"_pcm_map.bin");
    if (!sibling_path(
            map_path, ARRAYSIZE(map_path), relative_path)) {
        return 0;
    }
    file = CreateFileW(
        map_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(file, &size) ||
        size.QuadPart !=
            16LL + (LONGLONG)bank_spec->track_count * 16LL ||
        !ReadFile(file, header, sizeof(header), &read, NULL) ||
        read != sizeof(header) ||
        memcmp(header, "MPCM", 4) != 0) {
        goto done;
    }
    version = read_le32(header + 4);
    count = read_le32(header + 8);
    entry_size = read_le32(header + 12);
    if (version != 2 ||
        count != (uint32_t)bank_spec->track_count ||
        entry_size != 16u) {
        goto done;
    }
    position.QuadPart =
        16LL + (LONGLONG)track_index * 16LL + 12LL;
    read = 0;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
        !ReadFile(file, value, sizeof(value), &read, NULL) ||
        read != sizeof(value)) {
        goto done;
    }
    result = read_le32(value);
    total_chunks =
        (uint32_t)(bank_spec->expected_size / XBOX_WMA_CHUNK_SIZE);
    if (result >= total_chunks) goto done;
    *start_chunk = result;
    ok = 1;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static int load_xma_track(
    const XboxBank *bank_spec, int track_index, XboxTrack *track) {
    WCHAR relative_path[MAX_PATH * 2];
    WCHAR map_path[MAX_PATH * 2];
    WCHAR *extension;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    LARGE_INTEGER position;
    BYTE header[16];
    BYTE value[4];
    DWORD read = 0;
    uint32_t version;
    uint32_t count;
    uint32_t entry_size;
    uint32_t start_packet;
    uint32_t end_packet;
    uint32_t total_packets;
    int ok = 0;
    size_t length;

    if (!bank_spec || !track || track_index < 0 ||
        track_index >= (int)bank_spec->track_count ||
        !bank_spec->relative_path ||
        bank_spec->expected_size % 0x800u) {
        return 0;
    }
    length = wcslen(bank_spec->relative_path);
    if (length + wcslen(L"_pcm_map.bin") + 1 >
        ARRAYSIZE(relative_path)) {
        return 0;
    }
    wcscpy(relative_path, bank_spec->relative_path);
    extension = wcsrchr(relative_path, L'.');
    if (!extension) return 0;
    wcscpy(extension, L"_pcm_map.bin");
    if (!sibling_path(
            map_path, ARRAYSIZE(map_path), relative_path)) {
        return 0;
    }
    file = CreateFileW(
        map_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(file, &size) ||
        size.QuadPart !=
            16LL + (LONGLONG)bank_spec->track_count * 16LL ||
        !ReadFile(file, header, sizeof(header), &read, NULL) ||
        read != sizeof(header) ||
        memcmp(header, "MPCM", 4) != 0) {
        goto done;
    }
    version = read_le32(header + 4);
    count = read_le32(header + 8);
    entry_size = read_le32(header + 12);
    if (version != 2 ||
        count != (uint32_t)bank_spec->track_count ||
        entry_size != 16u) {
        goto done;
    }
    position.QuadPart =
        16LL + (LONGLONG)track_index * 16LL + 12LL;
    read = 0;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
        !ReadFile(file, value, sizeof(value), &read, NULL) ||
        read != sizeof(value)) {
        goto done;
    }
    start_packet = read_le32(value);
    total_packets = (uint32_t)(bank_spec->expected_size / 0x800u);
    if (track_index + 1 < (int)bank_spec->track_count) {
        position.QuadPart += 16LL;
        read = 0;
        if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
            !ReadFile(file, value, sizeof(value), &read, NULL) ||
            read != sizeof(value)) {
            goto done;
        }
        end_packet = read_le32(value);
    } else {
        end_packet = total_packets;
    }
    if (start_packet >= end_packet || end_packet > total_packets) {
        goto done;
    }
    memset(track, 0, sizeof(*track));
    track->index = track_index;
    track->start_offset = start_packet * 0x800u;
    track->data_size = (end_packet - start_packet) * 0x800u;
    ok = 1;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static int load_ps3_atrac3_track(
    const XboxBank *bank_spec, int track_index, XboxTrack *track) {
    WCHAR relative_path[MAX_PATH * 2];
    WCHAR map_path[MAX_PATH * 2];
    WCHAR *extension;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    LARGE_INTEGER position;
    BYTE header[16];
    BYTE entry[16];
    DWORD read = 0;
    uint32_t version;
    uint32_t count;
    uint32_t entry_size;
    uint32_t start_frame;
    uint32_t pcm_bytes;
    uint32_t frame_count;
    uint32_t end_frame;
    uint32_t total_frames;
    uint32_t block_align;
    uint32_t pcm_bytes_per_frame;
    uint64_t sample_count;
    int ok = 0;
    size_t length;

    if (!bank_spec || !track || track_index < 0 ||
        track_index >= (int)bank_spec->track_count ||
        !bank_spec->relative_path ||
        (bank_spec->channels != 1u && bank_spec->channels != 2u)) {
        return 0;
    }
    block_align = PS3_ATRAC3_SOUND_UNIT * bank_spec->channels;
    pcm_bytes_per_frame =
        PS3_ATRAC3_SAMPLES_PER_FRAME * bank_spec->channels * 2u;
    if (bank_spec->expected_size % block_align) return 0;
    length = wcslen(bank_spec->relative_path);
    if (length + wcslen(L"_pcm_map.bin") + 1 >
        ARRAYSIZE(relative_path)) {
        return 0;
    }
    wcscpy(relative_path, bank_spec->relative_path);
    extension = wcsrchr(relative_path, L'.');
    if (!extension) return 0;
    wcscpy(extension, L"_pcm_map.bin");
    if (!sibling_path(map_path, ARRAYSIZE(map_path), relative_path)) {
        return 0;
    }
    file = CreateFileW(
        map_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(file, &size) ||
        size.QuadPart !=
            16LL + (LONGLONG)bank_spec->track_count * 16LL ||
        !ReadFile(file, header, sizeof(header), &read, NULL) ||
        read != sizeof(header) ||
        memcmp(header, "MPCM", 4) != 0) {
        goto done;
    }
    version = read_le32(header + 4);
    count = read_le32(header + 8);
    entry_size = read_le32(header + 12);
    if (version != 2 ||
        count != (uint32_t)bank_spec->track_count ||
        entry_size != 16u) {
        goto done;
    }
    position.QuadPart = 16LL + (LONGLONG)track_index * 16LL;
    read = 0;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
        !ReadFile(file, entry, sizeof(entry), &read, NULL) ||
        read != sizeof(entry)) {
        goto done;
    }
    start_frame = read_le32(entry + 12);
    pcm_bytes = read_le32(entry + 8);
    if (start_frame == PS3_MAP_XBOX_FALLBACK) {
        ok = -1;
        goto done;
    }
    if (!pcm_bytes || pcm_bytes % pcm_bytes_per_frame) {
        goto done;
    }
    frame_count = pcm_bytes / pcm_bytes_per_frame;
    total_frames =
        (uint32_t)(bank_spec->expected_size / block_align);
    end_frame = start_frame + frame_count;
    if (start_frame >= end_frame || end_frame > total_frames) goto done;
    sample_count =
        (uint64_t)(end_frame - start_frame) *
        PS3_ATRAC3_SAMPLES_PER_FRAME;
    if (!sample_count || sample_count > 0xFFFFFFFFull) goto done;
    memset(track, 0, sizeof(*track));
    track->index = track_index;
    track->start_offset = start_frame * block_align;
    track->data_size =
        (end_frame - start_frame) * block_align;
    track->samples = (uint32_t)sample_count;
    track->average_bitrate_bps =
        block_align * 8u * 48000u /
        PS3_ATRAC3_SAMPLES_PER_FRAME;
    ok = 1;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static int read_wma_chunk(
    HANDLE bank, uint32_t chunk_index, BYTE *chunk) {
    LARGE_INTEGER position;
    DWORD read = 0;
    position.QuadPart =
        (LONGLONG)chunk_index * XBOX_WMA_CHUNK_SIZE;
    return SetFilePointerEx(bank, position, NULL, FILE_BEGIN) &&
        ReadFile(
            bank, chunk, XBOX_WMA_CHUNK_SIZE, &read, NULL) &&
        read == XBOX_WMA_CHUNK_SIZE;
}

static int find_wma_clip(
    HANDLE bank, const XboxBank *bank_spec, int wanted_index,
    XboxWmaClip *clip) {
    BYTE chunk[XBOX_WMA_CHUNK_SIZE];
    uint32_t total_chunks;
    uint32_t chunk_index;
    uint32_t first_chunk = 0;
    int current_clip = 0;
    uint32_t expected_sequence = 0;
    if (!bank_spec || !clip || wanted_index < 0 ||
        wanted_index >= (int)bank_spec->track_count ||
        bank_spec->expected_size % XBOX_WMA_CHUNK_SIZE) {
        return 0;
    }
    memset(clip, 0, sizeof(*clip));
    total_chunks =
        (uint32_t)(bank_spec->expected_size / XBOX_WMA_CHUNK_SIZE);
    if (load_wma_start_chunk(
            bank_spec, wanted_index, &first_chunk)) {
        current_clip = wanted_index;
    }
    for (chunk_index = first_chunk;
         chunk_index < total_chunks;
         ++chunk_index) {
        uint32_t channels;
        uint32_t block_align;
        uint32_t packet_count;
        uint32_t sample_rate;
        uint32_t decoded_bytes;
        uint32_t end_of_clip;
        uint32_t sequence;
        uint32_t average_bytes;
        uint32_t payload_offset;
        uint32_t payload_size;
        if (!read_wma_chunk(bank, chunk_index, chunk) ||
            read_be32(chunk) != XBOX_WMA_MARKER) {
            return 0;
        }
        channels = read_be32(chunk + 0x04);
        block_align = read_be32(chunk + 0x08);
        packet_count = read_be32(chunk + 0x0C);
        sample_rate = read_be32(chunk + 0x10);
        decoded_bytes = read_be32(chunk + 0x14);
        end_of_clip = read_be32(chunk + 0x20);
        sequence = read_be32(chunk + 0x24);
        average_bytes = read_be32(chunk + 0x34);
        payload_offset =
            XBOX_WMA_HEADER_SIZE + packet_count * 4u;
        payload_size = packet_count * block_align;
        if (!channels || !block_align || !packet_count ||
            !sample_rate || !decoded_bytes ||
            sequence != expected_sequence ||
            payload_offset > XBOX_WMA_CHUNK_SIZE ||
            payload_size > XBOX_WMA_CHUNK_SIZE - payload_offset) {
            return 0;
        }
        if (current_clip == wanted_index) {
            if (!clip->chunk_count) {
                clip->start_chunk = chunk_index;
                clip->channels = channels;
                clip->sample_rate = sample_rate;
                clip->average_bytes_per_second = average_bytes;
                clip->block_align = block_align;
            } else if (
                clip->channels != channels ||
                clip->sample_rate != sample_rate ||
                clip->average_bytes_per_second != average_bytes ||
                clip->block_align != block_align) {
                return 0;
            }
            ++clip->chunk_count;
            clip->packet_count += packet_count;
            clip->payload_size += payload_size;
            clip->decoded_pcm_bytes += decoded_bytes;
        }
        if (end_of_clip) {
            if (current_clip == wanted_index) {
                return clip->chunk_count != 0;
            }
            ++current_clip;
            expected_sequence = 0;
        } else {
            ++expected_sequence;
        }
    }
    return 0;
}

static int write_wma_stream(
    HANDLE bank, HANDLE output, const XboxWmaClip *clip) {
    BYTE chunk[XBOX_WMA_CHUNK_SIZE];
    uint32_t chunk_offset;
    uint32_t decoded_base = 0;
    uint32_t riff_body_size =
        46u + clip->packet_count * 4u + clip->payload_size +
        (clip->payload_size & 1u);

    if (!write_all(output, "RIFF", 4) ||
        !write_u32(output, riff_body_size) ||
        !write_all(output, "XWMAfmt ", 8) ||
        !write_u32(output, 18u) ||
        !write_u16(output, 0x0161u) ||
        !write_u16(output, (uint16_t)clip->channels) ||
        !write_u32(output, clip->sample_rate) ||
        !write_u32(output, clip->average_bytes_per_second) ||
        !write_u16(output, (uint16_t)clip->block_align) ||
        !write_u16(output, 16u) ||
        !write_u16(output, 0u) ||
        !write_all(output, "dpds", 4) ||
        !write_u32(output, clip->packet_count * 4u)) {
        return 0;
    }

    for (chunk_offset = 0;
         chunk_offset < clip->chunk_count;
         ++chunk_offset) {
        uint32_t packet_count;
        uint32_t packet;
        uint32_t decoded_bytes;
        if (!read_wma_chunk(
                bank, clip->start_chunk + chunk_offset, chunk)) {
            return 0;
        }
        packet_count = read_be32(chunk + 0x0C);
        decoded_bytes = read_be32(chunk + 0x14);
        for (packet = 0; packet < packet_count; ++packet) {
            uint32_t local = read_be32(
                chunk + XBOX_WMA_HEADER_SIZE + packet * 4u);
            if (!local || local > decoded_bytes ||
                !write_u32(output, decoded_base + local)) {
                return 0;
            }
        }
        decoded_base += decoded_bytes;
    }
    if (decoded_base != clip->decoded_pcm_bytes ||
        !write_all(output, "data", 4) ||
        !write_u32(output, clip->payload_size)) {
        return 0;
    }
    for (chunk_offset = 0;
         chunk_offset < clip->chunk_count;
         ++chunk_offset) {
        uint32_t packet_count;
        uint32_t payload_offset;
        uint32_t payload_size;
        if (!read_wma_chunk(
                bank, clip->start_chunk + chunk_offset, chunk)) {
            return 0;
        }
        packet_count = read_be32(chunk + 0x0C);
        payload_offset =
            XBOX_WMA_HEADER_SIZE + packet_count * 4u;
        payload_size = packet_count * clip->block_align;
        if (!write_all(output, chunk + payload_offset, payload_size)) {
            return 0;
        }
    }
    if (clip->payload_size & 1u) {
        BYTE padding = 0;
        if (!write_all(output, &padding, 1)) return 0;
    }
    return 1;
}

static int write_ps3_atrac3_stream(
    HANDLE bank, HANDLE output, const XboxBank *bank_spec,
    const XboxTrack *track) {
    BYTE *buffer;
    uint32_t remaining;
    LARGE_INTEGER position;
    uint32_t riff_body_size;
    uint32_t block_align;
    uint16_t channels;

    if (!bank_spec || !track || !track->data_size ||
        (bank_spec->channels != 1u && bank_spec->channels != 2u) ||
        !track->samples) {
        return 0;
    }
    channels = bank_spec->channels;
    block_align = PS3_ATRAC3_SOUND_UNIT * channels;
    if (track->data_size % block_align) return 0;
    riff_body_size = 68u + track->data_size;
    if (!write_all(output, "RIFF", 4) ||
        !write_u32(output, riff_body_size) ||
        !write_all(output, "WAVEfmt ", 8) ||
        !write_u32(output, 32u) ||
        !write_u16(output, 0x0270u) ||
        !write_u16(output, channels) ||
        !write_u32(output, 48000u) ||
        !write_u32(
            output,
            block_align * 48000u /
                PS3_ATRAC3_SAMPLES_PER_FRAME) ||
        !write_u16(output, (uint16_t)block_align) ||
        !write_u16(output, 0u) ||
        !write_u16(output, 14u) ||
        !write_u16(output, 1u) ||
        !write_u16(output, (uint16_t)(0x0800u * channels)) ||
        !write_u16(output, 0u) ||
        !write_u16(output, 0u) ||
        !write_u16(output, 0u) ||
        !write_u16(output, 1u) ||
        !write_u16(output, 0u) ||
        !write_all(output, "fact", 4) ||
        !write_u32(output, 8u) ||
        !write_u32(output, track->samples) ||
        !write_u32(output, 0u) ||
        !write_all(output, "data", 4) ||
        !write_u32(output, track->data_size)) {
        return 0;
    }

    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 256 * 1024);
    if (!buffer) return 0;
    position.QuadPart = track->start_offset;
    if (!SetFilePointerEx(bank, position, NULL, FILE_BEGIN)) {
        HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }
    remaining = track->data_size;
    while (remaining) {
        DWORD wanted = remaining > 256 * 1024 ? 256 * 1024 : remaining;
        DWORD read = 0;
        if (!ReadFile(bank, buffer, wanted, &read, NULL) ||
            read != wanted || !write_all(output, buffer, read)) {
            HeapFree(GetProcessHeap(), 0, buffer);
            return 0;
        }
        remaining -= read;
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return 1;
}

static uint32_t xbox_bank_sample_rate(const XboxBank *bank) {
    return bank && bank->sample_rate ? bank->sample_rate : 48000u;
}

static uint16_t xbox_bank_channels(const XboxBank *bank) {
    return bank && bank->channels ? bank->channels : 2u;
}

static int prepare_dynamic_xma_track(
    HANDLE bank_file, const XboxBank *bank, XboxTrack *track) {
    LARGE_INTEGER position;
    BYTE packet[0x800];
    DWORD packet_count;
    DWORD packet_index;
    uint64_t samples = 0;
    uint64_t bitrate;

    if (!bank || !track || !track->data_size ||
        track->data_size % 0x800u) {
        return 0;
    }
    position.QuadPart = track->start_offset;
    if (!SetFilePointerEx(bank_file, position, NULL, FILE_BEGIN)) {
        return 0;
    }
    packet_count = track->data_size / 0x800u;
    for (packet_index = 0; packet_index < packet_count; ++packet_index) {
        DWORD read = 0;
        if (!ReadFile(
                bank_file, packet, sizeof(packet), &read, NULL) ||
            read != sizeof(packet) ||
            packet[0] == 0 ||
            memcmp(packet + 1, "\x00\x01\x00", 3) != 0) {
            return 0;
        }
        samples += (uint64_t)packet[0] * 128u;
    }
    if (!samples || samples > 0xFFFFFFFFull) return 0;
    bitrate =
        ((uint64_t)track->data_size * 8u *
         xbox_bank_sample_rate(bank) + samples / 2u) /
        samples;
    if (!bitrate || bitrate > 0xFFFFFFFFull) return 0;
    track->samples = (uint32_t)samples;
    track->average_bitrate_bps = (uint32_t)bitrate;
    return 1;
}

static int validate_bank(
    HANDLE bank_file, const XboxBank *bank, const XboxTrack *track) {
    LARGE_INTEGER size;
    LARGE_INTEGER position;
    BYTE signature[5];
    DWORD read = 0;

    if (!bank || !track || !GetFileSizeEx(bank_file, &size) ||
        (uint64_t)size.QuadPart != bank->expected_size) {
        return 0;
    }
    position.QuadPart = (LONGLONG)track->start_offset + 6;
    if (!SetFilePointerEx(bank_file, position, NULL, FILE_BEGIN) ||
        !ReadFile(bank_file, signature, sizeof(signature), &read, NULL) ||
        read != sizeof(signature)) {
        return 0;
    }
    if (xbox_bank_channels(bank) == 1u) {
        if (memcmp(signature, "\xFC\x03", 2) != 0) return 0;
    } else if (
        memcmp(signature, "\xFC\x01\xC0\x01\x02", 5) != 0) {
        return 0;
    }
    return 1;
}

static int validate_ps3_atrac3_bank(
    HANDLE bank_file, const XboxBank *bank, const XboxTrack *track) {
    LARGE_INTEGER size;
    LARGE_INTEGER position;
    BYTE first_byte = 0;
    DWORD read = 0;
    if (!bank || !track ||
        !GetFileSizeEx(bank_file, &size) ||
        (uint64_t)size.QuadPart != bank->expected_size ||
        (uint64_t)track->start_offset + track->data_size >
            bank->expected_size) {
        return 0;
    }
    position.QuadPart = track->start_offset;
    return SetFilePointerEx(
            bank_file, position, NULL, FILE_BEGIN) &&
        ReadFile(bank_file, &first_byte, 1, &read, NULL) &&
        read == 1 &&
        (first_byte & 0xFCu) == 0xA0u;
}

static int write_xma_stream(
    HANDLE bank, HANDLE output, const XboxBank *bank_spec,
    const XboxTrack *track) {
    BYTE *buffer;
    uint32_t remaining = track->data_size;
    LARGE_INTEGER position;
    uint16_t channels = xbox_bank_channels(bank_spec);
    uint32_t sample_rate = xbox_bank_sample_rate(bank_spec);
    uint32_t channel_mask = channels == 1u ? 4u : 3u;
    uint16_t block_count =
        (uint16_t)((track->data_size + 0xFFFFu) / 0x10000u);
    uint32_t average_bytes = (track->average_bitrate_bps + 4u) / 8u;

    if ((channels != 1u && channels != 2u) ||
        !write_all(output, "RIFF", 4) ||
        !write_u32(output, 84u + track->data_size) ||
        !write_all(output, "WAVEfmt ", 8) ||
        !write_u32(output, 52u) ||
        !write_u16(output, 0x0166u) ||
        !write_u16(output, channels) ||
        !write_u32(output, sample_rate) ||
        !write_u32(output, average_bytes) ||
        !write_u16(output, channels * 2u) ||
        !write_u16(output, 16u) ||
        !write_u16(output, 34u) ||
        !write_u16(output, 1u) ||
        !write_u32(output, channel_mask) ||
        !write_u32(output, track->samples) ||
        !write_u32(output, 0x10000u) ||
        !write_u32(output, 0u) ||
        !write_u32(output, track->samples) ||
        !write_u32(output, 0u) ||
        !write_u32(output, 0u) ||
        !write_all(output, "\x00\x04", 2) ||
        !write_u16(output, block_count) ||
        !write_all(output, "fact", 4) ||
        !write_u32(output, 4u) ||
        !write_u32(output, track->samples) ||
        !write_all(output, "data", 4) ||
        !write_u32(output, track->data_size)) {
        return 0;
    }

    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 256 * 1024);
    if (!buffer) return 0;
    position.QuadPart = track->start_offset;
    if (!SetFilePointerEx(bank, position, NULL, FILE_BEGIN)) {
        HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }
    while (remaining) {
        DWORD wanted = remaining > 256 * 1024 ? 256 * 1024 : remaining;
        DWORD read = 0;
        if (!ReadFile(bank, buffer, wanted, &read, NULL) ||
            read != wanted || !write_all(output, buffer, read)) {
            HeapFree(GetProcessHeap(), 0, buffer);
            return 0;
        }
        remaining -= read;
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return 1;
}

typedef struct WrapperWriterContext {
    HANDLE bank;
    HANDLE output;
    const XboxBank *bank_spec;
    const XboxTrack *track;
    int track_index;
} WrapperWriterContext;

typedef struct DirectDecodeContext {
    HANDLE input;
} DirectDecodeContext;

typedef struct PcmConverter {
    SwrContext *swr;
    AVChannelLayout input_layout;
    AVChannelLayout output_layout;
    enum AVSampleFormat input_format;
    int input_rate;
    int output_rate;
    int channels;
    HANDLE output;
} PcmConverter;

static DWORD WINAPI wrapper_writer_thread(void *opaque) {
    WrapperWriterContext *context = (WrapperWriterContext *)opaque;
    XboxWmaClip wma_clip;
    int wrote;

    if (context->bank_spec->kind == XBOX_BANK_XMA2) {
        wrote = context->track &&
            write_xma_stream(
                context->bank, context->output,
                context->bank_spec, context->track);
    } else if (context->bank_spec->kind == PS3_BANK_ATRAC3) {
        wrote = context->track &&
            write_ps3_atrac3_stream(
                context->bank, context->output,
                context->bank_spec, context->track);
    } else if (
        context->bank_spec->kind == XBOX_BANK_XWMA_FILE) {
        wrote = copy_file_to_output(
            context->bank, context->output);
    } else {
        wrote = find_wma_clip(
                context->bank, context->bank_spec,
                context->track_index, &wma_clip) &&
            write_wma_stream(
                context->bank, context->output, &wma_clip);
    }
    CloseHandle(context->output);
    context->output = NULL;
    return wrote ? 0u : 1u;
}

static int direct_pipe_read(
    void *opaque, uint8_t *buffer, int buffer_size) {
    DirectDecodeContext *context = (DirectDecodeContext *)opaque;
    DWORD read = 0;
    if (!ReadFile(
            context->input, buffer, (DWORD)buffer_size,
            &read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE ||
            error == ERROR_HANDLE_EOF) {
            return AVERROR_EOF;
        }
        return AVERROR_EXTERNAL;
    }
    return read ? (int)read : AVERROR_EOF;
}

static void pcm_converter_free(PcmConverter *converter) {
    if (!converter) return;
    swr_free(&converter->swr);
    av_channel_layout_uninit(&converter->input_layout);
    av_channel_layout_uninit(&converter->output_layout);
    memset(converter, 0, sizeof(*converter));
}

static int pcm_converter_init(
    PcmConverter *converter, const AVFrame *frame,
    const AVCodecContext *decoder, HANDLE output,
    uint32_t wanted_output_rate) {
    const AVChannelLayout *source_layout = &frame->ch_layout;
    AVChannelLayout fallback_layout;
    int result;

    memset(&fallback_layout, 0, sizeof(fallback_layout));
    if (!source_layout->nb_channels) {
        if (decoder->ch_layout.nb_channels) {
            source_layout = &decoder->ch_layout;
        } else {
            int channels = decoder->ch_layout.nb_channels;
            if (!channels) channels = 1;
            av_channel_layout_default(&fallback_layout, channels);
            source_layout = &fallback_layout;
        }
    }
    converter->input_rate =
        frame->sample_rate ? frame->sample_rate : decoder->sample_rate;
    converter->output_rate =
        wanted_output_rate ?
            (int)wanted_output_rate : converter->input_rate;
    converter->input_format =
        (enum AVSampleFormat)frame->format;
    converter->output = output;
    if (converter->input_rate <= 0 ||
        converter->output_rate <= 0 ||
        source_layout->nb_channels <= 0 ||
        av_channel_layout_copy(
            &converter->input_layout, source_layout) < 0 ||
        av_channel_layout_copy(
            &converter->output_layout, source_layout) < 0) {
        av_channel_layout_uninit(&fallback_layout);
        pcm_converter_free(converter);
        return 0;
    }
    converter->channels = converter->output_layout.nb_channels;
    result = swr_alloc_set_opts2(
        &converter->swr,
        &converter->output_layout, AV_SAMPLE_FMT_S16,
        converter->output_rate,
        &converter->input_layout, converter->input_format,
        converter->input_rate, 0, NULL);
    av_channel_layout_uninit(&fallback_layout);
    if (result < 0 || !converter->swr ||
        swr_init(converter->swr) < 0) {
        pcm_converter_free(converter);
        return 0;
    }
    return 1;
}

static int pcm_converter_write(
    PcmConverter *converter, const AVFrame *frame) {
    uint8_t *output_data = NULL;
    int output_linesize = 0;
    int maximum_samples;
    int converted_samples;
    int64_t delay;
    int64_t byte_count;
    int ok;

    delay = swr_get_delay(
        converter->swr, converter->input_rate);
    maximum_samples = (int)av_rescale_rnd(
        delay + frame->nb_samples,
        converter->output_rate, converter->input_rate,
        AV_ROUND_UP);
    if (maximum_samples <= 0 ||
        av_samples_alloc(
            &output_data, &output_linesize,
            converter->channels, maximum_samples,
            AV_SAMPLE_FMT_S16, 0) < 0) {
        return 0;
    }
    converted_samples = swr_convert(
        converter->swr, &output_data, maximum_samples,
        (const uint8_t **)frame->extended_data,
        frame->nb_samples);
    if (converted_samples < 0) {
        av_freep(&output_data);
        return 0;
    }
    byte_count =
        (int64_t)converted_samples * converter->channels *
        (int)sizeof(int16_t);
    ok = byte_count <= UINT32_MAX &&
        write_all(
            converter->output, output_data, (DWORD)byte_count);
    av_freep(&output_data);
    return ok;
}

static int pcm_converter_flush(PcmConverter *converter) {
    for (;;) {
        uint8_t *output_data = NULL;
        int output_linesize = 0;
        int maximum_samples;
        int converted_samples;
        int64_t delay = swr_get_delay(
            converter->swr, converter->input_rate);
        int64_t byte_count;
        int ok;

        if (delay <= 0) return 1;
        maximum_samples = (int)av_rescale_rnd(
            delay, converter->output_rate,
            converter->input_rate, AV_ROUND_UP);
        if (maximum_samples <= 0 ||
            av_samples_alloc(
                &output_data, &output_linesize,
                converter->channels, maximum_samples,
                AV_SAMPLE_FMT_S16, 0) < 0) {
            return 0;
        }
        converted_samples = swr_convert(
            converter->swr, &output_data,
            maximum_samples, NULL, 0);
        if (converted_samples < 0) {
            av_freep(&output_data);
            return 0;
        }
        if (!converted_samples) {
            av_freep(&output_data);
            return 1;
        }
        byte_count =
            (int64_t)converted_samples * converter->channels *
            (int)sizeof(int16_t);
        ok = byte_count <= UINT32_MAX &&
            write_all(
                converter->output, output_data,
                (DWORD)byte_count);
        av_freep(&output_data);
        if (!ok) return 0;
    }
}

static int receive_decoded_frames(
    AVCodecContext *decoder, AVFrame *frame,
    PcmConverter *converter, int *converter_ready,
    HANDLE output, uint32_t output_sample_rate) {
    for (;;) {
        int result = avcodec_receive_frame(decoder, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return 1;
        }
        if (result < 0) return 0;
        if (!*converter_ready) {
            if (!pcm_converter_init(
                    converter, frame, decoder, output,
                    output_sample_rate)) {
                av_frame_unref(frame);
                return 0;
            }
            *converter_ready = 1;
        }
        if (frame->format != converter->input_format ||
            (frame->sample_rate &&
             frame->sample_rate != converter->input_rate) ||
            !pcm_converter_write(converter, frame)) {
            av_frame_unref(frame);
            return 0;
        }
        av_frame_unref(frame);
    }
}

static int decode_embedded_stream(
    HANDLE bank, const XboxBank *bank_spec, const XboxTrack *track,
    int track_index, HANDLE parent_output,
    uint32_t output_sample_rate) {
    SECURITY_ATTRIBUTES security;
    HANDLE input_read = NULL;
    HANDLE input_write = NULL;
    HANDLE writer_thread = NULL;
    DWORD writer_exit_code = 1;
    WrapperWriterContext writer_context;
    DirectDecodeContext direct_context;
    AVIOContext *avio = NULL;
    AVFormatContext *format = NULL;
    AVCodecContext *decoder = NULL;
    const AVInputFormat *input_format;
    const AVCodec *codec;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    PcmConverter converter;
    uint8_t *avio_buffer = NULL;
    int audio_stream = -1;
    int converter_ready = 0;
    int result = 0;
    unsigned int stream_index;

    memset(&converter, 0, sizeof(converter));
    memset(&writer_context, 0, sizeof(writer_context));
    memset(&direct_context, 0, sizeof(direct_context));
    if (!parent_output ||
        parent_output == INVALID_HANDLE_VALUE) {
        return 0;
    }
    input_format = av_find_input_format(
        bank_spec->kind == XBOX_BANK_XWMA_FILE ?
            "xwma" : "wav");
    if (!input_format) return 0;

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = FALSE;
    if (!CreatePipe(
            &input_read, &input_write,
            &security, 256 * 1024)) {
        goto done;
    }
    writer_context.bank = bank;
    writer_context.output = input_write;
    writer_context.bank_spec = bank_spec;
    writer_context.track = track;
    writer_context.track_index = track_index;
    writer_thread = CreateThread(
        NULL, 0, wrapper_writer_thread,
        &writer_context, 0, NULL);
    if (!writer_thread) goto done;
    input_write = NULL;

    av_log_set_level(AV_LOG_QUIET);
    avio_buffer = (uint8_t *)av_malloc(64 * 1024);
    if (!avio_buffer) goto done;
    direct_context.input = input_read;
    avio = avio_alloc_context(
        avio_buffer, 64 * 1024, 0,
        &direct_context, direct_pipe_read, NULL, NULL);
    if (!avio) goto done;
    avio_buffer = NULL;
    format = avformat_alloc_context();
    if (!format) goto done;
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(
            &format, NULL, input_format, NULL) < 0) {
        goto done;
    }
    for (stream_index = 0;
         stream_index < format->nb_streams;
         ++stream_index) {
        if (format->streams[stream_index]->
                codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream = (int)stream_index;
            break;
        }
    }
    if (audio_stream < 0) goto done;
    codec = avcodec_find_decoder(
        format->streams[audio_stream]->codecpar->codec_id);
    if (!codec) goto done;
    decoder = avcodec_alloc_context3(codec);
    if (!decoder ||
        avcodec_parameters_to_context(
            decoder,
            format->streams[audio_stream]->codecpar) < 0 ||
        avcodec_open2(decoder, codec, NULL) < 0) {
        goto done;
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) goto done;

    for (;;) {
        int read_result = av_read_frame(format, packet);
        if (read_result == AVERROR_EOF) break;
        if (read_result < 0) goto done;
        if (packet->stream_index == audio_stream) {
            int send_result = avcodec_send_packet(decoder, packet);
            if (send_result == AVERROR(EAGAIN)) {
                if (!receive_decoded_frames(
                        decoder, frame, &converter,
                        &converter_ready, parent_output,
                        output_sample_rate)) {
                    av_packet_unref(packet);
                    goto done;
                }
                send_result = avcodec_send_packet(decoder, packet);
            }
            if (send_result < 0 ||
                !receive_decoded_frames(
                    decoder, frame, &converter,
                    &converter_ready, parent_output,
                    output_sample_rate)) {
                av_packet_unref(packet);
                goto done;
            }
        }
        av_packet_unref(packet);
    }
    if (avcodec_send_packet(decoder, NULL) < 0 ||
        !receive_decoded_frames(
            decoder, frame, &converter, &converter_ready,
            parent_output, output_sample_rate)) {
        goto done;
    }
    if (!converter_ready ||
        !pcm_converter_flush(&converter)) {
        goto done;
    }
    result = 1;

done:
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (decoder) avcodec_free_context(&decoder);
    if (format) avformat_close_input(&format);
    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    } else {
        av_freep(&avio_buffer);
    }
    pcm_converter_free(&converter);
    if (input_read) {
        CloseHandle(input_read);
        input_read = NULL;
    }
    if (input_write) {
        CloseHandle(input_write);
        input_write = NULL;
    }
    if (writer_thread) {
        WaitForSingleObject(writer_thread, INFINITE);
        GetExitCodeThread(writer_thread, &writer_exit_code);
        CloseHandle(writer_thread);
    }
    return result && writer_exit_code == 0;
}

int audio_decoder_decode_track(
    const WCHAR *asset_name, int track_index,
    int loop_requested, HANDLE output) {
    const XboxBank *bank_spec;
    const XboxTrack *track;
    XboxTrack loaded_track;
    WCHAR bank_path[MAX_PATH * 2];
    WCHAR fallback_relative_path[MAX_PATH * 2];
    const WCHAR *bank_relative_path;
    HANDLE bank = INVALID_HANDLE_VALUE;
    int lines_ps_xbox_fallback = 0;
    int ps3_load_status = 0;
    int result = 1;

    if (!asset_name || !output ||
        output == INVALID_HANDLE_VALUE ||
        (loop_requested &&
         _wcsicmp(asset_name, L"envamb") != 0)) {
        return 2;
    }
    bank_spec = find_bank(asset_name);
    if (!bank_spec) return 3;
    track = find_track(bank_spec, track_index);
    if (bank_spec->kind == XBOX_BANK_XMA2 && !track &&
        load_xma_track(bank_spec, track_index, &loaded_track)) {
        track = &loaded_track;
    }
    if (bank_spec->kind == PS3_BANK_ATRAC3 && !track) {
        ps3_load_status = load_ps3_atrac3_track(
            bank_spec, track_index, &loaded_track);
        if (ps3_load_status > 0) {
            track = &loaded_track;
        } else if (
            ps3_load_status < 0 &&
            _wcsicmp(bank_spec->name, L"linesps") == 0) {
            bank_spec = &k_lines_ps_xbox_fallback;
            lines_ps_xbox_fallback = 1;
        }
    }
    if (track_index < 0 ||
        track_index >= (int)bank_spec->track_count ||
        ((bank_spec->kind == XBOX_BANK_XMA2 ||
          bank_spec->kind == PS3_BANK_ATRAC3) &&
         !track)) {
        return 3;
    }
    bank_relative_path = bank_spec->relative_path;
    if (lines_ps_xbox_fallback) {
        int count = _snwprintf(
            fallback_relative_path,
            ARRAYSIZE(fallback_relative_path) - 1,
            L"data\\lines_ps_xbox_fallback\\cue_%05d.xwma",
            track_index);
        if (count < 0 ||
            count >= (int)ARRAYSIZE(fallback_relative_path) - 1) {
            return 4;
        }
        fallback_relative_path[count] = L'\0';
        bank_relative_path = fallback_relative_path;
    }
    if (!sibling_path(
            bank_path, ARRAYSIZE(bank_path),
            bank_relative_path)) {
        return 4;
    }
    bank = CreateFileW(
        bank_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (bank == INVALID_HANDLE_VALUE) {
        result = 5;
        goto done;
    }
    if (bank_spec->kind == XBOX_BANK_XMA2 &&
        ((!track->samples &&
          !prepare_dynamic_xma_track(
              bank, bank_spec, (XboxTrack *)track)) ||
         !validate_bank(bank, bank_spec, track))) {
        result = 5;
        goto done;
    }
    if (bank_spec->kind == PS3_BANK_ATRAC3 &&
        !validate_ps3_atrac3_bank(bank, bank_spec, track)) {
        result = 5;
        goto done;
    }
    do {
        if (!decode_embedded_stream(
                bank, bank_spec, track, track_index, output,
                lines_ps_xbox_fallback ? 48000u : 0u)) {
            result = 6;
            goto done;
        }
    } while (loop_requested);
    result = 0;

done:
    if (bank != INVALID_HANDLE_VALUE) CloseHandle(bank);
    return result;
}
