/*
 * NBA2K.AudioFix.asi
 *
 * Routes supported NBA 2K audio-bank requests through the decoder
 * and submits PCM to the game's existing XAudio2 backend.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_decoder.h"
#include "game_profile.h"

typedef HRESULT (WINAPI *DirectInput8CreateFn)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HANDLE (WINAPI *CreateFileAFn)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFn)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFileFn)(
    HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandleFn)(HANDLE);

#pragma pack(push, 1)
typedef struct WaveFormatExTrace {
    WORD format_tag;
    WORD channels;
    DWORD samples_per_second;
    DWORD average_bytes_per_second;
    WORD block_align;
    WORD bits_per_sample;
    WORD extra_size;
} WaveFormatExTrace;

typedef struct Xma2WaveFormatTrace {
    WaveFormatExTrace base;
    WORD stream_count;
    DWORD channel_mask;
    DWORD samples_encoded;
    DWORD bytes_per_block;
    DWORD play_begin;
    DWORD play_length;
    DWORD loop_begin;
    DWORD loop_length;
    BYTE loop_count;
    BYTE encoder_version;
    WORD block_count;
} Xma2WaveFormatTrace;
#pragma pack(pop)

typedef struct XAudio2BufferTrace {
    UINT32 flags;
    UINT32 audio_bytes;
    const BYTE *audio_data;
    UINT32 play_begin;
    UINT32 play_length;
    UINT32 loop_begin;
    UINT32 loop_length;
    UINT32 loop_count;
    void *context;
} XAudio2BufferTrace;

typedef struct XAudio2BufferWmaTrace {
    const UINT32 *decoded_packet_cumulative_bytes;
    UINT32 packet_count;
} XAudio2BufferWmaTrace;

typedef struct XAudio2VoiceDetailsTrace {
    UINT32 creation_flags;
    UINT32 input_channels;
    UINT32 input_sample_rate;
} XAudio2VoiceDetailsTrace;

typedef struct XAudio2SendDescriptorTrace {
    UINT32 flags;
    void *output_voice;
} XAudio2SendDescriptorTrace;

typedef struct XAudio2VoiceSendsTrace {
    UINT32 send_count;
    const XAudio2SendDescriptorTrace *sends;
} XAudio2VoiceSendsTrace;

typedef struct XAudio2EffectDescriptorTrace {
    IUnknown *effect;
    BOOL initial_state;
    UINT32 output_channels;
} XAudio2EffectDescriptorTrace;

typedef struct XAudio2EffectChainTrace {
    UINT32 effect_count;
    const XAudio2EffectDescriptorTrace *effect_descriptors;
} XAudio2EffectChainTrace;

typedef struct XAudio2FilterParametersTrace {
    UINT32 type;
    float frequency;
    float one_over_q;
} XAudio2FilterParametersTrace;

typedef struct XAudio2VoiceStateLegacyTrace {
    void *current_buffer_context;
    UINT32 buffers_queued;
    uint64_t samples_played;
} XAudio2VoiceStateLegacyTrace;

typedef HRESULT (__stdcall *CreateSourceVoiceFn)(
    void *, void **, const WaveFormatExTrace *, UINT32, float, void *,
    const void *, const void *);
typedef HRESULT (__stdcall *SubmitSourceBufferFn)(
    void *, const XAudio2BufferTrace *, const XAudio2BufferWmaTrace *);
typedef void (__stdcall *DestroyVoiceFn)(void *);
typedef void (__stdcall *GetVoiceDetailsFn)(
    void *, XAudio2VoiceDetailsTrace *);
typedef HRESULT (__stdcall *SetOutputVoicesFn)(
    void *, const XAudio2VoiceSendsTrace *);
typedef HRESULT (__stdcall *SetEffectChainFn)(
    void *, const XAudio2EffectChainTrace *);
typedef HRESULT (__stdcall *EffectSwitchFn)(void *, UINT32, UINT32);
typedef void (__stdcall *GetEffectStateFn)(void *, UINT32, BOOL *);
typedef HRESULT (__stdcall *SetEffectParametersFn)(
    void *, UINT32, const void *, UINT32, UINT32);
typedef HRESULT (__stdcall *GetEffectParametersFn)(
    void *, UINT32, void *, UINT32);
typedef HRESULT (__stdcall *SetFilterParametersFn)(
    void *, const XAudio2FilterParametersTrace *, UINT32);
typedef void (__stdcall *GetFilterParametersFn)(
    void *, XAudio2FilterParametersTrace *);
typedef HRESULT (__stdcall *SetOutputFilterParametersFn)(
    void *, void *, const XAudio2FilterParametersTrace *, UINT32);
typedef void (__stdcall *GetOutputFilterParametersFn)(
    void *, void *, XAudio2FilterParametersTrace *);
typedef HRESULT (__stdcall *SetVolumeFn)(void *, float, UINT32);
typedef void (__stdcall *GetVolumeFn)(void *, float *);
typedef HRESULT (__stdcall *SetChannelVolumesFn)(
    void *, UINT32, const float *, UINT32);
typedef void (__stdcall *GetChannelVolumesFn)(
    void *, UINT32, float *);
typedef HRESULT (__stdcall *SetOutputMatrixFn)(
    void *, void *, UINT32, UINT32, const float *, UINT32);
typedef void (__stdcall *GetOutputMatrixFn)(
    void *, void *, UINT32, UINT32, float *);
typedef HRESULT (__stdcall *StartStopFn)(void *, UINT32, UINT32);
typedef HRESULT (__stdcall *SimpleSourceVoiceFn)(void *);
typedef HRESULT (__stdcall *ExitLoopFn)(void *, UINT32);
typedef void (__stdcall *GetStateLegacyFn)(
    void *, XAudio2VoiceStateLegacyTrace *);
typedef HRESULT (__stdcall *SetFrequencyRatioFn)(void *, float, UINT32);
typedef void (__stdcall *GetFrequencyRatioFn)(void *, float *);

typedef struct PcmStream PcmStream;
typedef struct PcmBufferContext PcmBufferContext;
typedef struct VoiceCallbackWrapper VoiceCallbackWrapper;

typedef struct VoiceCallbackVtable {
    void (__stdcall *on_voice_processing_pass_start)(void *, UINT32);
    void (__stdcall *on_voice_processing_pass_end)(void *);
    void (__stdcall *on_stream_end)(void *);
    void (__stdcall *on_buffer_start)(void *, void *);
    void (__stdcall *on_buffer_end)(void *, void *);
    void (__stdcall *on_loop_end)(void *, void *);
    void (__stdcall *on_voice_error)(void *, void *, HRESULT);
} VoiceCallbackVtable;

struct VoiceCallbackWrapper {
    const VoiceCallbackVtable *vtable;
    PcmStream *stream;
    void *original_callback;
};

#define PCM_STREAM_BUFFER_COUNT 12
#define PCM_STREAM_BUFFER_LIMIT (512u * 1024u)
#define PCM_STREAM_BUFFER_MAGIC 0x4D435058u
/*
 * Give a prefetched decoder enough kernel-side headroom to finish the first
 * several XAudio2 submissions before the game claims it.  This remains a
 * bounded streaming buffer; it is not a persistent decoded-audio cache.
 */
#define PCM_DECODE_PIPE_BUFFER (1024u * 1024u)

struct PcmBufferContext {
    DWORD magic;
    PcmStream *stream;
    void *original_context;
    BYTE *data;
    DWORD capacity;
    DWORD audio_bytes;
    int in_use;
};

struct PcmStream {
    CRITICAL_SECTION lock;
    int lock_initialized;
    PcmAssetKind asset;
    int track;
    HANDLE output_read;
    HANDLE decoder_worker;
    DWORD decoder_worker_id;
    WORD output_channels;
    DWORD output_sample_rate;
    DWORD output_bytes_per_frame;
    uint64_t expected_bytes;
    uint64_t bytes_read;
    uint64_t outstanding_bytes;
    uint64_t peak_outstanding_bytes;
    DWORD outstanding_buffers;
    DWORD peak_outstanding_buffers;
    DWORD loop_count;
    DWORD start_tick;
    int failed;
    int retired;
    void *owner_voice;
    PcmStream *next_retired;
    VoiceCallbackWrapper callback;
    PcmBufferContext buffers[PCM_STREAM_BUFFER_COUNT];
};

/*
 * Archive reads identify the exact console cue before XAudio2 submits its
 * first compressed packet. Use that lead time to start the decoder away
 * from the game's read and audio-submit threads.
 *
 * The ready cache is intentionally small: each prefetched stream can fill a
 * bounded pipe. A single worker prevents request storms when the game reads
 * several cues at once.
 */
#define PCM_PREFETCH_REQUEST_CAPACITY 32
#define PCM_PREFETCH_READY_CAPACITY 8
#define PCM_PREFETCH_STALE_MS 15000u

typedef struct PcmPrefetchRequest {
    LONG selection;
    DWORD queued_tick;
} PcmPrefetchRequest;

typedef struct PcmPrefetchReady {
    LONG selection;
    DWORD queued_tick;
    DWORD ready_tick;
    PcmStream *stream;
} PcmPrefetchReady;

typedef struct TrackedFile {
    HANDLE handle;
    char path[MAX_PATH * 2];
} TrackedFile;

#define DELAYED_MAX_SENDS 8
#define DELAYED_MAX_EFFECTS 8
#define DELAYED_MAX_MATRICES 8
#define DELAYED_MAX_MATRIX_LEVELS 64
#define DELAYED_MAX_OUTPUT_FILTERS 8
#define DELAYED_MAX_EFFECT_PARAMETER_RECORDS 8
#define DELAYED_MAX_EFFECT_PARAMETER_BYTES 256

typedef struct DelayedMatrixState {
    int valid;
    void *destination_voice;
    UINT32 source_channels;
    UINT32 destination_channels;
    float levels[DELAYED_MAX_MATRIX_LEVELS];
} DelayedMatrixState;

typedef struct DelayedOutputFilterState {
    int valid;
    void *destination_voice;
    XAudio2FilterParametersTrace parameters;
} DelayedOutputFilterState;

typedef struct DelayedEffectParameterState {
    int valid;
    UINT32 effect_index;
    UINT32 byte_count;
    BYTE bytes[DELAYED_MAX_EFFECT_PARAMETER_BYTES];
} DelayedEffectParameterState;

#define PCM_NATIVE_SIDECAR_COUNT 1
#define PCM_SELECTION_SIGNATURE_COUNT 6
typedef struct PcmPayloadSignature {
    DWORD audio_bytes;
    uint64_t payload_hash;
    BYTE prefix[16];
    BYTE prefix_size;
} PcmPayloadSignature;

typedef struct PcmNativeSidecar {
    void *voice;
    WORD channels;
    DWORD sample_rate;
    VoiceCallbackWrapper callback;
} PcmNativeSidecar;

typedef struct TrackedVoice {
    void *voice;
    WORD format_tag;
    WORD channels;
    DWORD samples_per_second;
    DWORD submit_count;
    int pcm_substitute;
    int pcm_track;
    float logical_volume;
    float pcm_gain;
    PcmStream *pcm_stream;
    uint64_t pcm_size;
    uint64_t pcm_frames;
    uint64_t pc_frames_consumed;
    uint64_t pcm_frames_submitted;
    uint64_t pcm_source_frames_submitted;
    int pcm_end_submitted;
    BYTE pcm_signature_count;
    PcmPayloadSignature
        pcm_signatures[PCM_SELECTION_SIGNATURE_COUNT];
    void *redirect_voice;
    PcmNativeSidecar native_sidecars[PCM_NATIVE_SIDECAR_COUNT];
    int native_sidecar_count;
    PcmNativeSidecar *active_native_sidecar;
    void *create_engine;
    UINT32 create_flags;
    float create_maximum_frequency_ratio;
    void *create_callback;
    XAudio2VoiceSendsTrace create_sends;
    XAudio2SendDescriptorTrace
        create_send_descriptors[DELAYED_MAX_SENDS];
    XAudio2EffectChainTrace create_effect_chain;
    XAudio2EffectDescriptorTrace
        create_effect_descriptors[DELAYED_MAX_EFFECTS];
    int delayed_started;
    int delayed_filter_valid;
    XAudio2FilterParametersTrace delayed_filter;
    int delayed_matrix_count;
    DelayedMatrixState delayed_matrices[DELAYED_MAX_MATRICES];
    int delayed_output_filter_count;
    DelayedOutputFilterState
        delayed_output_filters[DELAYED_MAX_OUTPUT_FILTERS];
    BYTE delayed_effect_state_valid[DELAYED_MAX_EFFECTS];
    BYTE delayed_effect_enabled[DELAYED_MAX_EFFECTS];
    int delayed_effect_parameter_count;
    DelayedEffectParameterState delayed_effect_parameters[
        DELAYED_MAX_EFFECT_PARAMETER_RECORDS];
} TrackedVoice;

typedef struct VoiceVolumeState {
    void *target;
    float logical_volume;
    float pcm_gain;
    int pcm_substitute;
} VoiceVolumeState;

typedef struct SubmitVtable {
    void **vtable;
    void *original_slots[28];
} SubmitVtable;


typedef struct PcmSubmitPlan {
    int valid;
    int voice_index;
    DWORD decoded_pc_bytes;
    uint64_t old_pc_frames;
    uint64_t old_pcm_frames;
    uint64_t new_pc_frames;
    uint64_t new_pcm_frames;
    uint64_t old_pcm_source_frames;
    uint64_t new_pcm_source_frames;
    DWORD pcm_bytes_per_frame;
    XAudio2BufferTrace buffer;
    PcmBufferContext *stream_buffer;
} PcmSubmitPlan;

static CreateFileAFn g_real_create_file_a;
static CreateFileWFn g_real_create_file_w;
static ReadFileFn g_real_read_file;
static CloseHandleFn g_real_close_handle;
static CRITICAL_SECTION g_lock;
static LONG g_initialized;
static LONG g_audio_hook_installed;
static TrackedFile g_files[128];
static TrackedVoice g_voices[128];
static PcmStream *g_retired_pcm_streams;
static PcmPrefetchRequest
    g_pcm_prefetch_requests[PCM_PREFETCH_REQUEST_CAPACITY];
static int g_pcm_prefetch_request_count;
static PcmPrefetchReady
    g_pcm_prefetch_ready[PCM_PREFETCH_READY_CAPACITY];
static int g_pcm_prefetch_ready_count;
static HANDLE g_pcm_prefetch_event;
static HANDLE g_pcm_prefetch_thread;
static volatile LONG g_pcm_prefetch_stop;
static LONG g_pcm_prefetch_inflight_selection = -1;
static int g_pcm_prefetch_inflight_canceled;
static HANDLE g_pcm_reaper_event;
static HANDLE g_pcm_reaper_thread;
static volatile LONG g_pcm_reaper_stop;
static SubmitVtable g_submit_vtables[16];
static CreateSourceVoiceFn g_real_create_source_voice;
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
typedef struct DynamicCueMapEntry {
    int64_t pc_start_offset;
    uint32_t expected_pcm_size;
    uint32_t reserved;
} DynamicCueMapEntry;

typedef char DynamicCueMapEntrySizeCheck[
    sizeof(DynamicCueMapEntry) == 16 ? 1 : -1];

typedef struct DynamicCueMap {
    DynamicCueMapEntry *entries;
    int capacity;
    const WCHAR *relative_path;
    const char *asset_name;
    volatile LONG state;
    int count;
} DynamicCueMap;

/*
 * File reads can run several cues ahead of XAudio2 voice creation.  A single
 * "last cue read" slot therefore binds the newest prefetched cue to an older
 * voice.  Preserve the read order and consume the oldest format-compatible
 * cue instead.
 */
#define PCM_SELECTION_QUEUE_CAPACITY 256
typedef struct PcmSelectionRecord {
    LONG selection;
    DWORD thread_id;
    DWORD queued_tick;
    BYTE signature_count;
    PcmPayloadSignature signatures[PCM_SELECTION_SIGNATURE_COUNT];
} PcmSelectionRecord;

#define PCM_SELECTION(asset, cue) \
    ((((LONG)(asset)) << 16) | ((LONG)(cue) & 0xFFFFL))

static volatile LONG g_pcm_selected_cue = -1;
/*
 * Arena gameplay reuses a small set of WMA source voices for many unrelated
 * banks and cues.  The current one-cue-per-voice router is safe for the
 * menu/loading/mentor paths, but permanently binding an arena voice to the
 * first queued cue makes later commentary mute and can route crowd audio to
 * the wrong voice. Keep the complete dataset available while leaving those
 * reusable voices on the game's original routing path.
 *
 * The override is used only by the local lifecycle harness.
 */
static volatile LONG g_pcm_arena_gate_override;
static PcmSelectionRecord
    g_pcm_selection_queue[PCM_SELECTION_QUEUE_CAPACITY];
static int g_pcm_selection_count;
static DynamicCueMapEntry
    g_jukebox_tracks_dynamic[JUKEBOX_TRACK_COUNT];
static DynamicCueMapEntry
    g_loading_sequence_tracks_dynamic[LOADING_SEQUENCE_TRACK_COUNT];
static DynamicCueMapEntry
    g_loading_sequence_jm_tracks_dynamic[
        LOADING_SEQUENCE_JM_TRACK_COUNT];
static DynamicCueMapEntry g_mentor_tracks[MENTOR_TRACK_COUNT];
static DynamicCueMapEntry g_mentor_ts_tracks[MENTOR_TS_TRACK_COUNT];
static DynamicCueMapEntry g_loading_vo_tracks[LOADING_VO_TRACK_COUNT];
static DynamicCueMapEntry g_loading_vo_ps_tracks[LOADING_VO_PS_TRACK_COUNT];
static DynamicCueMapEntry g_loading_vo_ts_tracks[LOADING_VO_TS_TRACK_COUNT];
static DynamicCueMapEntry g_lines_cs_tracks[LINES_CS_TRACK_COUNT];
static DynamicCueMapEntry g_env_amb_tracks[ENV_AMB_TRACK_COUNT];
static DynamicCueMapEntry
    g_overlay_audio_tracks[OVERLAY_AUDIO_TRACK_COUNT];
static DynamicCueMapEntry g_cairball_tracks[CAIRBALL_TRACK_COUNT];
static DynamicCueMapEntry g_ai_street_tracks[AI_STREET_TRACK_COUNT];
static DynamicCueMapEntry g_cwd_loop_tracks[CWD_LOOP_TRACK_COUNT];
static DynamicCueMapEntry g_ao_street_tracks[AO_STREET_TRACK_COUNT];
static DynamicCueMapEntry g_as_street_tracks[AS_STREET_TRACK_COUNT];
static DynamicCueMapEntry g_dunk_sfx_tracks[DUNK_SFX_TRACK_COUNT];
static DynamicCueMapEntry g_coaches_tracks[COACHES_TRACK_COUNT];
static DynamicCueMapEntry g_teams_tracks[TEAMS_TRACK_COUNT];
static DynamicCueMapEntry g_players_tracks[PLAYERS_TRACK_COUNT];
static DynamicCueMapEntry g_press_conf_tracks[PRESS_CONF_TRACK_COUNT];
static DynamicCueMapEntry g_pa_players_tracks[PA_PLAYERS_TRACK_COUNT];
static DynamicCueMapEntry g_cached_lines_tracks[CACHED_LINES_TRACK_COUNT];
static DynamicCueMapEntry g_lines_tracks[LINES_TRACK_COUNT];
static DynamicCueMapEntry g_lines_ps_tracks[LINES_PS_TRACK_COUNT];
static DynamicCueMapEntry g_lines_ts_tracks[LINES_TS_TRACK_COUNT];
static DynamicCueMapEntry g_pa_lines_tracks[PA_LINES_TRACK_COUNT];
static DynamicCueMapEntry
    g_cwd_str_loop_dunk_contest_tracks[
        CWD_STR_LOOP_DUNK_CONTEST_TRACK_COUNT];
static DynamicCueMapEntry
    g_cwd_str_loop_gym_tracks[CWD_STR_LOOP_GYM_TRACK_COUNT];
static DynamicCueMapEntry
    g_cwd_str_loop_inside_tracks[CWD_STR_LOOP_INSIDE_TRACK_COUNT];
static DynamicCueMapEntry
    g_cwd_str_sfx_dunk_contest_tracks[
        CWD_STR_SFX_DUNK_CONTEST_TRACK_COUNT];
static DynamicCueMapEntry
    g_cwd_str_sfx_inside_tracks[CWD_STR_SFX_INSIDE_TRACK_COUNT];
static DynamicCueMapEntry g_event_music_tracks[EVENT_MUSIC_TRACK_COUNT];
static DynamicCueMapEntry g_load_m_tracks[LOAD_M_TRACK_COUNT];
static DynamicCueMapEntry
    g_streamed_chatter_tracks[STREAMED_CHATTER_TRACK_COUNT];
static DynamicCueMapEntry
    g_streamed_player_chatter_tracks[
        STREAMED_PLAYER_CHATTER_TRACK_COUNT];

static int pcm_asset_runtime_binding_enabled(PcmAssetKind asset) {
    (void)asset;
    return 1;
}
static DynamicCueMap g_jukebox_map = {
    g_jukebox_tracks_dynamic, JUKEBOX_TRACK_COUNT,
    L"data\\jukeboxmusic_pcm_map.bin", "jukebox", 0, 0,
};
static DynamicCueMap g_loading_sequence_map = {
    g_loading_sequence_tracks_dynamic, LOADING_SEQUENCE_TRACK_COUNT,
    L"data\\loadingsequence_pcm_map.bin",
    "loadingsequence", 0, 0,
};
static DynamicCueMap g_loading_sequence_jm_map = {
    g_loading_sequence_jm_tracks_dynamic,
    LOADING_SEQUENCE_JM_TRACK_COUNT,
    L"data\\loadingsequencejm_pcm_map.bin",
    "loadingsequencejm", 0, 0,
};
static DynamicCueMap g_mentor_map = {
    g_mentor_tracks, MENTOR_TRACK_COUNT,
    L"data\\mentor_pcm_map.bin", "mentor", 0, 0,
};
static DynamicCueMap g_mentor_ts_map = {
    g_mentor_ts_tracks, MENTOR_TS_TRACK_COUNT,
    L"data\\mentor_ts_pcm_map.bin", "mentorts", 0, 0,
};
static DynamicCueMap g_loading_vo_map = {
    g_loading_vo_tracks, LOADING_VO_TRACK_COUNT,
    L"data\\loadingvo_pcm_map.bin", "loadingvo", 0, 0,
};
static DynamicCueMap g_loading_vo_ps_map = {
    g_loading_vo_ps_tracks, LOADING_VO_PS_TRACK_COUNT,
    L"data\\loadingvo_ps_pcm_map.bin", "loadingvops", 0, 0,
};
static DynamicCueMap g_loading_vo_ts_map = {
    g_loading_vo_ts_tracks, LOADING_VO_TS_TRACK_COUNT,
    L"data\\loadingvo_ts_pcm_map.bin", "loadingvots", 0, 0,
};
static DynamicCueMap g_lines_cs_map = {
    g_lines_cs_tracks, LINES_CS_TRACK_COUNT,
    L"data\\lines_cs_pcm_map.bin", "linescs", 0, 0,
};
static DynamicCueMap g_env_amb_map = {
    g_env_amb_tracks, ENV_AMB_TRACK_COUNT,
    L"data\\env_amb_pcm_map.bin", "envamb", 0, 0,
};
static DynamicCueMap g_overlay_audio_map = {
    g_overlay_audio_tracks, OVERLAY_AUDIO_TRACK_COUNT,
    L"data\\overlayaudio_pcm_map.bin", "overlayaudio", 0, 0,
};
static DynamicCueMap g_cairball_map = {
    g_cairball_tracks, CAIRBALL_TRACK_COUNT,
    L"data\\cairball_pcm_map.bin", "cairball", 0, 0,
};
static DynamicCueMap g_ai_street_map = {
    g_ai_street_tracks, AI_STREET_TRACK_COUNT,
    L"data\\aistreet_pcm_map.bin", "aistreet", 0, 0,
};
static DynamicCueMap g_cwd_loop_map = {
    g_cwd_loop_tracks, CWD_LOOP_TRACK_COUNT,
    L"data\\cwdloop_pcm_map.bin", "cwdloop", 0, 0,
};
static DynamicCueMap g_ao_street_map = {
    g_ao_street_tracks, AO_STREET_TRACK_COUNT,
    L"data\\aostreet_pcm_map.bin", "aostreet", 0, 0,
};
static DynamicCueMap g_as_street_map = {
    g_as_street_tracks, AS_STREET_TRACK_COUNT,
    L"data\\asstreet_pcm_map.bin", "asstreet", 0, 0,
};
static DynamicCueMap g_dunk_sfx_map = {
    g_dunk_sfx_tracks, DUNK_SFX_TRACK_COUNT,
    L"data\\dunksfx_pcm_map.bin", "dunksfx", 0, 0,
};
static DynamicCueMap g_coaches_map = {
    g_coaches_tracks, COACHES_TRACK_COUNT,
    L"data\\coaches_pcm_map.bin", "coaches", 0, 0,
};
static DynamicCueMap g_teams_map = {
    g_teams_tracks, TEAMS_TRACK_COUNT,
    L"data\\teams_pcm_map.bin", "teams", 0, 0,
};
static DynamicCueMap g_players_map = {
    g_players_tracks, PLAYERS_TRACK_COUNT,
    L"data\\players_pcm_map.bin", "players", 0, 0,
};
static DynamicCueMap g_press_conf_map = {
    g_press_conf_tracks, PRESS_CONF_TRACK_COUNT,
    L"data\\pressconf_pcm_map.bin", "pressconf", 0, 0,
};
static DynamicCueMap g_pa_players_map = {
    g_pa_players_tracks, PA_PLAYERS_TRACK_COUNT,
    L"data\\paplayers_pcm_map.bin", "paplayers", 0, 0,
};
static DynamicCueMap g_cached_lines_map = {
    g_cached_lines_tracks, CACHED_LINES_TRACK_COUNT,
    L"data\\cachedlines_pcm_map.bin", "cachedlines", 0, 0,
};
static DynamicCueMap g_lines_map = {
    g_lines_tracks, LINES_TRACK_COUNT,
    L"data\\lines_pcm_map.bin", "lines", 0, 0,
};
static DynamicCueMap g_lines_ps_map = {
    g_lines_ps_tracks, LINES_PS_TRACK_COUNT,
    L"data\\lines_ps_pcm_map.bin", "linesps", 0, 0,
};
static DynamicCueMap g_lines_ts_map = {
    g_lines_ts_tracks, LINES_TS_TRACK_COUNT,
    L"data\\lines_ts_pcm_map.bin", "linests", 0, 0,
};
static DynamicCueMap g_pa_lines_map = {
    g_pa_lines_tracks, PA_LINES_TRACK_COUNT,
    L"data\\palines_pcm_map.bin", "palines", 0, 0,
};
static DynamicCueMap g_cwd_str_loop_dunk_contest_map = {
    g_cwd_str_loop_dunk_contest_tracks,
    CWD_STR_LOOP_DUNK_CONTEST_TRACK_COUNT,
    L"data\\cwdstrloop_dunkcontest_pcm_map.bin",
    "cwdstrloopdunkcontest", 0, 0,
};
static DynamicCueMap g_cwd_str_loop_gym_map = {
    g_cwd_str_loop_gym_tracks, CWD_STR_LOOP_GYM_TRACK_COUNT,
    L"data\\cwdstrloop_gym_pcm_map.bin",
    "cwdstrloopgym", 0, 0,
};
static DynamicCueMap g_cwd_str_loop_inside_map = {
    g_cwd_str_loop_inside_tracks, CWD_STR_LOOP_INSIDE_TRACK_COUNT,
    L"data\\cwdstrloop_inside_pcm_map.bin",
    "cwdstrloopinside", 0, 0,
};
static DynamicCueMap g_cwd_str_sfx_dunk_contest_map = {
    g_cwd_str_sfx_dunk_contest_tracks,
    CWD_STR_SFX_DUNK_CONTEST_TRACK_COUNT,
    L"data\\cwdstrsfx_dunkcontest_pcm_map.bin",
    "cwdstrsfxdunkcontest", 0, 0,
};
static DynamicCueMap g_cwd_str_sfx_inside_map = {
    g_cwd_str_sfx_inside_tracks, CWD_STR_SFX_INSIDE_TRACK_COUNT,
    L"data\\cwdstrsfx_inside_pcm_map.bin",
    "cwdstrsfxinside", 0, 0,
};
static DynamicCueMap g_event_music_map = {
    g_event_music_tracks, EVENT_MUSIC_TRACK_COUNT,
    L"data\\eventmusic_pcm_map.bin", "eventmusic", 0, 0,
};
static DynamicCueMap g_load_m_map = {
    g_load_m_tracks, LOAD_M_TRACK_COUNT,
    L"data\\loadm_pcm_map.bin", "loadm", 0, 0,
};
static DynamicCueMap g_streamed_chatter_map = {
    g_streamed_chatter_tracks, STREAMED_CHATTER_TRACK_COUNT,
    L"data\\streamedchatter_pcm_map.bin",
    "streamedchatter", 0, 0,
};
static DynamicCueMap g_streamed_player_chatter_map = {
    g_streamed_player_chatter_tracks,
    STREAMED_PLAYER_CHATTER_TRACK_COUNT,
    L"data\\streamedplayerchatter_pcm_map.bin",
    "streamedplayerchatter", 0, 0,
};
extern IMAGE_DOS_HEADER __ImageBase;

#define IXAUDIO2_CREATE_SOURCE_VOICE_SLOT 8u
#define IXAUDIO2_VOICE_GET_DETAILS_SLOT 0u
#define IXAUDIO2_VOICE_SET_OUTPUT_VOICES_SLOT 1u
#define IXAUDIO2_VOICE_SET_EFFECT_CHAIN_SLOT 2u
#define IXAUDIO2_VOICE_ENABLE_EFFECT_SLOT 3u
#define IXAUDIO2_VOICE_DISABLE_EFFECT_SLOT 4u
#define IXAUDIO2_VOICE_GET_EFFECT_STATE_SLOT 5u
#define IXAUDIO2_VOICE_SET_EFFECT_PARAMETERS_SLOT 6u
#define IXAUDIO2_VOICE_GET_EFFECT_PARAMETERS_SLOT 7u
#define IXAUDIO2_VOICE_SET_FILTER_PARAMETERS_SLOT 8u
#define IXAUDIO2_VOICE_GET_FILTER_PARAMETERS_SLOT 9u
#define IXAUDIO2_VOICE_SET_OUTPUT_FILTER_PARAMETERS_SLOT 10u
#define IXAUDIO2_VOICE_GET_OUTPUT_FILTER_PARAMETERS_SLOT 11u
#define IXAUDIO2_VOICE_SET_VOLUME_SLOT 12u
#define IXAUDIO2_VOICE_GET_VOLUME_SLOT 13u
#define IXAUDIO2_VOICE_SET_CHANNEL_VOLUMES_SLOT 14u
#define IXAUDIO2_VOICE_GET_CHANNEL_VOLUMES_SLOT 15u
#define IXAUDIO2_VOICE_SET_OUTPUT_MATRIX_SLOT 16u
#define IXAUDIO2_VOICE_GET_OUTPUT_MATRIX_SLOT 17u
#define IXAUDIO2_VOICE_DESTROY_SLOT 18u
#define IXAUDIO2_SOURCE_START_SLOT 19u
#define IXAUDIO2_SOURCE_STOP_SLOT 20u
#define IXAUDIO2_SOURCE_SUBMIT_BUFFER_SLOT 21u
#define IXAUDIO2_SOURCE_FLUSH_SLOT 22u
#define IXAUDIO2_SOURCE_DISCONTINUITY_SLOT 23u
#define IXAUDIO2_SOURCE_EXIT_LOOP_SLOT 24u
#define IXAUDIO2_SOURCE_GET_STATE_SLOT 25u
#define IXAUDIO2_SOURCE_SET_FREQUENCY_RATIO_SLOT 26u
#define IXAUDIO2_SOURCE_GET_FREQUENCY_RATIO_SLOT 27u
#define IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT 28u

static void sibling_path(WCHAR *path, DWORD capacity, const WCHAR *filename);
static void enqueue_pcm_selection_with_data(
    LONG selection, DWORD thread_id, const void *data, DWORD data_bytes);
static void queue_pcm_prefetch(LONG selection, DWORD queued_tick);
static PcmStream *take_prefetched_pcm_stream(
    PcmAssetKind asset, int track, void *original_callback);
static int start_pcm_prefetch_worker(void);
static void stop_pcm_prefetch_worker(void);
static int start_pcm_reaper_worker(void);
static void stop_pcm_reaper_worker(void);
static int activate_native_pcm_stream(
    void *voice, LONG selection,
    const PcmSelectionRecord *selection_record,
    void **previous_target_out, void **new_target_out);
static void apply_active_pcm_gain(void *voice, void *target);
static void flush_reused_native_sidecar_before_submit(
    void *voice, void *previous_target, void *new_target);
static void switch_started_native_voice_after_submit(
    void *voice, void *previous_target, void *new_target);

static int ascii_lower(int value) {
    return (value >= 'A' && value <= 'Z') ? value + ('a' - 'A') : value;
}

static int ascii_iequals(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

static int ascii_icontains(const char *text, const char *needle) {
    size_t needle_len = strlen(needle);
    const char *cursor;
    if (!needle_len) return 1;
    for (cursor = text; *cursor; ++cursor) {
        size_t index;
        for (index = 0; index < needle_len; ++index) {
            if (!cursor[index] ||
                ascii_lower((unsigned char)cursor[index]) !=
                ascii_lower((unsigned char)needle[index])) break;
        }
        if (index == needle_len) return 1;
    }
    return 0;
}

static DynamicCueMap *dynamic_map_for_asset(PcmAssetKind asset) {
    switch (asset) {
        case PCM_ASSET_JUKEBOX: return &g_jukebox_map;
        case PCM_ASSET_LOADING_SEQUENCE:
            return &g_loading_sequence_map;
        case PCM_ASSET_LOADING_SEQUENCE_JM:
            return &g_loading_sequence_jm_map;
        case PCM_ASSET_MENTOR: return &g_mentor_map;
        case PCM_ASSET_MENTOR_TS: return &g_mentor_ts_map;
        case PCM_ASSET_LOADING_VO: return &g_loading_vo_map;
        case PCM_ASSET_LOADING_VO_PS: return &g_loading_vo_ps_map;
        case PCM_ASSET_LOADING_VO_TS: return &g_loading_vo_ts_map;
        case PCM_ASSET_LINES_CS: return &g_lines_cs_map;
        case PCM_ASSET_ENV_AMB: return &g_env_amb_map;
        case PCM_ASSET_OVERLAY_AUDIO: return &g_overlay_audio_map;
        case PCM_ASSET_CAIRBALL: return &g_cairball_map;
        case PCM_ASSET_AI_STREET: return &g_ai_street_map;
        case PCM_ASSET_CWD_LOOP: return &g_cwd_loop_map;
        case PCM_ASSET_AO_STREET: return &g_ao_street_map;
        case PCM_ASSET_AS_STREET: return &g_as_street_map;
        case PCM_ASSET_DUNK_SFX: return &g_dunk_sfx_map;
        case PCM_ASSET_COACHES: return &g_coaches_map;
        case PCM_ASSET_TEAMS: return &g_teams_map;
        case PCM_ASSET_PLAYERS: return &g_players_map;
        case PCM_ASSET_PRESS_CONF: return &g_press_conf_map;
        case PCM_ASSET_PA_PLAYERS: return &g_pa_players_map;
        case PCM_ASSET_CACHED_LINES: return &g_cached_lines_map;
        case PCM_ASSET_LINES: return &g_lines_map;
        case PCM_ASSET_LINES_PS: return &g_lines_ps_map;
        case PCM_ASSET_LINES_TS: return &g_lines_ts_map;
        case PCM_ASSET_PA_LINES: return &g_pa_lines_map;
        case PCM_ASSET_CWD_STR_LOOP_DUNK_CONTEST:
            return &g_cwd_str_loop_dunk_contest_map;
        case PCM_ASSET_CWD_STR_LOOP_GYM:
            return &g_cwd_str_loop_gym_map;
        case PCM_ASSET_CWD_STR_LOOP_INSIDE:
            return &g_cwd_str_loop_inside_map;
        case PCM_ASSET_CWD_STR_SFX_DUNK_CONTEST:
            return &g_cwd_str_sfx_dunk_contest_map;
        case PCM_ASSET_CWD_STR_SFX_INSIDE:
            return &g_cwd_str_sfx_inside_map;
        case PCM_ASSET_EVENT_MUSIC: return &g_event_music_map;
        case PCM_ASSET_LOAD_M: return &g_load_m_map;
        case PCM_ASSET_STREAMED_CHATTER:
            return &g_streamed_chatter_map;
        case PCM_ASSET_STREAMED_PLAYER_CHATTER:
            return &g_streamed_player_chatter_map;
        default: return NULL;
    }
}

static int dynamic_map_reserved_is_valid(
    const DynamicCueMap *map, int index, uint32_t version,
    uint32_t reserved) {
    int previous;
    if (version == 1) return reserved == 0;
    if (map == &g_lines_ps_map && reserved == UINT32_MAX) {
        return index >= 18940 && index < 18972;
    }
    if (index == 0) return reserved == 0;
    previous = index - 1;
    while (
        previous >= 0 &&
        map->entries[previous].reserved == UINT32_MAX) {
        --previous;
    }
    return previous >= 0 &&
        reserved > map->entries[previous].reserved;
}

static int load_dynamic_map(PcmAssetKind asset) {
    DynamicCueMap *map = dynamic_map_for_asset(asset);
    LONG previous;
    WCHAR path[MAX_PATH * 2];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE header[16];
    DWORD read = 0;
    uint32_t version;
    uint32_t count;
    uint32_t entry_size;
    int index;

    if (!map) return 0;
    previous = InterlockedCompareExchange(&map->state, 1, 0);
    if (previous != 0) {
        while (map->state == 1) Sleep(0);
        return map->state == 2;
    }
    sibling_path(path, ARRAYSIZE(path), map->relative_path);
    if (!path[0]) goto failed;
    file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(file, &size) ||
        size.QuadPart !=
            16LL + (LONGLONG)map->capacity * 16LL ||
        !ReadFile(file, header, sizeof(header), &read, NULL) ||
        read != sizeof(header) ||
        memcmp(header, "MPCM", 4) != 0) {
        goto failed;
    }
    memcpy(&version, header + 4, sizeof(version));
    memcpy(&count, header + 8, sizeof(count));
    memcpy(&entry_size, header + 12, sizeof(entry_size));
    if ((version != 1 && version != 2) ||
        count != (uint32_t)map->capacity ||
        entry_size != sizeof(DynamicCueMapEntry)) {
        goto failed;
    }
    for (index = 0; index < map->capacity; ++index) {
        DynamicCueMapEntry entry;
        read = 0;
        if (!ReadFile(
                file, &entry, sizeof(entry), &read, NULL) ||
            read != sizeof(entry) ||
            !dynamic_map_reserved_is_valid(
                map, index, version, entry.reserved) ||
            entry.expected_pcm_size == 0 ||
            (entry.expected_pcm_size & 1u) ||
             entry.pc_start_offset < 0 ||
            (index == 0 && entry.pc_start_offset != 0) ||
            (index > 0 &&
             entry.pc_start_offset <=
                map->entries[index - 1].pc_start_offset)) {
            goto failed;
        }
        map->entries[index] = entry;
    }
    CloseHandle(file);
    map->count = map->capacity;
    InterlockedExchange(&map->state, 2);
    return 1;

failed:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    map->count = 0;
    InterlockedExchange(&map->state, -1);
    return 0;
}


static const WCHAR *pcm_asset_name_w(PcmAssetKind asset) {
    switch (asset) {
        case PCM_ASSET_JUKEBOX: return L"jukebox";
        case PCM_ASSET_LOADING_SEQUENCE: return L"loadingsequence";
        case PCM_ASSET_LOADING_SEQUENCE_JM: return L"loadingsequencejm";
        case PCM_ASSET_MENTOR: return L"mentor";
        case PCM_ASSET_MENTOR_TS: return L"mentorts";
        case PCM_ASSET_LOADING_VO: return L"loadingvo";
        case PCM_ASSET_LOADING_VO_PS: return L"loadingvops";
        case PCM_ASSET_LOADING_VO_TS: return L"loadingvots";
        case PCM_ASSET_LINES_CS: return L"linescs";
        case PCM_ASSET_ENV_AMB: return L"envamb";
        case PCM_ASSET_OVERLAY_AUDIO: return L"overlayaudio";
        case PCM_ASSET_CAIRBALL: return L"cairball";
        case PCM_ASSET_AI_STREET: return L"aistreet";
        case PCM_ASSET_CWD_LOOP: return L"cwdloop";
        case PCM_ASSET_AO_STREET: return L"aostreet";
        case PCM_ASSET_AS_STREET: return L"asstreet";
        case PCM_ASSET_DUNK_SFX: return L"dunksfx";
        case PCM_ASSET_COACHES: return L"coaches";
        case PCM_ASSET_TEAMS: return L"teams";
        case PCM_ASSET_PLAYERS: return L"players";
        case PCM_ASSET_PRESS_CONF: return L"pressconf";
        case PCM_ASSET_PA_PLAYERS: return L"paplayers";
        case PCM_ASSET_CACHED_LINES: return L"cachedlines";
        case PCM_ASSET_LINES: return L"lines";
        case PCM_ASSET_LINES_PS: return L"linesps";
        case PCM_ASSET_LINES_TS: return L"linests";
        case PCM_ASSET_PA_LINES: return L"palines";
        case PCM_ASSET_CWD_STR_LOOP_DUNK_CONTEST:
            return L"cwdstrloopdunkcontest";
        case PCM_ASSET_CWD_STR_LOOP_GYM: return L"cwdstrloopgym";
        case PCM_ASSET_CWD_STR_LOOP_INSIDE:
            return L"cwdstrloopinside";
        case PCM_ASSET_CWD_STR_SFX_DUNK_CONTEST:
            return L"cwdstrsfxdunkcontest";
        case PCM_ASSET_CWD_STR_SFX_INSIDE:
            return L"cwdstrsfxinside";
        case PCM_ASSET_EVENT_MUSIC: return L"eventmusic";
        case PCM_ASSET_LOAD_M: return L"loadm";
        case PCM_ASSET_STREAMED_CHATTER: return L"streamedchatter";
        case PCM_ASSET_STREAMED_PLAYER_CHATTER:
            return L"streamedplayerchatter";
        default: return L"none";
    }
}


static int pcm_asset_track_count(PcmAssetKind asset) {
    DynamicCueMap *map = dynamic_map_for_asset(asset);
    return map && load_dynamic_map(asset) ? map->count : 0;
}

static uint32_t pcm_asset_expected_bytes(
    PcmAssetKind asset, int track) {
    DynamicCueMap *map;
    if (track < 0 || track >= pcm_asset_track_count(asset)) return 0;
    map = dynamic_map_for_asset(asset);
    return map ? map->entries[track].expected_pcm_size : 0;
}

static int64_t pcm_asset_pc_start(PcmAssetKind asset, int track) {
    DynamicCueMap *map;
    if (track < 0 || track >= pcm_asset_track_count(asset)) return -1;
    map = dynamic_map_for_asset(asset);
    return map ? map->entries[track].pc_start_offset : -1;
}

static PcmAssetKind pcm_asset_for_path(const char *path) {
    if (ascii_icontains(path, "jukeboxmusic.bin")) {
        return PCM_ASSET_JUKEBOX;
    }
    if (ascii_icontains(path, "loadingsequencejm.bin")) {
        return PCM_ASSET_LOADING_SEQUENCE_JM;
    }
    if (ascii_icontains(path, "loadingsequence.bin")) {
        return PCM_ASSET_LOADING_SEQUENCE;
    }
    if (ascii_icontains(path, "cachedlines.spc")) {
        return PCM_ASSET_CACHED_LINES;
    }
    if (ascii_icontains(path, "palines.bin")) {
        return PCM_ASSET_PA_LINES;
    }
    if (ascii_icontains(path, "lines_ps.bin")) {
        return PCM_ASSET_LINES_PS;
    }
    if (ascii_icontains(path, "lines_ts.bin")) {
        return PCM_ASSET_LINES_TS;
    }
    if (ascii_icontains(path, "lines.bin")) {
        return PCM_ASSET_LINES;
    }
    if (ascii_icontains(path, "cwdstrloop_dunkcontest.bin")) {
        return PCM_ASSET_CWD_STR_LOOP_DUNK_CONTEST;
    }
    if (ascii_icontains(path, "cwdstrloop_gym.bin")) {
        return PCM_ASSET_CWD_STR_LOOP_GYM;
    }
    if (ascii_icontains(path, "cwdstrloop_inside.bin")) {
        return PCM_ASSET_CWD_STR_LOOP_INSIDE;
    }
    if (ascii_icontains(path, "cwdstrsfx_dunkcontest.bin")) {
        return PCM_ASSET_CWD_STR_SFX_DUNK_CONTEST;
    }
    if (ascii_icontains(path, "cwdstrsfx_inside.bin")) {
        return PCM_ASSET_CWD_STR_SFX_INSIDE;
    }
    if (ascii_icontains(path, "eventmusic.bin")) {
        return PCM_ASSET_EVENT_MUSIC;
    }
    if (ascii_icontains(path, "loadm.bin")) {
        return PCM_ASSET_LOAD_M;
    }
    if (ascii_icontains(path, "streamedplayerchatter.bin")) {
        return PCM_ASSET_STREAMED_PLAYER_CHATTER;
    }
    if (ascii_icontains(path, "streamedchatter.bin")) {
        return PCM_ASSET_STREAMED_CHATTER;
    }
    if (ascii_icontains(path, "mentor.bin")) {
        return PCM_ASSET_MENTOR;
    }
    if (ascii_icontains(path, "mentor_ts.bin")) {
        return PCM_ASSET_MENTOR_TS;
    }
    if (ascii_icontains(path, "loadingvo_ps.bin")) {
        return PCM_ASSET_LOADING_VO_PS;
    }
    if (ascii_icontains(path, "loadingvo_ts.bin")) {
        return PCM_ASSET_LOADING_VO_TS;
    }
    if (ascii_icontains(path, "loadingvo.bin")) {
        return PCM_ASSET_LOADING_VO;
    }
    if (ascii_icontains(path, "lines_cs.bin")) {
        return PCM_ASSET_LINES_CS;
    }
    if (ascii_icontains(path, "env_amb.bin")) {
        return PCM_ASSET_ENV_AMB;
    }
    if (ascii_icontains(path, "overlayaudio.bin")) {
        return PCM_ASSET_OVERLAY_AUDIO;
    }
    if (ascii_icontains(path, "cairball.bin")) {
        return PCM_ASSET_CAIRBALL;
    }
    if (ascii_icontains(path, "aistreet.bin")) {
        return PCM_ASSET_AI_STREET;
    }
    if (ascii_icontains(path, "cwdloop.bin")) {
        return PCM_ASSET_CWD_LOOP;
    }
    if (ascii_icontains(path, "aostreet.bin")) {
        return PCM_ASSET_AO_STREET;
    }
    if (ascii_icontains(path, "asstreet.bin")) {
        return PCM_ASSET_AS_STREET;
    }
    if (ascii_icontains(path, "dunksfx.bin")) {
        return PCM_ASSET_DUNK_SFX;
    }
    if (ascii_icontains(path, "coaches.bin")) {
        return PCM_ASSET_COACHES;
    }
    if (ascii_icontains(path, "teams.bin")) {
        return PCM_ASSET_TEAMS;
    }
    if (ascii_icontains(path, "paplayers.bin")) {
        return PCM_ASSET_PA_PLAYERS;
    }
    if (ascii_icontains(path, "players.bin")) {
        return PCM_ASSET_PLAYERS;
    }
    if (ascii_icontains(path, "pressconf.bin")) {
        return PCM_ASSET_PRESS_CONF;
    }
    return PCM_ASSET_NONE;
}

static WORD pcm_asset_output_channels(PcmAssetKind asset) {
    switch (asset) {
        case PCM_ASSET_MENTOR:
        case PCM_ASSET_MENTOR_TS:
        case PCM_ASSET_LOADING_VO:
        case PCM_ASSET_LOADING_VO_PS:
        case PCM_ASSET_LOADING_VO_TS:
        case PCM_ASSET_LINES_CS:
        case PCM_ASSET_OVERLAY_AUDIO:
        case PCM_ASSET_COACHES:
        case PCM_ASSET_TEAMS:
        case PCM_ASSET_PLAYERS:
        case PCM_ASSET_PRESS_CONF:
        case PCM_ASSET_CACHED_LINES:
        case PCM_ASSET_LINES:
        case PCM_ASSET_LINES_PS:
        case PCM_ASSET_LINES_TS:
        case PCM_ASSET_STREAMED_CHATTER:
        case PCM_ASSET_STREAMED_PLAYER_CHATTER:
            return 1;
        default:
            return 2;
    }
}

static DWORD pcm_asset_output_sample_rate(PcmAssetKind asset) {
    (void)asset;
    return 48000u;
}

static int pcm_selection_for_read(
    const char *path, int64_t offset, PcmAssetKind *selected_asset) {
    PcmAssetKind asset = pcm_asset_for_path(path);
    DynamicCueMap *map;
    int track;
    if (selected_asset) *selected_asset = asset;
    if (asset == PCM_ASSET_NONE) return -1;
    map = dynamic_map_for_asset(asset);
    if (map && load_dynamic_map(asset)) {
        int low = 0;
        int high = map->count - 1;
        while (low <= high) {
            int middle = low + (high - low) / 2;
            int64_t candidate = map->entries[middle].pc_start_offset;
            if (offset < candidate) {
                high = middle - 1;
            } else if (offset > candidate) {
                low = middle + 1;
            } else {
                return middle;
            }
        }
        return -1;
    }
    for (track = 0; track < pcm_asset_track_count(asset); ++track) {
        if (offset == pcm_asset_pc_start(asset, track)) {
            return track;
        }
    }
    /*
     * Continuation reads must preserve the cue selected by the first chunk.
     * NBA 2K11 commonly reads more than one chunk before CreateSourceVoice.
     */
    return -1;
}


static int update_pcm_selection_from_read_with_data(
    const char *path, int64_t offset, const void *data, DWORD data_bytes,
    PcmAssetKind *selected_asset) {
    PcmAssetKind local_asset = PCM_ASSET_NONE;
    int track = pcm_selection_for_read(path, offset, &local_asset);
    if (selected_asset) *selected_asset = local_asset;
    if (track >= 0) {
        LONG selection = PCM_SELECTION(local_asset, track);
        InterlockedExchange(&g_pcm_selected_cue, selection);
        enqueue_pcm_selection_with_data(
            selection, GetCurrentThreadId(), data, data_bytes);
    }
    return track;
}

static int is_audio_resource(const char *path) {
    return path && (
        ascii_icontains(path, "jukeboxmusic.bin") ||
        ascii_icontains(path, "loadingvo_ts.bin") ||
        ascii_icontains(path, "loadingsequencejm.bin") ||
        ascii_icontains(path, "loadingsequence.bin") ||
        ascii_icontains(path, "cachedlines.spc") ||
        ascii_icontains(path, "lines.bin") ||
        ascii_icontains(path, "lines_ps.bin") ||
        ascii_icontains(path, "lines_ts.bin") ||
        ascii_icontains(path, "palines.bin") ||
        ascii_icontains(path, "cwdstrloop_dunkcontest.bin") ||
        ascii_icontains(path, "cwdstrloop_gym.bin") ||
        ascii_icontains(path, "cwdstrloop_inside.bin") ||
        ascii_icontains(path, "cwdstrsfx_dunkcontest.bin") ||
        ascii_icontains(path, "cwdstrsfx_inside.bin") ||
        ascii_icontains(path, "eventmusic.bin") ||
        ascii_icontains(path, "loadm.bin") ||
        ascii_icontains(path, "streamedchatter.bin") ||
        ascii_icontains(path, "streamedplayerchatter.bin") ||
        ascii_icontains(path, "mentor.bin") ||
        ascii_icontains(path, "mentor_ts.bin") ||
        ascii_icontains(path, "loadingvo.bin") ||
        ascii_icontains(path, "loadingvo_ps.bin") ||
        ascii_icontains(path, "lines_cs.bin") ||
        ascii_icontains(path, "env_amb.bin") ||
        ascii_icontains(path, "cairball.bin") ||
        ascii_icontains(path, "aistreet.bin") ||
        ascii_icontains(path, "cwdloop.bin") ||
        ascii_icontains(path, "aostreet.bin") ||
        ascii_icontains(path, "asstreet.bin") ||
        ascii_icontains(path, "dunksfx.bin") ||
        ascii_icontains(path, "coaches.bin") ||
        ascii_icontains(path, "teams.bin") ||
        ascii_icontains(path, "players.bin") ||
        ascii_icontains(path, "pressconf.bin") ||
        ascii_icontains(path, "paplayers.bin") ||
        ascii_icontains(path, "jukebox.iff") ||
        ascii_icontains(path, "overlayaudio.bin"));
}

static const WCHAR *redirected_fixed_bank(const char *path) {
    (void)path;
    return NULL;
}


/*
 * Arena audio stays on the game's original path until exact payload routing
 * is available for those reusable voices.
 */

static void close_pcm_stream(PcmStream *stream);

static PcmBufferContext *pcm_stream_context(
    VoiceCallbackWrapper *wrapper, void *context) {
    PcmBufferContext *pcm = (PcmBufferContext *)context;
    if (!wrapper || !pcm ||
        pcm->magic != PCM_STREAM_BUFFER_MAGIC ||
        !pcm->stream) {
        return NULL;
    }
    return pcm;
}

static const VoiceCallbackVtable *original_callback_vtable(
    const VoiceCallbackWrapper *wrapper) {
    if (!wrapper || !wrapper->original_callback) return NULL;
    return *(const VoiceCallbackVtable **)wrapper->original_callback;
}

static void release_pcm_stream_buffer(PcmBufferContext *context) {
    PcmStream *stream;
    int wake_reaper = 0;
    if (!context || context->magic != PCM_STREAM_BUFFER_MAGIC ||
        !context->stream) {
        return;
    }
    stream = context->stream;
    EnterCriticalSection(&stream->lock);
    if (context->in_use) {
        if (stream->outstanding_bytes >= context->audio_bytes) {
            stream->outstanding_bytes -= context->audio_bytes;
        } else {
            stream->outstanding_bytes = 0;
        }
        if (stream->outstanding_buffers) --stream->outstanding_buffers;
        context->original_context = NULL;
        context->audio_bytes = 0;
        context->in_use = 0;
        wake_reaper =
            stream->retired && !stream->outstanding_buffers;
    }
    LeaveCriticalSection(&stream->lock);
    if (wake_reaper && g_pcm_reaper_event) {
        SetEvent(g_pcm_reaper_event);
    }
}

static void __stdcall pcm_callback_pass_start(
    void *callback, UINT32 bytes_required) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    if (vtable && vtable->on_voice_processing_pass_start) {
        vtable->on_voice_processing_pass_start(original, bytes_required);
    }
}

static void __stdcall pcm_callback_pass_end(void *callback) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    if (vtable && vtable->on_voice_processing_pass_end) {
        vtable->on_voice_processing_pass_end(original);
    }
}

static void __stdcall pcm_callback_stream_end(void *callback) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    if (vtable && vtable->on_stream_end) {
        vtable->on_stream_end(original);
    }
}

static void __stdcall pcm_callback_buffer_start(
    void *callback, void *context) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    PcmBufferContext *pcm = pcm_stream_context(wrapper, context);
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    void *original_context = pcm ? pcm->original_context : context;
    if (vtable && vtable->on_buffer_start) {
        vtable->on_buffer_start(original, original_context);
    }
}

static void __stdcall pcm_callback_buffer_end(
    void *callback, void *context) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    PcmBufferContext *pcm = pcm_stream_context(wrapper, context);
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    void *original_context = pcm ? pcm->original_context : context;
    if (pcm) release_pcm_stream_buffer(pcm);
    if (vtable && vtable->on_buffer_end) {
        vtable->on_buffer_end(original, original_context);
    }
}

static void __stdcall pcm_callback_loop_end(
    void *callback, void *context) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    PcmBufferContext *pcm = pcm_stream_context(wrapper, context);
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    void *original_context = pcm ? pcm->original_context : context;
    if (vtable && vtable->on_loop_end) {
        vtable->on_loop_end(original, original_context);
    }
}

static void __stdcall pcm_callback_voice_error(
    void *callback, void *context, HRESULT error) {
    VoiceCallbackWrapper *wrapper = (VoiceCallbackWrapper *)callback;
    PcmBufferContext *pcm = pcm_stream_context(wrapper, context);
    const VoiceCallbackVtable *vtable =
        original_callback_vtable(wrapper);
    void *original = wrapper ? wrapper->original_callback : NULL;
    void *original_context = pcm ? pcm->original_context : context;
    if (vtable && vtable->on_voice_error) {
        vtable->on_voice_error(original, original_context, error);
    }
}

static const VoiceCallbackVtable g_pcm_callback_vtable = {
    pcm_callback_pass_start,
    pcm_callback_pass_end,
    pcm_callback_stream_end,
    pcm_callback_buffer_start,
    pcm_callback_buffer_end,
    pcm_callback_loop_end,
    pcm_callback_voice_error,
};

static void close_pcm_stream(PcmStream *stream) {
    DWORD wait_result;
    DWORD exit_code = STILL_ACTIVE;
    DWORD index;
    if (!stream) return;

    if (stream->lock_initialized) {
        EnterCriticalSection(&stream->lock);
        if (stream->output_read &&
            stream->output_read != INVALID_HANDLE_VALUE) {
            CloseHandle(stream->output_read);
            stream->output_read = NULL;
        }
        for (index = 0; index < PCM_STREAM_BUFFER_COUNT; ++index) {
            PcmBufferContext *context = &stream->buffers[index];
            context->in_use = 0;
            context->original_context = NULL;
            context->audio_bytes = 0;
        }
        stream->outstanding_buffers = 0;
        stream->outstanding_bytes = 0;
        LeaveCriticalSection(&stream->lock);
    }

    if (stream->decoder_worker) {
        wait_result = WaitForSingleObject(stream->decoder_worker, 1000);
        if (wait_result == WAIT_TIMEOUT) {
            /* Closing output_read above breaks any blocked decoder write. */
            WaitForSingleObject(stream->decoder_worker, INFINITE);
        }
        GetExitCodeThread(stream->decoder_worker, &exit_code);
        CloseHandle(stream->decoder_worker);
        stream->decoder_worker = NULL;
    }


    for (index = 0; index < PCM_STREAM_BUFFER_COUNT; ++index) {
        if (stream->buffers[index].data) {
            HeapFree(
                GetProcessHeap(), 0, stream->buffers[index].data);
            stream->buffers[index].data = NULL;
        }
    }
    if (stream->lock_initialized) {
        DeleteCriticalSection(&stream->lock);
        stream->lock_initialized = 0;
    }
    HeapFree(GetProcessHeap(), 0, stream);
}

static void cleanup_retired_pcm_streams(void) {
    for (;;) {
        PcmStream **link;
        PcmStream *ready = NULL;
        EnterCriticalSection(&g_lock);
        link = &g_retired_pcm_streams;
        while (*link) {
            PcmStream *stream = *link;
            DWORD outstanding;
            EnterCriticalSection(&stream->lock);
            outstanding = stream->outstanding_buffers;
            LeaveCriticalSection(&stream->lock);
            if (!outstanding) {
                *link = stream->next_retired;
                stream->next_retired = NULL;
                ready = stream;
                break;
            }
            link = &stream->next_retired;
        }
        LeaveCriticalSection(&g_lock);
        if (!ready) break;
        close_pcm_stream(ready);
    }
}

static void retire_pcm_stream(PcmStream *stream) {
    DWORD outstanding;
    if (!stream) return;
    EnterCriticalSection(&stream->lock);
    if (stream->retired) {
        LeaveCriticalSection(&stream->lock);
        return;
    }
    stream->retired = 1;
    outstanding = stream->outstanding_buffers;
    LeaveCriticalSection(&stream->lock);
    /*
     * Decoder shutdown can wait for an active worker. Never perform that wait
     * from SubmitSourceBuffer. A dedicated
     * low-priority reaper owns teardown once XAudio2 releases the last buffer.
     */
    if (g_pcm_reaper_event && g_pcm_reaper_thread) {
        EnterCriticalSection(&g_lock);
        stream->next_retired = g_retired_pcm_streams;
        g_retired_pcm_streams = stream;
        LeaveCriticalSection(&g_lock);
        SetEvent(g_pcm_reaper_event);
        return;
    }
    if (!outstanding) {
        close_pcm_stream(stream);
        return;
    }
    EnterCriticalSection(&g_lock);
    stream->next_retired = g_retired_pcm_streams;
    g_retired_pcm_streams = stream;
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI pcm_reaper_thread_main(LPVOID parameter) {
    (void)parameter;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        DWORD wait_result = WaitForSingleObject(
            g_pcm_reaper_event, INFINITE);
        if (wait_result != WAIT_OBJECT_0) break;
        cleanup_retired_pcm_streams();
        if (InterlockedCompareExchange(
                &g_pcm_reaper_stop, 0, 0)) {
            break;
        }
    }
    return 0;
}

static int start_pcm_reaper_worker(void) {
    HANDLE event_handle;
    HANDLE thread_handle;
    if (g_pcm_reaper_event && g_pcm_reaper_thread) return 1;
    InterlockedExchange(&g_pcm_reaper_stop, 0);
    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event_handle) return 0;
    g_pcm_reaper_event = event_handle;
    thread_handle = CreateThread(
        NULL, 0, pcm_reaper_thread_main, NULL, 0, NULL);
    if (!thread_handle) {
        CloseHandle(event_handle);
        g_pcm_reaper_event = NULL;
        return 0;
    }
    g_pcm_reaper_thread = thread_handle;
    return 1;
}

static void stop_pcm_reaper_worker(void) {
    HANDLE thread_handle = g_pcm_reaper_thread;
    HANDLE event_handle = g_pcm_reaper_event;
    if (!thread_handle && !event_handle) return;
    InterlockedExchange(&g_pcm_reaper_stop, 1);
    if (event_handle) SetEvent(event_handle);
    if (thread_handle) {
        WaitForSingleObject(thread_handle, INFINITE);
        CloseHandle(thread_handle);
        g_pcm_reaper_thread = NULL;
    }
    if (event_handle) {
        CloseHandle(event_handle);
        g_pcm_reaper_event = NULL;
    }
}

static void close_retired_pcm_streams_for_voice(void *voice) {
    for (;;) {
        PcmStream **link;
        PcmStream *found = NULL;
        EnterCriticalSection(&g_lock);
        link = &g_retired_pcm_streams;
        while (*link) {
            if ((*link)->owner_voice == voice) {
                found = *link;
                *link = found->next_retired;
                found->next_retired = NULL;
                break;
            }
            link = &(*link)->next_retired;
        }
        LeaveCriticalSection(&g_lock);
        if (!found) break;
        close_pcm_stream(found);
    }
}

typedef struct EmbeddedDecoderRequest {
    PcmAssetKind asset;
    int track;
    int loop_requested;
    HANDLE output;
} EmbeddedDecoderRequest;

static DWORD WINAPI embedded_decoder_thread_main(LPVOID parameter) {
    EmbeddedDecoderRequest *request =
        (EmbeddedDecoderRequest *)parameter;
    HANDLE output = request->output;
    int result;

    SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    result = audio_decoder_decode_track(
        pcm_asset_name_w(request->asset), request->track,
        request->loop_requested, output);
    CloseHandle(output);
    HeapFree(GetProcessHeap(), 0, request);
    return (DWORD)result;
}

static int open_pcm_stream(
    PcmAssetKind asset, int track, void *original_callback,
    PcmStream **output_stream) {
    SECURITY_ATTRIBUTES security;
    HANDLE output_read = NULL;
    HANDLE output_write = NULL;
    EmbeddedDecoderRequest *request = NULL;
    HANDLE decoder_thread = NULL;
    DWORD decoder_thread_id = 0;
    PcmStream *stream = NULL;
    DWORD index;

    if (!output_stream || track < 0 ||
        track >= pcm_asset_track_count(asset)) {
        return 0;
    }
    *output_stream = NULL;
    stream = (PcmStream *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream));
    if (!stream) return 0;
    InitializeCriticalSection(&stream->lock);
    stream->lock_initialized = 1;
    stream->asset = asset;
    stream->track = track;
    stream->expected_bytes = pcm_asset_expected_bytes(asset, track);
    stream->output_channels = pcm_asset_output_channels(asset);
    stream->output_sample_rate =
        pcm_asset_output_sample_rate(asset);
    stream->output_bytes_per_frame =
        (DWORD)stream->output_channels * 2u;
    if (!stream->expected_bytes ||
        !stream->output_channels ||
        !stream->output_sample_rate ||
        !stream->output_bytes_per_frame ||
        stream->expected_bytes % stream->output_bytes_per_frame) {
        goto failed;
    }
    stream->start_tick = GetTickCount();
    stream->callback.vtable = &g_pcm_callback_vtable;
    stream->callback.stream = stream;
    stream->callback.original_callback = original_callback;
    for (index = 0; index < PCM_STREAM_BUFFER_COUNT; ++index) {
        stream->buffers[index].magic = PCM_STREAM_BUFFER_MAGIC;
        stream->buffers[index].stream = stream;
    }

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = FALSE;
    if (!CreatePipe(
            &output_read, &output_write, &security,
            PCM_DECODE_PIPE_BUFFER)) {
        goto failed;
    }
    request = (EmbeddedDecoderRequest *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
    if (!request) goto failed;
    request->asset = asset;
    request->track = track;
    request->loop_requested = asset == PCM_ASSET_ENV_AMB;
    request->output = output_write;
    decoder_thread = CreateThread(
        NULL, 0, embedded_decoder_thread_main,
        request, 0, &decoder_thread_id);
    if (!decoder_thread) goto failed;
    request = NULL;
    output_write = NULL;
    stream->output_read = output_read;
    output_read = NULL;
    stream->decoder_worker = decoder_thread;
    stream->decoder_worker_id = decoder_thread_id;

    *output_stream = stream;
    return 1;

failed:
    if (output_read) CloseHandle(output_read);
    if (output_write) CloseHandle(output_write);
    if (request) HeapFree(GetProcessHeap(), 0, request);
    close_pcm_stream(stream);
    return 0;
}

static int pcm_prefetch_enabled_asset(PcmAssetKind asset) {
    /*
     * envamb is deliberately excluded: it loops forever and a speculative
     * instance would occupy a decoder/cache slot indefinitely.
     */
    return asset > PCM_ASSET_NONE &&
        asset <= PCM_ASSET_STREAMED_PLAYER_CHATTER &&
        asset != PCM_ASSET_ENV_AMB;
}

static void queue_pcm_prefetch(LONG selection, DWORD queued_tick) {
    PcmAssetKind asset;
    int track;
    if (selection < 0 || !g_pcm_prefetch_event ||
        InterlockedCompareExchange(&g_pcm_prefetch_stop, 0, 0)) {
        return;
    }
    asset = (PcmAssetKind)((selection >> 16) & 0xFFFF);
    track = (int)(selection & 0xFFFF);
    if (!pcm_prefetch_enabled_asset(asset) ||
        track < 0 || track >= pcm_asset_track_count(asset)) {
        return;
    }

    EnterCriticalSection(&g_lock);
    if (g_pcm_prefetch_request_count >=
        PCM_PREFETCH_REQUEST_CAPACITY) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_pcm_prefetch_requests[g_pcm_prefetch_request_count].selection =
        selection;
    g_pcm_prefetch_requests[g_pcm_prefetch_request_count].queued_tick =
        queued_tick;
    ++g_pcm_prefetch_request_count;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_pcm_prefetch_event);
}

static PcmStream *take_prefetched_pcm_stream(
    PcmAssetKind asset, int track, void *original_callback) {
    LONG selection = PCM_SELECTION(asset, track);
    PcmStream *stream = NULL;
    int index;

    if (!g_pcm_prefetch_event ||
        !pcm_prefetch_enabled_asset(asset)) {
        return NULL;
    }

    EnterCriticalSection(&g_lock);
    for (index = 0; index < g_pcm_prefetch_ready_count; ++index) {
        if (g_pcm_prefetch_ready[index].selection == selection) {
            PcmPrefetchReady *ready = &g_pcm_prefetch_ready[index];
            stream = ready->stream;
            if (index + 1 < g_pcm_prefetch_ready_count) {
                memmove(
                    &g_pcm_prefetch_ready[index],
                    &g_pcm_prefetch_ready[index + 1],
                    sizeof(g_pcm_prefetch_ready[0]) *
                        (g_pcm_prefetch_ready_count - index - 1));
            }
            --g_pcm_prefetch_ready_count;
            break;
        }
    }
    if (!stream) {
        /*
         * A synchronous miss is still the reliability path. Remove exactly
         * one corresponding speculative request so it cannot start a second
         * decoder for the same cue after the submit thread has opened one.
         */
        for (index = 0;
             index < g_pcm_prefetch_request_count; ++index) {
            if (g_pcm_prefetch_requests[index].selection == selection) {
                if (index + 1 < g_pcm_prefetch_request_count) {
                    memmove(
                        &g_pcm_prefetch_requests[index],
                        &g_pcm_prefetch_requests[index + 1],
                        sizeof(g_pcm_prefetch_requests[0]) *
                            (g_pcm_prefetch_request_count - index - 1));
                }
                --g_pcm_prefetch_request_count;
                break;
            }
        }
        if (g_pcm_prefetch_inflight_selection == selection) {
            g_pcm_prefetch_inflight_canceled = 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_pcm_prefetch_event);

    if (!stream) {
        return NULL;
    }

    EnterCriticalSection(&stream->lock);
    stream->callback.original_callback = original_callback;
    LeaveCriticalSection(&stream->lock);
    return stream;
}

static DWORD WINAPI pcm_prefetch_thread_main(LPVOID parameter) {
    (void)parameter;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        DWORD wait_result = WaitForSingleObject(
            g_pcm_prefetch_event, INFINITE);
        if (wait_result != WAIT_OBJECT_0 ||
            InterlockedCompareExchange(
                &g_pcm_prefetch_stop, 0, 0)) {
            break;
        }
        for (;;) {
            PcmPrefetchRequest request;
            PcmStream *stream = NULL;
            PcmStream *stale_stream = NULL;
            PcmAssetKind asset;
            int track;
            int canceled;
            int index;

            memset(&request, 0, sizeof(request));
            EnterCriticalSection(&g_lock);
            if (g_pcm_prefetch_ready_count >=
                PCM_PREFETCH_READY_CAPACITY) {
                DWORD now = GetTickCount();
                for (index = 0;
                     index < g_pcm_prefetch_ready_count; ++index) {
                    if (now -
                            g_pcm_prefetch_ready[index].queued_tick >=
                        PCM_PREFETCH_STALE_MS) {
                        stale_stream =
                            g_pcm_prefetch_ready[index].stream;
                        if (index + 1 <
                            g_pcm_prefetch_ready_count) {
                            memmove(
                                &g_pcm_prefetch_ready[index],
                                &g_pcm_prefetch_ready[index + 1],
                                sizeof(g_pcm_prefetch_ready[0]) *
                                    (g_pcm_prefetch_ready_count -
                                     index - 1));
                        }
                        --g_pcm_prefetch_ready_count;
                        break;
                    }
                }
            }
            if (!stale_stream &&
                (g_pcm_prefetch_ready_count >=
                     PCM_PREFETCH_READY_CAPACITY ||
                 !g_pcm_prefetch_request_count)) {
                LeaveCriticalSection(&g_lock);
                break;
            }
            if (stale_stream) {
                LeaveCriticalSection(&g_lock);
                close_pcm_stream(stale_stream);
                continue;
            }

            request = g_pcm_prefetch_requests[0];
            if (g_pcm_prefetch_request_count > 1) {
                memmove(
                    &g_pcm_prefetch_requests[0],
                    &g_pcm_prefetch_requests[1],
                    sizeof(g_pcm_prefetch_requests[0]) *
                        (g_pcm_prefetch_request_count - 1));
            }
            --g_pcm_prefetch_request_count;
            g_pcm_prefetch_inflight_selection = request.selection;
            g_pcm_prefetch_inflight_canceled = 0;
            LeaveCriticalSection(&g_lock);

            asset = (PcmAssetKind)(
                (request.selection >> 16) & 0xFFFF);
            track = (int)(request.selection & 0xFFFF);
            open_pcm_stream(asset, track, NULL, &stream);

            EnterCriticalSection(&g_lock);
            canceled = g_pcm_prefetch_inflight_canceled ||
                InterlockedCompareExchange(
                    &g_pcm_prefetch_stop, 0, 0);
            g_pcm_prefetch_inflight_selection = -1;
            g_pcm_prefetch_inflight_canceled = 0;
            if (stream && !canceled &&
                g_pcm_prefetch_ready_count <
                    PCM_PREFETCH_READY_CAPACITY) {
                PcmPrefetchReady *ready =
                    &g_pcm_prefetch_ready[
                        g_pcm_prefetch_ready_count++];
                ready->selection = request.selection;
                ready->queued_tick = request.queued_tick;
                ready->ready_tick = GetTickCount();
                ready->stream = stream;
                stream = NULL;
            }
            LeaveCriticalSection(&g_lock);

            if (stream) close_pcm_stream(stream);
            if (InterlockedCompareExchange(
                    &g_pcm_prefetch_stop, 0, 0)) {
                break;
            }
        }
    }
    return 0;
}

static int start_pcm_prefetch_worker(void) {
    HANDLE event_handle;
    HANDLE thread_handle;
    if (g_pcm_prefetch_event && g_pcm_prefetch_thread) return 1;
    InterlockedExchange(&g_pcm_prefetch_stop, 0);
    event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event_handle) {
        return 0;
    }
    g_pcm_prefetch_event = event_handle;
    thread_handle = CreateThread(
        NULL, 0, pcm_prefetch_thread_main, NULL, 0, NULL);
    if (!thread_handle) {
        CloseHandle(event_handle);
        g_pcm_prefetch_event = NULL;
        return 0;
    }
    g_pcm_prefetch_thread = thread_handle;
    return 1;
}

static void stop_pcm_prefetch_worker(void) {
    HANDLE thread_handle = g_pcm_prefetch_thread;
    HANDLE event_handle = g_pcm_prefetch_event;
    PcmStream *ready_streams[PCM_PREFETCH_READY_CAPACITY];
    int ready_count = 0;
    int index;
    if (!event_handle && !thread_handle) return;

    InterlockedExchange(&g_pcm_prefetch_stop, 1);
    if (event_handle) SetEvent(event_handle);
    if (thread_handle) {
        /*
         * Never terminate this thread while it may own g_lock. Closing the
         * pipe makes a blocked decoder exit, so a normal join is both bounded
         * in practice and safe for explicit ASI unloads.
         */
        WaitForSingleObject(thread_handle, INFINITE);
        CloseHandle(thread_handle);
        g_pcm_prefetch_thread = NULL;
    }

    EnterCriticalSection(&g_lock);
    ready_count = g_pcm_prefetch_ready_count;
    for (index = 0; index < ready_count; ++index) {
        ready_streams[index] = g_pcm_prefetch_ready[index].stream;
        memset(
            &g_pcm_prefetch_ready[index], 0,
            sizeof(g_pcm_prefetch_ready[index]));
    }
    g_pcm_prefetch_ready_count = 0;
    g_pcm_prefetch_request_count = 0;
    g_pcm_prefetch_inflight_selection = -1;
    g_pcm_prefetch_inflight_canceled = 0;
    LeaveCriticalSection(&g_lock);

    for (index = 0; index < ready_count; ++index) {
        close_pcm_stream(ready_streams[index]);
    }
    if (event_handle) {
        CloseHandle(event_handle);
        g_pcm_prefetch_event = NULL;
    }
}

static void close_tracked_voice_pcm(TrackedVoice *tracked) {
    if (!tracked) return;
    if (tracked->pcm_stream) {
        close_pcm_stream(tracked->pcm_stream);
    }
    tracked->pcm_substitute = 0;
    tracked->pcm_track = 0;
    tracked->pcm_stream = NULL;
    tracked->pcm_size = 0;
    tracked->pcm_frames = 0;
    tracked->pc_frames_consumed = 0;
    tracked->pcm_frames_submitted = 0;
    tracked->pcm_source_frames_submitted = 0;
    tracked->pcm_end_submitted = 0;
}

static void cleanup_all_pcm_voices(void) {
    int index;
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        close_tracked_voice_pcm(&g_voices[index]);
    }
}

static int is_streamable_pc_wma_voice(
    const WaveFormatExTrace *format, PcmAssetKind asset) {
    if (!format || format->format_tag != 0x0161 ||
        format->samples_per_second != 22050) {
        return 0;
    }
    return format->channels == pcm_asset_output_channels(asset) &&
        format->average_bytes_per_second == 6000 &&
        format->block_align == (format->channels == 1 ? 929 : 1487);
}

static int is_reusable_pc_wma_voice(
    const WaveFormatExTrace *format) {
    return format &&
        format->format_tag == 0x0161 &&
        format->samples_per_second == 22050 &&
        (format->channels == 1 || format->channels == 2) &&
        format->average_bytes_per_second == 6000 &&
        format->block_align ==
            (format->channels == 1 ? 929 : 1487);
}

static uint64_t pcm_payload_hash(const BYTE *data, DWORD data_bytes) {
    uint64_t hash = UINT64_C(14695981039346656037);
    DWORD index;
    if (!data && data_bytes) return 0;
    for (index = 0; index < data_bytes; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static BYTE build_pcm_payload_signatures(
    const void *data, DWORD data_bytes,
    PcmPayloadSignature signatures[PCM_SELECTION_SIGNATURE_COUNT]) {
    const BYTE *bytes = (const BYTE *)data;
    DWORD chunk_offset;
    BYTE count = 0;
    if (!bytes || data_bytes < 0x1000u) return 0;
    for (chunk_offset = 0;
         chunk_offset + 0x1000u <= data_bytes &&
         count < PCM_SELECTION_SIGNATURE_COUNT;
         chunk_offset += 0x1000u) {
        const BYTE *chunk = bytes + chunk_offset;
        DWORD marker = 0;
        DWORD block_align = 0;
        DWORD packet_count = 0;
        DWORD payload_offset;
        uint64_t payload_bytes64;
        DWORD payload_bytes;
        DWORD prefix_size;
        memcpy(&marker, chunk + 0x00, sizeof(marker));
        memcpy(&block_align, chunk + 0x08, sizeof(block_align));
        memcpy(&packet_count, chunk + 0x0C, sizeof(packet_count));
        if (marker != 0xD2BEA169u ||
            !block_align || !packet_count || packet_count > 64u) {
            break;
        }
        payload_offset = 0x38u + packet_count * 4u;
        payload_bytes64 =
            (uint64_t)block_align * (uint64_t)packet_count;
        if (payload_bytes64 > UINT32_MAX ||
            payload_offset > 0x1000u ||
            payload_bytes64 > 0x1000u - payload_offset) {
            break;
        }
        payload_bytes = (DWORD)payload_bytes64;
        signatures[count].audio_bytes = payload_bytes;
        signatures[count].payload_hash = pcm_payload_hash(
            chunk + payload_offset, payload_bytes);
        prefix_size = payload_bytes;
        if (prefix_size > sizeof(signatures[count].prefix)) {
            prefix_size = sizeof(signatures[count].prefix);
        }
        signatures[count].prefix_size = (BYTE)prefix_size;
        memcpy(
            signatures[count].prefix,
            chunk + payload_offset, prefix_size);
        ++count;
    }
    return count;
}

static void enqueue_pcm_selection_with_data(
    LONG selection, DWORD thread_id, const void *data, DWORD data_bytes) {
    PcmSelectionRecord *record;
    PcmPayloadSignature
        signatures[PCM_SELECTION_SIGNATURE_COUNT];
    PcmAssetKind asset;
    DWORD now;
    int index;
    BYTE signature_count;
    if (selection < 0) return;
    asset = (PcmAssetKind)((selection >> 16) & 0xFFFF);
    if (!pcm_asset_runtime_binding_enabled(asset)) {
        return;
    }
    memset(signatures, 0, sizeof(signatures));
    signature_count = build_pcm_payload_signatures(
        data, data_bytes, signatures);
    now = GetTickCount();
    EnterCriticalSection(&g_lock);
    for (index = 0; index < g_pcm_selection_count; ++index) {
        const PcmSelectionRecord *queued =
            &g_pcm_selection_queue[index];
        if (queued->selection == selection &&
            queued->thread_id == thread_id &&
            now - queued->queued_tick <= 250u &&
            queued->signature_count == signature_count &&
            !memcmp(
                queued->signatures, signatures,
                sizeof(signatures))) {
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    if (g_pcm_selection_count == PCM_SELECTION_QUEUE_CAPACITY) {
        memmove(
            &g_pcm_selection_queue[0],
            &g_pcm_selection_queue[1],
            sizeof(g_pcm_selection_queue[0]) *
                (PCM_SELECTION_QUEUE_CAPACITY - 1));
        --g_pcm_selection_count;
    }
    record = &g_pcm_selection_queue[g_pcm_selection_count++];
    memset(record, 0, sizeof(*record));
    record->selection = selection;
    record->thread_id = thread_id;
    record->queued_tick = now;
    record->signature_count = signature_count;
    memcpy(record->signatures, signatures, sizeof(signatures));
    LeaveCriticalSection(&g_lock);
    queue_pcm_prefetch(selection, now);
}

static int hash_process_payload(
    const BYTE *data, DWORD data_bytes, uint64_t *hash_out,
    BYTE prefix[16], BYTE *prefix_size_out) {
    BYTE block[256];
    DWORD offset = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    BYTE prefix_size = 0;
    if (!data || !data_bytes || !hash_out ||
        !prefix || !prefix_size_out) {
        return 0;
    }
    while (offset < data_bytes) {
        SIZE_T copied = 0;
        DWORD wanted = data_bytes - offset;
        DWORD index;
        if (wanted > sizeof(block)) wanted = sizeof(block);
        if (!ReadProcessMemory(
                GetCurrentProcess(), data + offset,
                block, wanted, &copied) ||
            copied != wanted) {
            return 0;
        }
        if (!offset) {
            DWORD first_bytes = wanted;
            if (first_bytes > 16) first_bytes = 16;
            prefix_size = (BYTE)first_bytes;
            memcpy(prefix, block, prefix_size);
        }
        for (index = 0; index < wanted; ++index) {
            hash ^= block[index];
            hash *= UINT64_C(1099511628211);
        }
        offset += wanted;
    }
    *hash_out = hash;
    *prefix_size_out = prefix_size;
    return 1;
}


static LONG claim_pcm_selection_for_payload(
    const XAudio2BufferTrace *buffer, DWORD thread_id,
    WORD voice_channels,
    PcmSelectionRecord *claimed_record) {
    LONG selection = -1;
    int selected_index = -1;
    DWORD now = GetTickCount();
    uint64_t payload_hash = 0;
    BYTE prefix[16];
    BYTE prefix_size = 0;
    int index;
    int remaining;
    if (claimed_record) memset(claimed_record, 0, sizeof(*claimed_record));
    if (!buffer || !buffer->audio_data || !buffer->audio_bytes ||
        !voice_channels ||
        !hash_process_payload(
            buffer->audio_data, buffer->audio_bytes,
            &payload_hash, prefix, &prefix_size)) {
        return -1;
    }

    EnterCriticalSection(&g_lock);
    for (index = 0; index < g_pcm_selection_count;) {
        PcmSelectionRecord *record =
            &g_pcm_selection_queue[index];
        PcmAssetKind asset = (PcmAssetKind)(
            (record->selection >> 16) & 0xFFFF);
        /*
         * cachedlines and arena introductions can be prefetched well before
         * their reusable voice submits the compressed payload.  Exact hashes
         * make stale false claims impractical, so retain records for five
         * minutes and let the bounded queue discard the oldest if necessary.
         */
        if (now - record->queued_tick > 300000u) {
            if (index + 1 < g_pcm_selection_count) {
                memmove(
                    &g_pcm_selection_queue[index],
                    &g_pcm_selection_queue[index + 1],
                    sizeof(g_pcm_selection_queue[0]) *
                        (g_pcm_selection_count - index - 1));
            }
            --g_pcm_selection_count;
            continue;
        }
        if (pcm_asset_output_channels(asset) == voice_channels) {
            int signature_index;
            for (signature_index = 0;
                 signature_index < record->signature_count;
                 ++signature_index) {
                const PcmPayloadSignature *signature =
                    &record->signatures[signature_index];
                if (signature->audio_bytes == buffer->audio_bytes &&
                    signature->payload_hash == payload_hash &&
                    signature->prefix_size == prefix_size &&
                    !memcmp(
                        signature->prefix, prefix, prefix_size)) {
                    selected_index = index;
                    break;
                }
            }
        }
        if (selected_index >= 0) break;
        ++index;
    }
    if (selected_index >= 0) {
        if (claimed_record) {
            *claimed_record = g_pcm_selection_queue[selected_index];
        }
        selection =
            g_pcm_selection_queue[selected_index].selection;
        if (selected_index + 1 < g_pcm_selection_count) {
            memmove(
                &g_pcm_selection_queue[selected_index],
                &g_pcm_selection_queue[selected_index + 1],
                sizeof(g_pcm_selection_queue[0]) *
                    (g_pcm_selection_count - selected_index - 1));
        }
        --g_pcm_selection_count;
    }
    remaining = g_pcm_selection_count;
    InterlockedExchange(
        &g_pcm_selected_cue,
        remaining
            ? g_pcm_selection_queue[remaining - 1].selection
            : -1);
    LeaveCriticalSection(&g_lock);

    return selection;
}

static int active_pcm_payload_matches(
    void *voice, const XAudio2BufferTrace *buffer) {
    uint64_t payload_hash = 0;
    BYTE prefix[16];
    BYTE prefix_size = 0;
    int index;
    int matches = 0;

    if (!voice || !buffer || !buffer->audio_data ||
        !buffer->audio_bytes ||
        !hash_process_payload(
            buffer->audio_data, buffer->audio_bytes,
            &payload_hash, prefix, &prefix_size)) {
        return 0;
    }

    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        TrackedVoice *tracked = &g_voices[index];
        int signature_index;
        if (tracked->voice != voice ||
            !tracked->pcm_substitute ||
            tracked->pcm_end_submitted ||
            !tracked->pcm_stream) {
            continue;
        }
        for (signature_index = 0;
             signature_index < tracked->pcm_signature_count;
             ++signature_index) {
            const PcmPayloadSignature *signature =
                &tracked->pcm_signatures[signature_index];
            if (signature->audio_bytes == buffer->audio_bytes &&
                signature->payload_hash == payload_hash &&
                signature->prefix_size == prefix_size &&
                !memcmp(signature->prefix, prefix, prefix_size)) {
                matches = 1;
                break;
            }
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
    return matches;
}

static void *original_voice_slot(void *voice, UINT32 slot) {
    void **vtable;
    void *original = NULL;
    int index;
    if (!voice || slot >= IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT) return NULL;
    vtable = *(void ***)voice;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_submit_vtables); ++index) {
        if (g_submit_vtables[index].vtable == vtable) {
            original = g_submit_vtables[index].original_slots[slot];
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return original;
}

static void *redirected_voice(void *voice) {
    void *target = voice;
    int index;
    if (!voice) return NULL;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            g_voices[index].redirect_voice) {
            target = g_voices[index].redirect_voice;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return target;
}

static void snapshot_voice_volume_state(
    void *voice, VoiceVolumeState *state) {
    int index;
    if (!state) return;
    state->target = voice;
    state->logical_volume = 1.0f;
    state->pcm_gain = 1.0f;
    state->pcm_substitute = 0;
    if (!voice) return;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        const TrackedVoice *tracked = &g_voices[index];
        if (tracked->voice != voice) continue;
        if (tracked->redirect_voice) {
            state->target = tracked->redirect_voice;
        }
        state->logical_volume = tracked->logical_volume;
        if (tracked->pcm_substitute && tracked->redirect_voice) {
            state->pcm_gain = tracked->pcm_gain;
            state->pcm_substitute = 1;
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

static void store_voice_logical_volume(void *voice, float volume) {
    int index;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice) {
            g_voices[index].logical_volume = volume;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/*
 * A redirected PCM sidecar is not guaranteed to share the WMA proxy's
 * concrete XAudio2 source-voice implementation.  Always dispatch a method
 * through the target object's own vtable.  Patched/shared vtables resolve to
 * their saved original slot; an unpatched sidecar vtable can be called
 * directly.
 */
static void *voice_slot_for_target(
    void *proxy, void *target, UINT32 slot) {
    void **vtable;
    void *method;
    if (slot >= IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT) return NULL;
    if (!target) target = proxy;
    method = original_voice_slot(target, slot);
    if (method) return method;
    if (target && target != proxy) {
        vtable = *(void ***)target;
        if (vtable) return vtable[slot];
    }
    return original_voice_slot(proxy, slot);
}


static int snapshot_native_sidecar_voices(
    void *voice, void *exclude,
    void *voices[PCM_NATIVE_SIDECAR_COUNT]) {
    int voice_index;
    int sidecar_index;
    int count = 0;
    if (!voice || !voices) return 0;
    EnterCriticalSection(&g_lock);
    for (voice_index = 0;
         voice_index < (int)ARRAYSIZE(g_voices);
         ++voice_index) {
        const TrackedVoice *tracked = &g_voices[voice_index];
        if (tracked->voice != voice) continue;
        for (sidecar_index = 0;
             sidecar_index < tracked->native_sidecar_count &&
             sidecar_index < PCM_NATIVE_SIDECAR_COUNT;
             ++sidecar_index) {
            void *candidate =
                tracked->native_sidecars[sidecar_index].voice;
            if (candidate && candidate != exclude) {
                voices[count++] = candidate;
            }
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
    return count;
}


static int tracked_voice_submit(
    void *voice, WORD *format_tag, WORD *channels, DWORD *sample_rate,
    DWORD *submit_index) {
    int index;
    int found = 0;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice) {
            *format_tag = g_voices[index].format_tag;
            *channels = g_voices[index].channels;
            *sample_rate = g_voices[index].samples_per_second;
            *submit_index = g_voices[index].submit_count++;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

static PcmBufferContext *acquire_pcm_stream_buffer(
    PcmStream *stream, DWORD wanted) {
    PcmBufferContext *context = NULL;
    DWORD index;
    DWORD capacity;
    BYTE *data;
    if (!stream || !wanted || wanted > PCM_STREAM_BUFFER_LIMIT) {
        return NULL;
    }
    for (index = 0; index < PCM_STREAM_BUFFER_COUNT; ++index) {
        if (!stream->buffers[index].in_use) {
            context = &stream->buffers[index];
            break;
        }
    }
    if (!context) return NULL;
    if (context->capacity < wanted) {
        capacity = (wanted + 0xFFFFu) & ~0xFFFFu;
        if (capacity > PCM_STREAM_BUFFER_LIMIT) return NULL;
        if (context->data) {
            data = (BYTE *)HeapReAlloc(
                GetProcessHeap(), 0, context->data, capacity);
        } else {
            data = (BYTE *)HeapAlloc(
                GetProcessHeap(), 0, capacity);
        }
        if (!data) return NULL;
        context->data = data;
        context->capacity = capacity;
    }
    context->in_use = 1;
    context->audio_bytes = wanted;
    ++stream->outstanding_buffers;
    stream->outstanding_bytes += wanted;
    if (stream->outstanding_buffers > stream->peak_outstanding_buffers) {
        stream->peak_outstanding_buffers =
            stream->outstanding_buffers;
    }
    if (stream->outstanding_bytes > stream->peak_outstanding_bytes) {
        stream->peak_outstanding_bytes = stream->outstanding_bytes;
    }
    return context;
}

static int read_pcm_stream_exact(
    PcmStream *stream, PcmBufferContext *context, DWORD wanted) {
    DWORD total = 0;
    if (!stream || !context || !context->data) return 0;
    while (total < wanted) {
        DWORD read = 0;
        if (!ReadFile(
                stream->output_read, context->data + total,
                wanted - total, &read, NULL) ||
            !read) {
            stream->bytes_read += total;
            stream->failed = 1;
            return 0;
        }
        total += read;
    }
    stream->bytes_read += total;
    return 1;
}

static int prepare_pcm_submit(
    void *voice, const XAudio2BufferTrace *buffer,
    const XAudio2BufferWmaTrace *wma, PcmSubmitPlan *plan) {
    UINT32 decoded_pc_bytes = 0;
    SIZE_T copied = 0;
    TrackedVoice *tracked = NULL;
    PcmStream *stream = NULL;
    PcmBufferContext *context = NULL;
    int index;
    uint64_t pc_frames;
    uint64_t target_pcm_frames;
    uint64_t submit_frames;
    uint64_t remaining_frames;
    uint64_t source_frames;
    DWORD pc_bytes_per_frame;
    DWORD pcm_bytes_per_frame;
    DWORD submit_bytes;
    DWORD source_bytes;
    int end_of_stream;
    int looping_stream;

    if (!voice || !buffer || !wma || !plan ||
        !wma->decoded_packet_cumulative_bytes ||
        wma->packet_count == 0 || wma->packet_count > 64 ||
        buffer->play_begin || buffer->play_length ||
        buffer->loop_begin || buffer->loop_length ||
        buffer->loop_count) {
        return 0;
    }
    memset(plan, 0, sizeof(*plan));
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            wma->decoded_packet_cumulative_bytes + (wma->packet_count - 1),
            &decoded_pc_bytes, sizeof(decoded_pc_bytes), &copied) ||
        copied != sizeof(decoded_pc_bytes) ||
        decoded_pc_bytes == 0) {
        return 0;
    }

    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            g_voices[index].pcm_substitute &&
            g_voices[index].pcm_stream) {
            tracked = &g_voices[index];
            stream = tracked->pcm_stream;
            EnterCriticalSection(&stream->lock);
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (!tracked || !stream) return 0;

    pc_bytes_per_frame = (DWORD)tracked->channels * 2u;
    pcm_bytes_per_frame = stream->output_bytes_per_frame;
    end_of_stream = (buffer->flags & 0x40u) != 0;
    looping_stream = stream->asset == PCM_ASSET_ENV_AMB;
    if (!pc_bytes_per_frame || !pcm_bytes_per_frame ||
        decoded_pc_bytes % pc_bytes_per_frame ||
        !tracked->samples_per_second ||
        !stream->output_sample_rate ||
        tracked->pcm_source_frames_submitted > tracked->pcm_frames) {
        LeaveCriticalSection(&stream->lock);
        return 0;
    }
    /*
     * Arena ambience is continuously reused by the PC engine. Its decoder
     * keeps decoding the console cue in a loop, so wrap the scheduling
     * counters only after one exact pass has been consumed. EOS still means an
     * intentional early or final stop and must not wrap.
     */
    if (looping_stream && !end_of_stream &&
        tracked->pcm_source_frames_submitted >= tracked->pcm_frames &&
        stream->expected_bytes &&
        stream->bytes_read >= stream->expected_bytes &&
        stream->bytes_read % stream->expected_bytes == 0) {
        tracked->pc_frames_consumed = 0;
        tracked->pcm_frames_submitted = 0;
        tracked->pcm_source_frames_submitted = 0;
        tracked->pcm_end_submitted = 0;
        ++stream->loop_count;
    }
    pc_frames = tracked->pc_frames_consumed +
        ((uint64_t)decoded_pc_bytes / pc_bytes_per_frame);
    target_pcm_frames =
        (pc_frames * stream->output_sample_rate +
         tracked->samples_per_second / 2u) /
        tracked->samples_per_second;
    /*
     * The PC and console encoders do not always decode to identical cue
     * durations. Finish every non-looping console cue on the PC EOS packet.
     * If the console cue ends first, later PC packets are represented by
     * silence so XAudio2 still receives their callbacks instead of E_FAIL.
     */
    if (!looping_stream && end_of_stream &&
        target_pcm_frames < tracked->pcm_frames) {
        target_pcm_frames = tracked->pcm_frames;
    }
    if (target_pcm_frames < tracked->pcm_frames_submitted ||
        stream->failed) {
        LeaveCriticalSection(&stream->lock);
        return 0;
    }
    submit_frames =
        target_pcm_frames - tracked->pcm_frames_submitted;
    remaining_frames =
        tracked->pcm_frames -
        tracked->pcm_source_frames_submitted;
    if (looping_stream && submit_frames > remaining_frames) {
        submit_frames = remaining_frames;
    }
    if (!submit_frames) {
        /*
         * A zero-length XAudio2 submission is invalid.  If codec padding made
         * the PC issue one final EOS buffer after the Xbox cue was exhausted,
         * submit one silent frame so callbacks and stream completion still
         * occur instead of returning E_FAIL and triggering endless retries.
         */
        if (!end_of_stream ||
            tracked->pcm_source_frames_submitted <
                tracked->pcm_frames) {
            LeaveCriticalSection(&stream->lock);
            return 0;
        }
        submit_frames = 1;
    } else if (submit_frames >
               (UINT32_MAX / pcm_bytes_per_frame)) {
        LeaveCriticalSection(&stream->lock);
        return 0;
    }
    submit_bytes = (DWORD)(
        submit_frames * pcm_bytes_per_frame);
    source_frames = submit_frames;
    if (source_frames > remaining_frames) {
        source_frames = remaining_frames;
    }
    source_bytes = (DWORD)(
        source_frames * pcm_bytes_per_frame);
    context = acquire_pcm_stream_buffer(stream, submit_bytes);
    if (!context) {
        stream->failed = 1;
        LeaveCriticalSection(&stream->lock);
        return 0;
    }
    context->original_context = buffer->context;
    if (source_bytes &&
        !read_pcm_stream_exact(stream, context, source_bytes)) {
        release_pcm_stream_buffer(context);
        LeaveCriticalSection(&stream->lock);
        return 0;
    }
    if (source_bytes < submit_bytes) {
        memset(
            context->data + source_bytes, 0,
            submit_bytes - source_bytes);
    }

    plan->valid = 1;
    plan->voice_index = index;
    plan->decoded_pc_bytes = decoded_pc_bytes;
    plan->old_pc_frames = tracked->pc_frames_consumed;
    plan->old_pcm_frames = tracked->pcm_frames_submitted;
    plan->old_pcm_source_frames =
        tracked->pcm_source_frames_submitted;
    plan->new_pc_frames = pc_frames;
    plan->new_pcm_frames =
        tracked->pcm_frames_submitted + submit_frames;
    plan->new_pcm_source_frames =
        tracked->pcm_source_frames_submitted + source_frames;
    plan->pcm_bytes_per_frame = pcm_bytes_per_frame;
    plan->buffer = *buffer;
    plan->buffer.audio_data = context->data;
    plan->buffer.audio_bytes = submit_bytes;
    plan->buffer.play_begin = 0;
    plan->buffer.play_length = 0;
    plan->buffer.loop_begin = 0;
    plan->buffer.loop_length = 0;
    plan->buffer.loop_count = 0;
    plan->buffer.context = context;
    plan->stream_buffer = context;
    tracked->pc_frames_consumed = pc_frames;
    tracked->pcm_frames_submitted = plan->new_pcm_frames;
    tracked->pcm_source_frames_submitted =
        plan->new_pcm_source_frames;
    LeaveCriticalSection(&stream->lock);
    return 1;
}

static int is_pcm_stream_voice(void *voice) {
    int index;
    int result = 0;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            g_voices[index].pcm_substitute &&
            g_voices[index].pcm_stream) {
            result = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

static int active_pcm_selection_before_eos(
    void *voice, LONG selection) {
    PcmAssetKind asset = (PcmAssetKind)(
        (selection >> 16) & 0xFFFF);
    int track = (int)(selection & 0xFFFF);
    int index;
    int active = 0;

    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        TrackedVoice *tracked = &g_voices[index];
        PcmStream *stream;
        if (tracked->voice != voice ||
            !tracked->pcm_substitute ||
            tracked->pcm_end_submitted ||
            !tracked->pcm_stream) {
            continue;
        }
        stream = tracked->pcm_stream;
        EnterCriticalSection(&stream->lock);
        active = !stream->failed &&
            stream->asset == asset && stream->track == track;
        LeaveCriticalSection(&stream->lock);
        break;
    }
    LeaveCriticalSection(&g_lock);
    return active;
}

static void record_pcm_end_submitted(
    void *voice, const PcmSubmitPlan *plan) {
    PcmStream *stream;
    TrackedVoice *tracked;
    if (!voice || !plan || !plan->valid ||
        !plan->stream_buffer ||
        plan->voice_index < 0 ||
        plan->voice_index >= (int)ARRAYSIZE(g_voices)) {
        return;
    }
    stream = plan->stream_buffer->stream;
    EnterCriticalSection(&g_lock);
    tracked = &g_voices[plan->voice_index];
    if (tracked->voice == voice && tracked->pcm_stream == stream) {
        tracked->pcm_end_submitted = 1;
    }
    LeaveCriticalSection(&g_lock);
}

static void rollback_pcm_submit(
    void *voice, const PcmSubmitPlan *plan) {
    TrackedVoice *tracked;
    PcmStream *stream;
    if (!voice || !plan || !plan->valid ||
        plan->voice_index < 0 ||
        plan->voice_index >= (int)ARRAYSIZE(g_voices)) {
        return;
    }
    EnterCriticalSection(&g_lock);
    tracked = &g_voices[plan->voice_index];
    if (tracked->voice != voice || !tracked->pcm_stream) {
        LeaveCriticalSection(&g_lock);
        if (plan->stream_buffer) {
            release_pcm_stream_buffer(plan->stream_buffer);
        }
        return;
    }
    stream = tracked->pcm_stream;
    EnterCriticalSection(&stream->lock);
    if (tracked->pc_frames_consumed == plan->new_pc_frames &&
        tracked->pcm_frames_submitted == plan->new_pcm_frames &&
        tracked->pcm_source_frames_submitted ==
            plan->new_pcm_source_frames) {
        tracked->pc_frames_consumed = plan->old_pc_frames;
        tracked->pcm_frames_submitted = plan->old_pcm_frames;
        tracked->pcm_source_frames_submitted =
            plan->old_pcm_source_frames;
    }
    stream->failed = 1;
    LeaveCriticalSection(&stream->lock);
    LeaveCriticalSection(&g_lock);
    if (plan->stream_buffer) {
        release_pcm_stream_buffer(plan->stream_buffer);
    }
}

static int copy_delayed_sends(
    TrackedVoice *tracked, const XAudio2VoiceSendsTrace *sends) {
    UINT32 count = sends ? sends->send_count : 0;
    if (!tracked || count > DELAYED_MAX_SENDS ||
        (count && (!sends || !sends->sends))) {
        return 0;
    }
    tracked->create_sends.send_count = count;
    tracked->create_sends.sends =
        count ? tracked->create_send_descriptors : NULL;
    if (count) {
        memcpy(
            tracked->create_send_descriptors, sends->sends,
            count * sizeof(tracked->create_send_descriptors[0]));
    }
    return 1;
}

static int copy_delayed_effect_chain(
    TrackedVoice *tracked, const XAudio2EffectChainTrace *chain) {
    UINT32 count = chain ? chain->effect_count : 0;
    if (!tracked || count > DELAYED_MAX_EFFECTS ||
        (count && (!chain || !chain->effect_descriptors))) {
        return 0;
    }
    tracked->create_effect_chain.effect_count = count;
    tracked->create_effect_chain.effect_descriptors =
        count ? tracked->create_effect_descriptors : NULL;
    if (count) {
        memcpy(
            tracked->create_effect_descriptors,
            chain->effect_descriptors,
            count * sizeof(tracked->create_effect_descriptors[0]));
    }
    return 1;
}

static void record_delayed_started(void *voice, int started) {
    int index;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice) {
            g_voices[index].delayed_started = started;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void record_delayed_filter(
    void *voice, const XAudio2FilterParametersTrace *parameters) {
    int index;
    if (!parameters) return;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            !g_voices[index].redirect_voice) {
            g_voices[index].delayed_filter = *parameters;
            g_voices[index].delayed_filter_valid = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void record_delayed_output_filter(
    void *voice, void *destination,
    const XAudio2FilterParametersTrace *parameters) {
    int voice_index;
    int index;
    if (!parameters) return;
    EnterCriticalSection(&g_lock);
    for (voice_index = 0;
         voice_index < (int)ARRAYSIZE(g_voices);
         ++voice_index) {
        TrackedVoice *tracked = &g_voices[voice_index];
        if (tracked->voice != voice || tracked->redirect_voice) continue;
        for (index = 0;
             index < tracked->delayed_output_filter_count;
             ++index) {
            if (tracked->delayed_output_filters[index].destination_voice ==
                destination) {
                break;
            }
        }
        if (index == tracked->delayed_output_filter_count &&
            index < DELAYED_MAX_OUTPUT_FILTERS) {
            ++tracked->delayed_output_filter_count;
        }
        if (index < DELAYED_MAX_OUTPUT_FILTERS) {
            tracked->delayed_output_filters[index].valid = 1;
            tracked->delayed_output_filters[index].destination_voice =
                destination;
            tracked->delayed_output_filters[index].parameters = *parameters;
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

static void record_delayed_matrix(
    void *voice, void *destination, UINT32 source_channels,
    UINT32 destination_channels, const float *levels) {
    int voice_index;
    int index;
    uint64_t level_count64 =
        (uint64_t)source_channels * destination_channels;
    UINT32 level_count;
    if (!levels || !source_channels || !destination_channels ||
        level_count64 > DELAYED_MAX_MATRIX_LEVELS) {
        return;
    }
    level_count = (UINT32)level_count64;
    EnterCriticalSection(&g_lock);
    for (voice_index = 0;
         voice_index < (int)ARRAYSIZE(g_voices);
         ++voice_index) {
        TrackedVoice *tracked = &g_voices[voice_index];
        if (tracked->voice != voice || tracked->redirect_voice) continue;
        for (index = 0; index < tracked->delayed_matrix_count; ++index) {
            if (tracked->delayed_matrices[index].destination_voice ==
                destination) {
                break;
            }
        }
        if (index == tracked->delayed_matrix_count &&
            index < DELAYED_MAX_MATRICES) {
            ++tracked->delayed_matrix_count;
        }
        if (index < DELAYED_MAX_MATRICES) {
            DelayedMatrixState *matrix =
                &tracked->delayed_matrices[index];
            matrix->valid = 1;
            matrix->destination_voice = destination;
            matrix->source_channels = source_channels;
            matrix->destination_channels = destination_channels;
            memcpy(
                matrix->levels, levels,
                level_count * sizeof(matrix->levels[0]));
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

static void record_delayed_effect_state(
    void *voice, UINT32 effect_index, int enabled) {
    int index;
    if (effect_index >= DELAYED_MAX_EFFECTS) return;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            !g_voices[index].redirect_voice) {
            g_voices[index].delayed_effect_state_valid[effect_index] = 1;
            g_voices[index].delayed_effect_enabled[effect_index] =
                enabled ? 1 : 0;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void record_delayed_effect_parameters(
    void *voice, UINT32 effect_index, const void *parameters,
    UINT32 byte_count) {
    int voice_index;
    int index;
    if (!parameters || !byte_count ||
        byte_count > DELAYED_MAX_EFFECT_PARAMETER_BYTES) {
        return;
    }
    EnterCriticalSection(&g_lock);
    for (voice_index = 0;
         voice_index < (int)ARRAYSIZE(g_voices);
         ++voice_index) {
        TrackedVoice *tracked = &g_voices[voice_index];
        if (tracked->voice != voice || tracked->redirect_voice) continue;
        for (index = 0;
             index < tracked->delayed_effect_parameter_count;
             ++index) {
            if (tracked->delayed_effect_parameters[index].effect_index ==
                effect_index) {
                break;
            }
        }
        if (index == tracked->delayed_effect_parameter_count &&
            index < DELAYED_MAX_EFFECT_PARAMETER_RECORDS) {
            ++tracked->delayed_effect_parameter_count;
        }
        if (index < DELAYED_MAX_EFFECT_PARAMETER_RECORDS) {
            DelayedEffectParameterState *state =
                &tracked->delayed_effect_parameters[index];
            state->valid = 1;
            state->effect_index = effect_index;
            state->byte_count = byte_count;
            memcpy(state->bytes, parameters, byte_count);
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

static int update_delayed_sends(
    void *voice, const XAudio2VoiceSendsTrace *sends) {
    int index;
    int copied = 1;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            !g_voices[index].redirect_voice) {
            copied = copy_delayed_sends(&g_voices[index], sends);
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return copied;
}

static int update_delayed_effect_chain(
    void *voice, const XAudio2EffectChainTrace *chain) {
    int index;
    int copied = 1;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice &&
            !g_voices[index].redirect_voice) {
            copied = copy_delayed_effect_chain(&g_voices[index], chain);
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return copied;
}

static HRESULT __stdcall hook_submit_source_buffer(
    void *voice, const XAudio2BufferTrace *buffer,
    const XAudio2BufferWmaTrace *wma) {
    SubmitSourceBufferFn original = NULL;
    void *submission_voice = voice;
    WORD format_tag = 0;
    WORD channels = 0;
    DWORD sample_rate = 0;
    DWORD submit_index = 0;
    int tracked = tracked_voice_submit(
        voice, &format_tag, &channels, &sample_rate, &submit_index);
    int route_activated = 0;
    LONG route_selection = -1;
    PcmSelectionRecord claimed_record;
    void *previous_target = voice;
    void *new_target = voice;
    memset(&claimed_record, 0, sizeof(claimed_record));
    if (tracked && format_tag == 0x0161 &&
        sample_rate == 22050 &&
        (channels == 1 || channels == 2)) {
        if (!active_pcm_payload_matches(voice, buffer)) {
            route_selection = claim_pcm_selection_for_payload(
                buffer, GetCurrentThreadId(), channels,
                &claimed_record);
        }
        if (route_selection >= 0) {
            if (active_pcm_selection_before_eos(
                    voice, route_selection)) {
                route_selection = -1;
            } else {
                route_activated = activate_native_pcm_stream(
                    voice, route_selection, &claimed_record,
                    &previous_target, &new_target);
            }
        }
    }
    submission_voice = redirected_voice(voice);
    if (route_activated) {
        flush_reused_native_sidecar_before_submit(
            voice, previous_target, new_target);
        apply_active_pcm_gain(voice, submission_voice);
    }
    HRESULT result;
    PcmSubmitPlan pcm_plan;
    int pcm_submit = 0;
    int pcm_stream_voice = 0;

    original = (SubmitSourceBufferFn)(uintptr_t)
        voice_slot_for_target(
            voice, submission_voice,
            IXAUDIO2_SOURCE_SUBMIT_BUFFER_SLOT);
    if (!original) {
        return E_FAIL;
    }
    if (route_selection >= 0 && !route_activated) {
        return E_FAIL;
    }

    pcm_stream_voice = is_pcm_stream_voice(voice);
    pcm_submit = prepare_pcm_submit(voice, buffer, wma, &pcm_plan);


    if (pcm_submit) {
        result = original(submission_voice, &pcm_plan.buffer, NULL);
        if (FAILED(result)) rollback_pcm_submit(voice, &pcm_plan);
        if (SUCCEEDED(result) && (buffer->flags & 0x40u)) {
            record_pcm_end_submitted(voice, &pcm_plan);
        }
        /*
         * Do not start an empty delayed sidecar.  The gameplay crash boundary
         * was the first SubmitSourceBuffer immediately after an already
         * started env_amb sidecar was activated.  Queue the first PCM buffer
         * successfully, then transfer start/stop state.
         */
        if (SUCCEEDED(result) && route_activated) {
            switch_started_native_voice_after_submit(
                voice, previous_target, new_target);
        }
    } else if (pcm_stream_voice) {
        result = E_FAIL;
    } else {
        result = original(submission_voice, buffer, wma);
    }
    return result;
}

static void __stdcall hook_get_voice_details(
    void *voice, XAudio2VoiceDetailsTrace *details) {
    GetVoiceDetailsFn original = (GetVoiceDetailsFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_DETAILS_SLOT);
    if (original) original(redirected_voice(voice), details);
}

static HRESULT __stdcall hook_set_output_voices(
    void *voice, const XAudio2VoiceSendsTrace *sends) {
    SetOutputVoicesFn original = (SetOutputVoicesFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_OUTPUT_VOICES_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, sends);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetOutputVoicesFn mirror =
                (SetOutputVoicesFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_OUTPUT_VOICES_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(candidates[candidate_index], sends)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        update_delayed_sends(voice, sends);
    }
    return result;
}

static HRESULT __stdcall hook_set_effect_chain(
    void *voice, const XAudio2EffectChainTrace *chain) {
    SetEffectChainFn original = (SetEffectChainFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_EFFECT_CHAIN_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, chain);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetEffectChainFn mirror =
                (SetEffectChainFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_EFFECT_CHAIN_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(candidates[candidate_index], chain)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        update_delayed_effect_chain(voice, chain);
    }
    return result;
}

static HRESULT __stdcall hook_enable_effect(
    void *voice, UINT32 effect_index, UINT32 operation_set) {
    EffectSwitchFn original = (EffectSwitchFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_ENABLE_EFFECT_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, effect_index, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            EffectSwitchFn mirror =
                (EffectSwitchFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_ENABLE_EFFECT_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    effect_index, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_effect_state(voice, effect_index, 1);
    }
    return result;
}

static HRESULT __stdcall hook_disable_effect(
    void *voice, UINT32 effect_index, UINT32 operation_set) {
    EffectSwitchFn original = (EffectSwitchFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_DISABLE_EFFECT_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, effect_index, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            EffectSwitchFn mirror =
                (EffectSwitchFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_DISABLE_EFFECT_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    effect_index, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_effect_state(voice, effect_index, 0);
    }
    return result;
}

static void __stdcall hook_get_effect_state(
    void *voice, UINT32 effect_index, BOOL *enabled) {
    GetEffectStateFn original = (GetEffectStateFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_EFFECT_STATE_SLOT);
    if (original) {
        original(redirected_voice(voice), effect_index, enabled);
    }
}

static HRESULT __stdcall hook_set_effect_parameters(
    void *voice, UINT32 effect_index, const void *parameters,
    UINT32 byte_count, UINT32 operation_set) {
    SetEffectParametersFn original = (SetEffectParametersFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_EFFECT_PARAMETERS_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(
        target, effect_index, parameters,
        byte_count, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetEffectParametersFn mirror =
                (SetEffectParametersFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_EFFECT_PARAMETERS_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    effect_index, parameters,
                    byte_count, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_effect_parameters(
            voice, effect_index, parameters, byte_count);
    }
    return result;
}

static HRESULT __stdcall hook_get_effect_parameters(
    void *voice, UINT32 effect_index, void *parameters,
    UINT32 byte_count) {
    GetEffectParametersFn original = (GetEffectParametersFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_EFFECT_PARAMETERS_SLOT);
    if (!original) return E_FAIL;
    return original(
        redirected_voice(voice), effect_index, parameters, byte_count);
}

static HRESULT __stdcall hook_set_filter_parameters(
    void *voice, const XAudio2FilterParametersTrace *parameters,
    UINT32 operation_set) {
    SetFilterParametersFn original = (SetFilterParametersFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_FILTER_PARAMETERS_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, parameters, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetFilterParametersFn mirror =
                (SetFilterParametersFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_FILTER_PARAMETERS_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    parameters, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_filter(voice, parameters);
    }
    return result;
}

static void __stdcall hook_get_filter_parameters(
    void *voice, XAudio2FilterParametersTrace *parameters) {
    GetFilterParametersFn original = (GetFilterParametersFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_FILTER_PARAMETERS_SLOT);
    if (original) original(redirected_voice(voice), parameters);
}

static HRESULT __stdcall hook_set_output_filter_parameters(
    void *voice, void *destination,
    const XAudio2FilterParametersTrace *parameters,
    UINT32 operation_set) {
    SetOutputFilterParametersFn original =
        (SetOutputFilterParametersFn)(uintptr_t)voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_OUTPUT_FILTER_PARAMETERS_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(
        target, destination, parameters, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetOutputFilterParametersFn mirror =
                (SetOutputFilterParametersFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_OUTPUT_FILTER_PARAMETERS_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    destination, parameters, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_output_filter(voice, destination, parameters);
    }
    return result;
}

static void __stdcall hook_get_output_filter_parameters(
    void *voice, void *destination,
    XAudio2FilterParametersTrace *parameters) {
    GetOutputFilterParametersFn original =
        (GetOutputFilterParametersFn)(uintptr_t)voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_OUTPUT_FILTER_PARAMETERS_SLOT);
    if (original) {
        original(redirected_voice(voice), destination, parameters);
    }
}

static HRESULT __stdcall hook_set_volume(
    void *voice, float volume, UINT32 operation_set) {
    VoiceVolumeState state;
    SetVolumeFn original;
    HRESULT result;
    snapshot_voice_volume_state(voice, &state);
    original = (SetVolumeFn)(uintptr_t)voice_slot_for_target(
        voice, state.target, IXAUDIO2_VOICE_SET_VOLUME_SLOT);
    if (!original) return E_FAIL;
    result = original(
        state.target, volume * state.pcm_gain, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, state.target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetVolumeFn mirror =
                (SetVolumeFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_VOLUME_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    volume, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        store_voice_logical_volume(voice, volume);
    }
    return result;
}

static void __stdcall hook_get_volume(void *voice, float *volume) {
    VoiceVolumeState state;
    GetVolumeFn original;
    if (!volume) return;
    snapshot_voice_volume_state(voice, &state);
    if (state.pcm_substitute && state.pcm_gain != 1.0f) {
        *volume = state.logical_volume;
        return;
    }
    original = (GetVolumeFn)(uintptr_t)voice_slot_for_target(
        voice, state.target, IXAUDIO2_VOICE_GET_VOLUME_SLOT);
    if (original) original(state.target, volume);
}

static HRESULT __stdcall hook_set_channel_volumes(
    void *voice, UINT32 channels, const float *volumes,
    UINT32 operation_set) {
    SetChannelVolumesFn original = (SetChannelVolumesFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_CHANNEL_VOLUMES_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(
        target, channels, volumes, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetChannelVolumesFn mirror =
                (SetChannelVolumesFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_CHANNEL_VOLUMES_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    channels, volumes, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
    }
    return result;
}

static void __stdcall hook_get_channel_volumes(
    void *voice, UINT32 channels, float *volumes) {
    GetChannelVolumesFn original = (GetChannelVolumesFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_CHANNEL_VOLUMES_SLOT);
    if (original) {
        original(redirected_voice(voice), channels, volumes);
    }
}

static HRESULT __stdcall hook_set_output_matrix(
    void *voice, void *destination, UINT32 source_channels,
    UINT32 destination_channels, const float *levels,
    UINT32 operation_set) {
    SetOutputMatrixFn original = (SetOutputMatrixFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_SET_OUTPUT_MATRIX_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(
        target, destination, source_channels,
        destination_channels, levels, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetOutputMatrixFn mirror =
                (SetOutputMatrixFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_VOICE_SET_OUTPUT_MATRIX_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    destination, source_channels,
                    destination_channels, levels, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
        record_delayed_matrix(
            voice, destination, source_channels,
            destination_channels, levels);
    }
    return result;
}

static void __stdcall hook_get_output_matrix(
    void *voice, void *destination, UINT32 source_channels,
    UINT32 destination_channels, float *levels) {
    GetOutputMatrixFn original = (GetOutputMatrixFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_VOICE_GET_OUTPUT_MATRIX_SLOT);
    if (original) {
        original(
            redirected_voice(voice), destination, source_channels,
            destination_channels, levels);
    }
}

static HRESULT __stdcall hook_start_voice(
    void *voice, UINT32 flags, UINT32 operation_set) {
    StartStopFn original = (StartStopFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_START_SLOT);
    HRESULT result;
    if (!original) return E_FAIL;
    result = original(redirected_voice(voice), flags, operation_set);
    if (SUCCEEDED(result)) record_delayed_started(voice, 1);
    return result;
}

static HRESULT __stdcall hook_stop_voice(
    void *voice, UINT32 flags, UINT32 operation_set) {
    StartStopFn original = (StartStopFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_STOP_SLOT);
    HRESULT result;
    if (!original) return E_FAIL;
    result = original(redirected_voice(voice), flags, operation_set);
    if (SUCCEEDED(result)) record_delayed_started(voice, 0);
    return result;
}

static HRESULT __stdcall hook_flush_source_buffers(void *voice) {
    SimpleSourceVoiceFn original = (SimpleSourceVoiceFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_FLUSH_SLOT);
    if (!original) return E_FAIL;
    return original(redirected_voice(voice));
}

static HRESULT __stdcall hook_discontinuity(void *voice) {
    SimpleSourceVoiceFn original = (SimpleSourceVoiceFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_DISCONTINUITY_SLOT);
    if (!original) return E_FAIL;
    return original(redirected_voice(voice));
}

static HRESULT __stdcall hook_exit_loop(
    void *voice, UINT32 operation_set) {
    ExitLoopFn original = (ExitLoopFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_EXIT_LOOP_SLOT);
    if (!original) return E_FAIL;
    return original(redirected_voice(voice), operation_set);
}

static void __stdcall hook_get_state_legacy(
    void *voice, XAudio2VoiceStateLegacyTrace *state) {
    GetStateLegacyFn original = (GetStateLegacyFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_GET_STATE_SLOT);
    if (original) original(redirected_voice(voice), state);
}

static HRESULT __stdcall hook_set_frequency_ratio(
    void *voice, float ratio, UINT32 operation_set) {
    SetFrequencyRatioFn original = (SetFrequencyRatioFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_SET_FREQUENCY_RATIO_SLOT);
    HRESULT result;
    void *target;
    if (!original) return E_FAIL;
    target = redirected_voice(voice);
    result = original(target, ratio, operation_set);
    if (SUCCEEDED(result)) {
        void *candidates[PCM_NATIVE_SIDECAR_COUNT];
        int candidate_count = snapshot_native_sidecar_voices(
            voice, target, candidates);
        int candidate_index;
        for (candidate_index = 0;
             candidate_index < candidate_count;
             ++candidate_index) {
            SetFrequencyRatioFn mirror =
                (SetFrequencyRatioFn)(uintptr_t)
                    voice_slot_for_target(
                        voice, candidates[candidate_index],
                        IXAUDIO2_SOURCE_SET_FREQUENCY_RATIO_SLOT);
            HRESULT mirror_result = mirror
                ? mirror(
                    candidates[candidate_index],
                    ratio, operation_set)
                : E_FAIL;
            if (FAILED(mirror_result)) {
            }
        }
    }
    return result;
}

static void __stdcall hook_get_frequency_ratio(
    void *voice, float *ratio) {
    GetFrequencyRatioFn original = (GetFrequencyRatioFn)(uintptr_t)
        voice_slot_for_target(
            voice, redirected_voice(voice),
            IXAUDIO2_SOURCE_GET_FREQUENCY_RATIO_SLOT);
    if (original) original(redirected_voice(voice), ratio);
}

static void untrack_voice(void *voice) {
    int index;
    PcmStream *stream = NULL;
    if (!voice) return;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice) {
            stream = g_voices[index].pcm_stream;
            g_voices[index].pcm_stream = NULL;
            memset(&g_voices[index], 0, sizeof(g_voices[index]));
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (stream) close_pcm_stream(stream);
    close_retired_pcm_streams_for_voice(voice);
}

static void __stdcall hook_destroy_voice(void *voice) {
    void *target = redirected_voice(voice);
    void *native_sidecars[PCM_NATIVE_SIDECAR_COUNT];
    int native_sidecar_count = snapshot_native_sidecar_voices(
        voice, NULL, native_sidecars);
    int native_sidecar_index;
    if (native_sidecar_count > 0) {
        for (native_sidecar_index = 0;
             native_sidecar_index < native_sidecar_count;
             ++native_sidecar_index) {
            DestroyVoiceFn sidecar_destroy =
                (DestroyVoiceFn)(uintptr_t)
                    voice_slot_for_target(
                        voice,
                        native_sidecars[native_sidecar_index],
                        IXAUDIO2_VOICE_DESTROY_SLOT);
            if (sidecar_destroy) {
                sidecar_destroy(
                    native_sidecars[native_sidecar_index]);
            }
        }
    } else if (target && target != voice) {
        DestroyVoiceFn target_destroy =
            (DestroyVoiceFn)(uintptr_t)
                voice_slot_for_target(
                    voice, target,
                    IXAUDIO2_VOICE_DESTROY_SLOT);
        if (target_destroy) target_destroy(target);
    }
    {
        DestroyVoiceFn proxy_destroy =
            (DestroyVoiceFn)(uintptr_t)
                voice_slot_for_target(
                    voice, voice,
                    IXAUDIO2_VOICE_DESTROY_SLOT);
        if (!proxy_destroy) {
            return;
        }
        proxy_destroy(voice);
    }
    untrack_voice(voice);
}

static int patch_submit_vtable(void *voice) {
    void **vtable;
    DWORD old_protect;
    void *hooks[IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT];
    int index;
    UINT32 slot;
    int empty_index = -1;
    if (!voice) return 0;
    vtable = *(void ***)voice;
    if (!vtable) return 0;
    hooks[IXAUDIO2_VOICE_GET_DETAILS_SLOT] =
        (void *)(uintptr_t)hook_get_voice_details;
    hooks[IXAUDIO2_VOICE_SET_OUTPUT_VOICES_SLOT] =
        (void *)(uintptr_t)hook_set_output_voices;
    hooks[IXAUDIO2_VOICE_SET_EFFECT_CHAIN_SLOT] =
        (void *)(uintptr_t)hook_set_effect_chain;
    hooks[IXAUDIO2_VOICE_ENABLE_EFFECT_SLOT] =
        (void *)(uintptr_t)hook_enable_effect;
    hooks[IXAUDIO2_VOICE_DISABLE_EFFECT_SLOT] =
        (void *)(uintptr_t)hook_disable_effect;
    hooks[IXAUDIO2_VOICE_GET_EFFECT_STATE_SLOT] =
        (void *)(uintptr_t)hook_get_effect_state;
    hooks[IXAUDIO2_VOICE_SET_EFFECT_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_set_effect_parameters;
    hooks[IXAUDIO2_VOICE_GET_EFFECT_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_get_effect_parameters;
    hooks[IXAUDIO2_VOICE_SET_FILTER_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_set_filter_parameters;
    hooks[IXAUDIO2_VOICE_GET_FILTER_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_get_filter_parameters;
    hooks[IXAUDIO2_VOICE_SET_OUTPUT_FILTER_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_set_output_filter_parameters;
    hooks[IXAUDIO2_VOICE_GET_OUTPUT_FILTER_PARAMETERS_SLOT] =
        (void *)(uintptr_t)hook_get_output_filter_parameters;
    hooks[IXAUDIO2_VOICE_SET_VOLUME_SLOT] =
        (void *)(uintptr_t)hook_set_volume;
    hooks[IXAUDIO2_VOICE_GET_VOLUME_SLOT] =
        (void *)(uintptr_t)hook_get_volume;
    hooks[IXAUDIO2_VOICE_SET_CHANNEL_VOLUMES_SLOT] =
        (void *)(uintptr_t)hook_set_channel_volumes;
    hooks[IXAUDIO2_VOICE_GET_CHANNEL_VOLUMES_SLOT] =
        (void *)(uintptr_t)hook_get_channel_volumes;
    hooks[IXAUDIO2_VOICE_SET_OUTPUT_MATRIX_SLOT] =
        (void *)(uintptr_t)hook_set_output_matrix;
    hooks[IXAUDIO2_VOICE_GET_OUTPUT_MATRIX_SLOT] =
        (void *)(uintptr_t)hook_get_output_matrix;
    hooks[IXAUDIO2_VOICE_DESTROY_SLOT] =
        (void *)(uintptr_t)hook_destroy_voice;
    hooks[IXAUDIO2_SOURCE_START_SLOT] =
        (void *)(uintptr_t)hook_start_voice;
    hooks[IXAUDIO2_SOURCE_STOP_SLOT] =
        (void *)(uintptr_t)hook_stop_voice;
    hooks[IXAUDIO2_SOURCE_SUBMIT_BUFFER_SLOT] =
        (void *)(uintptr_t)hook_submit_source_buffer;
    hooks[IXAUDIO2_SOURCE_FLUSH_SLOT] =
        (void *)(uintptr_t)hook_flush_source_buffers;
    hooks[IXAUDIO2_SOURCE_DISCONTINUITY_SLOT] =
        (void *)(uintptr_t)hook_discontinuity;
    hooks[IXAUDIO2_SOURCE_EXIT_LOOP_SLOT] =
        (void *)(uintptr_t)hook_exit_loop;
    hooks[IXAUDIO2_SOURCE_GET_STATE_SLOT] =
        (void *)(uintptr_t)hook_get_state_legacy;
    hooks[IXAUDIO2_SOURCE_SET_FREQUENCY_RATIO_SLOT] =
        (void *)(uintptr_t)hook_set_frequency_ratio;
    hooks[IXAUDIO2_SOURCE_GET_FREQUENCY_RATIO_SLOT] =
        (void *)(uintptr_t)hook_get_frequency_ratio;

    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_submit_vtables); ++index) {
        if (g_submit_vtables[index].vtable == vtable) {
            LeaveCriticalSection(&g_lock);
            return 1;
        }
        if (empty_index < 0 && !g_submit_vtables[index].vtable) {
            empty_index = index;
        }
    }
    if (empty_index < 0) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    if (!VirtualProtect(
            vtable,
            sizeof(*vtable) * IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT,
            PAGE_EXECUTE_READWRITE, &old_protect)) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    g_submit_vtables[empty_index].vtable = vtable;
    for (slot = 0;
         slot < IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT;
         ++slot) {
        g_submit_vtables[empty_index].original_slots[slot] = vtable[slot];
        vtable[slot] = hooks[slot];
    }
    VirtualProtect(
        vtable,
        sizeof(*vtable) * IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT,
        old_protect, &old_protect);
    FlushInstructionCache(
        GetCurrentProcess(), vtable,
        sizeof(*vtable) * IXAUDIO2_SOURCE_VTABLE_SLOT_COUNT);
    LeaveCriticalSection(&g_lock);
    return 1;
}

static int track_voice(
    void *voice, const WaveFormatExTrace *format, void *engine,
    UINT32 create_flags, float maximum_frequency_ratio,
    void *callback, const XAudio2VoiceSendsTrace *sends,
    const XAudio2EffectChainTrace *effect_chain) {
    int index;
    int tracked = 0;
    PcmStream *old_stream = NULL;
    if (!voice || !format) return 0;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (!g_voices[index].voice || g_voices[index].voice == voice) {
            old_stream = g_voices[index].pcm_stream;
            g_voices[index].pcm_stream = NULL;
            memset(&g_voices[index], 0, sizeof(g_voices[index]));
            g_voices[index].voice = voice;
            g_voices[index].format_tag = format->format_tag;
            g_voices[index].channels = format->channels;
            g_voices[index].samples_per_second =
                format->samples_per_second;
            g_voices[index].submit_count = 0;
            g_voices[index].logical_volume = 1.0f;
            g_voices[index].pcm_gain = 1.0f;
            g_voices[index].create_engine = engine;
            g_voices[index].create_flags = create_flags;
            g_voices[index].create_maximum_frequency_ratio =
                maximum_frequency_ratio;
            g_voices[index].create_callback = callback;
            if (!copy_delayed_sends(&g_voices[index], sends) ||
                !copy_delayed_effect_chain(
                    &g_voices[index], effect_chain)) {
                memset(&g_voices[index], 0, sizeof(g_voices[index]));
                break;
            }
            tracked = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (old_stream) close_pcm_stream(old_stream);
    return tracked;
}

static int track_voice_pcm(
    void *voice, const WaveFormatExTrace *original_format,
    PcmStream *stream) {
    int index;
    int tracked = 0;
    PcmStream *old_stream = NULL;
    if (!voice || !original_format || !stream) return 0;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (!g_voices[index].voice || g_voices[index].voice == voice) {
            old_stream = g_voices[index].pcm_stream;
            g_voices[index].pcm_stream = NULL;
            memset(&g_voices[index], 0, sizeof(g_voices[index]));
            g_voices[index].voice = voice;
            g_voices[index].format_tag = original_format->format_tag;
            g_voices[index].channels = original_format->channels;
            g_voices[index].samples_per_second =
                original_format->samples_per_second;
            g_voices[index].pcm_substitute = 1;
            g_voices[index].pcm_track = stream->track;
            g_voices[index].logical_volume = 1.0f;
            g_voices[index].pcm_gain = game_profile_pcm_gain(stream->asset);
            g_voices[index].pcm_stream = stream;
            g_voices[index].pcm_size = stream->expected_bytes;
            g_voices[index].pcm_frames =
                stream->expected_bytes /
                stream->output_bytes_per_frame;
            tracked = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (old_stream) close_pcm_stream(old_stream);
    return tracked;
}

static int create_native_pcm_sidecars(void *voice) {
    static const DWORD mono_rates[] = {48000u};
    static const DWORD stereo_rates[] = {48000u};
    TrackedVoice *tracked = NULL;
    const DWORD *rates;
    int rate_count;
    int voice_index;
    int sidecar_index;
    int created_count = 0;
    if (!voice || !g_real_create_source_voice) return 0;

    EnterCriticalSection(&g_lock);
    for (voice_index = 0;
         voice_index < (int)ARRAYSIZE(g_voices);
         ++voice_index) {
        if (g_voices[voice_index].voice == voice) {
            tracked = &g_voices[voice_index];
            break;
        }
    }
    if (!tracked ||
        (tracked->channels != 1 && tracked->channels != 2)) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    rates = tracked->channels == 1 ? mono_rates : stereo_rates;
    rate_count = tracked->channels == 1
        ? (int)ARRAYSIZE(mono_rates)
        : (int)ARRAYSIZE(stereo_rates);
    tracked->native_sidecar_count = rate_count;
    for (sidecar_index = 0;
         sidecar_index < rate_count;
         ++sidecar_index) {
        PcmNativeSidecar *sidecar =
            &tracked->native_sidecars[sidecar_index];
        memset(sidecar, 0, sizeof(*sidecar));
        sidecar->channels = tracked->channels;
        sidecar->sample_rate = rates[sidecar_index];
        sidecar->callback.vtable = &g_pcm_callback_vtable;
        sidecar->callback.original_callback =
            tracked->create_callback;
    }
    LeaveCriticalSection(&g_lock);

    for (sidecar_index = 0;
         sidecar_index < rate_count;
         ++sidecar_index) {
        PcmNativeSidecar *sidecar =
            &tracked->native_sidecars[sidecar_index];
        WaveFormatExTrace pcm_format;
        HRESULT result;
        memset(&pcm_format, 0, sizeof(pcm_format));
        pcm_format.format_tag = 0x0001;
        pcm_format.channels = sidecar->channels;
        pcm_format.samples_per_second = sidecar->sample_rate;
        pcm_format.block_align =
            (WORD)(pcm_format.channels * 2u);
        pcm_format.average_bytes_per_second =
            pcm_format.samples_per_second *
            pcm_format.block_align;
        pcm_format.bits_per_sample = 16;
        result = g_real_create_source_voice(
            tracked->create_engine, &sidecar->voice,
            &pcm_format, tracked->create_flags,
            tracked->create_maximum_frequency_ratio,
            &sidecar->callback,
            tracked->create_sends.send_count
                ? &tracked->create_sends
                : NULL,
            tracked->create_effect_chain.effect_count
                ? &tracked->create_effect_chain
                : NULL);
        if (FAILED(result)) sidecar->voice = NULL;
        if (sidecar->voice) ++created_count;
    }
    return created_count;
}

static int activate_native_pcm_stream(
    void *voice, LONG selection,
    const PcmSelectionRecord *selection_record,
    void **previous_target_out, void **new_target_out) {
    PcmAssetKind asset = (PcmAssetKind)(
        (selection >> 16) & 0xFFFF);
    int track = (int)(selection & 0xFFFF);
    WORD channels = pcm_asset_output_channels(asset);
    DWORD sample_rate = pcm_asset_output_sample_rate(asset);
    void *callback = NULL;
    PcmStream *stream = NULL;
    PcmStream *old_stream = NULL;
    PcmNativeSidecar *sidecar = NULL;
    void *previous_target = voice;
    int voice_index = -1;
    int sidecar_index;
    int index;
    if (previous_target_out) *previous_target_out = voice;
    if (new_target_out) *new_target_out = voice;
    if (track < 0 || track >= pcm_asset_track_count(asset)) {
        return 0;
    }

    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        TrackedVoice *tracked = &g_voices[index];
        if (tracked->voice != voice) continue;
        voice_index = index;
        callback = tracked->create_callback;
        previous_target = tracked->redirect_voice
            ? tracked->redirect_voice : voice;
        for (sidecar_index = 0;
             sidecar_index < tracked->native_sidecar_count &&
             sidecar_index < PCM_NATIVE_SIDECAR_COUNT;
             ++sidecar_index) {
            PcmNativeSidecar *candidate =
                &tracked->native_sidecars[sidecar_index];
            if (candidate->voice &&
                candidate->channels == channels &&
                candidate->sample_rate == sample_rate) {
                sidecar = candidate;
                break;
            }
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
    if (voice_index < 0 || !sidecar || !sidecar->voice) {
        return 0;
    }
    stream = take_prefetched_pcm_stream(
        asset, track, callback);
    if (!stream &&
        !open_pcm_stream(asset, track, callback, &stream)) {
        return 0;
    }
    stream->owner_voice = voice;

    EnterCriticalSection(&g_lock);
    if (voice_index < (int)ARRAYSIZE(g_voices) &&
        g_voices[voice_index].voice == voice &&
        sidecar->voice) {
        TrackedVoice *tracked = &g_voices[voice_index];
        old_stream = tracked->pcm_stream;
        sidecar->callback.stream = stream;
        tracked->active_native_sidecar = sidecar;
        tracked->redirect_voice = sidecar->voice;
        tracked->pcm_substitute = 1;
        tracked->pcm_track = track;
        tracked->pcm_gain = game_profile_pcm_gain(asset);
        tracked->pcm_stream = stream;
        tracked->pcm_size = stream->expected_bytes;
        tracked->pcm_frames =
            stream->expected_bytes /
            stream->output_bytes_per_frame;
        tracked->pc_frames_consumed = 0;
        tracked->pcm_frames_submitted = 0;
        tracked->pcm_source_frames_submitted = 0;
        tracked->pcm_end_submitted = 0;
        tracked->pcm_signature_count = 0;
        memset(
            tracked->pcm_signatures, 0,
            sizeof(tracked->pcm_signatures));
        if (selection_record &&
            selection_record->selection == selection) {
            tracked->pcm_signature_count =
                selection_record->signature_count;
            if (tracked->pcm_signature_count >
                PCM_SELECTION_SIGNATURE_COUNT) {
                tracked->pcm_signature_count =
                    PCM_SELECTION_SIGNATURE_COUNT;
            }
            memcpy(
                tracked->pcm_signatures,
                selection_record->signatures,
                sizeof(tracked->pcm_signatures[0]) *
                    tracked->pcm_signature_count);
        }
        stream = NULL;
    }
    LeaveCriticalSection(&g_lock);
    if (stream) {
        close_pcm_stream(stream);
        return 0;
    }
    if (old_stream) retire_pcm_stream(old_stream);
    if (previous_target_out) {
        *previous_target_out = previous_target;
    }
    if (new_target_out) *new_target_out = sidecar->voice;
    return 1;
}

static void apply_active_pcm_gain(void *voice, void *target) {
    VoiceVolumeState state;
    SetVolumeFn set_volume;
    if (!voice || !target) return;
    snapshot_voice_volume_state(voice, &state);
    if (!state.pcm_substitute || state.target != target) return;
    set_volume = (SetVolumeFn)(uintptr_t)voice_slot_for_target(
        voice, target, IXAUDIO2_VOICE_SET_VOLUME_SLOT);
    if (set_volume) {
        set_volume(
            target, state.logical_volume * state.pcm_gain, 0);
    }
}

static void flush_reused_native_sidecar_before_submit(
    void *voice, void *previous_target, void *new_target) {
    SimpleSourceVoiceFn flush;
    HRESULT result;
    if (!voice || !new_target ||
        previous_target != new_target ||
        new_target == voice) {
        return;
    }
    flush = (SimpleSourceVoiceFn)(uintptr_t)
        voice_slot_for_target(
            voice, new_target,
            IXAUDIO2_SOURCE_FLUSH_SLOT);
    if (!flush) return;
    result = flush(new_target);
    if (FAILED(result)) {
    }
}

static void switch_started_native_voice_after_submit(
    void *voice, void *previous_target, void *new_target) {
    StartStopFn start = (StartStopFn)(uintptr_t)
        voice_slot_for_target(
            voice, new_target,
            IXAUDIO2_SOURCE_START_SLOT);
    StartStopFn stop = (StartStopFn)(uintptr_t)
        voice_slot_for_target(
            voice, previous_target,
            IXAUDIO2_SOURCE_STOP_SLOT);
    SimpleSourceVoiceFn flush =
        (SimpleSourceVoiceFn)(uintptr_t)
            voice_slot_for_target(
                voice, previous_target,
                IXAUDIO2_SOURCE_FLUSH_SLOT);
    int started = 0;
    int index;
    if (!voice || !new_target ||
        previous_target == new_target) {
        return;
    }
    EnterCriticalSection(&g_lock);
    for (index = 0; index < (int)ARRAYSIZE(g_voices); ++index) {
        if (g_voices[index].voice == voice) {
            started = g_voices[index].delayed_started;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (started && stop && previous_target) {
        stop(previous_target, 0, 0);
    }
    if (previous_target && previous_target != voice && flush) {
        HRESULT result = flush(previous_target);
        if (FAILED(result)) {
        }
    }
    if (started && start) start(new_target, 0, 0);
}

static HRESULT __stdcall hook_create_source_voice(
    void *engine, void **output_voice, const WaveFormatExTrace *format,
    UINT32 flags, float maximum_frequency_ratio, void *callback,
    const void *send_list, const void *effect_chain) {
    HRESULT result;
    WORD format_tag = format ? format->format_tag : 0;
    PcmStream *pcm_stream = NULL;
    WaveFormatExTrace pcm_format;
    /*
     * Reusable voices are routed by the exact first compressed payload in
     * SubmitSourceBuffer.  Never consume a prefetched cue merely because a
     * new voice has a compatible WMA format.
     */
    LONG selected_cue = -1;
    PcmAssetKind selected_asset = selected_cue < 0
        ? PCM_ASSET_NONE
        : (PcmAssetKind)((selected_cue >> 16) & 0xFFFF);
    int selected_track = selected_cue < 0
        ? -1
        : (int)(selected_cue & 0xFFFF);
    int pcm_created = 0;


    if (is_streamable_pc_wma_voice(format, selected_asset) &&
        selected_track >= 0 &&
        selected_track < pcm_asset_track_count(selected_asset)
        ) {
        if (open_pcm_stream(
                selected_asset, selected_track, callback, &pcm_stream)) {
            memset(&pcm_format, 0, sizeof(pcm_format));
            pcm_format.format_tag = 0x0001;
            pcm_format.channels =
                pcm_asset_output_channels(selected_asset);
            pcm_format.samples_per_second =
                pcm_asset_output_sample_rate(selected_asset);
            pcm_format.block_align =
                (WORD)(pcm_format.channels * 2u);
            pcm_format.average_bytes_per_second =
                pcm_format.samples_per_second *
                pcm_format.block_align;
            pcm_format.bits_per_sample = 16;
            result = g_real_create_source_voice(
                engine, output_voice, &pcm_format, flags,
                maximum_frequency_ratio,
                &pcm_stream->callback,
                send_list, effect_chain);
            if (SUCCEEDED(result)) {
                pcm_created = 1;
            } else {
                close_pcm_stream(pcm_stream);
                pcm_stream = NULL;
            }
        }
    }
    if (!pcm_created) {
        result = g_real_create_source_voice(
            engine, output_voice, format, flags, maximum_frequency_ratio,
            callback, send_list, effect_chain);
    }

    if (SUCCEEDED(result) && output_voice && *output_voice && format &&
        (format_tag == 0x0161 || format_tag == 0x0166)) {
        int voice_tracked;
        int voice_vtable_patched;
        if (pcm_created) {
            voice_tracked = track_voice_pcm(
                *output_voice, format, pcm_stream);
            /*
             * XAudio2 now owns a callback pointer inside this stream.
             * Never free it here, even in the practically unreachable
             * tracking-table-full case.
             */
            pcm_stream = NULL;
        } else {
            voice_tracked = track_voice(
                *output_voice, format, engine, flags,
                maximum_frequency_ratio, callback,
                (const XAudio2VoiceSendsTrace *)send_list,
                (const XAudio2EffectChainTrace *)effect_chain);
        }
        voice_vtable_patched = patch_submit_vtable(*output_voice);
        if (voice_tracked && voice_vtable_patched &&
            !pcm_created &&
            is_reusable_pc_wma_voice(format)) {
            int sidecars =
                create_native_pcm_sidecars(*output_voice);
            (void)sidecars;
        }
    }
    if (pcm_stream) close_pcm_stream(pcm_stream);
    return result;
}

static DWORD WINAPI audio_backend_hook_thread(LPVOID parameter) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    const GameProfile *profile = game_profile_active();
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    DWORD attempt;
    (void)parameter;
    if (!base || !profile) return 1;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 1;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage != profile->expected_image_size) {
        return 1;
    }

    for (attempt = 0; attempt < 6000; ++attempt) {
        void *engine = *(void **)(base + profile->xaudio2_global_rva);
        if (engine) {
            void **vtable = *(void ***)engine;
            void **slot;
            DWORD old_protect;
            if (!vtable) break;
            slot = &vtable[IXAUDIO2_CREATE_SOURCE_VOICE_SLOT];
            if (!VirtualProtect(
                    slot, sizeof(*slot), PAGE_EXECUTE_READWRITE,
                    &old_protect)) break;
            g_real_create_source_voice =
                (CreateSourceVoiceFn)(uintptr_t)*slot;
            *slot = (void *)(uintptr_t)hook_create_source_voice;
            VirtualProtect(
                slot, sizeof(*slot), old_protect, &old_protect);
            FlushInstructionCache(
                GetCurrentProcess(), slot, sizeof(*slot));
            InterlockedExchange(&g_audio_hook_installed, 1);
            return 0;
        }
        Sleep(10);
    }
    return 1;
}

static void sibling_path(WCHAR *path, DWORD capacity, const WCHAR *filename) {
    WCHAR *slash;
    DWORD length = GetModuleFileNameW((HMODULE)&__ImageBase, path, capacity);
    if (!length || length >= capacity) {
        path[0] = L'\0';
        return;
    }
    slash = path + length;
    while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
    lstrcpynW(slash, filename, (int)(capacity - (DWORD)(slash - path)));
}

static void track_file(HANDLE handle, const char *path) {
    int index;
    if (handle == INVALID_HANDLE_VALUE || !path) return;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < 128; ++index) {
        if (!g_files[index].handle || g_files[index].handle == handle) {
            g_files[index].handle = handle;
            snprintf(
                g_files[index].path, sizeof(g_files[index].path),
                "%s", path);
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static int tracked_path(HANDLE handle, char *output, size_t output_size) {
    int index;
    int found = 0;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < 128; ++index) {
        if (g_files[index].handle == handle) {
            strncpy(output, g_files[index].path, output_size - 1);
            output[output_size - 1] = '\0';
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

static void untrack_file(HANDLE handle) {
    int index;
    EnterCriticalSection(&g_lock);
    for (index = 0; index < 128; ++index) {
        if (g_files[index].handle == handle) {
            g_files[index].handle = NULL;
            g_files[index].path[0] = '\0';
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static HANDLE WINAPI hook_create_file_a(
    LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE template_file) {
    const WCHAR *redirect_relative = redirected_fixed_bank(path);
    WCHAR redirect_path[MAX_PATH * 2];
    char narrow_redirect[MAX_PATH * 2];
    HANDLE handle = INVALID_HANDLE_VALUE;
    int redirected = 0;
    redirect_path[0] = L'\0';
    narrow_redirect[0] = '\0';
    if (redirect_relative && (access & GENERIC_READ) &&
        disposition == OPEN_EXISTING) {
        sibling_path(
            redirect_path, ARRAYSIZE(redirect_path), redirect_relative);
        if (redirect_path[0]) {
            WideCharToMultiByte(
                CP_ACP, 0, redirect_path, -1, narrow_redirect,
                ARRAYSIZE(narrow_redirect), NULL, NULL);
            if (narrow_redirect[0]) {
                handle = g_real_create_file_a(
                    narrow_redirect, access, share, security, disposition,
                    flags, template_file);
                redirected = handle != INVALID_HANDLE_VALUE;
            }
        }
    }
    if (!redirected) {
        handle = g_real_create_file_a(
            path, access, share, security, disposition, flags,
            template_file);
    }
    if (is_audio_resource(path)) {
        track_file(handle, path);
    }
    return handle;
}

static HANDLE WINAPI hook_create_file_w(
    LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE template_file) {
    char narrow_path[MAX_PATH * 2];
    const WCHAR *redirect_relative;
    WCHAR redirect_path[MAX_PATH * 2];
    HANDLE handle = INVALID_HANDLE_VALUE;
    int redirected = 0;
    narrow_path[0] = '\0';
    redirect_path[0] = L'\0';
    if (path) {
        WideCharToMultiByte(
            CP_UTF8, 0, path, -1, narrow_path, ARRAYSIZE(narrow_path),
            NULL, NULL);
    }
    redirect_relative = redirected_fixed_bank(narrow_path);
    if (redirect_relative && (access & GENERIC_READ) &&
        disposition == OPEN_EXISTING) {
        sibling_path(
            redirect_path, ARRAYSIZE(redirect_path), redirect_relative);
        if (redirect_path[0]) {
            handle = g_real_create_file_w(
                redirect_path, access, share, security, disposition,
                flags, template_file);
            redirected = handle != INVALID_HANDLE_VALUE;
        }
    }
    if (!redirected) {
        handle = g_real_create_file_w(
            path, access, share, security, disposition, flags,
            template_file);
    }
    if (is_audio_resource(narrow_path)) {
        track_file(handle, narrow_path);
    }
    return handle;
}

static BOOL WINAPI hook_read_file(
    HANDLE handle, LPVOID buffer, DWORD requested, LPDWORD actual,
    LPOVERLAPPED overlapped) {
    LARGE_INTEGER zero;
    LARGE_INTEGER before;
    char path[MAX_PATH * 2];
    BOOL tracked = tracked_path(handle, path, sizeof(path));
    BOOL ok;
    DWORD local_actual = 0;
    zero.QuadPart = 0;
    before.QuadPart = -1;
    if (tracked) {
        if (overlapped) {
            before.LowPart = overlapped->Offset;
            before.HighPart = (LONG)overlapped->OffsetHigh;
        } else {
            SetFilePointerEx(handle, zero, &before, FILE_CURRENT);
        }
    }
    ok = g_real_read_file(handle, buffer, requested, actual, overlapped);
    if (actual) local_actual = *actual;
    if (tracked && ok && local_actual &&
        pcm_asset_for_path(path) != PCM_ASSET_NONE) {
        PcmAssetKind selected_asset = PCM_ASSET_NONE;
        int selected_track = update_pcm_selection_from_read_with_data(
            path, before.QuadPart, buffer, local_actual, &selected_asset);
        (void)selected_track;
        (void)selected_asset;
    }
    return ok;
}

static BOOL WINAPI hook_close_handle(HANDLE handle) {
    char path[MAX_PATH * 2];
    int tracked = tracked_path(handle, path, sizeof(path));
    BOOL ok = g_real_close_handle(handle);
    if (tracked) {
        untrack_file(handle);
    }
    return ok;
}

static FARPROC patch_iat(const char *dll_name, const char *function_name, FARPROC replacement) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char *imported_dll = (const char *)(base + descriptor->Name);
        IMAGE_THUNK_DATA32 *names;
        IMAGE_THUNK_DATA32 *slots;
        if (!ascii_iequals(imported_dll, dll_name)) continue;
        names = (IMAGE_THUNK_DATA32 *)(base +
            (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        slots = (IMAGE_THUNK_DATA32 *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            IMAGE_IMPORT_BY_NAME *name;
            DWORD old_protect;
            FARPROC original;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)name->Name, function_name) != 0) continue;
            original = (FARPROC)(uintptr_t)slots->u1.Function;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                    PAGE_READWRITE, &old_protect)) return NULL;
            slots->u1.Function = (DWORD)(uintptr_t)replacement;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), &slots->u1.Function,
                sizeof(slots->u1.Function));
            return original;
        }
    }
    return NULL;
}


static BOOL initialize_proxy(void) {
    const GameProfile *profile;
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) {
        while (g_initialized == 1) Sleep(0);
        return g_initialized == 2;
    }
    profile = game_profile_load((HMODULE)&__ImageBase);
    if (!profile) {
        InterlockedExchange(&g_initialized, 3);
        return FALSE;
    }
    InitializeCriticalSection(&g_lock);
    audio_decoder_set_module((HMODULE)&__ImageBase);
    audio_decoder_set_game_profile(profile->id);


    g_real_create_file_a = (CreateFileAFn)patch_iat(
        "KERNEL32.dll", "CreateFileA", (FARPROC)hook_create_file_a);
    g_real_create_file_w = (CreateFileWFn)patch_iat(
        "KERNEL32.dll", "CreateFileW", (FARPROC)hook_create_file_w);
    g_real_read_file = (ReadFileFn)patch_iat(
        "KERNEL32.dll", "ReadFile", (FARPROC)hook_read_file);
    /*
     * CloseHandle is part of the routing path, not just the diagnostic path:
     * it removes audio-bank handles from g_files. Keep it in performance
     * builds and require the patch below.
     */
    g_real_close_handle = (CloseHandleFn)patch_iat(
        "KERNEL32.dll", "CloseHandle", (FARPROC)hook_close_handle);
    if (!g_real_create_file_a || !g_real_read_file ||
        !g_real_close_handle
        ) goto fail;
    /*
     * Prefetch is an optimization only. If the worker cannot start, the
     * exact-submit router retains its synchronous stream-open path.
     */
    start_pcm_prefetch_worker();
    /*
     * Teardown is also an optimization. If this worker cannot start,
     * retire_pcm_stream keeps the original synchronous reliability path.
     */
    start_pcm_reaper_worker();
    InterlockedExchange(&g_initialized, 2);
    return TRUE;

fail:
    InterlockedExchange(&g_initialized, 3);
    return FALSE;
}


static DWORD WINAPI initialize_thread(LPVOID parameter) {
    (void)parameter;
    if (!initialize_proxy()) return 1;
    return audio_backend_hook_thread(NULL);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        DisableThreadLibraryCalls(instance);
        thread = CreateThread(NULL, 0, initialize_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    if (reason == DLL_PROCESS_DETACH && g_initialized == 2) {
        /* The OS owns thread and handle teardown during process termination. */
        if (reserved) return TRUE;
        stop_pcm_prefetch_worker();
        stop_pcm_reaper_worker();
        cleanup_all_pcm_voices();
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}
