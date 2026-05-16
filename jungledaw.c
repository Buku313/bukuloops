#include <SDL.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define W 640
#define H 480
#define BARS 16
#define LANES 6
#define MAX_SAMPLES 512
#define MAX_PATTERNS 16
#define MAX_CLIPS 192
#define MAX_VOICES 12
#define SAMPLE_RATE 48000
#define PI 3.14159265358979323846f
#define MAX_SEQUENCES 8

typedef enum { PAGE_SONG = 0, PAGE_STEP, PAGE_PERFORM, PAGE_FX, PAGE_COUNT } Page;

typedef enum {
    INST_808 = 0,
    INST_KICK,
    INST_HAT,
    INST_SNARE,
    INST_CLAP,
    INST_LEAD,
    INST_PLUCK,
    INST_BASS,
    INST_BELL,
    INST_ZAP,
    INST_COUNT
} InstKind;

static const char *INST_NAMES[INST_COUNT] = {
    "808","KICK","HAT","SNARE","CLAP","LEAD","PLUCK","BASS","BELL","ZAP"
};
static const char *TRACK_NAMES[LANES]     = {"808","KICK","HAT","SNARE","MEL","FX"};

typedef enum { VK_SAMPLE = 0, VK_SYNTH } VoiceKind;

typedef struct {
    char name[64];
    char folder[64];
    float *data;
    int frames;
} Sample;

#define MAX_PATTERN_STEPS 64
#define MAX_TRACKS 8
#define MAX_BUSES 3
#define DELAY_BUF_SIZE (SAMPLE_RATE * 2)
#define GB_BUF_SIZE (SAMPLE_RATE * 2)
#define COMB_BUF_MAX 1617
#define AP_BUF_MAX 341

#define PATTERN_LAYER_COUNT 3

typedef struct {
    int active;
    int sample_ref;   /* >=0 -> sample, else use inst */
    int inst;
    int note_delta;   /* semitones from pattern base note */
    float gain;       /* 0..1 */
} PatternLayer;

/* Track = one instrument's step sequence within a pattern */
typedef struct {
    char name[32];
    int steps[MAX_PATTERN_STEPS];
    int step_notes[MAX_PATTERN_STEPS];
    int step_prob[MAX_PATTERN_STEPS]; /* 0-100, chance the step fires */
    int inst;
    int note;
    int sample_ref;
    int slices;
    int chop_mode;        /* 0=cut, 1=thru (play to end), 2=loop slice */
    float lfo_rate;
    float lfo_depth;
    float attack_mul;
    float decay_mul;
    int step_rand_mode[MAX_PATTERN_STEPS]; /* per-step: 0=off, 1=up, 2=up+down, 3=down */
    int note_rand_range;  /* scale degrees to randomize (shared per track) */
    int bus;              /* -1=DIRECT, 0=BUS_A, 1=BUS_B */
    PatternLayer layers[PATTERN_LAYER_COUNT];
} Track;

/* Pattern = a full groove containing multiple tracks */
typedef struct {
    char name[64];
    Track tracks[MAX_TRACKS];
    int track_count;
    int length;       /* steps: 16, 32, or 64 */
} Pattern;

/* ===== MIXER BUS ===== */

typedef struct {
    float buf[COMB_BUF_MAX];
    int len, pos;
} CombFilter;

typedef struct {
    float buf[AP_BUF_MAX];
    int len, pos;
} AllpassFilter;

typedef struct {
    CombFilter combs[4];
    AllpassFilter aps[2];
    float damp, lp_state;
} ReverbState;

typedef struct {
    float rec_buf[GB_BUF_SIZE];
    int rec_pos;
    float read_pos;
    int mode; /* 0=off,1=halftime,2=stutter8,3=stutter16,4=reverse,5=gate */
} GBeatState;

typedef struct {
    float volume;
    int mute;

    float drive;
    int drive_on;

    float cutoff;
    float reso;           /* 0..0.95 resonance/Q */
    float lp_state;       /* biquad input z-1 */
    float bp_state;       /* biquad input z-2 */
    float hp_z1;          /* biquad output z-1 */
    float hp_z2;          /* biquad output z-2 */
    int filter_on;

    float crush_rate;     /* 0..1: 1=clean, 0=extreme reduction */
    float crush_bits;     /* 1..16 bit depth */
    float crush_hold;     /* sample-and-hold state */
    float crush_phase;    /* phase accumulator */
    int crush_on;

    float grain_density;  /* 0..1: grains per sample */
    float grain_size;     /* 0..1: grain length (maps to 5-200ms) */
    float grain_pitch;    /* 0..1: pitch scatter (0=original, 1=±2 octaves) */
    float grain_pos;      /* 0..1: position scatter in buffer */
    float grain_buf[GB_BUF_SIZE];
    int grain_wpos;
    float grain_voices[8]; /* 8 grain read positions */
    float grain_ages[8];
    float grain_rates[8];
    int grain_on;

    float delay_buf[DELAY_BUF_SIZE];
    int delay_pos;
    float delay_time;
    float delay_fb;
    float delay_wet;
    int delay_on;

    ReverbState reverb;
    float rev_wet;
    float rev_decay;
    int reverb_on;

    GBeatState gbeat;
    int gbeat_on;
} MixBus;

#define MAX_CHAIN 32

typedef struct {
    int used;
    int type;
    int ref;
    int lane;
    int bar;
    int len;
} Clip;

#define MAX_BEAT_ENTRIES 24
#define MAX_BEATS 8

typedef struct {
    int active;
    char name[32];
    int count;
    int types[MAX_BEAT_ENTRIES];
    int refs[MAX_BEAT_ENTRIES];
    int lanes[MAX_BEAT_ENTRIES];
    int bar_offsets[MAX_BEAT_ENTRIES];
    int lens[MAX_BEAT_ENTRIES];
    int span_bars;
} Beat;

typedef struct {
    char name[32];
    int pat_active[MAX_PATTERNS];
} Sequence;

typedef struct {
    int active;
    int kind;
    int sample_idx;
    int frame;
    int sample_end_frame;   /* exclusive; 0 = play to natural end */
    int sample_start_frame; /* for loop mode */
    int loop;               /* 1 = loop between start and end */
    int inst;
    float t;
    float phase;
    float note_hz;
    float gain;
    float age;
    float lfo_phase;
    float lfo_rate;
    float lfo_depth;
    float attack_mul;
    float decay_mul;
    int cut_group;
    int bus;           /* -1=DIRECT, 0=BUS_A, 1=BUS_B */
} Voice;

typedef struct {
    SDL_AudioDeviceID dev;
    SDL_GameController *controller;
    Sample samples[MAX_SAMPLES];
    Pattern patterns[MAX_PATTERNS];
    Clip clips[MAX_CLIPS];
    Voice voices[MAX_VOICES];
    Beat beats[MAX_BEATS];
    int beat_count;
    int sample_count;
    int pattern_count;
    int selected_type;
    int selected_ref;
    int selected_clip;
    int cursor_lane;
    int cursor_bar;
    int playing;
    double song_time;
    int bpm;
    float level;
    MixBus buses[MAX_BUSES];
    Page page;
    int pad_bank;
    int step_pat;       /* which pattern (groove) is active */
    int step_track;     /* which track within the pattern we're editing */
    int step_col;
    int step_scroll_x;    /* horizontal scroll offset for step grid */
    int pad_flash[4];
    int last_played_step;
    int song_bars;
    int browse_open;
    int browse_idx;
    int browse_panel;     /* 0=folders, 1=items */
    int browse_folder_idx;
    int settings_open;
    int settings_focus;   /* 0=BPM 1=pitch 2=scale 3=root */
    float master_pitch;
    int scale_idx;        /* 0=chromatic 1=major 2=minor 3=pentatonic 4=dorian 5=blues */
    int scale_root;       /* 0=C ... 11=B */
    int piano_open;
    int piano_row;        /* 0..23, pitch row (0 = base - 12, 23 = base + 11) */
    int piano_col;        /* 0..15 */
    int fx_bus;           /* which bus the FX page is editing */
    int fx_row;           /* which row is focused in FX page */
    int fx_mode;          /* 0=mixer view, 1=bus detail */
    int synth_open;       /* synth lab overlay */
    int synth_focus;      /* 0..3 */
    int song_pick_open;   /* SONG picker overlay */
    int song_pick_idx;
    int file_open;        /* song file browser overlay */
    int file_idx;
    int file_action;      /* 0=load, 1=save, 2=new, 3=clone */
    char file_name[64];   /* name being edited */
    int file_name_pos;    /* cursor in name editor */
    int file_name_edit;   /* 1 = editing name */
    int speaker_mute;     /* 0=on, 1=muted (headphones only) */
    int theme;            /* 0=gunmetal, 1=matrix, 2=midnight, 3=ember */
    int was_playing;      /* restore playback when closing menu */
    float swing;          /* 0.5 = none, up to 0.75 (heavy) */
    int recording;        /* MPC loop record mode */
    Sequence sequences[MAX_SEQUENCES];
    int seq_count;
    int cur_seq;          /* currently playing sequence */
    int queued_seq;       /* queued for next bar boundary, -1 = none */
    int perf_cursor;      /* PERFORM page: which sequence is highlighted */
    int queued_pat;       /* pattern queued to play next, -1 = none */
    int pat_mode;         /* 0=manual, 1=chain, 2=random */
    int chain[MAX_CHAIN]; /* pattern chain for performance mode */
    int chain_len;
    int chain_pos;        /* current position in chain during playback */
    int chain_loop;       /* 1 = loop chain, 0 = stop at end */
    int last_played_half; /* for accent retrigger at half-step */
} App;

static App app;
static int select_held = 0;
static int start_held = 0;

static SDL_Color C_BG     = {  0,   6,   2, 255};
static SDL_Color C_PANEL  = {  6,  18,   9, 255};
static SDL_Color C_PANEL2 = { 10,  30,  14, 255};
static SDL_Color C_LINE   = { 30,  80,  40, 255};
static SDL_Color C_INK    = {180, 255, 180, 255};
static SDL_Color C_MUTED  = { 90, 160,  98, 255};
static SDL_Color C_ACID   = {120, 255,  80, 255};
static SDL_Color C_CYAN   = { 95, 232, 255, 255};
static SDL_Color C_PINK   = {255, 100, 120, 255};
static SDL_Color C_PURPLE = {200, 140, 255, 255};
static SDL_Color C_GOLD   = {255, 200,  60, 255};

static SDL_Color C_PAD[4] = {
    {239,  83,  80, 255},
    {255, 213,  79, 255},
    { 95, 232, 255, 255},
    {183, 255,  43, 255}
};
static const char PAD_LETTER[4] = {'A','B','X','Y'};

static SDL_Color INST_COLOR[INST_COUNT] = {
    {255, 215,  64, 255}, /* 808   gold   */
    {255,  93, 159, 255}, /* KICK  pink   */
    { 95, 232, 255, 255}, /* HAT   cyan   */
    {239,  83,  80, 255}, /* SNARE red    */
    {178,  98, 255, 255}, /* CLAP  purple */
    {183, 255,  43, 255}, /* LEAD  acid   */
    {120, 200, 255, 255}, /* PLUCK sky    */
    {180, 120,  60, 255}, /* BASS  bronze */
    {255, 230, 150, 255}, /* BELL  cream  */
    {255,  80,  40, 255}  /* ZAP   orange */
};

static int pattern_total_sources(void);
static void set_track_source(Track *t, int idx);
static void bpm_nudge(int delta);
static SDL_Color pitch_shift_color(SDL_Color base, int delta);
static void place_clip(void);
static void drop_beat_at_cursor(Beat *b);
static void delete_beat_at(int idx);
static void preview_track_step(Track *t, int step_val);
static void fire_track_layers(Track *t, int step_idx, float vel, float pitch_mul);
static const char *layer_label(PatternLayer *L);
static SDL_Color layer_color(PatternLayer *L);
static int layer_source_index(PatternLayer *L);
static void set_layer_source(PatternLayer *L, int idx);
static void cycle_layer_source(int layer_idx, int delta);
static void toggle_layer_active(int layer_idx);
static Track *cur_track(void);
static SDL_Color bus_color(int bi);
extern const char *SESSION_PATH;

static const char *BUS_NAMES[] = {"A","B","C"};
#define THEME_COUNT 4
static const char *THEME_NAMES[] = {"GUNMETAL","MATRIX","MIDNIGHT","EMBER"};

static SDL_Color C_DARK, C_DARKER, C_LIGHT, C_LIGHTER, C_GRID1, C_GRID2, C_BAR4, C_BAR16;

static void apply_theme(int idx) {
    switch (idx) {
        case 0: /* gunmetal */
            C_BG    = (SDL_Color){18,18,20,255};
            C_PANEL = (SDL_Color){28,28,32,255};
            C_PANEL2= (SDL_Color){36,36,40,255};
            C_LINE  = (SDL_Color){60,60,68,255};
            C_INK   = (SDL_Color){200,200,210,255};
            C_MUTED = (SDL_Color){100,100,110,255};
            break;
        case 1: /* matrix */
            C_BG    = (SDL_Color){0,6,2,255};
            C_PANEL = (SDL_Color){6,18,9,255};
            C_PANEL2= (SDL_Color){10,30,14,255};
            C_LINE  = (SDL_Color){30,80,40,255};
            C_INK   = (SDL_Color){180,255,180,255};
            C_MUTED = (SDL_Color){90,160,98,255};
            break;
        case 2: /* midnight */
            C_BG    = (SDL_Color){8,10,22,255};
            C_PANEL = (SDL_Color){14,18,36,255};
            C_PANEL2= (SDL_Color){20,26,48,255};
            C_LINE  = (SDL_Color){40,50,90,255};
            C_INK   = (SDL_Color){180,195,255,255};
            C_MUTED = (SDL_Color){90,100,160,255};
            break;
        case 3: /* ember */
            C_BG    = (SDL_Color){22,14,8,255};
            C_PANEL = (SDL_Color){36,22,12,255};
            C_PANEL2= (SDL_Color){48,30,16,255};
            C_LINE  = (SDL_Color){90,55,30,255};
            C_INK   = (SDL_Color){255,210,170,255};
            C_MUTED = (SDL_Color){160,110,75,255};
            break;
    }
    C_DARKER  = (SDL_Color){(Uint8)(C_BG.r/2),(Uint8)(C_BG.g/2),(Uint8)(C_BG.b/2),255};
    C_DARK    = (SDL_Color){(Uint8)(C_BG.r*2/3+C_PANEL.r/3),(Uint8)(C_BG.g*2/3+C_PANEL.g/3),(Uint8)(C_BG.b*2/3+C_PANEL.b/3),255};
    C_LIGHT   = (SDL_Color){(Uint8)(C_PANEL2.r+15),(Uint8)(C_PANEL2.g+15),(Uint8)(C_PANEL2.b+15),255};
    C_LIGHTER = (SDL_Color){(Uint8)(C_LINE.r+20),(Uint8)(C_LINE.g+20),(Uint8)(C_LINE.b+20),255};
    C_GRID1   = (SDL_Color){(Uint8)(C_BG.r+4),(Uint8)(C_BG.g+4),(Uint8)(C_BG.b+4),255};
    C_GRID2   = (SDL_Color){(Uint8)(C_BG.r+10),(Uint8)(C_BG.g+10),(Uint8)(C_BG.b+10),255};
    C_BAR4    = (SDL_Color){(Uint8)(C_LINE.r),(Uint8)(C_LINE.g),(Uint8)(C_LINE.b),255};
    C_BAR16   = (SDL_Color){(Uint8)(C_LINE.r+30),(Uint8)(C_LINE.g+30),(Uint8)(C_LINE.b+30),255};
}

static SDL_Color TRACK_COLORS[] = {
    {255, 90,140,255}, {255,180, 50,255}, { 80,230,255,255}, {120,255, 80,255},
    {200,130,255,255}, {255,130, 60,255}, {100,255,180,255}, {255,100,200,255}
};
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float noise_signed(void) { return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f; }
static float beat_dur(void) { return 60.0f / (float)app.bpm; }
static float bar_dur(void)  { return beat_dur() * 4.0f; }
static float step_dur(void) { return beat_dur() * 0.25f; }
static float song_dur(void) { return bar_dur() * app.song_bars; }
static float midi_hz(int note) { return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f); }

static const char *SCALE_NAMES[] = {"CHROMATIC","MAJOR","MINOR","PENTATONIC","DORIAN","BLUES"};
#define SCALE_COUNT 6
static const char *NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static int scale_has(int scale_idx, int root, int note) {
    if (scale_idx == 0) return 1;
    static const int sets[][8] = {
        {-1},
        {0,2,4,5,7,9,11,-1},
        {0,2,3,5,7,8,10,-1},
        {0,2,4,7,9,-1,-1,-1},
        {0,2,3,5,7,9,10,-1},
        {0,3,5,6,7,10,-1,-1}
    };
    int pc = ((note - root) % 12 + 12) % 12;
    const int *s = sets[scale_idx];
    for (int i = 0; s[i] >= 0; i++) if (s[i] == pc) return 1;
    return 0;
}

static int snap_note(int note, int direction, int scale_idx, int root) {
    if (scale_idx == 0) return note + direction;
    int n = note;
    for (int tries = 0; tries < 12; tries++) {
        n += direction;
        if (n < 12) return 12;
        if (n > 108) return 108;
        if (scale_has(scale_idx, root, n)) return n;
    }
    return note;
}

static int default_note_for(int inst) {
    switch (inst) {
        case INST_808:   return 28;
        case INST_KICK:  return 36;
        case INST_HAT:   return 42;
        case INST_SNARE: return 38;
        case INST_CLAP:  return 39;
        case INST_LEAD:  return 60;
        default: return 60;
    }
}

/* ===== SAMPLES ===== */

static void add_sample_with_folder(const char *name, const char *folder, float *data, int frames) {
    if (app.sample_count >= MAX_SAMPLES) { free(data); return; }
    Sample *s = &app.samples[app.sample_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->folder, sizeof(s->folder), "%s", folder ? folder : "");
    s->data = data;
    s->frames = frames;
    if (app.selected_type < 0) {
        app.selected_type = 0;
        app.selected_ref = app.sample_count - 1;
    }
}

static void make_break_sample(void) {
    int frames = (int)(SAMPLE_RATE * 60.0f / 170.0f * 4.0f * 2.0f);
    float *data = (float *)calloc((size_t)frames, sizeof(float));
    if (!data) return;
    float sd = 60.0f / 170.0f * 0.25f;
    int hits[] = {0,6,10,14,18,24,29};
    int snares[] = {4,12,20,27};
    int ghosts[] = {7,15,23,30};
    for (int h = 0; h < 7; h++) {
        int start = (int)(hits[h] * sd * SAMPLE_RATE);
        int len = (int)(0.18f * SAMPLE_RATE);
        for (int i = 0; i < len && start + i < frames; i++) {
            float x = (float)i / (float)len;
            float env = (1.0f - x) * (1.0f - x);
            float hz = 95.0f - 55.0f * x;
            data[start + i] += sinf(2.0f * PI * hz * (float)i / SAMPLE_RATE) * env * 0.86f;
        }
    }
    for (int h = 0; h < 4; h++) {
        int start = (int)(snares[h] * sd * SAMPLE_RATE);
        int len = (int)(0.13f * SAMPLE_RATE);
        for (int i = 0; i < len && start + i < frames; i++) {
            float x = (float)i / (float)len;
            float env = (1.0f - x) * (1.0f - x);
            data[start + i] += noise_signed() * env * 0.72f;
        }
    }
    for (int h = 0; h < 4; h++) {
        int start = (int)(ghosts[h] * sd * SAMPLE_RATE);
        int len = (int)(0.08f * SAMPLE_RATE);
        for (int i = 0; i < len && start + i < frames; i++) {
            float x = (float)i / (float)len;
            float env = (1.0f - x) * (1.0f - x);
            data[start + i] += noise_signed() * env * 0.20f;
        }
    }
    for (int st = 0; st < 32; st += 2) {
        int start = (int)(st * sd * SAMPLE_RATE);
        int len = (int)(0.035f * SAMPLE_RATE);
        for (int i = 0; i < len && start + i < frames; i++) {
            float x = (float)i / (float)len;
            float env = (1.0f - x) * (1.0f - x) * (1.0f - x);
            data[start + i] += noise_signed() * env * 0.25f;
        }
    }
    for (int i = 0; i < frames; i++) data[i] = clampf(data[i], -0.95f, 0.95f);
    add_sample_with_folder("amen-ish break", "built-in", data, frames);
}

static int load_wav_sample(const char *path) {
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return 0;
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                          AUDIO_F32SYS, 1, SAMPLE_RATE) < 0) {
        SDL_FreeWAV(buf);
        return 0;
    }
    cvt.len = (int)len;
    size_t alloc = (size_t)cvt.len * (size_t)(cvt.len_mult > 0 ? cvt.len_mult : 1);
    if (alloc == 0 || alloc > 64*1024*1024) { SDL_FreeWAV(buf); return 0; }
    cvt.buf = (Uint8 *)SDL_malloc(alloc);
    if (!cvt.buf) { SDL_FreeWAV(buf); return 0; }
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (SDL_ConvertAudio(&cvt) < 0) { SDL_free(cvt.buf); return 0; }
    int frames = cvt.len_cvt / (int)sizeof(float);
    int cap = SAMPLE_RATE * 6;
    if (frames > cap) frames = cap;
    float *data = (float *)malloc((size_t)frames * sizeof(float));
    if (!data) { SDL_free(cvt.buf); return 0; }
    memcpy(data, cvt.buf, (size_t)frames * sizeof(float));
    SDL_free(cvt.buf);
    const char *slash = strrchr(path, '/');
    char nm[64];
    snprintf(nm, sizeof(nm), "%s", slash ? slash + 1 : path);
    char *dot = strrchr(nm, '.');
    if (dot) *dot = 0;
    char folder[64] = "";
    if (slash && slash > path) {
        const char *prev = slash - 1;
        while (prev > path && *prev != '/') prev--;
        if (*prev == '/') prev++;
        int flen = (int)(slash - prev);
        if (flen > 63) flen = 63;
        memcpy(folder, prev, (size_t)flen);
        folder[flen] = 0;
    }
    add_sample_with_folder(nm, folder, data, frames);
    return 1;
}

/* ===== MIDI ===== */

static int read_varlen(const Uint8 **pp, const Uint8 *end) {
    int v = 0;
    const Uint8 *p = *pp;
    for (int i = 0; i < 4 && p < end; i++) {
        Uint8 b = *p++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

static int load_midi_file(const char *path) {
    if (app.pattern_count >= MAX_PATTERNS) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 14 || sz > 4*1024*1024) { fclose(f); return 0; }
    Uint8 *buf = (Uint8 *)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);

    if (memcmp(buf, "MThd", 4) != 0) { free(buf); return 0; }
    int ntrks = (buf[10] << 8) | buf[11];
    short raw_div = (short)((buf[12] << 8) | buf[13]);
    if (raw_div <= 0) { free(buf); return 0; }
    int division = raw_div;

    Track tmp;
    memset(&tmp, 0, sizeof(tmp));
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char pname[64];
    snprintf(pname, sizeof(pname), "%s", base);
    char *dot = strrchr(pname, '.');
    if (dot) *dot = 0;
    if (strlen(pname) > 10) pname[10] = 0;
    snprintf(tmp.name, sizeof(tmp.name), "%s", pname);
    tmp.inst = INST_LEAD;
    tmp.note = 60;
    tmp.sample_ref = -1;
    tmp.attack_mul = 1.0f;
    tmp.decay_mul = 1.0f;
    for (int i = 0; i < MAX_PATTERN_STEPS; i++) tmp.step_prob[i] = 100;
    for (int i = 0; i < PATTERN_LAYER_COUNT; i++) {
        tmp.layers[i].sample_ref = -1;
        tmp.layers[i].inst = INST_808;
        tmp.layers[i].note_delta = -12;
        tmp.layers[i].gain = 0.65f;
    }

    int first_pitch = -1;
    int kick_hits = 0, snare_hits = 0, hat_hits = 0;
    int has_drum = 0, has_mel = 0;
    int note_count = 0;

    const Uint8 *cur = buf + 14;
    const Uint8 *end = buf + sz;
    int track = 0;
    while (cur + 8 <= end && track < ntrks) {
        if (memcmp(cur, "MTrk", 4) != 0) break;
        int len = (cur[4]<<24)|(cur[5]<<16)|(cur[6]<<8)|cur[7];
        const Uint8 *track_end = cur + 8 + len;
        if (track_end > end) track_end = end;
        const Uint8 *p2 = cur + 8;
        int abs_ticks = 0;
        Uint8 last_status = 0;
        while (p2 < track_end) {
            int delta = read_varlen(&p2, track_end);
            abs_ticks += delta;
            if (p2 >= track_end) break;
            Uint8 status = *p2;
            if (status & 0x80) p2++;
            else status = last_status;
            int hi = status & 0xF0;
            int ch = status & 0x0F;
            if (hi == 0x90) {
                if (p2 + 2 > track_end) break;
                Uint8 note = *p2++;
                Uint8 vel = *p2++;
                last_status = status;
                if (vel > 0) {
                    int total = (abs_ticks * 4) / division;
                    int step = total % 16;
                    if (step >= 0 && step < 16) {
                        if (tmp.steps[step] < (vel > 100 ? 2 : 1))
                            tmp.steps[step] = vel > 100 ? 2 : 1;
                    }
                    if (first_pitch < 0) first_pitch = note;
                    note_count++;
                    if (ch == 9 || (note >= 35 && note <= 50)) {
                        has_drum = 1;
                        if (note == 35 || note == 36) kick_hits++;
                        else if (note == 38 || note == 40) snare_hits++;
                        else if (note == 42 || note == 44 || note == 46) hat_hits++;
                    } else {
                        has_mel = 1;
                    }
                }
            } else if (hi == 0x80) { p2 += 2; last_status = status; }
            else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) { p2 += 2; last_status = status; }
            else if (hi == 0xC0 || hi == 0xD0) { p2 += 1; last_status = status; }
            else if (status == 0xFF) {
                if (p2 < track_end) p2++;
                int mlen = read_varlen(&p2, track_end);
                p2 += mlen;
            } else if (status == 0xF0 || status == 0xF7) {
                int mlen = read_varlen(&p2, track_end);
                p2 += mlen;
            } else break;
        }
        cur = track_end;
        track++;
    }
    free(buf);

    if (note_count == 0) return 0;
    int any = 0;
    for (int i = 0; i < 16; i++) if (tmp.steps[i]) { any = 1; break; }
    if (!any) return 0;

    if (has_drum && !has_mel) {
        if (kick_hits >= snare_hits && kick_hits >= hat_hits) { tmp.inst = INST_KICK; tmp.note = 36; }
        else if (snare_hits >= hat_hits) { tmp.inst = INST_SNARE; tmp.note = 38; }
        else { tmp.inst = INST_HAT; tmp.note = 42; }
    } else if (has_mel) {
        if (first_pitch >= 0 && first_pitch < 36) { tmp.inst = INST_808; tmp.note = first_pitch; }
        else { tmp.inst = INST_LEAD; tmp.note = first_pitch >= 0 ? first_pitch : 60; }
    }

    Pattern *pat = &app.patterns[app.pattern_count++];
    memset(pat, 0, sizeof(*pat));
    snprintf(pat->name, sizeof(pat->name), "%s", pname);
    pat->length = 16;
    pat->track_count = 1;
    pat->tracks[0] = tmp;
    return 1;
}

/* ===== PATTERN/CLIP MGMT ===== */

static void init_track(Track *t, const char *name, int inst, int note, const int *steps16) {
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->inst = inst;
    t->note = note;
    t->sample_ref = -1;
    t->attack_mul = 1.0f;
    t->decay_mul = 1.0f;
    for (int i = 0; i < MAX_PATTERN_STEPS; i++) t->step_prob[i] = 100;
    for (int i = 0; i < PATTERN_LAYER_COUNT; i++) {
        t->layers[i].active = 0;
        t->layers[i].sample_ref = -1;
        t->layers[i].inst = INST_808;
        t->layers[i].note_delta = -12;
        t->layers[i].gain = 0.65f;
    }
    if (steps16) for (int i = 0; i < 16; i++) t->steps[i] = steps16[i];
}

static int add_track_to_pattern(Pattern *pat, const char *name, int inst, int note, const int *steps16) {
    if (pat->track_count >= MAX_TRACKS) return -1;
    int ti = pat->track_count++;
    init_track(&pat->tracks[ti], name, inst, note, steps16);
    return ti;
}

static int add_clip(int type, int ref, int lane, int bar, int len) {
    for (int i = 0; i < MAX_CLIPS; i++) {
        if (app.clips[i].used) continue;
        app.clips[i].used = 1;
        app.clips[i].type = type;
        app.clips[i].ref = ref;
        app.clips[i].lane = lane;
        app.clips[i].bar = bar;
        app.clips[i].len = len;
        app.selected_clip = i;
        return i;
    }
    return -1;
}

/* ===== RECURSIVE SCAN ===== */

static int has_ext_ci(const char *path, const char *ext) {
    size_t lp = strlen(path), le = strlen(ext);
    if (lp < le) return 0;
    return strcasecmp(path + lp - le, ext) == 0;
}

static void scan_dir_recursive(const char *dir, int depth) {
    if (depth > 4) return;
    if (app.sample_count >= MAX_SAMPLES && app.pattern_count >= MAX_PATTERNS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (has_ext_ci(full, ".wav") && app.sample_count < MAX_SAMPLES) {
            load_wav_sample(full);
        } else if ((has_ext_ci(full, ".mid") || has_ext_ci(full, ".midi"))
                   && app.pattern_count < MAX_PATTERNS) {
            load_midi_file(full);
        }
    }
    rewinddir(d);
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir_recursive(full, depth + 1);
    }
    closedir(d);
}

/* ===== VOICES ===== */

static void trigger_voice_full(int kind, int sample_idx, int sample_start, int sample_end,
                               int inst, float note_hz, float gain,
                               float lfo_rate, float lfo_depth,
                               float attack_mul, float decay_mul, int cut_group, int bus) {
    if (cut_group > 0) {
        for (int i = 0; i < MAX_VOICES; i++) {
            if (app.voices[i].active && app.voices[i].cut_group == cut_group)
                app.voices[i].active = 0;
        }
    }
    int slot = -1;
    float oldest = -1.0f;
    int oldest_i = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!app.voices[i].active) { slot = i; break; }
        if (app.voices[i].age > oldest) { oldest = app.voices[i].age; oldest_i = i; }
    }
    if (slot < 0) slot = oldest_i;
    if (slot < 0) return;
    Voice *vv = &app.voices[slot];
    vv->active = 1;
    vv->kind = kind;
    vv->sample_idx = sample_idx;
    vv->frame = sample_start;
    vv->sample_end_frame = sample_end;
    vv->inst = inst;
    vv->t = (kind == VK_SAMPLE) ? (float)sample_start : 0.0f;
    vv->phase = 0.0f;
    vv->note_hz = note_hz;
    vv->gain = gain;
    vv->age = 0.0f;
    vv->lfo_phase = 0.0f;
    vv->lfo_rate = lfo_rate;
    vv->lfo_depth = lfo_depth;
    vv->attack_mul = attack_mul > 0.05f ? attack_mul : 0.05f;
    vv->decay_mul = decay_mul > 0.05f ? decay_mul : 0.05f;
    vv->cut_group = cut_group;
    vv->bus = bus;
    vv->sample_start_frame = sample_start;
    vv->loop = 0;
}

static void trigger_voice_internal(int kind, int sample_idx, int inst, float note_hz, float gain) {
    trigger_voice_full(kind, sample_idx, 0, 0, inst, note_hz, gain, 0.0f, 0.0f, 1.0f, 1.0f, 0, -1);
}

static void trigger_sample_voice(int sample_idx, float gain) {
    if (sample_idx < 0 || sample_idx >= app.sample_count) return;
    if (app.dev) SDL_LockAudioDevice(app.dev);
    trigger_voice_internal(VK_SAMPLE, sample_idx, 0, 0.0f, gain);
    if (app.dev) SDL_UnlockAudioDevice(app.dev);
}

static void preview_track_step(Track *t, int step_val) {
    float vel = step_val == 2 ? 1.0f : 0.72f;
    int pcol = app.step_col < 0 ? 0 : app.step_col;
    if (app.dev) SDL_LockAudioDevice(app.dev);
    if (t->sample_ref >= 0 && t->sample_ref < app.sample_count) {
        Sample *smp = &app.samples[t->sample_ref];
        int s_start = 0, s_end = 0;
        if (t->slices > 0 && smp->frames > 0) {
            int idx = t->step_notes[pcol];
            if (idx < 0) idx = 0;
            if (idx >= t->slices) idx = t->slices - 1;
            int per = smp->frames / t->slices;
            s_start = idx * per;
            s_end = s_start + per;
        }
        int cg = (app.step_pat + 1) * MAX_TRACKS + app.step_track + 1;
        float smp_pitch = (t->slices > 0) ? 1.0f : powf(2.0f, (float)t->step_notes[pcol] / 12.0f);
        trigger_voice_full(VK_SAMPLE, t->sample_ref, s_start, s_end, 0, smp_pitch, vel,
                           t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, cg, t->bus);
    } else {
        int cg = (app.step_pat + 1) * MAX_TRACKS + app.step_track + 1;
        int n = t->note + t->step_notes[pcol];
        trigger_voice_full(VK_SYNTH, -1, 0, 0, t->inst,
                           midi_hz(n) * app.master_pitch, vel,
                           t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, cg, t->bus);
    }
    fire_track_layers(t, pcol, vel, app.master_pitch);
    if (app.dev) SDL_UnlockAudioDevice(app.dev);
}

static Track *cur_track(void) {
    if (app.pattern_count == 0) return NULL;
    Pattern *pat = &app.patterns[app.step_pat];
    if (pat->track_count == 0) return NULL;
    if (app.step_track >= pat->track_count) app.step_track = pat->track_count - 1;
    if (app.step_track < 0) app.step_track = 0;
    return &pat->tracks[app.step_track];
}

static int pad_sample_idx(int pad) {
    if (pad < 0 || pad > 3) return -1;
    int idx = app.pad_bank * 4 + pad;
    if (idx >= app.sample_count) return -1;
    return idx;
}

static void trigger_pad(int pad) {
    int idx = pad_sample_idx(pad);
    if (idx < 0) return;
    trigger_sample_voice(idx, 0.9f);
    app.pad_flash[pad] = 14;
}

/* ===== SYNTH RENDERING ===== */

static float render_synth_voice(Voice *v) {
    float dt = 1.0f / (float)SAMPLE_RATE;
    float x = v->t;
    float val = 0.0f;
    float dm = v->decay_mul > 0.05f ? v->decay_mul : 0.05f;
    float am = v->attack_mul > 0.05f ? v->attack_mul : 0.05f;

    float lfo_mul = 1.0f;
    if (v->lfo_rate > 0 && v->lfo_depth > 0) {
        float lfo_amt = sinf(v->lfo_phase) * v->lfo_depth * 7.0f;
        lfo_mul = powf(2.0f, lfo_amt / 12.0f);
        v->lfo_phase += 2.0f * PI * v->lfo_rate * dt;
    }
    float hz = v->note_hz * lfo_mul;

    switch (v->inst) {
        case INST_808: {
            if (x > 1.4f * dm) { v->active = 0; break; }
            float p_env = expf(-x * 1.6f / dm);
            float h = hz * (0.55f + 0.55f * p_env);
            float amp = expf(-x * 1.6f / dm) * v->gain * 0.9f;
            val = sinf(v->phase) * amp;
            v->phase += 2.0f * PI * h * dt;
        } break;
        case INST_KICK: {
            if (x > 0.32f * dm) { v->active = 0; break; }
            float p_env = expf(-x * 32.0f);
            float h = hz * (0.5f + 1.5f * p_env);
            float amp = expf(-x * 8.0f / dm) * v->gain * 0.8f;
            val = sinf(v->phase) * amp;
            v->phase += 2.0f * PI * h * dt;
        } break;
        case INST_HAT: {
            if (x > 0.09f * dm) { v->active = 0; break; }
            float env = expf(-x * 55.0f / dm) * v->gain * 0.45f;
            float n = noise_signed();
            float n2 = noise_signed() * 0.6f;
            val = (n - n2) * env;
        } break;
        case INST_SNARE: {
            if (x > 0.26f * dm) { v->active = 0; break; }
            float env = expf(-x * 9.0f / dm);
            float tone = sinf(2.0f * PI * hz * x) * 0.35f;
            float n = noise_signed();
            val = (n * 0.72f + tone) * env * v->gain * 0.6f;
        } break;
        case INST_CLAP: {
            if (x > 0.24f * dm) { v->active = 0; break; }
            float burst_t[] = {0.000f, 0.018f, 0.034f, 0.050f};
            float bursts = 0.0f;
            for (int i = 0; i < 4; i++) {
                float bt = burst_t[i];
                if (x >= bt && x < bt + 0.014f) {
                    float be = 1.0f - (x - bt) / 0.014f;
                    bursts += noise_signed() * be;
                }
            }
            float tail = noise_signed() * expf(-x * 7.0f / dm) * 0.32f;
            val = (bursts * 0.55f + tail) * v->gain * 0.55f;
        } break;
        case INST_LEAD: {
            if (x > 0.55f * dm) { v->active = 0; break; }
            float att = 0.005f * am;
            float att_env = x < att ? x / att : 1.0f;
            float env = att_env * expf(-x * 3.2f / dm);
            float ph2 = v->phase * 1.005f;
            float saw1 = fmodf(v->phase, 2.0f * PI) / PI - 1.0f;
            float saw2 = fmodf(ph2, 2.0f * PI) / PI - 1.0f;
            val = (saw1 + saw2) * 0.5f * env * v->gain * 0.32f;
            v->phase += 2.0f * PI * hz * dt;
        } break;
        case INST_PLUCK: {
            if (x > 0.6f * dm) { v->active = 0; break; }
            float env = expf(-x * 6.5f / dm);
            float ph_f = fmodf(v->phase / (2.0f * PI), 1.0f);
            float tri = 1.0f - 4.0f * fabsf(ph_f - 0.5f) + 1.0f;
            tri = clampf(tri, -1.0f, 1.0f);
            val = tri * env * v->gain * 0.55f;
            v->phase += 2.0f * PI * hz * dt;
        } break;
        case INST_BASS: {
            if (x > 1.2f * dm) { v->active = 0; break; }
            float att = 0.02f * am;
            float att_env = x < att ? x / att : 1.0f;
            float env = att_env * expf(-x * 1.4f / dm);
            float s = sinf(v->phase);
            float sq = s > 0 ? 0.7f : -0.7f;
            float mix = sq * 0.65f + s * 0.45f;
            val = mix * env * v->gain * 0.55f;
            v->phase += 2.0f * PI * hz * dt;
        } break;
        case INST_BELL: {
            if (x > 1.8f * dm) { v->active = 0; break; }
            float mod_env = expf(-x * 4.5f);
            float mod_idx = 3.5f * mod_env;
            float mod = sinf(v->phase * 1.41f) * mod_idx;
            float env = expf(-x * 1.0f / dm);
            val = sinf(v->phase + mod) * env * v->gain * 0.5f;
            v->phase += 2.0f * PI * hz * dt;
        } break;
        case INST_ZAP: {
            if (x > 0.35f * dm) { v->active = 0; break; }
            float p_env = expf(-x * 7.0f);
            float h = hz * (0.25f + 3.0f * p_env);
            float env = expf(-x * 9.0f / dm);
            val = sinf(v->phase) * env * v->gain * 0.65f;
            float n = noise_signed() * env * 0.15f;
            val += n;
            v->phase += 2.0f * PI * h * dt;
        } break;
        default: break;
    }
    return val;
}

static float soft_clip(float x) {
    const float a = 0.72f;
    const float k = 1.0f - a;
    if (x >  a) return  a + k * tanhf((x - a) / k);
    if (x < -a) return -a + k * tanhf((x + a) / k);
    return x;
}

/* ===== NOTE RANDOMIZER ===== */

static int randomize_note_in_scale(int base_note, int mode, int range, int scale_idx, int root) {
    if (mode == 0 || range == 0) return base_note;
    int candidates[24];
    int count = 0;
    if (mode == 1 || mode == 2) {
        int found = 0;
        for (int d = 1; d <= 36 && found < range; d++) {
            int n = base_note + d;
            if (n > 108) break;
            if (scale_idx == 0 || scale_has(scale_idx, root, n)) {
                candidates[count++] = n;
                found++;
                if (count >= 24) break;
            }
        }
    }
    if (mode == 2 || mode == 3) {
        int found = 0;
        for (int d = 1; d <= 36 && found < range; d++) {
            int n = base_note - d;
            if (n < 12) break;
            if (scale_idx == 0 || scale_has(scale_idx, root, n)) {
                candidates[count++] = n;
                found++;
                if (count >= 24) break;
            }
        }
    }
    if (count == 0) return base_note;
    return candidates[rand() % count];
}

/* ===== MIXER DSP ===== */

static void reverb_init(ReverbState *rv) {
    memset(rv, 0, sizeof(*rv));
    int comb_lens[] = {1557, 1617, 1491, 1422};
    for (int i = 0; i < 4; i++) rv->combs[i].len = comb_lens[i];
    int ap_lens[] = {225, 341};
    for (int i = 0; i < 2; i++) rv->aps[i].len = ap_lens[i];
    rv->damp = 0.4f;
}

static float reverb_process(ReverbState *rv, float in, float decay) {
    float fb = 0.7f + decay * 0.2f;
    float out = 0.0f;
    for (int i = 0; i < 4; i++) {
        CombFilter *c = &rv->combs[i];
        float rd = c->buf[c->pos];
        if (fabsf(rd) < 1e-10f) rd = 0;
        rv->lp_state = rd * (1.0f - rv->damp) + rv->lp_state * rv->damp;
        if (fabsf(rv->lp_state) < 1e-15f) rv->lp_state = 0;
        c->buf[c->pos] = in + rv->lp_state * fb;
        if (++c->pos >= c->len) c->pos = 0;
        out += rd;
    }
    out *= 0.25f;
    for (int i = 0; i < 2; i++) {
        AllpassFilter *a = &rv->aps[i];
        float rd = a->buf[a->pos];
        a->buf[a->pos] = out + rd * 0.5f;
        out = rd - out * 0.5f;
        if (++a->pos >= a->len) a->pos = 0;
    }
    return out;
}

static float gbeat_process(GBeatState *gb, float in, float step_dur_samps) {
    gb->rec_buf[gb->rec_pos] = in;
    int wp = gb->rec_pos;
    gb->rec_pos = (gb->rec_pos + 1) % GB_BUF_SIZE;
    if (gb->mode == 0) return in;
    float out = in;
    switch (gb->mode) {
        case 1: { /* halftime */
            int rp = (int)gb->read_pos;
            if (rp < 0) rp += GB_BUF_SIZE;
            rp = rp % GB_BUF_SIZE;
            out = gb->rec_buf[rp];
            gb->read_pos += 0.5f;
            if (gb->read_pos >= (float)GB_BUF_SIZE) gb->read_pos -= (float)GB_BUF_SIZE;
        } break;
        case 2: { /* stutter 1/8 */
            float loop_len = step_dur_samps * 2.0f;
            if (loop_len < 1) loop_len = 1;
            int rp = wp - (int)loop_len + ((int)gb->read_pos % (int)loop_len);
            if (rp < 0) rp += GB_BUF_SIZE;
            out = gb->rec_buf[rp % GB_BUF_SIZE];
            gb->read_pos += 1.0f;
            if (gb->read_pos >= loop_len) gb->read_pos = 0;
        } break;
        case 3: { /* stutter 1/16 */
            float loop_len = step_dur_samps;
            if (loop_len < 1) loop_len = 1;
            int rp = wp - (int)loop_len + ((int)gb->read_pos % (int)loop_len);
            if (rp < 0) rp += GB_BUF_SIZE;
            out = gb->rec_buf[rp % GB_BUF_SIZE];
            gb->read_pos += 1.0f;
            if (gb->read_pos >= loop_len) gb->read_pos = 0;
        } break;
        case 4: { /* reverse */
            int rp = (int)gb->read_pos;
            if (rp < 0) rp += GB_BUF_SIZE;
            out = gb->rec_buf[rp % GB_BUF_SIZE];
            gb->read_pos -= 1.0f;
            if (gb->read_pos < 0) gb->read_pos += (float)GB_BUF_SIZE;
        } break;
        case 5: { /* gate */
            int period = (int)(step_dur_samps * 0.5f);
            if (period < 1) period = 1;
            int phase = gb->rec_pos % (period * 2);
            out = (phase < period) ? in : 0.0f;
        } break;
        default: break;
    }
    return out;
}

static unsigned g_audio_rng = 48271;
static float audio_randf(void) {
    g_audio_rng = g_audio_rng * 1103515245u + 12345u;
    return (float)((g_audio_rng >> 8) & 0xFFFF) / 65536.0f;
}

static float process_bus(MixBus *bus, float in, float step_dur_samps) {
    if (bus->mute) return 0.0f;
    if (fabsf(in) < 1e-8f && !bus->delay_on && !bus->reverb_on && !bus->grain_on && !bus->gbeat_on)
        return 0.0f;
    float v = in * bus->volume;
    if (bus->drive_on && bus->drive > 0.01f)
        v = tanhf(v * (1.0f + bus->drive * 8.0f));
    if (bus->filter_on) {
        float fc = clampf(bus->cutoff, 300.0f, 8000.0f);
        float w = tanf(PI * fc / (float)SAMPLE_RATE);
        float q = 0.5f + bus->reso * 4.5f;
        float n = 1.0f / (w * w + w / q + 1.0f);
        float a0 = w * w * n;
        float a1 = 2.0f * a0;
        float b1 = 2.0f * (w * w - 1.0f) * n;
        float b2 = (w * w - w / q + 1.0f) * n;
        float out = a0 * v + a1 * bus->lp_state + a0 * bus->bp_state
                  - b1 * bus->hp_z1 - b2 * bus->hp_z2;
        bus->bp_state = bus->lp_state;
        bus->lp_state = v;
        bus->hp_z2 = bus->hp_z1;
        bus->hp_z1 = out;
        if (fabsf(out) > 10.0f) out = 0;
        if (fabsf(bus->hp_z1) < 1e-15f) bus->hp_z1 = 0;
        if (fabsf(bus->hp_z2) < 1e-15f) bus->hp_z2 = 0;
        v = out;
    }
    if (bus->crush_on) {
        float rate = 0.05f + bus->crush_rate * 0.95f;
        bus->crush_phase += rate;
        if (bus->crush_phase >= 1.0f) {
            bus->crush_phase -= 1.0f;
            float bits = 1.0f + bus->crush_bits * 15.0f;
            float steps = powf(2.0f, bits);
            bus->crush_hold = floorf(v * steps + 0.5f) / steps;
        }
        v = bus->crush_hold;
    }
    if (bus->delay_on) {
        int dly = (int)(bus->delay_time * SAMPLE_RATE);
        if (dly < 1) dly = 1;
        if (dly >= DELAY_BUF_SIZE) dly = DELAY_BUF_SIZE - 1;
        int rd = bus->delay_pos - dly;
        if (rd < 0) rd += DELAY_BUF_SIZE;
        float delayed = bus->delay_buf[rd];
        if (fabsf(delayed) < 1e-10f) delayed = 0;
        bus->delay_buf[bus->delay_pos] = v + delayed * bus->delay_fb;
        if (++bus->delay_pos >= DELAY_BUF_SIZE) bus->delay_pos = 0;
        v = v + delayed * bus->delay_wet;
    }
    if (bus->reverb_on && bus->rev_wet > 0.01f) {
        float rw = bus->rev_wet > 0.85f ? 0.85f : bus->rev_wet;
        float rv = reverb_process(&bus->reverb, v, bus->rev_decay);
        v = v * (1.0f - rw) + rv * rw;
    }
    if (bus->gbeat_on && bus->gbeat.mode > 0) {
        v = gbeat_process(&bus->gbeat, v, step_dur_samps);
    }
    if (bus->grain_on) {
        bus->grain_buf[bus->grain_wpos] = v;
        bus->grain_wpos = (bus->grain_wpos + 1) % GB_BUF_SIZE;
        float spawn_chance = bus->grain_density * 0.02f;
        for (int gi = 0; gi < 8; gi++) {
            if (bus->grain_ages[gi] <= 0 && audio_randf() < spawn_chance) {
                float grain_ms = 5.0f + bus->grain_size * 195.0f;
                bus->grain_ages[gi] = grain_ms * (float)SAMPLE_RATE / 1000.0f;
                float scatter = bus->grain_pos * (float)GB_BUF_SIZE * 0.5f;
                float offset = audio_randf() * scatter;
                bus->grain_voices[gi] = (float)(bus->grain_wpos - (int)offset);
                if (bus->grain_voices[gi] < 0) bus->grain_voices[gi] += (float)GB_BUF_SIZE;
                float pitch_scatter = bus->grain_pitch * 2.0f;
                bus->grain_rates[gi] = powf(2.0f, (audio_randf() - 0.5f) * pitch_scatter);
            }
        }
        float grain_len = (5.0f + bus->grain_size * 195.0f) * (float)SAMPLE_RATE / 1000.0f;
        if (grain_len < 1.0f) grain_len = 1.0f;
        float grain_sum = 0;
        int active_grains = 0;
        for (int gi = 0; gi < 8; gi++) {
            if (bus->grain_ages[gi] <= 0) continue;
            int rp = (int)bus->grain_voices[gi] % GB_BUF_SIZE;
            if (rp < 0) rp += GB_BUF_SIZE;
            float env = sinf(PI * (1.0f - bus->grain_ages[gi] / grain_len));
            if (env < 0) env = 0;
            grain_sum += bus->grain_buf[rp] * env * 0.3f;
            bus->grain_voices[gi] += bus->grain_rates[gi];
            if (bus->grain_voices[gi] >= (float)GB_BUF_SIZE) bus->grain_voices[gi] -= (float)GB_BUF_SIZE;
            bus->grain_ages[gi] -= 1.0f;
            active_grains++;
        }
        if (active_grains > 0) v = v * 0.4f + grain_sum;
    }
    return clampf(v, -4.0f, 4.0f);
}

/* ===== AUDIO HELPERS ===== */

static void fire_track_step(App *a, Track *t, int step_idx, int ti, int pat_idx, float pitch) {
    int s = t->steps[step_idx];
    int prob = t->step_prob[step_idx];
    if (prob < 100 && s > 0) {
        if ((rand() % 100) >= prob) s = 0;
    }
    if (s <= 0) return;
    float vel = 0.72f;
    if (t->sample_ref >= 0 && t->sample_ref < a->sample_count) {
        Sample *smp = &a->samples[t->sample_ref];
        int s_start = 0, s_end = 0;
        if (t->slices > 0 && smp->frames > 0) {
            int idx = t->step_notes[step_idx];
            if (idx < 0) idx = 0;
            if (idx >= t->slices) idx = t->slices - 1;
            int per = smp->frames / t->slices;
            s_start = idx * per;
            if (t->chop_mode == 1) s_end = 0;
            else s_end = s_start + per;
        }
        int cg = (pat_idx + 1) * MAX_TRACKS + ti + 1;
        float sp = (t->slices > 0) ? powf(2.0f, (float)(t->note - 60) / 12.0f)
                                    : powf(2.0f, (float)t->step_notes[step_idx] / 12.0f);
        trigger_voice_full(VK_SAMPLE, t->sample_ref, s_start, s_end,
            0, sp, vel, t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, cg, t->bus);
        if (t->chop_mode == 2 && t->slices > 0 && s_end > s_start) {
            for (int vi = 0; vi < MAX_VOICES; vi++) {
                if (app.voices[vi].active && app.voices[vi].cut_group == cg) {
                    app.voices[vi].loop = 1;
                    break;
                }
            }
        }
    } else {
        int cg = (pat_idx + 1) * MAX_TRACKS + ti + 1;
        int n_note = t->note + t->step_notes[step_idx];
        if (t->step_rand_mode[step_idx] > 0 && t->note_rand_range > 0)
            n_note = randomize_note_in_scale(n_note, t->step_rand_mode[step_idx], t->note_rand_range, a->scale_idx, a->scale_root);
        trigger_voice_full(VK_SYNTH, -1, 0, 0, t->inst,
            midi_hz(n_note) * pitch, vel,
            t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, cg, t->bus);
    }
    fire_track_layers(t, step_idx, vel, pitch);
}

/* ===== AUDIO CALLBACK ===== */

static void audio_cb(void *ud, Uint8 *stream, int len) {
    App *a = (App *)ud;
    float *out = (float *)stream;
    int n = len / (int)sizeof(float);
    float maxv = 0.0f;
    float inv_sr = 1.0f / (float)SAMPLE_RATE;
    float pitch = a->master_pitch;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f) pitch = 4.0f;
    float dt_eff = inv_sr * pitch;
    for (int i = 0; i < n; i++) {
        float v = 0.0f;
        if (a->playing) {
            float t = (float)fmod(a->song_time, song_dur());
            int now_step = (int)(t / step_dur());
            if ((now_step & 1) == 1 && a->swing > 0.5f) {
                float step_in = fmodf(t, step_dur());
                float swing_off = (a->swing - 0.5f) * 2.0f * step_dur();
                if (step_in < swing_off) now_step -= 1;
            }
            if (now_step != a->last_played_step) {
                a->last_played_step = now_step;
                int pattern_mode = (a->page == PAGE_STEP || a->page == PAGE_PERFORM || a->page == PAGE_FX);
                /* Pattern switch at bar boundary */
                if (a->pattern_count > 0 && (now_step % 16) == 0) {
                    if (a->queued_pat >= 0 && a->queued_pat < a->pattern_count) {
                        a->step_pat = a->queued_pat;
                        a->queued_pat = -1;
                    } else if (a->pat_mode == 1 && a->chain_len > 0) {
                        a->chain_pos++;
                        if (a->chain_pos >= a->chain_len) {
                            a->chain_pos = a->chain_loop ? 0 : a->chain_len - 1;
                        }
                        int cp = a->chain[a->chain_pos];
                        if (cp >= 0 && cp < a->pattern_count) a->step_pat = cp;
                    } else if (a->pat_mode == 1) {
                        a->step_pat = (a->step_pat + 1) % a->pattern_count;
                    } else if (a->pat_mode == 2) {
                        a->step_pat = rand() % a->pattern_count;
                    }
                }
                if (pattern_mode) {
                    /* PO-33 style: ONE pattern plays at a time, all tracks sound. */
                    if (a->pattern_count > 0 && a->step_pat >= 0 && a->step_pat < a->pattern_count) {
                        Pattern *pat = &a->patterns[a->step_pat];
                        int plen = pat->length > 0 ? pat->length : 16;
                        int step_idx = ((now_step % plen) + plen) % plen;
                        for (int ti = 0; ti < pat->track_count; ti++) {
                            Track *trk = &pat->tracks[ti];
                            if (trk->steps[step_idx] > 0)
                                fire_track_step(a, trk, step_idx, ti, a->step_pat, pitch);
                        }
                    }
                } else {
                    for (int c = 0; c < MAX_CLIPS; c++) {
                        Clip *cl = &a->clips[c];
                        if (!cl->used || cl->type != 1) continue;
                        if (cl->ref < 0 || cl->ref >= a->pattern_count) continue;
                        float start = cl->bar * bar_dur();
                        float end = start + cl->len * bar_dur();
                        if (t < start || t >= end) continue;
                        Pattern *cpat = &a->patterns[cl->ref];
                        int len = cpat->length > 0 ? cpat->length : 16;
                        int step_offset = now_step - cl->bar * 16;
                        if (step_offset < 0) continue;
                        int step_idx = step_offset % len;
                        for (int ti = 0; ti < cpat->track_count; ti++) {
                            Track *ct = &cpat->tracks[ti];
                            int s = ct->steps[step_idx];
                            if (s > 0) {
                                float vel = (s == 2) ? 1.0f : 0.72f;
                                if (ct->sample_ref >= 0 && ct->sample_ref < a->sample_count) {
                                    Sample *smp = &a->samples[ct->sample_ref];
                                    int s_start = 0, s_end = 0;
                                    if (ct->slices > 0 && smp->frames > 0) {
                                        int idx = ct->step_notes[step_idx];
                                        if (idx < 0) idx = 0;
                                        if (idx >= ct->slices) idx = ct->slices - 1;
                                        int per = smp->frames / ct->slices;
                                        s_start = idx * per;
                                        s_end = s_start + per;
                                    }
                                    int cg = (cl->ref + 1) * MAX_TRACKS + ti + 1;
                                    float sp = (ct->slices > 0) ? 1.0f : powf(2.0f, (float)ct->step_notes[step_idx] / 12.0f);
                                    trigger_voice_full(VK_SAMPLE, ct->sample_ref, s_start, s_end,
                                        0, sp, vel, ct->lfo_rate, ct->lfo_depth, ct->attack_mul, ct->decay_mul, cg, ct->bus);
                                } else {
                                    int cg = (cl->ref + 1) * MAX_TRACKS + ti + 1;
                                    int n_note = ct->note + ct->step_notes[step_idx];
                                    if (ct->step_rand_mode[step_idx] > 0 && ct->note_rand_range > 0)
                                        n_note = randomize_note_in_scale(n_note, ct->step_rand_mode[step_idx], ct->note_rand_range, a->scale_idx, a->scale_root);
                                    trigger_voice_full(VK_SYNTH, -1, 0, 0, ct->inst,
                                        midi_hz(n_note) * pitch, vel,
                                        ct->lfo_rate, ct->lfo_depth, ct->attack_mul, ct->decay_mul, cg, ct->bus);
                                }
                                fire_track_layers(ct, step_idx, vel, pitch);
                            }
                        }
                    }
                }
            }
            /* Half-step retrigger for accent (s==2) = roll */
            {
                int now_half = (int)(t / (step_dur() * 0.5f));
                if (now_half != a->last_played_half) {
                    a->last_played_half = now_half;
                    if ((now_half & 1) && a->pattern_count > 0 && a->step_pat >= 0 && a->step_pat < a->pattern_count) {
                        int pattern_mode = (a->page == PAGE_STEP || a->page == PAGE_PERFORM || a->page == PAGE_FX);
                        if (pattern_mode) {
                            Pattern *hp = &a->patterns[a->step_pat];
                            int hlen = hp->length > 0 ? hp->length : 16;
                            int hstep = (((now_half / 2) % hlen) + hlen) % hlen;
                            for (int hti = 0; hti < hp->track_count; hti++) {
                                if (hp->tracks[hti].steps[hstep] == 2)
                                    fire_track_step(a, &hp->tracks[hti], hstep, hti, a->step_pat, pitch);
                            }
                        }
                    }
                }
            }
            for (int c = 0; c < MAX_CLIPS; c++) {
                Clip *cl = &a->clips[c];
                if (!cl->used || cl->type != 0) continue;
                if (cl->ref < 0 || cl->ref >= a->sample_count) continue;
                float start = cl->bar * bar_dur();
                float end = start + cl->len * bar_dur();
                if (t < start || t >= end) continue;
                Sample *s = &a->samples[cl->ref];
                if (s->frames <= 0) continue;
                float clip_dur = cl->len * bar_dur();
                if (clip_dur <= 0) continue;
                float local_t = (float)(t - start);
                if (local_t < 0 || local_t >= clip_dur) continue;
                float rate = (float)s->frames / (clip_dur * (float)SAMPLE_RATE);
                int local_frame = (int)(local_t * SAMPLE_RATE * rate);
                if (local_frame >= 0 && local_frame < s->frames)
                    v += s->data[local_frame] * 0.72f;
            }
            a->song_time += dt_eff;
        }
        float bus_sum[MAX_BUSES] = {0};
        float direct_sum = 0.0f;
        for (int vi = 0; vi < MAX_VOICES; vi++) {
            Voice *vv = &a->voices[vi];
            if (!vv->active) continue;
            float vs = 0.0f;
            if (vv->kind == VK_SAMPLE) {
                if (vv->sample_idx < 0 || vv->sample_idx >= a->sample_count) { vv->active = 0; continue; }
                Sample *s = &a->samples[vv->sample_idx];
                int end = vv->sample_end_frame > 0 ? vv->sample_end_frame : s->frames;
                if (end > s->frames) end = s->frames;
                int f = vv->frame;
                if (f >= end || f < 0) {
                    if (vv->loop && end > vv->sample_start_frame) {
                        vv->t = (float)vv->sample_start_frame;
                        vv->frame = vv->sample_start_frame;
                        f = vv->frame;
                    } else { vv->active = 0; continue; }
                }
                vs = s->data[f] * vv->gain;
                float srate = pitch * (vv->note_hz > 0.01f ? vv->note_hz : 1.0f);
                vv->t += srate;
                vv->frame = (int)vv->t;
            } else {
                vs = render_synth_voice(vv);
                vv->t += dt_eff;
            }
            vv->age += inv_sr;
            if (vv->bus >= 0 && vv->bus < MAX_BUSES) bus_sum[vv->bus] += vs;
            else direct_sum += vs;
        }
        float sd = step_dur() * (float)SAMPLE_RATE;
        float y = v + direct_sum;
        for (int bi = 0; bi < MAX_BUSES; bi++)
            y += process_bus(&a->buses[bi], bus_sum[bi], sd);
        out[i] = soft_clip(y * 0.85f);
        if (fabsf(out[i]) > maxv) maxv = fabsf(out[i]);
    }
    a->level = a->level * 0.85f + maxv * 0.15f;
}

/* ===== GRAPHICS PRIMITIVES ===== */

static void rect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rr = {x,y,w,h};
    SDL_RenderFillRect(r, &rr);
}
static void rect_outline(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rr = {x,y,w,h};
    SDL_RenderDrawRect(r, &rr);
}
static void line(SDL_Renderer *r, int x1, int y1, int x2, int y2, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(r, x1, y1, x2, y2);
}

static const char *glyph5x7(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    switch (ch) {
        case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
        case 'C': return "01111" "10000" "10000" "10000" "10000" "10000" "01111";
        case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
        case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
        case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
        case 'G': return "01111" "10000" "10000" "10011" "10001" "10001" "01110";
        case 'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'I': return "11111" "00100" "00100" "00100" "00100" "00100" "11111";
        case 'J': return "00111" "00010" "00010" "00010" "10010" "10010" "01100";
        case 'K': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
        case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
        case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
        case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
        case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
        case 'Q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
        case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
        case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
        case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
        case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'V': return "10001" "10001" "10001" "10001" "10001" "01010" "00100";
        case 'W': return "10001" "10001" "10001" "10101" "10101" "10101" "01010";
        case 'X': return "10001" "10001" "01010" "00100" "01010" "10001" "10001";
        case 'Y': return "10001" "10001" "01010" "00100" "00100" "00100" "00100";
        case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
        case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
        case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
        case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
        case '3': return "11110" "00001" "00001" "01110" "00001" "00001" "11110";
        case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
        case '5': return "11111" "10000" "10000" "11110" "00001" "00001" "11110";
        case '6': return "01110" "10000" "10000" "11110" "10001" "10001" "01110";
        case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
        case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
        case '9': return "01110" "10001" "10001" "01111" "00001" "00001" "01110";
        case '/': return "00001" "00010" "00010" "00100" "01000" "01000" "10000";
        case '-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
        case '+': return "00000" "00100" "00100" "11111" "00100" "00100" "00000";
        case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
        case ',': return "00000" "00000" "00000" "00000" "01100" "01100" "01000";
        case ':': return "00000" "01100" "01100" "00000" "01100" "01100" "00000";
        case '>': return "10000" "01000" "00100" "00010" "00100" "01000" "10000";
        case '<': return "00001" "00010" "00100" "01000" "00100" "00010" "00001";
        case '(': return "00010" "00100" "01000" "01000" "01000" "00100" "00010";
        case ')': return "01000" "00100" "00010" "00010" "00010" "00100" "01000";
        case '[': return "01110" "01000" "01000" "01000" "01000" "01000" "01110";
        case ']': return "01110" "00010" "00010" "00010" "00010" "00010" "01110";
        case '!': return "00100" "00100" "00100" "00100" "00100" "00000" "00100";
        case '?': return "01110" "10001" "00001" "00010" "00100" "00000" "00100";
        case '*': return "00000" "01010" "00100" "01110" "00100" "01010" "00000";
        case '=': return "00000" "00000" "11111" "00000" "11111" "00000" "00000";
        case '#': return "01010" "11111" "01010" "01010" "01010" "11111" "01010";
        case '_': return "00000" "00000" "00000" "00000" "00000" "00000" "11111";
        default:  return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *s, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch == ' ') { x += 6; continue; }
        const char *bits = glyph5x7(ch);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (bits[row * 5 + col] == '1') rect(r, x + col, y + row, 1, 1, c);
            }
        }
        x += 6;
    }
}
static void draw_text_2x(SDL_Renderer *r, int x, int y, const char *s, SDL_Color c) {
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch == ' ') { x += 12; continue; }
        const char *bits = glyph5x7(ch);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (bits[row * 5 + col] == '1') rect(r, x + col*2, y + row*2, 2, 2, c);
            }
        }
        x += 12;
    }
}
static const char *graffiti_glyph(char ch) {
    switch (ch) {
        case 'B': return
            ".######.."
            ".#######."
            ".##....#."
            ".##....#."
            ".##....#."
            ".#######."
            ".##....#."
            ".##....#."
            ".##....#."
            ".#######."
            ".######..";
        case 'U': return
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".########"
            "..######.";
        case 'K': return
            ".##...##."
            ".##..##.."
            ".##.##..."
            ".####...."
            ".###....."
            ".####...."
            ".##.##..."
            ".##..##.."
            ".##...##."
            ".##....#."
            ".##....#.";
        case 'L': return
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".##......"
            ".########"
            ".########";
        case 'O': return
            "..#####.."
            ".#######."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".##...##."
            ".#######."
            "..#####..";
        case 'P': return
            ".######.."
            ".#######."
            ".##....#."
            ".##....#."
            ".##....#."
            ".#######."
            ".######.."
            ".##......"
            ".##......"
            ".##......"
            ".##......";
        case 'S': return
            "..######."
            ".########"
            ".##...##."
            ".##......"
            ".##......"
            "..######."
            "......##."
            "......##."
            ".##...##."
            ".########"
            ".######..";
        default: return NULL;
    }
}

static void draw_graffiti(SDL_Renderer *r, int x, int y, const char *s, int scale,
                          SDL_Color fill, SDL_Color outline, SDL_Color shadow) {
    int gw = 9, gh = 11;
    int letter_w = gw * scale;
    int gap = scale;
    for (int pass = 0; pass < 3; pass++) {
        int xx = x;
        for (int i = 0; s[i]; i++) {
            const char *g = graffiti_glyph(s[i]);
            if (g) {
                for (int row = 0; row < gh; row++) {
                    int slant = ((gh - row - 1) * scale) / 3;
                    for (int col = 0; col < gw; col++) {
                        if (g[row * gw + col] != '#') continue;
                        int px = xx + col * scale + slant;
                        int py = y + row * scale;
                        if (pass == 0) {
                            rect(r, px + scale * 2, py + scale * 2, scale, scale, shadow);
                        } else if (pass == 1) {
                            rect(r, px - 1, py - 1, scale + 2, scale + 2, outline);
                        } else {
                            rect(r, px, py, scale, scale, fill);
                        }
                    }
                }
            }
            xx += letter_w + gap;
        }
    }
}

static void draw_text_3x(SDL_Renderer *r, int x, int y, const char *s, SDL_Color c) {
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch == ' ') { x += 18; continue; }
        const char *bits = glyph5x7(ch);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (bits[row * 5 + col] == '1') rect(r, x + col*3, y + row*3, 3, 3, c);
            }
        }
        x += 18;
    }
}

static void draw_wave(SDL_Renderer *r, Sample *s, int x, int y, int w, int h, SDL_Color c) {
    if (!s || s->frames <= 0) return;
    for (int px = 0; px < w; px++) {
        int a = px * s->frames / w;
        int b = (px + 1) * s->frames / w;
        if (b <= a) b = a + 1;
        float mn = 1.0f, mx = -1.0f;
        int step = (b - a) / 32; if (step < 1) step = 1;
        for (int i = a; i < b; i += step) {
            float v = s->data[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int y1 = y + h / 2 - (int)(mx * h * 0.42f);
        int y2 = y + h / 2 - (int)(mn * h * 0.42f);
        line(r, x + px, y1, x + px, y2, c);
    }
}

static void draw_track_thumb(SDL_Renderer *r, Track *t, int x, int y, int w, int h, SDL_Color c) {
    if (!t) return;
    int sw = w / 16;
    if (sw < 2) sw = 2;
    for (int i = 0; i < 16; i++) {
        if (!t->steps[i]) continue;
        int bh = t->steps[i] == 2 ? h - 4 : h / 2;
        rect(r, x + i * sw + 1, y + h - bh - 2, sw > 3 ? sw - 2 : 2, bh, c);
    }
}

/* ===== HEADER + FOOTER ===== */

static void draw_header(SDL_Renderer *r) {
    rect(r, 0, 0, W, 40, C_PANEL2);
    line(r, 0, 40, W, 40, C_LINE);
    SDL_Color graf_fill    = {140, 255, 110, 255};
    SDL_Color graf_outline = {  0,  60,  20, 255};
    SDL_Color graf_shadow  = {  0,  20,   6, 255};
    draw_graffiti(r, 6, 4, "BUKU", 2, graf_fill, graf_outline, graf_shadow);

    /* Voice dots — mini meter inspired by minimeters */
    int vdot_x = 96;
    int vdot_y = 26;
    Uint32 tick = SDL_GetTicks();
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *vv = &app.voices[i];
        if (vv->active) {
            SDL_Color vc = (vv->kind == VK_SYNTH) ? INST_COLOR[vv->inst] : C_ACID;
            float fade = 1.0f - clampf(vv->age * 3.0f, 0.0f, 0.7f);
            vc.r = (Uint8)(vc.r * fade); vc.g = (Uint8)(vc.g * fade); vc.b = (Uint8)(vc.b * fade);
            rect(r, vdot_x, vdot_y, 4, 4, vc);
        } else {
            rect(r, vdot_x, vdot_y, 4, 4, (SDL_Color){20,16,28,255});
        }
        vdot_x += 6;
    }

    /* Micro VU — vertical bar, pulses with the beat */
    float lv = clampf(app.level * 2.5f, 0, 1);
    int vu_x = vdot_x + 6;
    int vu_h = 16;
    int vu_y = 18;
    rect(r, vu_x, vu_y, 3, vu_h, (SDL_Color){12,10,18,255});
    int fill_h = (int)(vu_h * lv);
    SDL_Color vu_c = lv > 0.85f ? C_PINK : (lv > 0.5f ? C_GOLD : C_ACID);
    if (fill_h > 0) rect(r, vu_x, vu_y + vu_h - fill_h, 3, fill_h, vu_c);

    /* Tabs */
    const int tab_pages[] = {PAGE_STEP, PAGE_PERFORM, PAGE_FX};
    const char *tab_names[] = {"STEP","PERF","FX"};
    int tab_count = 3;
    int tabw = 56;
    int gap = 4;
    int total = tab_count * tabw + (tab_count - 1) * gap;
    int x0 = (W - total) / 2;
    for (int i = 0; i < tab_count; i++) {
        int x = x0 + i * (tabw + gap);
        int active = (app.page == tab_pages[i]);
        SDL_Color bgc = active ? C_GOLD : C_LINE;
        SDL_Color fgc = active ? C_BG : C_INK;
        rect(r, x, 6, tabw, 26, bgc);
        if (active) rect(r, x, 33, tabw, 3, C_GOLD);
        int tx = x + (tabw - (int)strlen(tab_names[i]) * 6) / 2;
        draw_text(r, tx, 16, tab_names[i], fgc);
    }

    /* BPM + pitch */
    char tmp[32];
    if (app.master_pitch < 0.99f || app.master_pitch > 1.01f) {
        snprintf(tmp, sizeof(tmp), "BPM %d  %.2fx", app.bpm, app.master_pitch);
        draw_text(r, W - 240, 8, tmp, C_PINK);
    } else {
        snprintf(tmp, sizeof(tmp), "BPM %d", app.bpm);
        draw_text(r, W - 196, 8, tmp, C_MUTED);
    }

    /* Master level bar */
    rect(r, W - 196, 22, 100, 8, C_DARKER);
    int safe_w = (int)(98.0f * (lv < 0.85f ? lv : 0.85f));
    int hot_w  = lv > 0.85f ? (int)(98.0f * (lv - 0.85f)) : 0;
    rect(r, W - 195, 23, safe_w, 6, C_ACID);
    if (hot_w > 0) rect(r, W - 195 + safe_w, 23, hot_w, 6, C_GOLD);
    if (lv >= 0.98f) draw_text(r, W - 230, 24, "CLIP", C_PINK);

    /* Transport */
    int stopped = (!app.playing && app.song_time < 0.0001);
    SDL_Color tcol = app.playing ? C_ACID
                   : stopped     ? C_LINE
                                 : C_GOLD;
    rect(r, W - 90, 8, 80, 24, tcol);
    SDL_Color tfg = app.playing ? C_BG : (stopped ? C_INK : C_BG);
    const char *tlabel = app.playing ? "> PLAY" : (stopped ? "[] STOP" : "|| PAUS");
    draw_text(r, W - 78, 16, tlabel, tfg);
    if (app.recording) {
        int blink = ((tick / 320) & 1);
        if (blink) {
            rect(r, W - 100, 8, 8, 8, C_PINK);
            draw_text(r, W - 102, 22, "REC", C_PINK);
        } else {
            rect_outline(r, W - 100, 8, 8, 8, C_PINK);
            draw_text(r, W - 102, 22, "REC", C_PINK);
        }
    }
    if (app.swing > 0.51f) {
        char st[16];
        snprintf(st, sizeof(st), "SW%d", (int)((app.swing - 0.5f) * 400));
        draw_text(r, W - 232, 32, st, C_PURPLE);
    }
}

static void draw_footer(SDL_Renderer *r, const char *line1, const char *line2) {
    rect(r, 0, H - 28, W, 28, C_PANEL2);
    line(r, 0, H - 28, W, H - 28, C_LINE);
    if (line1) draw_text(r, 8, H - 24, line1, C_INK);
    if (line2) draw_text(r, 8, H - 12, line2, C_MUTED);
    draw_text(r, W - 90, H - 18, "SEL+ST EXIT", C_MUTED);
}

/* ===== SONG PAGE ===== */

static int cursor_clip(void) {
    for (int i = 0; i < MAX_CLIPS; i++) {
        Clip *c = &app.clips[i];
        if (!c->used) continue;
        if (c->lane == app.cursor_lane &&
            app.cursor_bar >= c->bar &&
            app.cursor_bar < c->bar + c->len) return i;
    }
    return -1;
}

static void update_selected_from_cursor(void) {
    int i = cursor_clip();
    if (i >= 0) {
        app.selected_clip = i;
        app.selected_type = app.clips[i].type;
        app.selected_ref = app.clips[i].ref;
    }
}

static void draw_song(SDL_Renderer *r) {
    int rack_x = 0, rack_y = 42, rack_w = 122, rack_h = H - 28 - 42;
    int grid_x = rack_w, grid_y = 42, grid_w = W - rack_w - 122;
    int side_x = grid_x + grid_w, side_y = 42;
    int grid_h = H - 28 - 42;

    rect(r, rack_x, rack_y, rack_w, rack_h, C_PANEL);
    rect(r, side_x, side_y, W - side_x, H - 28 - side_y, C_PANEL);
    rect(r, grid_x, grid_y, grid_w, grid_h, C_BG);

    draw_text(r, 6, rack_y + 6, "SAMPLES", C_MUTED);
    int rack_top = rack_y + 18;
    int row_h = 16;
    int max_rows = 9;
    int sstart = 0;
    if (app.selected_type == 0 && app.selected_ref >= max_rows)
        sstart = app.selected_ref - max_rows + 1;
    if (sstart > app.sample_count - max_rows) sstart = app.sample_count - max_rows;
    if (sstart < 0) sstart = 0;
    for (int row = 0; row < max_rows && sstart + row < app.sample_count; row++) {
        int i = sstart + row;
        int y = rack_top + row * row_h;
        int active = (app.selected_type == 0 && app.selected_ref == i);
        rect(r, 4, y, rack_w - 8, row_h - 2, active ? C_GOLD : C_LIGHT);
        char nm[14];
        snprintf(nm, sizeof(nm), "%-12.12s", app.samples[i].name);
        draw_text(r, 8, y + 4, nm, active ? C_BG : C_INK);
    }
    char cnt[20];
    snprintf(cnt, sizeof(cnt), "%d / %d", app.sample_count, MAX_SAMPLES);
    draw_text(r, 6, rack_top + max_rows * row_h + 2, cnt, C_MUTED);

    int pat_top = rack_top + max_rows * row_h + 18;
    draw_text(r, 6, pat_top - 12, "PATTERNS", C_MUTED);
    int max_p = 7;
    for (int p = 0; p < app.pattern_count && p < max_p; p++) {
        int y = pat_top + p * row_h;
        int active = (app.selected_type == 1 && app.selected_ref == p);
        SDL_Color icol = app.patterns[p].track_count > 0 ? INST_COLOR[app.patterns[p].tracks[0].inst] : C_MUTED;
        rect(r, 4, y, rack_w - 8, row_h - 2, active ? icol : C_LIGHT);
        char nm[14];
        snprintf(nm, sizeof(nm), "%-12.12s", app.patterns[p].name);
        draw_text(r, 8, y + 4, nm, active ? C_BG : C_INK);
    }

    int labelw = 48;
    int cellw = (grid_w - labelw - 4) / BARS;
    int cellh = (grid_h - 22) / LANES;
    for (int b = 0; b < BARS; b++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", b + 1);
        int tx = grid_x + labelw + b * cellw + cellw/2 - (b >= 9 ? 5 : 2);
        draw_text(r, tx, grid_y + 6, tmp, b % 4 == 0 ? C_GOLD : C_MUTED);
    }
    for (int l = 0; l < LANES; l++) {
        int y = grid_y + 22 + l * cellh;
        rect(r, grid_x + 2, y, labelw - 4, cellh - 2, (SDL_Color){26,22,34,255});
        draw_text(r, grid_x + 6, y + cellh/2 - 4, TRACK_NAMES[l], C_MUTED);
        for (int b = 0; b < BARS; b++) {
            int x = grid_x + labelw + b * cellw;
            rect(r, x, y, cellw - 1, cellh - 1, C_BG);
            if ((b & 3) == 0) line(r, x, y, x, y + cellh - 1, (SDL_Color){42,38,56,255});
            if (app.cursor_lane == l && app.cursor_bar == b) {
                rect_outline(r, x, y, cellw - 1, cellh - 1, C_CYAN);
                rect_outline(r, x+1, y+1, cellw - 3, cellh - 3, C_CYAN);
            }
        }
    }
    float nowbar = (float)fmod(app.song_time, song_dur()) / bar_dur();
    int phx = grid_x + labelw + (int)(nowbar * cellw);
    rect(r, phx, grid_y + 20, 2, grid_h - 24, C_ACID);

    for (int i = 0; i < MAX_CLIPS; i++) {
        Clip *c = &app.clips[i];
        if (!c->used) continue;
        int x = grid_x + labelw + c->bar * cellw + 2;
        int y = grid_y + 24 + c->lane * cellh;
        int w = c->len * cellw - 4;
        int h = cellh - 6;
        SDL_Color cc;
        if (c->type == 0) cc = (SDL_Color){127,176,34,255};
        else if (c->ref >= 0 && c->ref < app.pattern_count && app.patterns[c->ref].track_count > 0) cc = INST_COLOR[app.patterns[c->ref].tracks[0].inst];
        else cc = C_PINK;
        rect(r, x, y, w, h, cc);
        if (app.selected_clip == i) rect_outline(r, x-1, y-1, w+2, h+2, C_CYAN);
        if (nowbar >= c->bar && nowbar < c->bar + c->len) rect(r, x, y, w, 3, C_ACID);
        if (c->type == 0 && c->ref < app.sample_count)
            draw_wave(r, &app.samples[c->ref], x + 3, y + 11, w - 6, h - 14, C_BG);
        else if (c->type == 1 && c->ref < app.pattern_count && app.patterns[c->ref].track_count > 0)
            draw_track_thumb(r, &app.patterns[c->ref].tracks[0], x + 3, y + 11, w - 6, h - 14, C_BG);
        char nm[14];
        const char *n = c->type == 0 ? app.samples[c->ref].name : app.patterns[c->ref].name;
        snprintf(nm, sizeof(nm), "%-10.10s", n);
        draw_text(r, x + 3, y + 3, nm, C_BG);
    }

    int sx = side_x + 6;
    int sy = side_y + 6;
    draw_text(r, sx, sy, "CURSOR", C_MUTED);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s  BAR %d", TRACK_NAMES[app.cursor_lane], app.cursor_bar + 1);
    draw_text(r, sx, sy + 14, tmp, C_INK);

    int cc_i = cursor_clip();
    draw_text(r, sx, sy + 38, "ON CURSOR", C_MUTED);
    if (cc_i >= 0) {
        Clip *c = &app.clips[cc_i];
        const char *n = c->type == 0 ? app.samples[c->ref].name : app.patterns[c->ref].name;
        snprintf(tmp, sizeof(tmp), "%-12.12s", n);
        draw_text(r, sx, sy + 52, tmp, c->type == 0 ? (SDL_Color){127,176,34,255}
                                                    : (app.patterns[c->ref].track_count > 0 ? INST_COLOR[app.patterns[c->ref].tracks[0].inst] : C_MUTED));
        snprintf(tmp, sizeof(tmp), "%d BAR LEN", c->len);
        draw_text(r, sx, sy + 66, tmp, C_INK);
        draw_text(r, sx, sy + 82, "B = DELETE", C_PINK);
        draw_text(r, sx, sy + 96, "X/Y RESIZE", C_MUTED);
    } else {
        draw_text(r, sx, sy + 52, "(empty cell)", C_MUTED);
        draw_text(r, sx, sy + 82, "A = PLACE", C_ACID);
    }

    draw_text(r, sx, sy + 124, "PLACING", C_MUTED);
    const char *pname = "(none)";
    SDL_Color pc = C_MUTED;
    if (app.selected_type == 0 && app.selected_ref < app.sample_count) {
        pname = app.samples[app.selected_ref].name;
        pc = C_GOLD;
    } else if (app.selected_type == 1 && app.selected_ref < app.pattern_count) {
        pname = app.patterns[app.selected_ref].name;
        pc = app.patterns[app.selected_ref].track_count > 0 ? INST_COLOR[app.patterns[app.selected_ref].tracks[0].inst] : C_MUTED;
    }
    snprintf(tmp, sizeof(tmp), "%-12.12s", pname); draw_text(r, sx, sy + 138, tmp, pc);
    if (app.selected_type == 1 && app.selected_ref < app.pattern_count) {
        snprintf(tmp, sizeof(tmp), "TRK %d", app.patterns[app.selected_ref].track_count);
        draw_text(r, sx, sy + 152, tmp, C_INK);
    }

    draw_text(r, sx, sy + 184, "BUSES", C_MUTED);
    for (int bi = 0; bi < MAX_BUSES; bi++) {
        MixBus *mb = &app.buses[bi];
        snprintf(tmp, sizeof(tmp), "%s V%d%% %s%s%s", BUS_NAMES[bi],
                 (int)(mb->volume * 100),
                 mb->delay_on ? "DLY " : "", mb->reverb_on ? "REV " : "",
                 mb->gbeat_on ? "GB" : "");
        draw_text(r, sx, sy + 198 + bi * 14, tmp, bi == 0 ? C_PINK : C_PURPLE);
    }

    draw_footer(r,
        "A PLACE   B DEL   X RESIZE-   Y PICKER   SEL+Y RESIZE+",
        "L2/R2 CYCLE   SEL+A DUP   SEL+B CLR LANE   START+Y SAVE BEAT");
}

/* ===== STEP PAGE ===== */

static const char *track_source_label(Track *t) {
    if (t->sample_ref >= 0 && t->sample_ref < app.sample_count)
        return app.samples[t->sample_ref].name;
    return INST_NAMES[t->inst];
}

static SDL_Color track_source_color(Track *t) {
    if (t->sample_ref >= 0) return C_GOLD;
    return INST_COLOR[t->inst];
}

static void draw_step(SDL_Renderer *r) {
    rect(r, 0, 42, W, H - 28 - 42, C_BG);

    if (app.pattern_count == 0) {
        draw_text_2x(r, 160, 200, "NO PATTERNS YET", C_MUTED);
        draw_text(r, 160, 240, "GO TO PERF AND PRESS Y", C_INK);
        draw_footer(r, "L1/R1 PAGE", NULL);
        return;
    }

    Pattern *pat = &app.patterns[app.step_pat];
    int plen = pat->length > 0 ? pat->length : 16;
    if (app.step_col >= plen) app.step_col = plen - 1;
    if (app.step_col < -1) app.step_col = -1;
    if (app.step_track >= pat->track_count) app.step_track = pat->track_count - 1;
    if (app.step_track < 0) app.step_track = 0;

    if (pat->track_count == 0) {
        draw_text_2x(r, 160, 200, "NO TRACKS YET", C_MUTED);
        draw_text(r, 160, 240, "PRESS B TO ADD A TRACK", C_INK);
        draw_footer(r, "B +TRACK   L1/R1 PAGE", NULL);
        return;
    }

    int label_w = 110;
    int margin_x = 6;
    int grid_x = margin_x + label_w;
    int grid_w = W - margin_x - grid_x;

    int now_step = -1;
    if (app.playing) now_step = (int)(fmod(app.song_time, song_dur()) / step_dur());

    int vis_cells = 16;
    if (plen <= 16) vis_cells = plen;
    int cellw = grid_w / vis_cells;
    /* Camera: snap to keep cursor/playhead visible */
    int focus_col = app.step_col;
    if (focus_col < 0 && app.playing && now_step >= 0)
        focus_col = ((now_step % plen) + plen) % plen;
    if (focus_col >= 0) {
        if (focus_col < app.step_scroll_x + 2)
            app.step_scroll_x = focus_col - 2;
        if (focus_col >= app.step_scroll_x + vis_cells - 2)
            app.step_scroll_x = focus_col - vis_cells + 3;
    }
    if (app.step_scroll_x < 0) app.step_scroll_x = 0;
    if (plen > vis_cells && app.step_scroll_x > plen - vis_cells)
        app.step_scroll_x = plen - vis_cells;
    if (app.step_scroll_x < 0) app.step_scroll_x = 0;
    int grid_right = grid_x + cellw * vis_cells;

    int body_top = 46;
    int body_bot = H - 28;
    int body_h = body_bot - body_top;
    int max_rows = 8;
    int row_h = body_h / max_rows;
    if (row_h > 44) row_h = 44;

    int tc = pat->track_count;
    int visible = tc < max_rows ? tc : max_rows;
    int scroll = 0;
    if (app.step_track >= visible) scroll = app.step_track - visible + 1;
    if (scroll > tc - visible) scroll = tc - visible;
    if (scroll < 0) scroll = 0;

    for (int vi2 = 0; vi2 < vis_cells; vi2++) {
        int b = vi2 + app.step_scroll_x;
        if (b >= plen) break;
        int x = grid_x + vi2 * cellw;
        if (b % 4 == 0) rect(r, x, body_top, 2, body_h, C_BAR4);
        if (b % 16 == 0) rect(r, x, body_top, 3, body_h, C_BAR16);
        if (cellw >= 22) {
            char num[4];
            snprintf(num, sizeof(num), "%d", b + 1);
            draw_text(r, x + cellw/2 - (b >= 9 ? 5 : 2), body_bot - 9, num,
                      b % 4 == 0 ? C_GOLD : C_MUTED);
        }
    }
    if (plen > vis_cells) {
        int sb_w = (vis_cells * (grid_right - grid_x)) / plen;
        int sb_x = grid_x + (app.step_scroll_x * (grid_right - grid_x)) / plen;
        rect(r, grid_x, body_bot - 3, grid_right - grid_x, 2, C_DARK);
        rect(r, sb_x, body_bot - 3, sb_w > 4 ? sb_w : 4, 2, C_CYAN);
    }

    for (int row = 0; row < visible; row++) {
        int t_idx = row + scroll;
        if (t_idx >= tc) break;
        Track *t = &pat->tracks[t_idx];
        int y = body_top + row * row_h;
        int active = (t_idx == app.step_track);

        int label_sel = (active && app.step_col < 0);
        SDL_Color label_bg = label_sel ? C_LIGHT
                           : active   ? C_PANEL2
                                      : C_PANEL;
        rect(r, margin_x, y + 2, label_w - 4, row_h - 4, label_bg);

        /* Track color strip on left edge */
        SDL_Color tc_edge = TRACK_COLORS[t_idx % 8];
        rect(r, margin_x, y + 2, 3, row_h - 4, tc_edge);
        if (t->bus >= 0 && t->bus < MAX_BUSES) {
            rect(r, margin_x + 3, y + 2, 2, row_h - 4, bus_color(t->bus));
        }
        if (label_sel) rect_outline(r, margin_x, y + 2, label_w - 4, row_h - 4, C_CYAN);

        SDL_Color src_c = track_source_color(t);
        const char *src_lbl = track_source_label(t);
        rect(r, margin_x + 6, y + 4, 8, 8, src_c);
        char tag[16];
        if (t->sample_ref >= 0 && t->slices > 0)
            snprintf(tag, sizeof(tag), "ch%d", t->slices);
        else
            snprintf(tag, sizeof(tag), "%s",
                     t->sample_ref >= 0 ? "smp" : INST_NAMES[t->inst]);
        draw_text(r, margin_x + 18, y + 4, tag, src_c);

        /* Bus badge */
        if (t->bus >= 0 && t->bus < MAX_BUSES) {
            SDL_Color bbc = bus_color(t->bus);
            rect(r, margin_x + label_w - 22, y + 4, 16, 9, bbc);
            draw_text(r, margin_x + label_w - 19, y + 5, BUS_NAMES[t->bus], C_BG);
        } else {
            char mods[12] = "";
            int mi = 0;
            if (t->lfo_rate > 0.05f && t->lfo_depth > 0.01f) mods[mi++] = '~';
            int lcount = 0;
            for (int li = 0; li < PATTERN_LAYER_COUNT; li++) if (t->layers[li].active) lcount++;
            if (lcount > 0) { mods[mi++] = '+'; mods[mi++] = '0' + lcount; }
            mods[mi] = 0;
            if (mi > 0) draw_text(r, margin_x + label_w - 4 - mi*6, y + 4, mods, C_PURPLE);
        }
        char nm[14];
        snprintf(nm, sizeof(nm), "%-12.12s", src_lbl);
        draw_text(r, margin_x + 6, y + row_h/2 + 2, nm, C_INK);

        int now_in_p = -1;
        if (now_step >= 0) {
            now_in_p = ((now_step % plen) + plen) % plen;
        }

        SDL_Color tc2 = TRACK_COLORS[t_idx % 8];
        for (int vi3 = 0; vi3 < vis_cells; vi3++) {
            int b = vi3 + app.step_scroll_x;
            if (b >= plen) break;
            int x = grid_x + vi3 * cellw;
            int beat = b / 4;
            int cx = x + 1, cy = y + 2, cw = cellw - 2, ch = row_h - 6;
            /* Recessed empty cell */
            SDL_Color slot_bg = (beat & 1) ? C_GRID1 : C_GRID2;
            rect(r, cx, cy, cw, ch, slot_bg);
            rect(r, cx, cy, cw, 1, C_DARKER);
            rect(r, cx, cy, 1, ch, C_DARKER);
            rect(r, cx, cy + ch - 1, cw, 1, C_DARK);
            rect(r, cx + cw - 1, cy, 1, ch, C_DARK);
            /* Playhead glow */
            if (now_in_p == b) {
                rect(r, cx + 1, cy + 1, cw - 2, ch - 2, (SDL_Color){(Uint8)(C_BG.r+10),(Uint8)(C_BG.g+25),(Uint8)(C_BG.b+6),255});
                rect(r, cx, cy, cw, 2, C_ACID);
            }
            int s = t->steps[b];
            if (s > 0) {
                int prob = t->step_prob[b];
                SDL_Color sc = tc2;
                if (s == 2) { sc.r = (Uint8)(sc.r/2 + 127); sc.g = (Uint8)(sc.g/2 + 127); sc.b = (Uint8)(sc.b/2 + 127); }
                sc = pitch_shift_color(sc, t->step_notes[b]);
                if (prob < 100) {
                    float fp = (float)prob / 100.0f;
                    sc.r = (Uint8)(sc.r * fp); sc.g = (Uint8)(sc.g * fp); sc.b = (Uint8)(sc.b * fp);
                }
                int m = 3;
                int bx = cx+m, by = cy+m, bw = cw-m*2, bh = ch-m*2;
                if (bw < 2) bw = 2; if (bh < 2) bh = 2;
                /* Raised 3D button */
                SDL_Color hi = {(Uint8)clampf(sc.r*1.5f,0,255), (Uint8)clampf(sc.g*1.5f,0,255), (Uint8)clampf(sc.b*1.5f,0,255), 255};
                SDL_Color md = sc;
                SDL_Color sh = {(Uint8)(sc.r/3), (Uint8)(sc.g/3), (Uint8)(sc.b/3), 255};
                SDL_Color dk = {(Uint8)(sc.r/5), (Uint8)(sc.g/5), (Uint8)(sc.b/5), 255};
                /* Shadow behind */
                rect(r, bx+2, by+2, bw, bh, dk);
                /* Main face */
                rect(r, bx, by, bw, bh, md);
                /* Top highlight */
                rect(r, bx, by, bw, 3, hi);
                rect(r, bx, by, 3, bh, hi);
                /* Bottom shadow */
                rect(r, bx, by+bh-3, bw, 3, sh);
                rect(r, bx+bw-3, by, 3, bh, sh);
                /* Inner bevel */
                rect(r, bx+3, by+3, bw-6, bh-6, md);
                /* Accent marker */
                if (s == 2) {
                    rect(r, bx+bw/2-1, by+3, 2, bh-6, hi);
                    rect(r, bx+3, by+bh/2-1, bw-6, 2, hi);
                }
                if (prob < 100 && active) {
                    char pct[5]; snprintf(pct, sizeof(pct), "%d", prob);
                    draw_text(r, bx+1, by+bh-9, pct, C_PINK);
                }
                if (t->step_rand_mode[b] > 0) {
                    SDL_Color rmc = {180,100,255,255};
                    rect(r, bx, by, 3, t->step_rand_mode[b]==2 ? bh : bh/2, rmc);
                    if (t->step_rand_mode[b]==3) rect(r, bx, by+bh/2, 3, bh/2, rmc);
                }
            }
            if (active && app.step_col == b) {
                rect_outline(r, cx-1, cy-1, cw+2, ch+2, C_CYAN);
                rect_outline(r, cx, cy, cw, ch, (SDL_Color){60,200,230,128});
            }
        }
    }

    if (tc > visible) {
        int sb_x = grid_right + 1;
        int sb_h = (visible * body_h) / tc;
        int sb_y = body_top + (scroll * body_h) / tc;
        rect(r, sb_x, body_top, 2, body_h, C_DARK);
        rect(r, sb_x, sb_y, 2, sb_h, C_CYAN);
    }

    /* Chop waveform visualizer + info bar */
    {
        Track *st = &pat->tracks[app.step_track];
        int col = app.step_col;
        char info[128];

        if (col >= 0 && st->slices > 0 && st->sample_ref >= 0 && st->sample_ref < app.sample_count) {
            Sample *smp = &app.samples[st->sample_ref];
            int wv_y = H - 68;
            int wv_h = 24;
            int wv_x = 8;
            int wv_w = W - 16;
            rect(r, wv_x - 2, wv_y - 2, wv_w + 4, wv_h + 4, (SDL_Color){6,4,10,255});

            int slice_idx = st->step_notes[col];
            if (slice_idx < 0) slice_idx = 0;
            if (slice_idx >= st->slices) slice_idx = st->slices - 1;
            int slice_w = wv_w / st->slices;

            for (int si = 0; si < st->slices; si++) {
                int sx = wv_x + si * slice_w;
                int sw2 = slice_w - 1;
                int is_active = (si == slice_idx);
                SDL_Color bg = is_active ? (SDL_Color){40,20,50,255} : (SDL_Color){12,10,18,255};
                rect(r, sx, wv_y, sw2, wv_h, bg);

                int smp_start = si * smp->frames / st->slices;
                int smp_end = (si + 1) * smp->frames / st->slices;
                SDL_Color wc = is_active ? C_CYAN : (SDL_Color){50,45,65,255};
                for (int px = 0; px < sw2 && px < slice_w; px++) {
                    int a = smp_start + px * (smp_end - smp_start) / sw2;
                    int b2 = smp_start + (px + 1) * (smp_end - smp_start) / sw2;
                    if (b2 <= a) b2 = a + 1;
                    float mn = 1, mx = -1;
                    int step2 = (b2 - a) / 8; if (step2 < 1) step2 = 1;
                    for (int j = a; j < b2 && j < smp->frames; j += step2) {
                        if (smp->data[j] < mn) mn = smp->data[j];
                        if (smp->data[j] > mx) mx = smp->data[j];
                    }
                    int y1 = wv_y + wv_h/2 - (int)(mx * wv_h * 0.4f);
                    int y2 = wv_y + wv_h/2 - (int)(mn * wv_h * 0.4f);
                    if (y1 > y2) { int tmp2 = y1; y1 = y2; y2 = tmp2; }
                    if (y2 - y1 < 1) y2 = y1 + 1;
                    line(r, sx + px, y1, sx + px, y2, wc);
                }

                if (is_active) {
                    rect_outline(r, sx, wv_y, sw2, wv_h, C_CYAN);
                    char sl[8]; snprintf(sl, sizeof(sl), "%d", si + 1);
                    draw_text(r, sx + 2, wv_y + 2, sl, C_ACID);
                    if (st->chop_mode == 1) {
                        int arrow_end = wv_x + wv_w;
                        for (int ax = sx + sw2; ax < arrow_end; ax += 3)
                            rect(r, ax, wv_y + wv_h/2, 2, 1, C_ACID);
                        draw_text(r, arrow_end - 18, wv_y + 2, ">>", C_ACID);
                    } else if (st->chop_mode == 2) {
                        rect(r, sx, wv_y + wv_h - 3, sw2, 2, C_PINK);
                        draw_text(r, sx + sw2 - 12, wv_y + 2, "LP", C_PINK);
                    }
                }
                if (si > 0) rect(r, sx - 1, wv_y, 1, wv_h, (SDL_Color){60,50,80,255});
            }

            const char *cm_names[] = {"CUT","THRU","LOOP"};
            int cm = st->chop_mode; if (cm < 0 || cm > 2) cm = 0;
            snprintf(info, sizeof(info), "CHOP %d/%d  %s  STEP %d  [L3 SLICES  SEL+L3 MODE]",
                     slice_idx + 1, st->slices, cm_names[cm], col + 1);
            SDL_Color cm_c = cm == 0 ? C_CYAN : (cm == 1 ? C_ACID : C_PINK);
            draw_text(r, 8, H - 42, info, cm_c);
        } else if (col < 0) {
            const char *blbl = st->bus >= 0 && st->bus < MAX_BUSES ? BUS_NAMES[st->bus] : "DIR";
            snprintf(info, sizeof(info), "%s  BUS [%s]  A = CYCLE BUS (DIR>A>B>C)",
                     track_source_label(st), blbl);
            SDL_Color ic = st->bus >= 0 ? bus_color(st->bus) : C_MUTED;
            draw_text(r, 8, H - 42, info, ic);
        } else {
            const char *rnd_names[] = {"---","UP","UP+DN","DOWN"};
            int rm = st->step_rand_mode[col];
            if (rm < 0 || rm > 3) rm = 0;
            if (st->steps[col] > 0) {
                snprintf(info, sizeof(info), "STEP %d  NOTE %+d  PROB %d%%  RND %s  RANGE %d  [R3]",
                         col + 1, st->step_notes[col], st->step_prob[col],
                         rnd_names[rm], st->note_rand_range);
            } else {
                snprintf(info, sizeof(info), "STEP %d  (empty)", col + 1);
            }
            draw_text(r, 8, H - 42, info, rm > 0 ? C_PURPLE : C_MUTED);
        }
    }

    draw_footer(r,
        "A ON/OFF   SEL+A ACCENT   B +TRACK   X PIANO   Y BROWSE",
        "L2/R2 SRC   SEL+Y SYNTH   START+X DUP   L3 CHOP   SEL+B DEL");
}

/* ===== PERFORM PAGE ===== */

static void draw_perform(SDL_Renderer *r) {
    rect(r, 0, 42, W, H - 28 - 42, C_BG);

    int top = 52;
    int row_h = 44;
    int left = 16;
    int row_w = W - 32;
    char tmp[128];

    const char *mode_names[] = {"MANUAL", "CHAIN", "RANDOM"};
    snprintf(tmp, sizeof(tmp), "PERFORM   [%s]", mode_names[app.pat_mode]);
    draw_text_2x(r, left, top, tmp, C_GOLD);
    if (app.pat_mode == 1 && app.chain_len > 0) {
        char chain_str[128] = "CHAIN: ";
        int clen = (int)strlen(chain_str);
        for (int ci = 0; ci < app.chain_len && clen < 110; ci++) {
            char seg[8];
            if (ci == app.chain_pos)
                snprintf(seg, sizeof(seg), "[%d]", app.chain[ci] + 1);
            else
                snprintf(seg, sizeof(seg), "%d", app.chain[ci] + 1);
            int slen = (int)strlen(seg);
            memcpy(chain_str + clen, seg, (size_t)slen);
            clen += slen;
            if (ci + 1 < app.chain_len) { chain_str[clen++] = '>'; }
        }
        chain_str[clen] = 0;
        if (app.chain_loop) { chain_str[clen++] = ' '; chain_str[clen++] = 'L'; chain_str[clen++] = 'P'; chain_str[clen] = 0; }
        draw_text(r, left + 300, top + 8, chain_str, C_PINK);
    }
    top += 28;

    int max_vis = (H - top - 36) / row_h;
    int scroll = 0;
    if (app.perf_cursor >= max_vis) scroll = app.perf_cursor - max_vis + 1;
    if (scroll > app.pattern_count - max_vis) scroll = app.pattern_count - max_vis;
    if (scroll < 0) scroll = 0;

    for (int row = 0; row < max_vis && row + scroll < app.pattern_count; row++) {
        int pi = row + scroll;
        Pattern *p = &app.patterns[pi];
        int y = top + row * row_h;
        int is_playing = (pi == app.step_pat);
        int is_queued = (pi == app.queued_pat);
        int is_cursor = (pi == app.perf_cursor);

        SDL_Color bg = is_playing ? (SDL_Color){30,50,25,255}
                     : is_cursor  ? C_DARK
                                  : C_BG;
        rect(r, left, y, row_w, row_h - 4, bg);

        if (is_playing) rect(r, left, y, 4, row_h - 4, C_ACID);
        if (is_queued)  rect(r, left, y, 4, row_h - 4, C_PINK);
        if (is_cursor)  rect_outline(r, left, y, row_w, row_h - 4, C_CYAN);

        SDL_Color ic = p->track_count > 0 ? INST_COLOR[p->tracks[0].inst] : C_MUTED;
        rect(r, left + 8, y + 8, 12, row_h - 20, ic);
        snprintf(tmp, sizeof(tmp), "%d %-12.12s", pi + 1, p->name);
        SDL_Color nc = is_playing ? C_ACID : is_cursor ? C_CYAN : C_INK;
        draw_text(r, left + 26, y + 6, tmp, nc);

        /* Mini step preview: show combined hits from all tracks */
        int plen = p->length > 0 ? p->length : 16;
        int dot_x = left + 26;
        int dot_y = y + 22;
        for (int s = 0; s < plen && dot_x < left + row_w - 60; s++) {
            int any_hit = 0;
            for (int ti = 0; ti < p->track_count; ti++) {
                if (p->tracks[ti].steps[s] > 0) { any_hit = 1; break; }
            }
            SDL_Color dc = any_hit ? ic : C_DARK;
            rect(r, dot_x, dot_y, 8, 8, dc);
            dot_x += 10;
        }

        int in_chain = 0;
        if (app.pat_mode == 1) {
            for (int ci = 0; ci < app.chain_len; ci++) {
                if (app.chain[ci] == pi) { in_chain = 1; break; }
            }
        }
        if (is_playing) draw_text(r, left + row_w - 42, y + 10, "PLAY", C_ACID);
        else if (is_queued) draw_text(r, left + row_w - 42, y + 10, "NEXT", C_PINK);
        else if (in_chain) draw_text(r, left + row_w - 42, y + 10, "CHN", C_PURPLE);
    }

    draw_footer(r,
        app.pat_mode == 1
            ? "A +CHAIN  X NOW  B MODE  Y NEW  SEL+Y DUP  SEL+A LOOP  SEL+X CLR"
            : "A QUEUE  X NOW  B MODE  Y NEW  SEL+Y DUP PAT",
        "L1/R1 PAGE   LEFT/RIGHT BPM   START PLAY/STOP");
}

/* ===== FX PAGE ===== */

static const char *GBEAT_NAMES[] = {"OFF","HALF","STT8","STT16","REV","GATE"};
#define GBEAT_MODE_COUNT 6
#define BUS_DETAIL_ROWS 8

static void draw_fx_mixer(SDL_Renderer *r) {
    rect(r, 0, 42, W, H - 28 - 42, C_BG);
    int top = 48;
    int margin = 12;
    draw_text_2x(r, margin, top, "MIXER", C_GOLD);
    if (app.pattern_count == 0) {
        draw_text(r, margin, top + 30, "NO PATTERN", C_MUTED);
        draw_footer(r, "L1/R1 PAGE", NULL);
        return;
    }
    Pattern *pat = &app.patterns[app.step_pat];
    int row_h = 36;
    int rtop = top + 24;
    char tmp[64];
    for (int ti = 0; ti < pat->track_count && ti < MAX_TRACKS; ti++) {
        Track *t = &pat->tracks[ti];
        int y = rtop + ti * row_h;
        int active = (app.fx_row == ti);
        rect(r, margin, y, W - margin*2, row_h - 4,
             active ? C_PANEL2 : C_PANEL);
        if (active) rect(r, margin, y, 3, row_h - 4, C_CYAN);
        SDL_Color ic = track_source_color(t);
        rect(r, margin + 8, y + 6, 8, row_h - 16, ic);
        snprintf(tmp, sizeof(tmp), "%d %-8.8s", ti + 1, t->name);
        draw_text(r, margin + 22, y + 6, tmp, active ? C_CYAN : C_INK);
        int bar_x = margin + 110;
        int bar_w = 200;
        rect(r, bar_x, y + 10, bar_w, 12, C_DARKER);
        int fw = (int)(bar_w * clampf(t->layers[0].gain, 0, 1));
        /* Use gain from layer 0 as track volume placeholder — we'll add a real vol later */
        rect(r, bar_x, y + 10, fw, 12, active ? C_ACID : ic);
        const char *bus_lbl = t->bus < 0 ? "DIR" : BUS_NAMES[t->bus];
        SDL_Color bc = t->bus == 0 ? C_PINK : (t->bus == 1 ? C_PURPLE : (t->bus == 2 ? C_CYAN : C_MUTED));
        rect(r, W - margin - 80, y + 4, 32, row_h - 12, bc);
        int tx = W - margin - 80 + (32 - (int)strlen(bus_lbl) * 6) / 2;
        draw_text(r, tx, y + 10, bus_lbl, C_BG);
        if (t->bus >= 0) {
            MixBus *b = &app.buses[t->bus];
            if (b->mute) draw_text(r, W - margin - 42, y + 10, "M", C_PINK);
        }
    }
    int bus_y = rtop + pat->track_count * row_h + 8;
    for (int bi = 0; bi < MAX_BUSES; bi++) {
        MixBus *b = &app.buses[bi];
        int bx = margin + bi * (W/2 - margin);
        SDL_Color bc = bi == 0 ? C_PINK : C_PURPLE;
        snprintf(tmp, sizeof(tmp), "BUS %s  V%.0f%% %s%s%s%s",
                 BUS_NAMES[bi], (double)(b->volume * 100),
                 b->drive_on ? "D" : "", b->delay_on ? "L" : "",
                 b->reverb_on ? "R" : "", b->gbeat_on ? "G" : "");
        draw_text(r, bx, bus_y, tmp, bc);
        rect(r, bx, bus_y + 12, W/2 - margin*2, 6, C_DARKER);
        rect(r, bx, bus_y + 12, (int)((W/2 - margin*2) * b->volume), 6, bc);
    }
    draw_footer(r,
        "UP/DN TRACK  A CYCLE BUS  X BUS DETAIL  SEL+A MUTE BUS",
        "L1/R1 PAGE   LEFT/RIGHT TRACK VOL");
}

static SDL_Color bus_color(int bi) {
    if (bi == 0) return C_PINK;
    if (bi == 1) return C_PURPLE;
    return C_CYAN;
}

static void draw_fx_drive(SDL_Renderer *r, int x, int y, int w, int h, float amt, int on, int sel) {
    SDL_Color c = on ? (sel ? C_GOLD : (SDL_Color){180,140,40,255}) : C_DARK;
    int cx = x + w/2, cy = y + h/2;
    int rad = h/2 - 4;
    for (int a = -140; a <= 140; a += 4) {
        float th = (float)a * PI / 180.0f;
        int px = cx + (int)(cosf(th) * (float)rad);
        int py = cy - (int)(sinf(th) * (float)rad);
        rect(r, px, py, 2, 2, C_DARK);
    }
    int fill_deg = (int)(280.0f * amt);
    for (int a = -140; a <= -140 + fill_deg; a += 4) {
        float th = (float)a * PI / 180.0f;
        int px = cx + (int)(cosf(th) * (float)rad);
        int py = cy - (int)(sinf(th) * (float)rad);
        float heat = (float)(a + 140) / 280.0f;
        SDL_Color dc = {(Uint8)(255*heat), (Uint8)(200*(1-heat)), 40, 255};
        rect(r, px, py, 3, 3, on ? dc : C_DARK);
    }
    char tmp[8]; snprintf(tmp, sizeof(tmp), "%d", (int)(amt*100));
    draw_text(r, cx - 8, cy - 3, tmp, c);
}

static void draw_fx_filter(SDL_Renderer *r, int x, int y, int w, int h, float cutoff_n, int on, int sel) {
    SDL_Color c = on ? (sel ? C_ACID : (SDL_Color){90,200,60,255}) : C_DARK;
    int base_y = y + h - 8;
    int cut_x = x + (int)(w * cutoff_n);
    for (int px = 0; px < w; px++) {
        int ax = x + px;
        float dist = (float)(ax - cut_x) / (float)(w * 0.15f);
        float curve = 1.0f / (1.0f + dist * dist);
        int bh = (int)((h - 16) * curve);
        if (bh < 1) bh = 1;
        SDL_Color fc = on ? c : C_DARK;
        if (px == (int)(w * cutoff_n)) fc = sel ? C_CYAN : C_INK;
        rect(r, ax, base_y - bh, 1, bh, fc);
    }
}

static void draw_fx_delay(SDL_Renderer *r, int x, int y, int w, int h, float time_n, float fb, float wet, int on, int sel) {
    SDL_Color c = on ? (sel ? C_CYAN : (SDL_Color){70,180,200,255}) : C_DARK;
    int taps = 5;
    int base_y = y + h - 6;
    float spacing = time_n * (float)w / (float)taps;
    if (spacing < 8) spacing = 8;
    for (int t = 0; t < taps; t++) {
        int tx = x + 10 + (int)((float)t * spacing);
        if (tx >= x + w - 4) break;
        float decay = powf(fb, (float)t) * wet;
        int bh = (int)((h - 12) * decay);
        if (bh < 2) bh = 2;
        SDL_Color dc = c;
        dc.r = (Uint8)(dc.r * (1.0f - (float)t * 0.15f));
        dc.g = (Uint8)(dc.g * (1.0f - (float)t * 0.15f));
        dc.b = (Uint8)(dc.b * (1.0f - (float)t * 0.15f));
        rect(r, tx, base_y - bh, 6, bh, on ? dc : C_DARK);
        if (t == 0) rect(r, tx, base_y - bh, 6, 2, C_INK);
    }
}

static void draw_fx_reverb(SDL_Renderer *r, int x, int y, int w, int h, float decay, float wet, int on, int sel) {
    SDL_Color c = on ? (sel ? C_PURPLE : (SDL_Color){160,110,220,255}) : C_DARK;
    int base_y = y + h - 6;
    Uint32 tick = SDL_GetTicks();
    for (int px = 0; px < w; px += 2) {
        float t = (float)px / (float)w;
        float env = expf(-t * (3.0f - decay * 2.5f)) * wet;
        float wobble = sinf((float)px * 0.08f + (float)tick * 0.003f) * 0.3f;
        int bh = (int)((h - 12) * (env + env * wobble));
        if (bh < 0) bh = 0;
        SDL_Color rc = c;
        rc.r = (Uint8)clampf(rc.r * (1.0f - t*0.5f), 0, 255);
        rc.g = (Uint8)clampf(rc.g * (1.0f - t*0.3f), 0, 255);
        rect(r, x + px, base_y - bh, 2, bh > 0 ? bh : 1, on ? rc : (SDL_Color){25,20,35,255});
    }
}

static void draw_fx_gbeat(SDL_Renderer *r, int x, int y, int w, int h, int mode, int on, int sel) {
    SDL_Color c = on ? (sel ? C_GOLD : (SDL_Color){200,160,40,255}) : C_DARK;
    Uint32 tick = SDL_GetTicks();
    int mid_y = y + h / 2;
    int seg_w = w / 16;
    for (int s = 0; s < 16; s++) {
        int sx = x + s * seg_w;
        int bh = 0;
        float phase = (float)s / 16.0f;
        switch (mode) {
            case 0: bh = h/6; break;
            case 1: bh = (int)((h/2) * (1.0f - phase * 0.5f)); break;
            case 2: bh = (s % 4 == 0) ? h/2 : h/6; break;
            case 3: bh = (s % 2 == 0) ? h/2 : h/6; break;
            case 4: bh = (int)((h/2) * (1.0f - (15.0f - (float)s) / 16.0f)); break;
            case 5: bh = (s % 2 == 0) ? h/3 : 2; break;
        }
        int pulse = on ? (int)(sinf((float)tick * 0.006f + phase * 6.0f) * 3.0f) : 0;
        bh += pulse;
        if (bh < 2) bh = 2;
        SDL_Color sc = c;
        if (on && mode > 0) {
            float bright = 0.6f + 0.4f * sinf((float)tick * 0.004f + (float)s * 0.5f);
            sc.r = (Uint8)(sc.r * bright); sc.g = (Uint8)(sc.g * bright);
        }
        rect(r, sx + 1, mid_y - bh/2, seg_w - 2, bh, on ? sc : (SDL_Color){25,20,35,255});
    }
}

static void draw_fx_crush(SDL_Renderer *r, int x, int y, int w, int h, float rate, float bits, int on, int sel) {
    SDL_Color c = on ? (sel ? C_GOLD : (SDL_Color){200,160,40,255}) : C_DARK;
    int base_y = y + h - 4;
    float step_w = (1.0f - rate) * 8.0f + 1.0f;
    float bval = 0;
    for (int px = 0; px < w; px++) {
        float raw = sinf((float)px * 0.15f) * 0.8f + sinf((float)px * 0.37f) * 0.3f;
        if ((float)px >= step_w * floorf((float)px / step_w) && (float)px < step_w * floorf((float)px / step_w) + 1.0f)
            bval = raw;
        float q = powf(2.0f, 1.0f + bits * 7.0f);
        float crushed = floorf(bval * q) / q;
        int bh = (int)(fabsf(crushed) * (h - 8));
        if (bh < 1) bh = 1;
        rect(r, x + px, base_y - bh, 1, bh, on ? c : (SDL_Color){25,20,35,255});
    }
}

static void draw_fx_grain(SDL_Renderer *r, int x, int y, int w, int h, float density, float size, float pitch_s, int on, int sel) {
    SDL_Color c = on ? (sel ? C_ACID : (SDL_Color){80,220,120,255}) : C_DARK;
    Uint32 tick = SDL_GetTicks();
    int mid_y = y + h / 2;
    int num_dots = 3 + (int)(density * 20);
    unsigned seed = tick / 60;
    for (int gi = 0; gi < num_dots; gi++) {
        seed = seed * 1103515245 + 12345;
        int gx = x + (int)((seed >> 4) % (unsigned)w);
        seed = seed * 1103515245 + 12345;
        int gy_off = (int)(((float)(seed % 1000) / 1000.0f - 0.5f) * (float)h * 0.7f);
        seed = seed * 1103515245 + 12345;
        int gsz = 2 + (int)(size * 6.0f + (float)(seed % 3));
        float wobble = sinf((float)tick * 0.005f + (float)gi * 1.3f);
        gy_off += (int)(wobble * pitch_s * 8.0f);
        SDL_Color gc = c;
        gc.r = (Uint8)clampf(gc.r + (float)(seed % 60) - 30, 0, 255);
        gc.g = (Uint8)clampf(gc.g + (float)(seed % 40) - 20, 0, 255);
        rect(r, gx, mid_y + gy_off - gsz/2, gsz, gsz, on ? gc : C_PANEL);
    }
}

static void draw_fx_detail(SDL_Renderer *r) {
    rect(r, 0, 42, W, H - 28 - 42, C_BG);
    int bi = app.fx_bus;
    if (bi < 0 || bi >= MAX_BUSES) bi = 0;
    MixBus *b = &app.buses[bi];
    int m = 8;
    SDL_Color bc = bus_color(bi);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "BUS %s", BUS_NAMES[bi]);
    draw_text_2x(r, m, 46, hdr, bc);
    if (b->mute) draw_text(r, m + 60, 50, "MUTED", C_PINK);
    char vol[16]; snprintf(vol, sizeof(vol), "V%d%%", (int)(b->volume * 100));
    draw_text(r, m + 100, 50, vol, C_INK);

    int pw = (W - m*2 - 4) / 2;
    int ph = 58;
    int gap = 3;
    int r1y = 64;
    int r2y = r1y + ph + gap;
    int r3y = r2y + ph + gap;
    int c1 = m, c2 = m + pw + 4;

    typedef struct { const char *name; int x, y, w2, h2; } Slot;
    Slot slots[7] = {
        {"DRIVE",  c1, r1y, pw, ph}, {"FILTER", c2, r1y, pw, ph},
        {"CRUSH",  c1, r2y, pw, ph}, {"DELAY",  c2, r2y, pw, ph},
        {"REVERB", c1, r3y, pw, ph}, {"GRAIN",  c2, r3y, pw, ph},
        {"GBEAT",  c1, r3y + ph + gap, W - m*2, H - 28 - (r3y + ph + gap) - 4}
    };

    for (int i = 0; i < 7; i++) {
        int sel = (app.fx_row == i);
        int sx = slots[i].x, sy = slots[i].y;
        int sw3 = slots[i].w2, sh = slots[i].h2;
        rect(r, sx, sy, sw3, sh, sel ? C_PANEL2 : C_BG);
        if (sel) rect_outline(r, sx, sy, sw3, sh, C_CYAN);
        int *onp = NULL;
        switch (i) {
            case 0: onp = &b->drive_on; break; case 1: onp = &b->filter_on; break;
            case 2: onp = &b->crush_on; break; case 3: onp = &b->delay_on; break;
            case 4: onp = &b->reverb_on; break; case 5: onp = &b->grain_on; break;
            case 6: onp = &b->gbeat_on; break;
        }
        SDL_Color nc = (onp && *onp) ? (sel ? C_CYAN : C_INK) : C_MUTED;
        draw_text(r, sx + 3, sy + 1, slots[i].name, nc);
        if (onp && *onp) rect(r, sx + sw3 - 14, sy + 2, 10, 6, C_ACID);
    }

    draw_fx_drive(r, c1+3, r1y+12, pw-6, ph-14, b->drive, b->drive_on, app.fx_row==0);

    char ftxt[16]; snprintf(ftxt, sizeof(ftxt), "Q%d", (int)(b->reso*100));
    if (app.fx_row==1) draw_text(r, c2+pw-30, r1y+1, ftxt, C_MUTED);
    draw_fx_filter(r, c2+3, r1y+12, pw-6, ph-14, (b->cutoff-300)/(8000-300), b->filter_on, app.fx_row==1);

    char ctxt[16]; snprintf(ctxt, sizeof(ctxt), "B%d", (int)(b->crush_bits*16));
    if (app.fx_row==2) draw_text(r, c1+pw-30, r2y+1, ctxt, C_MUTED);
    draw_fx_crush(r, c1+3, r2y+12, pw-6, ph-14, b->crush_rate, b->crush_bits, b->crush_on, app.fx_row==2);

    char dtxt[16]; snprintf(dtxt, sizeof(dtxt), "W%d", (int)(b->delay_wet*100));
    if (app.fx_row==3) draw_text(r, c2+pw-30, r2y+1, dtxt, C_MUTED);
    draw_fx_delay(r, c2+3, r2y+12, pw-6, ph-14, (b->delay_time-0.04f)/0.71f, b->delay_fb, b->delay_wet, b->delay_on, app.fx_row==3);

    char rtxt[16]; snprintf(rtxt, sizeof(rtxt), "W%d", (int)(b->rev_wet*100));
    if (app.fx_row==4) draw_text(r, c1+pw-30, r3y+1, rtxt, C_MUTED);
    draw_fx_reverb(r, c1+3, r3y+12, pw-6, ph-14, b->rev_decay, b->rev_wet, b->reverb_on, app.fx_row==4);

    char gtxt[16]; snprintf(gtxt, sizeof(gtxt), "P%d", (int)(b->grain_pitch*100));
    if (app.fx_row==5) draw_text(r, c2+pw-30, r3y+1, gtxt, C_MUTED);
    draw_fx_grain(r, c2+3, r3y+12, pw-6, ph-14, b->grain_density, b->grain_size, b->grain_pitch, b->grain_on, app.fx_row==5);

    int gy = slots[6].y;
    int gh = slots[6].h2;
    snprintf(hdr, sizeof(hdr), "[%s]", GBEAT_NAMES[b->gbeat.mode]);
    draw_text(r, m + 50, gy + 1, hdr, app.fx_row==6 ? C_CYAN : C_MUTED);
    draw_fx_gbeat(r, m+3, gy+12, W-m*2-6, gh-14, b->gbeat.mode, b->gbeat_on, app.fx_row==6);

    draw_footer(r,
        "UP/DN FX  L/R TWEAK  SEL+L/R 2ND KNOB  SEL+A ON/OFF",
        "Y BUS  X MIXER  A/B NUDGE  L2/R2 FINE  L1/R1 PAGE");
}

static void draw_fx(SDL_Renderer *r) {
    if (app.fx_mode == 0) draw_fx_mixer(r);
    else draw_fx_detail(r);
}

/* ===== SAMPLE BROWSER OVERLAY ===== */

#define MAX_FOLDERS 64
static char g_folders[MAX_FOLDERS][32];
static int g_folder_count = 0;
static int g_folder_first[MAX_FOLDERS];
static int g_folder_size[MAX_FOLDERS];

static void rebuild_folder_index(void) {
    g_folder_count = 0;
    int total = pattern_total_sources();
    g_folders[0][0] = 0;
    snprintf(g_folders[0], sizeof(g_folders[0]), "SYNTH");
    g_folder_first[0] = 0;
    g_folder_size[0] = INST_COUNT;
    g_folder_count = 1;
    const char *prev = "";
    for (int si = 0; si < app.sample_count && g_folder_count < MAX_FOLDERS; si++) {
        const char *f = app.samples[si].folder[0] ? app.samples[si].folder : "other";
        if (strcmp(f, prev) != 0) {
            snprintf(g_folders[g_folder_count], sizeof(g_folders[0]), "%.31s", f);
            g_folder_first[g_folder_count] = INST_COUNT + si;
            g_folder_size[g_folder_count] = 1;
            g_folder_count++;
            prev = f;
        } else {
            g_folder_size[g_folder_count - 1]++;
        }
    }
    (void)total;
}

static void draw_browser(SDL_Renderer *r) {
    if (g_folder_count == 0) rebuild_folder_index();
    rect(r, 0, 42, W, H - 42, C_BG);

    int fi = app.browse_folder_idx;
    if (fi < 0) fi = 0;
    if (fi >= g_folder_count) fi = g_folder_count - 1;
    app.browse_folder_idx = fi;

    int total = pattern_total_sources();
    if (app.browse_idx < g_folder_first[fi])
        app.browse_idx = g_folder_first[fi];
    if (app.browse_idx >= g_folder_first[fi] + g_folder_size[fi])
        app.browse_idx = g_folder_first[fi] + g_folder_size[fi] - 1;
    if (app.browse_idx >= total) app.browse_idx = total - 1;
    if (app.browse_idx < 0) app.browse_idx = 0;

    /* Left panel: folders */
    int lw = 140, rw = W - lw - 4;
    int top = 46, bot = H - 28;
    int frow_h = 22;
    int max_frows = (bot - top - 24) / frow_h;
    int fscroll = 0;
    if (fi >= max_frows) fscroll = fi - max_frows / 2;
    if (fscroll > g_folder_count - max_frows) fscroll = g_folder_count - max_frows;
    if (fscroll < 0) fscroll = 0;

    int on_folders = (app.browse_panel == 0);
    rect(r, 0, top, lw, bot - top, on_folders ? C_BG : C_DARKER);
    draw_text(r, 6, top + 4, "FOLDERS", on_folders ? C_GOLD : C_MUTED);

    SDL_Color folder_colors[] = {
        {255,100,120,255}, {255,200,60,255}, {120,255,80,255}, {95,232,255,255},
        {200,140,255,255}, {255,180,100,255}, {140,255,200,255}, {255,140,200,255}
    };

    for (int row = 0; row < max_frows && fscroll + row < g_folder_count; row++) {
        int idx = fscroll + row;
        int fy = top + 20 + row * frow_h;
        int sel = (idx == fi);
        SDL_Color fc = folder_colors[idx % 8];
        if (sel && on_folders) {
            rect(r, 2, fy, lw - 4, frow_h - 2, C_PANEL2);
            rect(r, 2, fy, 3, frow_h - 2, fc);
            rect_outline(r, 2, fy, lw - 4, frow_h - 2, fc);
        } else if (sel) {
            rect(r, 2, fy, lw - 4, frow_h - 2, C_PANEL);
            rect(r, 2, fy, 3, frow_h - 2, fc);
        }
        rect(r, 8, fy + 4, 6, frow_h - 10, fc);
        char flbl[16];
        snprintf(flbl, sizeof(flbl), "%-12.12s", g_folders[idx]);
        draw_text(r, 18, fy + 5, flbl, sel ? C_INK : C_MUTED);
        char cnt[6]; snprintf(cnt, sizeof(cnt), "%d", g_folder_size[idx]);
        draw_text(r, lw - 24, fy + 5, cnt, C_MUTED);
    }

    /* Right panel: icon grid */
    int rx = lw + 2;
    int on_items = (app.browse_panel == 1);
    rect(r, rx, top, rw, bot - top, on_items ? C_BG : C_DARKER);

    SDL_Color fc = folder_colors[fi % 8];
    char fhdr[40]; snprintf(fhdr, sizeof(fhdr), "%s", g_folders[fi]);
    draw_text_2x(r, rx + 8, top + 2, fhdr, fc);

    int itop = top + 22;
    int cols = 4;
    int pad = 4;
    int tile_w = (rw - pad * (cols + 1)) / cols;
    int tile_h = 42;
    int rows_vis = (bot - itop - 4) / (tile_h + pad);
    int fsize = g_folder_size[fi];
    int local_idx = app.browse_idx - g_folder_first[fi];
    if (local_idx < 0) local_idx = 0;
    if (local_idx >= fsize) local_idx = fsize - 1;
    int tiles_per_page = cols * rows_vis;
    int page = local_idx / tiles_per_page;
    int page_start = page * tiles_per_page;

    for (int ti = 0; ti < tiles_per_page && page_start + ti < fsize; ti++) {
        int li = page_start + ti;
        int global = g_folder_first[fi] + li;
        int col = ti % cols;
        int row = ti / cols;
        int tx = rx + pad + col * (tile_w + pad);
        int ty = itop + row * (tile_h + pad);
        int sel = (li == local_idx && on_items);
        int is_cur = (li == local_idx);

        SDL_Color tile_c;
        const char *label;
        if (global < INST_COUNT) {
            tile_c = INST_COLOR[global];
            label = INST_NAMES[global];
        } else {
            int si = global - INST_COUNT;
            label = app.samples[si].name;
            unsigned h = 0;
            for (const char *p2 = label; *p2; p2++) h = h * 31 + (unsigned)*p2;
            tile_c = (SDL_Color){
                (Uint8)(100 + (h % 120)),
                (Uint8)(80 + ((h >> 8) % 140)),
                (Uint8)(120 + ((h >> 16) % 100)), 255
            };
        }

        SDL_Color bg = sel ? tile_c : is_cur ? C_PANEL2 : C_DARK;
        rect(r, tx, ty, tile_w, tile_h, bg);
        if (sel) {
            rect_outline(r, tx - 1, ty - 1, tile_w + 2, tile_h + 2, C_CYAN);
        } else {
            rect(r, tx, ty, tile_w, 3, tile_c);
        }

        SDL_Color text_c = sel ? C_BG : (is_cur ? C_INK : C_MUTED);
        char short_nm[12];
        snprintf(short_nm, sizeof(short_nm), "%.10s", label);
        draw_text(r, tx + 3, ty + 6, short_nm, text_c);
        if (strlen(label) > 10) {
            snprintf(short_nm, sizeof(short_nm), "%.10s", label + 10);
            draw_text(r, tx + 3, ty + 16, short_nm, text_c);
        }
        if (global < INST_COUNT) {
            draw_text(r, tx + 3, ty + tile_h - 12, "SYN", tile_c);
        } else {
            draw_text(r, tx + 3, ty + tile_h - 12, "WAV",
                      sel ? (SDL_Color){40,30,20,255} : (SDL_Color){60,55,45,255});
        }
    }

    int total_pages = (fsize + tiles_per_page - 1) / tiles_per_page;
    if (total_pages > 1) {
        char pg[16]; snprintf(pg, sizeof(pg), "%d/%d", page + 1, total_pages);
        draw_text(r, rx + rw - 36, top + 8, pg, C_MUTED);
    }

    /* Footer */
    rect(r, 0, bot, W, 28, C_PANEL2);
    line(r, 0, bot, W, bot, C_LINE);
    if (on_folders)
        draw_text(r, 8, bot + 4, "UP/DN FOLDER  RIGHT > SOUNDS  A OPEN  B BACK", C_INK);
    else
        draw_text(r, 8, bot + 4, "DPAD BROWSE  A SELECT  X PREVIEW  B < FOLDERS", C_INK);
}

static void handle_browser_input(SDL_GameControllerButton b) {
    if (g_folder_count == 0) rebuild_folder_index();
    int total = pattern_total_sources();
    if (total <= 0) { app.browse_open = 0; return; }
    int fi = app.browse_folder_idx;
    if (fi < 0) fi = 0;
    if (fi >= g_folder_count) fi = g_folder_count - 1;

    if (app.browse_panel == 0) {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                fi = (fi - 1 + g_folder_count) % g_folder_count;
                app.browse_folder_idx = fi;
                app.browse_idx = g_folder_first[fi];
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                fi = (fi + 1) % g_folder_count;
                app.browse_folder_idx = fi;
                app.browse_idx = g_folder_first[fi];
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            case SDL_CONTROLLER_BUTTON_A:
                app.browse_panel = 1;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                app.browse_open = 0;
                break;
            default: break;
        }
    } else {
        int fstart = g_folder_first[fi];
        int fsize = g_folder_size[fi];
        int local = app.browse_idx - fstart;
        int grid_cols = 4;
        switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                local -= grid_cols;
                if (local < 0) local = (local + fsize) % fsize;
                app.browse_idx = fstart + local;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                local += grid_cols;
                if (local >= fsize) local = local % fsize;
                app.browse_idx = fstart + local;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                if (local % grid_cols == 0) { app.browse_panel = 0; break; }
                local--;
                app.browse_idx = fstart + local;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                local++;
                if (local >= fsize) local = 0;
                app.browse_idx = fstart + local;
                break;
            case SDL_CONTROLLER_BUTTON_X: {
                Track *bt = cur_track();
                if (bt) {
                    set_track_source(bt, app.browse_idx);
                    preview_track_step(bt, 1);
                }
            } break;
            case SDL_CONTROLLER_BUTTON_A: {
                Track *bt = cur_track();
                if (bt) {
                    set_track_source(bt, app.browse_idx);
                    preview_track_step(bt, 1);
                }
                app.browse_open = 0;
            } break;
            case SDL_CONTROLLER_BUTTON_B:
                app.browse_panel = 0;
                break;
            default: break;
        }
    }
}

/* ===== SETTINGS OVERLAY ===== */

static const char *SETTINGS_LABELS[9] = {"BPM","PITCH","SWING","SCALE","ROOT","THEME","SPEAKER","SAVE","LOAD"};
#define SETTINGS_ROWS 9

static int save_session(const char *path);
static int load_session(const char *path);
static int g_save_flash = 0;
static int g_load_flash = 0;

static void draw_settings(SDL_Renderer *r) {
    int pw = 460, ph = 400;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    rect(r, x - 6, y - 6, pw + 12, ph + 12, (SDL_Color){0,0,0,255});
    rect(r, x, y, pw, ph, C_PANEL);
    rect_outline(r, x, y, pw, ph, C_GOLD);
    rect_outline(r, x+1, y+1, pw-2, ph-2, C_GOLD);
    draw_text_2x(r, x + 16, y + 14, "SETTINGS", C_GOLD);

    int row_h = 34;
    int top = y + 46;
    char tmp[64];
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        int ry = top + i * row_h;
        int active = (app.settings_focus == i);
        SDL_Color rbg = active ? C_LIGHT : C_PANEL;
        rect(r, x + 12, ry, pw - 24, row_h - 6, rbg);
        if (active) rect(r, x + 12, ry, 4, row_h - 6, C_CYAN);
        draw_text_2x(r, x + 24, ry + 7, SETTINGS_LABELS[i], active ? C_CYAN : C_INK);

        if (i < 5) {
            int bx = x + 160;
            int bw = pw - 24 - (bx - x - 12) - 100;
            int by = ry + 12;
            rect(r, bx, by, bw, 12, C_DARKER);
            float n = 0.0f;
            if (i == 0) n = (app.bpm - 60.0f) / (220.0f - 60.0f);
            else if (i == 1) n = (app.master_pitch - 0.5f) / (2.0f - 0.5f);
            else if (i == 2) n = (app.swing - 0.5f) / 0.25f;
            else if (i == 3) n = (float)app.scale_idx / (SCALE_COUNT - 1);
            else n = (float)app.scale_root / 11.0f;
            if (n < 0) n = 0; if (n > 1) n = 1;
            rect(r, bx + 1, by + 1, (int)((bw - 2) * n), 10, active ? C_ACID : C_PURPLE);
            if (i == 0) snprintf(tmp, sizeof(tmp), "%d", app.bpm);
            else if (i == 1) snprintf(tmp, sizeof(tmp), "%.2fx", (double)app.master_pitch);
            else if (i == 2) snprintf(tmp, sizeof(tmp), "%d%%", (int)((app.swing - 0.5f) * 400));
            else if (i == 3) snprintf(tmp, sizeof(tmp), "%s", SCALE_NAMES[app.scale_idx]);
            else snprintf(tmp, sizeof(tmp), "%s", NOTE_NAMES[app.scale_root]);
            draw_text(r, x + pw - 92, ry + 12, tmp, active ? C_ACID : C_INK);
        } else if (i == 5) {
            int th = app.theme;
            if (th < 0 || th >= THEME_COUNT) th = 0;
            draw_text(r, x + pw - 92, ry + 12, THEME_NAMES[th], active ? C_ACID : C_INK);
        } else if (i == 6) {
            const char *sl = app.speaker_mute ? "HEADPHONES" : "SPK + HP";
            SDL_Color sc2 = app.speaker_mute ? C_PINK : C_ACID;
            draw_text(r, x + pw - 92, ry + 12, sl, active ? sc2 : C_INK);
        } else if (i == 7) {
            const char *t2 = g_save_flash > 0 ? "SAVED!" : "press A";
            draw_text(r, x + pw - 92, ry + 12, t2, g_save_flash > 0 ? C_ACID : C_MUTED);
            if (g_save_flash > 0) g_save_flash--;
        } else if (i == 8) {
            const char *t2 = g_load_flash > 0 ? "LOADED!" : "press A";
            draw_text(r, x + pw - 92, ry + 12, t2, g_load_flash > 0 ? C_ACID : C_MUTED);
            if (g_load_flash > 0) g_load_flash--;
        }
    }
    draw_text(r, x + 16, y + ph - 20,
        "DPAD UP/DN PICK   LEFT/RIGHT ADJUST   A CONFIRM   B CLOSE", C_MUTED);
}

static void handle_settings_input(SDL_GameControllerButton b) {
    int dir = 0;
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            app.settings_focus = (app.settings_focus + SETTINGS_ROWS - 1) % SETTINGS_ROWS; return;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            app.settings_focus = (app.settings_focus + 1) % SETTINGS_ROWS; return;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dir = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dir = 1; break;
        case SDL_CONTROLLER_BUTTON_A: dir = 1; break;
        case SDL_CONTROLLER_BUTTON_X: dir = -1; break;
        case SDL_CONTROLLER_BUTTON_Y: dir = 1; break;
        case SDL_CONTROLLER_BUTTON_B:
            app.settings_open = 0; return;
        default: return;
    }
    if (app.settings_focus == 0) {
        int step = (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_X) ? 5 : 1;
        bpm_nudge(dir * step);
    } else if (app.settings_focus == 1) {
        float step = (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_X) ? 0.05f : 0.01f;
        app.master_pitch = clampf(app.master_pitch + dir * step, 0.5f, 2.0f);
    } else if (app.settings_focus == 2) {
        app.swing = clampf(app.swing + dir * 0.01f, 0.5f, 0.75f);
    } else if (app.settings_focus == 3) {
        app.scale_idx = (app.scale_idx + dir + SCALE_COUNT) % SCALE_COUNT;
    } else if (app.settings_focus == 4) {
        app.scale_root = (app.scale_root + dir + 12) % 12;
    } else if (app.settings_focus == 5) {
        app.theme = (app.theme + dir + THEME_COUNT) % THEME_COUNT;
        apply_theme(app.theme);
    } else if (app.settings_focus == 6) {
        app.speaker_mute = !app.speaker_mute;
        if (app.speaker_mute)
            system("amixer -c 0 sset 'Playback Path' 'HP' >/dev/null 2>&1");
        else
            system("amixer -c 0 sset 'Playback Path' 'SPK_HP' >/dev/null 2>&1");
    } else if (app.settings_focus == 7) {
        if (b == SDL_CONTROLLER_BUTTON_A) {
            if (save_session(SESSION_PATH)) g_save_flash = 60;
        }
    } else if (app.settings_focus == 8) {
        if (b == SDL_CONTROLLER_BUTTON_A) {
            if (load_session(SESSION_PATH)) g_load_flash = 60;
        }
    }
}

/* ===== SYNTH LAB OVERLAY ===== */

#define SYNTH_LAB_ROWS 8

static void draw_synth_lab(SDL_Renderer *r) {
    Track *t = cur_track();
    if (!t) { app.synth_open = 0; return; }
    int pw = 480, ph = 400;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    rect(r, x - 6, y - 6, pw + 12, ph + 12, (SDL_Color){0,0,0,255});
    rect(r, x, y, pw, ph, C_PANEL);
    rect_outline(r, x, y, pw, ph, C_PURPLE);
    rect_outline(r, x+1, y+1, pw-2, ph-2, C_PURPLE);
    draw_text_2x(r, x + 16, y + 14, "SYNTH LAB", C_PURPLE);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s  [%s]", t->name,
             t->sample_ref >= 0 ? "smp" : INST_NAMES[t->inst]);
    draw_text(r, x + 180, y + 22, buf, C_MUTED);

    const char *labels[SYNTH_LAB_ROWS] = {
        "LFO RATE","LFO DEPTH","ATTACK","DECAY","RND RANGE","LAYER 1","LAYER 2","LAYER 3"
    };
    int row_h = 38;
    int top = y + 48;
    char val[40];
    for (int i = 0; i < SYNTH_LAB_ROWS; i++) {
        int ry = top + i * row_h;
        int active = (app.synth_focus == i);
        rect(r, x + 12, ry, pw - 24, row_h - 6,
             active ? C_LIGHT : C_PANEL);
        if (active) rect(r, x + 12, ry, 4, row_h - 6, C_CYAN);
        SDL_Color label_c = active ? C_CYAN : C_INK;
        if (i >= 5) {
            PatternLayer *L = &t->layers[i - 5];
            label_c = L->active ? layer_color(L) : C_MUTED;
        }
        draw_text_2x(r, x + 24, ry + 6, labels[i], label_c);

        if (i < 4) {
            int bx = x + 200, bw = pw - 24 - (bx - x - 12) - 100;
            int by = ry + 14, bh = 12;
            rect(r, bx, by, bw, bh, C_DARKER);
            float n = 0.0f;
            if (i == 0) n = t->lfo_rate / 20.0f;
            else if (i == 1) n = t->lfo_depth;
            else if (i == 2) n = (t->attack_mul - 0.1f) / (2.0f - 0.1f);
            else n = (t->decay_mul - 0.1f) / (3.0f - 0.1f);
            if (n < 0) n = 0; if (n > 1) n = 1;
            rect(r, bx + 1, by + 1, (int)((bw - 2) * n), bh - 2, active ? C_ACID : C_PURPLE);
            if (i == 0) snprintf(val, sizeof(val), "%.1fHZ", (double)t->lfo_rate);
            else if (i == 1) snprintf(val, sizeof(val), "%.2f", (double)t->lfo_depth);
            else snprintf(val, sizeof(val), "%.2fX",
                          (double)(i == 2 ? t->attack_mul : t->decay_mul));
            draw_text(r, x + pw - 90, ry + 14, val, active ? C_ACID : C_INK);
        } else if (i == 4) {
            int bx = x + 200, bw = pw - 24 - (bx - x - 12) - 100;
            int by = ry + 14, bh = 12;
            rect(r, bx, by, bw, bh, C_DARKER);
            float n = (float)t->note_rand_range / 12.0f;
            if (n < 0) n = 0; if (n > 1) n = 1;
            rect(r, bx + 1, by + 1, (int)((bw - 2) * n), bh - 2, active ? C_ACID : C_PINK);
            snprintf(val, sizeof(val), "%d deg", t->note_rand_range);
            draw_text(r, x + pw - 90, ry + 14, val, active ? C_ACID : C_INK);
        } else {
            PatternLayer *L = &t->layers[i - 5];
            int sx = x + 168, sy = ry + 8;
            if (L->active) {
                snprintf(val, sizeof(val), "%s %+d", layer_label(L), L->note_delta);
                draw_text(r, sx, sy, val, layer_color(L));
                int bx = x + 320, bw = pw - 24 - (bx - x - 12) - 100;
                int by = ry + 18, bh = 8;
                rect(r, bx, by, bw, bh, C_DARKER);
                rect(r, bx + 1, by + 1, (int)((bw - 2) * L->gain), bh - 2, active ? C_ACID : layer_color(L));
                draw_text(r, x + pw - 90, ry + 14, "ON", active ? C_ACID : C_INK);
            } else {
                draw_text(r, sx, sy + 4, "(empty - A to enable)", C_MUTED);
                draw_text(r, x + pw - 90, ry + 14, "off", C_MUTED);
            }
        }
    }
    draw_text(r, x + 16, y + ph - 36,
              "MOD ROWS: LEFT/RIGHT ADJUST   A PREVIEW", C_MUTED);
    draw_text(r, x + 16, y + ph - 20,
              "LAYER ROWS: A TOGGLE   LEFT/RIGHT SOURCE   X RESET   Y GAIN   B CLOSE", C_MUTED);
}

static void handle_synth_lab_input(SDL_GameControllerButton b) {
    Track *t = cur_track();
    if (!t) { app.synth_open = 0; return; }
    int dir = 0;
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            app.synth_focus = (app.synth_focus + SYNTH_LAB_ROWS - 1) % SYNTH_LAB_ROWS; return;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            app.synth_focus = (app.synth_focus + 1) % SYNTH_LAB_ROWS; return;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dir = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dir = 1; break;
        case SDL_CONTROLLER_BUTTON_A:
            if (app.synth_focus < 5) {
                preview_track_step(t, 1);
            } else {
                toggle_layer_active(app.synth_focus - 5);
                if (t->layers[app.synth_focus - 5].active)
                    preview_track_step(t, 1);
            }
            return;
        case SDL_CONTROLLER_BUTTON_X:
            if (app.synth_focus >= 5) {
                int li = app.synth_focus - 5;
                t->layers[li].active = 0;
                t->layers[li].note_delta = 0;
                t->layers[li].gain = 0.65f;
            }
            return;
        case SDL_CONTROLLER_BUTTON_Y:
            if (app.synth_focus >= 5) {
                int li = app.synth_focus - 5;
                t->layers[li].gain = clampf(t->layers[li].gain + 0.05f, 0.0f, 1.0f);
                if (t->layers[li].gain >= 0.99f) t->layers[li].gain = 0.0f;
            }
            return;
        case SDL_CONTROLLER_BUTTON_B:
            app.synth_open = 0; return;
        default: return;
    }
    if (app.synth_focus == 0) t->lfo_rate = clampf(t->lfo_rate + dir * 0.5f, 0.0f, 20.0f);
    else if (app.synth_focus == 1) t->lfo_depth = clampf(t->lfo_depth + dir * 0.05f, 0.0f, 1.0f);
    else if (app.synth_focus == 2) t->attack_mul = clampf(t->attack_mul + dir * 0.05f, 0.1f, 2.0f);
    else if (app.synth_focus == 3) t->decay_mul = clampf(t->decay_mul + dir * 0.1f, 0.1f, 3.0f);
    else if (app.synth_focus == 4) {
        t->note_rand_range += dir;
        if (t->note_rand_range < 0) t->note_rand_range = 0;
        if (t->note_rand_range > 12) t->note_rand_range = 12;
    } else {
        int li = app.synth_focus - 5;
        if (select_held) {
            int nd = t->layers[li].note_delta + dir;
            if (nd < -24) nd = -24;
            if (nd > 24) nd = 24;
            t->layers[li].note_delta = nd;
        } else {
            cycle_layer_source(li, dir);
        }
    }
}

/* ===== SONG PICKER OVERLAY ===== */

static void draw_song_picker(SDL_Renderer *r) {
    int total = app.pattern_count + app.sample_count + app.beat_count;
    int pw = 520, ph = 400;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    rect(r, x - 6, y - 6, pw + 12, ph + 12, (SDL_Color){0,0,0,255});
    rect(r, x, y, pw, ph, C_PANEL);
    rect_outline(r, x, y, pw, ph, C_GOLD);
    rect_outline(r, x+1, y+1, pw-2, ph-2, C_GOLD);
    draw_text_2x(r, x + 16, y + 14, "PLACE @CURSOR", C_GOLD);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s  BAR %d", TRACK_NAMES[app.cursor_lane], app.cursor_bar + 1);
    draw_text(r, x + 280, y + 22, hdr, C_MUTED);

    if (total == 0) {
        draw_text(r, x + 16, y + 50, "no patterns, samples, or beats loaded", C_MUTED);
        draw_text(r, x + 16, y + ph - 20, "B CANCEL", C_MUTED);
        return;
    }
    if (app.song_pick_idx >= total) app.song_pick_idx = total - 1;
    if (app.song_pick_idx < 0) app.song_pick_idx = 0;

    int row_h = 22;
    int top = y + 50;
    int list_h = ph - 70;
    int max_rows = list_h / row_h;
    int scroll = 0;
    if (app.song_pick_idx >= max_rows) scroll = app.song_pick_idx - max_rows / 2;
    if (scroll > total - max_rows) scroll = total - max_rows;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < max_rows && scroll + i < total; i++) {
        int idx = scroll + i;
        int ry = top + i * row_h;
        int active = (idx == app.song_pick_idx);
        rect(r, x + 8, ry, pw - 16, row_h - 2,
             active ? C_LIGHT : (SDL_Color){18,16,12,255});
        if (active) rect(r, x + 8, ry, 3, row_h - 2, C_CYAN);

        if (idx < app.pattern_count) {
            Pattern *pp = &app.patterns[idx];
            SDL_Color c = pp->track_count > 0 ? track_source_color(&pp->tracks[0]) : C_MUTED;
            rect(r, x + 18, ry + 5, 10, row_h - 12, c);
            const char *plbl = pp->track_count > 0 ? track_source_label(&pp->tracks[0]) : "---";
            draw_text(r, x + 34, ry + 6, plbl, c);
            char nm[24];
            snprintf(nm, sizeof(nm), "%-16.16s", pp->name);
            draw_text(r, x + 70, ry + 6, nm, active ? C_INK : C_MUTED);
            if (pp->track_count > 0)
                draw_track_thumb(r, &pp->tracks[0], x + pw - 200, ry + 5, 180, row_h - 10, c);
        } else if (idx < app.pattern_count + app.sample_count) {
            int si = idx - app.pattern_count;
            rect(r, x + 18, ry + 5, 10, row_h - 12, C_GOLD);
            draw_text(r, x + 34, ry + 6, "wav", C_GOLD);
            char nm[28];
            snprintf(nm, sizeof(nm), "%-20.20s", app.samples[si].name);
            draw_text(r, x + 70, ry + 6, nm, active ? C_INK : C_MUTED);
            draw_wave(r, &app.samples[si], x + pw - 200, ry + 5, 180, row_h - 10, C_GOLD);
        } else {
            int bi = idx - app.pattern_count - app.sample_count;
            Beat *bt = &app.beats[bi];
            rect(r, x + 18, ry + 5, 10, row_h - 12, C_PURPLE);
            draw_text(r, x + 34, ry + 6, "BEAT", C_PURPLE);
            char nm[40];
            snprintf(nm, sizeof(nm), "%-16.16s %dbar %dclips", bt->name, bt->span_bars, bt->count);
            draw_text(r, x + 70, ry + 6, nm, active ? C_INK : C_MUTED);
        }
    }
    draw_text(r, x + 16, y + ph - 20,
              "A PLACE   B CANCEL   X DEL BEAT   DPAD MOVE", C_MUTED);
}

static void handle_song_picker_input(SDL_GameControllerButton b) {
    int total = app.pattern_count + app.sample_count + app.beat_count;
    if (total == 0) { app.song_pick_open = 0; return; }
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            app.song_pick_idx = (app.song_pick_idx - 1 + total) % total; return;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            app.song_pick_idx = (app.song_pick_idx + 1) % total; return;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            app.song_pick_idx -= 8;
            if (app.song_pick_idx < 0) app.song_pick_idx = 0;
            return;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            app.song_pick_idx += 8;
            if (app.song_pick_idx >= total) app.song_pick_idx = total - 1;
            return;
        case SDL_CONTROLLER_BUTTON_A: {
            int idx = app.song_pick_idx;
            if (idx < app.pattern_count) {
                app.selected_type = 1;
                app.selected_ref = idx;
                place_clip();
            } else if (idx < app.pattern_count + app.sample_count) {
                app.selected_type = 0;
                app.selected_ref = idx - app.pattern_count;
                place_clip();
            } else {
                int bi = idx - app.pattern_count - app.sample_count;
                drop_beat_at_cursor(&app.beats[bi]);
            }
            app.song_pick_open = 0;
            return;
        }
        case SDL_CONTROLLER_BUTTON_X: {
            int idx = app.song_pick_idx;
            int beat_base = app.pattern_count + app.sample_count;
            if (idx >= beat_base) delete_beat_at(idx - beat_base);
            return;
        }
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_Y:
            app.song_pick_open = 0; return;
        default: return;
    }
}

/* ===== PIANO ROLL OVERLAY ===== */

static void draw_piano(SDL_Renderer *r) {
    Track *t = cur_track();
    if (!t) { app.piano_open = 0; return; }
    rect(r, 0, 42, W, H - 28 - 42, C_BG);

    int title_y = 46;
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "PIANO ROLL  %s  INST %s  ROOT %s%d",
             t->name,
             t->sample_ref >= 0 ? "smp" : INST_NAMES[t->inst],
             NOTE_NAMES[t->note % 12], t->note / 12 - 1);
    draw_text(r, 8, title_y, tmp, C_GOLD);
    draw_text(r, 8, title_y + 12,
              "DPAD MOVE   A PLACE/CLEAR NOTE   B CLOSE   L2/R2 TRANSPOSE BASE", C_MUTED);

    int grid_top = 78;
    int grid_left = 50;
    int grid_right = W - 10;
    int grid_bot = H - 36;
    int rows = 24;
    int row_h = (grid_bot - grid_top) / rows;
    int cell_w = (grid_right - grid_left) / 16;

    int now_step = -1;
    if (app.playing) now_step = ((int)(fmod(app.song_time, song_dur()) / step_dur())) & 15;

    for (int row = 0; row < rows; row++) {
        int delta = 11 - row;
        int note = t->note + delta;
        int row_y = grid_top + row * row_h;
        int pc = ((note % 12) + 12) % 12;
        int is_black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        SDL_Color rowbg = is_black ? (SDL_Color){12,10,18,255} : (SDL_Color){20,16,28,255};
        if (delta == 0) rowbg = (SDL_Color){40,32,56,255};
        if (app.scale_idx > 0 && !scale_has(app.scale_idx, app.scale_root, note))
            rowbg = (SDL_Color){8,6,14,255};
        rect(r, grid_left, row_y, grid_right - grid_left, row_h - 1, rowbg);
        rect(r, 6, row_y, grid_left - 8, row_h - 1, is_black ? (SDL_Color){10,8,14,255} : (SDL_Color){28,24,36,255});
        if (pc == 0) {
            char nm[8];
            snprintf(nm, sizeof(nm), "%s%d", NOTE_NAMES[pc], note / 12 - 1);
            draw_text(r, 8, row_y + (row_h - 7)/2, nm, C_GOLD);
        } else if (delta == 0) {
            draw_text(r, 8, row_y + (row_h - 7)/2, NOTE_NAMES[pc], C_CYAN);
        }
        for (int col = 0; col < 16; col++) {
            int cx = grid_left + col * cell_w;
            if (col % 4 == 0) line(r, cx, grid_top, cx, grid_bot, (SDL_Color){50,40,70,255});
            else line(r, cx, row_y, cx, row_y + row_h - 1, (SDL_Color){26,22,38,255});
            if (now_step == col) rect(r, cx + 1, row_y, cell_w - 2, 2, C_ACID);
        }
    }

    for (int col = 0; col < 16; col++) {
        if (!t->steps[col]) continue;
        int delta = t->step_notes[col];
        int row = 11 - delta;
        if (row < 0 || row >= rows) continue;
        int x = grid_left + col * cell_w;
        int y = grid_top + row * row_h;
        SDL_Color nc = t->steps[col] == 2 ? C_ACID : INST_COLOR[t->inst];
        if (t->sample_ref >= 0) nc = C_GOLD;
        rect(r, x + 2, y + 2, cell_w - 3, row_h - 4, nc);
    }

    int cx = grid_left + app.piano_col * cell_w;
    int cy = grid_top + app.piano_row * row_h;
    rect_outline(r, cx, cy, cell_w, row_h, C_CYAN);
    rect_outline(r, cx + 1, cy + 1, cell_w - 2, row_h - 2, C_CYAN);
}

static void handle_piano_input(SDL_GameControllerButton b) {
    Track *t = cur_track();
    if (!t) { app.piano_open = 0; return; }
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (app.piano_row > 0) app.piano_row--; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (app.piano_row < 23) app.piano_row++; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            app.piano_col = (app.piano_col - 1 + 16) % 16; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            app.piano_col = (app.piano_col + 1) % 16; break;
        case SDL_CONTROLLER_BUTTON_A: {
            int delta = 11 - app.piano_row;
            if (app.scale_idx > 0 && !scale_has(app.scale_idx, app.scale_root, t->note + delta)) {
                int snapped = snap_note(t->note + delta, delta >= 0 ? 1 : -1, app.scale_idx, app.scale_root);
                delta = snapped - t->note;
                if (delta > 11) delta = 11;
                if (delta < -12) delta = -12;
                app.piano_row = 11 - delta;
            }
            if (t->steps[app.piano_col] > 0 && t->step_notes[app.piano_col] == delta) {
                t->steps[app.piano_col] = 0;
            } else {
                t->steps[app.piano_col] = 1;
                t->step_notes[app.piano_col] = delta;
            }
        } break;
        case SDL_CONTROLLER_BUTTON_B:
            app.piano_open = 0; break;
        case SDL_CONTROLLER_BUTTON_X: {
            int n = snap_note(t->note, -1, app.scale_idx, app.scale_root);
            if (n >= 12) t->note = n;
        } break;
        case SDL_CONTROLLER_BUTTON_Y: {
            int n = snap_note(t->note, 1, app.scale_idx, app.scale_root);
            if (n <= 108) t->note = n;
        } break;
        default: break;
    }
}

/* ===== SONG FILE BROWSER ===== */

#define MAX_SONG_FILES 32
static char g_song_files[MAX_SONG_FILES][64];
static int g_song_file_count = 0;

static void scan_song_files(void) {
    g_song_file_count = 0;
    DIR *d = opendir(".");
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_song_file_count < MAX_SONG_FILES) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len > 9 && strcmp(ent->d_name + len - 9, ".bukuloop") == 0) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", ent->d_name);
            nm[len - 9] = 0;
            snprintf(g_song_files[g_song_file_count++], sizeof(g_song_files[0]), "%s", nm);
        }
    }
    closedir(d);
}

static void draw_file_browser(SDL_Renderer *r) {
    int pw = 500, ph = 400;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    rect(r, x - 6, y - 6, pw + 12, ph + 12, (SDL_Color){0,0,0,255});
    rect(r, x, y, pw, ph, C_DARKER);
    rect_outline(r, x, y, pw, ph, C_GOLD);
    rect_outline(r, x+1, y+1, pw-2, ph-2, C_GOLD);
    draw_text_2x(r, x + 16, y + 12, "SONGS", C_GOLD);

    const char *action_names[] = {"LOAD","SAVE","NEW","CLONE"};
    SDL_Color action_colors[] = {C_CYAN, C_ACID, C_PINK, C_PURPLE};
    int atop = y + 10;
    for (int i = 0; i < 4; i++) {
        int ax = x + 140 + i * 80;
        int sel = (!app.file_name_edit && app.file_action == i);
        rect(r, ax, atop, 68, 20, sel ? action_colors[i] : (SDL_Color){30,26,42,255});
        SDL_Color tc = sel ? C_BG : C_MUTED;
        int tx = ax + (68 - (int)strlen(action_names[i]) * 6) / 2;
        draw_text(r, tx, atop + 6, action_names[i], tc);
    }

    if (app.file_name_edit) {
        int ey = y + 40;
        rect(r, x + 16, ey, pw - 32, 30, (SDL_Color){20,16,30,255});
        rect_outline(r, x + 16, ey, pw - 32, 30, C_ACID);
        draw_text(r, x + 20, ey + 4, "NAME:", C_MUTED);
        draw_text_2x(r, x + 70, ey + 2, app.file_name, C_ACID);
        int cx = x + 70 + app.file_name_pos * 12;
        Uint32 blink = (SDL_GetTicks() / 400) & 1;
        if (blink) rect(r, cx, ey + 2, 2, 18, C_CYAN);
        draw_text(r, x + 16, ey + 34, "DPAD L/R CURSOR  UP/DN LETTER  A CONFIRM  B CANCEL", C_MUTED);
        return;
    }

    int row_h = 28;
    int ltop = y + 44;
    int max_rows = (ph - 80) / row_h;
    int fscroll = 0;
    if (app.file_idx >= max_rows) fscroll = app.file_idx - max_rows / 2;
    if (fscroll > g_song_file_count - max_rows) fscroll = g_song_file_count - max_rows;
    if (fscroll < 0) fscroll = 0;

    if (g_song_file_count == 0) {
        draw_text(r, x + 40, ltop + 20, "NO SONGS SAVED YET", C_MUTED);
        draw_text(r, x + 40, ltop + 40, "SELECT SAVE OR NEW TO CREATE ONE", C_INK);
    }

    for (int row = 0; row < max_rows && fscroll + row < g_song_file_count; row++) {
        int idx = fscroll + row;
        int fy = ltop + row * row_h;
        int sel = (idx == app.file_idx);
        rect(r, x + 12, fy, pw - 24, row_h - 4,
             sel ? (SDL_Color){36,30,52,255} : C_BG);
        if (sel) {
            rect(r, x + 12, fy, 4, row_h - 4, action_colors[app.file_action]);
            rect_outline(r, x + 12, fy, pw - 24, row_h - 4, C_CYAN);
        }
        char nm[48]; snprintf(nm, sizeof(nm), "%-40.40s", g_song_files[idx]);
        draw_text(r, x + 24, fy + 8, nm, sel ? C_INK : C_MUTED);
        rect(r, x + pw - 36, fy + 6, 16, row_h - 14, C_GOLD);
        draw_text(r, x + pw - 34, fy + 8, "F", C_BG);
    }

    draw_text(r, x + 16, y + ph - 18,
              "UP/DN FILE  L/R ACTION  A DO IT  X RENAME  Y DEL  L1/R1 SETTINGS  B CLOSE", C_MUTED);
}

static void start_name_edit(const char *initial) {
    app.file_name_edit = 1;
    snprintf(app.file_name, sizeof(app.file_name), "%s", initial ? initial : "");
    app.file_name_pos = (int)strlen(app.file_name);
}

static void handle_file_name_input(SDL_GameControllerButton b) {
    int len = (int)strlen(app.file_name);
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (app.file_name_pos > 0) app.file_name_pos--;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (app.file_name_pos < len && app.file_name_pos < 30) {
                app.file_name_pos++;
            } else if (len < 30) {
                app.file_name[len] = 'a';
                app.file_name[len + 1] = 0;
                app.file_name_pos = len + 1;
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (app.file_name_pos < len) {
                char *c = &app.file_name[app.file_name_pos];
                if (*c >= 'a' && *c < 'z') (*c)++;
                else if (*c >= 'A' && *c < 'Z') (*c)++;
                else if (*c >= '0' && *c < '9') (*c)++;
                else if (*c == '9') *c = 'a';
                else if (*c == 'z') *c = 'A';
                else if (*c == 'Z') *c = '0';
                else if (*c == ' ') *c = 'a';
                else (*c)++;
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (app.file_name_pos < len) {
                char *c = &app.file_name[app.file_name_pos];
                if (*c > 'a' && *c <= 'z') (*c)--;
                else if (*c > 'A' && *c <= 'Z') (*c)--;
                else if (*c > '0' && *c <= '9') (*c)--;
                else if (*c == 'a') *c = '9';
                else if (*c == 'A') *c = 'z';
                else if (*c == '0') *c = 'Z';
                else if (*c == ' ') *c = 'z';
                else (*c)--;
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            if (app.file_name_pos < len) {
                memmove(&app.file_name[app.file_name_pos], &app.file_name[app.file_name_pos + 1],
                        (size_t)(len - app.file_name_pos));
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (len < 30 && app.file_name_pos <= len) {
                memmove(&app.file_name[app.file_name_pos + 1], &app.file_name[app.file_name_pos],
                        (size_t)(len - app.file_name_pos + 1));
                app.file_name[app.file_name_pos] = ' ';
            }
            break;
        case SDL_CONTROLLER_BUTTON_A: {
            if (strlen(app.file_name) == 0) break;
            char path[128];
            snprintf(path, sizeof(path), "%s.bukuloop", app.file_name);
            if (app.file_action == 1 || app.file_action == 2 || app.file_action == 3) {
                save_session(path);
            }
            app.file_name_edit = 0;
            scan_song_files();
        } break;
        case SDL_CONTROLLER_BUTTON_B:
            app.file_name_edit = 0;
            break;
        default: break;
    }
}

static void handle_file_browser_input(SDL_GameControllerButton b) {
    if (app.file_name_edit) { handle_file_name_input(b); return; }
    if (app.settings_open) { handle_settings_input(b); return; }
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (g_song_file_count > 0) app.file_idx = (app.file_idx - 1 + g_song_file_count) % g_song_file_count;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (g_song_file_count > 0) app.file_idx = (app.file_idx + 1) % g_song_file_count;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            app.file_action = (app.file_action + 3) % 4;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            app.file_action = (app.file_action + 1) % 4;
            break;
        case SDL_CONTROLLER_BUTTON_A: {
            char path[128];
            if (app.file_action == 0 && g_song_file_count > 0) {
                snprintf(path, sizeof(path), "%s.bukuloop", g_song_files[app.file_idx]);
                if (app.dev) SDL_LockAudioDevice(app.dev);
                load_session(path);
                if (app.dev) SDL_UnlockAudioDevice(app.dev);
                app.file_open = 0;
                app.playing = 0;
            } else if (app.file_action == 1) {
                if (g_song_file_count > 0)
                    start_name_edit(g_song_files[app.file_idx]);
                else
                    start_name_edit("my song");
            } else if (app.file_action == 2) {
                if (app.dev) SDL_LockAudioDevice(app.dev);
                app.pattern_count = 0;
                app.step_pat = 0; app.step_track = 0;
                if (app.dev) SDL_UnlockAudioDevice(app.dev);
                start_name_edit("new song");
            } else if (app.file_action == 3 && g_song_file_count > 0) {
                char clone_nm[64];
                snprintf(clone_nm, sizeof(clone_nm), "%s copy", g_song_files[app.file_idx]);
                start_name_edit(clone_nm);
            }
        } break;
        case SDL_CONTROLLER_BUTTON_X:
            if (g_song_file_count > 0) {
                start_name_edit(g_song_files[app.file_idx]);
                app.file_action = 1;
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (g_song_file_count > 0) {
                char path[128];
                snprintf(path, sizeof(path), "%s.bukuloop", g_song_files[app.file_idx]);
                remove(path);
                scan_song_files();
                if (app.file_idx >= g_song_file_count) app.file_idx = g_song_file_count - 1;
                if (app.file_idx < 0) app.file_idx = 0;
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
            if (app.settings_open) {
                app.settings_open = 0;
            } else {
                app.file_open = 0;
                app.playing = app.was_playing;
            }
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            app.settings_open = !app.settings_open;
            break;
        default: break;
    }
}

/* ===== RENDER DISPATCH ===== */

static void render(SDL_Renderer *r) {
    rect(r, 0, 0, W, H, C_BG);
    SDL_Color scan = {(Uint8)(C_BG.r+8),(Uint8)(C_BG.g+8),(Uint8)(C_BG.b+8),255};
    for (int y = 0; y < H; y += 3) line(r, 0, y, W, y, scan);
    draw_header(r);
    if (app.piano_open && app.page == PAGE_STEP) {
        draw_piano(r);
    } else {
        switch (app.page) {
            case PAGE_SONG:    draw_song(r); break;
            case PAGE_STEP:    draw_step(r); break;
            case PAGE_PERFORM: draw_perform(r); break;
            case PAGE_FX:      draw_fx(r); break;
            default: break;
        }
        if (app.browse_open && app.page == PAGE_STEP) draw_browser(r);
        if (app.synth_open && app.page == PAGE_STEP) draw_synth_lab(r);
        if (app.song_pick_open && app.page == PAGE_SONG) draw_song_picker(r);
    }
    if (app.file_open) {
        draw_file_browser(r);
        if (app.settings_open) draw_settings(r);
    }
}

/* ===== ACTIONS ===== */

static void place_clip(void) {
    if (app.selected_type < 0) return;
    add_clip(app.selected_type, app.selected_ref, app.cursor_lane, app.cursor_bar,
             app.selected_type == 0 ? 2 : 1);
}

static void duplicate_selected_clip(void) {
    int src = cursor_clip();
    if (src < 0 && app.selected_clip >= 0 && app.clips[app.selected_clip].used) src = app.selected_clip;
    if (src < 0) return;
    Clip *c = &app.clips[src];
    int dst_bar = c->bar + c->len;
    if (dst_bar + c->len > BARS) dst_bar = BARS - c->len;
    if (dst_bar < 0) return;
    add_clip(c->type, c->ref, c->lane, dst_bar, c->len);
}

static void clear_lane(void) {
    for (int i = 0; i < MAX_CLIPS; i++) {
        if (app.clips[i].used && app.clips[i].lane == app.cursor_lane)
            app.clips[i].used = 0;
    }
}

static void toggle_selected_type(void) {
    if (app.selected_type == 0 && app.pattern_count > 0) {
        app.selected_type = 1;
        if (app.selected_ref >= app.pattern_count) app.selected_ref = 0;
    } else if (app.sample_count > 0) {
        app.selected_type = 0;
        if (app.selected_ref >= app.sample_count) app.selected_ref = 0;
    }
}

static void cycle_selected_ref(int delta) {
    int count = app.selected_type == 0 ? app.sample_count : app.pattern_count;
    if (count <= 0) return;
    app.selected_ref = (app.selected_ref + delta + count) % count;
}

static void stretch_selected(int delta) {
    int i = cursor_clip();
    if (i < 0) i = app.selected_clip;
    if (i < 0 || !app.clips[i].used) return;
    Clip *c = &app.clips[i];
    c->len += delta;
    if (c->len < 1) c->len = 1;
    if (c->bar + c->len > BARS) c->len = BARS - c->bar;
    app.selected_clip = i;
}

static void delete_at_cursor(void) {
    int i = cursor_clip();
    if (i >= 0) {
        app.clips[i].used = 0;
        return;
    }
    if (app.selected_clip >= 0 && app.clips[app.selected_clip].used)
        app.clips[app.selected_clip].used = 0;
}

#define FX_DETAIL_SLOTS 7

static void adjust_bus_param(int delta, float scale) {
    int bi = app.fx_bus;
    if (bi < 0 || bi >= MAX_BUSES) return;
    MixBus *b = &app.buses[bi];
    float d = (float)delta * scale;
    switch (app.fx_row) {
        case 0: b->drive = clampf(b->drive + d * 0.05f, 0, 1); break;
        case 1:
            if (select_held) b->reso = clampf(b->reso + d * 0.05f, 0, 0.95f);
            else b->cutoff = clampf(b->cutoff + d * 350.0f, 300, 8000);
            break;
        case 2:
            if (select_held) b->crush_bits = clampf(b->crush_bits + d * 0.05f, 0, 1);
            else b->crush_rate = clampf(b->crush_rate + d * 0.05f, 0, 1);
            break;
        case 3:
            if (select_held) b->delay_fb = clampf(b->delay_fb + d * 0.04f, 0, 0.85f);
            else b->delay_time = clampf(b->delay_time + d * 0.025f, 0.04f, 0.75f);
            break;
        case 4:
            if (select_held) b->rev_wet = clampf(b->rev_wet + d * 0.04f, 0, 0.85f);
            else b->rev_decay = clampf(b->rev_decay + d * 0.05f, 0, 1);
            break;
        case 5:
            if (select_held) b->grain_pitch = clampf(b->grain_pitch + d * 0.05f, 0, 1);
            else b->grain_density = clampf(b->grain_density + d * 0.05f, 0, 1);
            break;
        case 6: {
            int m = b->gbeat.mode + delta;
            if (m < 0) m = GBEAT_MODE_COUNT - 1;
            if (m >= GBEAT_MODE_COUNT) m = 0;
            b->gbeat.mode = m;
            b->gbeat.read_pos = (float)b->gbeat.rec_pos;
        } break;
    }
}

static void adjust_bus_param2(int delta, float scale) {
    int bi = app.fx_bus;
    if (bi < 0 || bi >= MAX_BUSES) return;
    MixBus *b = &app.buses[bi];
    float d = (float)delta * scale;
    switch (app.fx_row) {
        case 1: b->reso = clampf(b->reso + d * 0.05f, 0, 0.95f); break;
        case 2: b->crush_bits = clampf(b->crush_bits + d * 0.05f, 0, 1); break;
        case 3: b->delay_wet = clampf(b->delay_wet + d * 0.04f, 0, 0.85f); break;
        case 5: b->grain_size = clampf(b->grain_size + d * 0.05f, 0, 1); break;
        default: break;
    }
}

static void toggle_bus_fx(void) {
    int bi = app.fx_bus;
    if (bi < 0 || bi >= MAX_BUSES) return;
    MixBus *b = &app.buses[bi];
    switch (app.fx_row) {
        case 0: b->drive_on = !b->drive_on; break;
        case 1: b->filter_on = !b->filter_on; break;
        case 2: b->crush_on = !b->crush_on; break;
        case 3: b->delay_on = !b->delay_on; break;
        case 4: b->reverb_on = !b->reverb_on; break;
        case 5: b->grain_on = !b->grain_on; break;
        case 6: b->gbeat_on = !b->gbeat_on; break;
    }
}

static void shift_pattern(int delta) {
    Track *t = cur_track();
    if (!t) return;
    Pattern *pat = &app.patterns[app.step_pat];
    int len = pat->length > 0 ? pat->length : 16;
    int tmp[MAX_PATTERN_STEPS];
    for (int i = 0; i < len; i++) tmp[i] = t->steps[((i - delta) % len + len) % len];
    for (int i = 0; i < len; i++) t->steps[i] = tmp[i];
}

static int step_col_resolved(void) {
    Pattern *pat = &app.patterns[app.step_pat];
    int len = pat->length > 0 ? pat->length : 16;
    if (app.recording && app.playing) {
        float tm = (float)fmod(app.song_time, song_dur());
        int total_step = (int)(tm / step_dur() + 0.5f);
        if (total_step >= 16 * BARS) total_step -= 16 * BARS;
        int col = ((total_step % len) + len) % len;
        app.step_col = col;
        return col;
    }
    int col = app.step_col;
    if (col < 0) col = 0;
    if (col >= len) col = len - 1;
    return col;
}

static void toggle_step(void) {
    Track *t = cur_track();
    if (!t) return;
    int col = step_col_resolved();
    t->steps[col] = t->steps[col] > 0 ? 0 : 1;
}

static void toggle_accent(void) {
    Track *t = cur_track();
    if (!t) return;
    int col = step_col_resolved();
    if (t->steps[col] == 2) t->steps[col] = 1;
    else t->steps[col] = 2;
}

static void clear_pattern_steps(void) {
    Track *t = cur_track();
    if (!t) return;
    Pattern *pat = &app.patterns[app.step_pat];
    int len = pat->length > 0 ? pat->length : 16;
    for (int i = 0; i < len; i++) t->steps[i] = 0;
}

static void add_blank_pattern(void) {
    if (app.pattern_count >= MAX_PATTERNS) return;
    Pattern *pat = &app.patterns[app.pattern_count++];
    memset(pat, 0, sizeof(*pat));
    snprintf(pat->name, sizeof(pat->name), "pat %d", app.pattern_count);
    pat->length = 16;
    pat->track_count = 1;
    init_track(&pat->tracks[0], "kick", INST_KICK, 36, NULL);
    app.step_pat = app.pattern_count - 1;
    app.step_track = 0;
}

static int pattern_total_sources(void) { return INST_COUNT + app.sample_count; }

static void set_track_source(Track *t, int idx) {
    int total = pattern_total_sources();
    if (total <= 0) return;
    idx = ((idx % total) + total) % total;
    if (idx < INST_COUNT) {
        t->inst = idx;
        t->note = default_note_for(idx);
        t->sample_ref = -1;
    } else {
        t->sample_ref = idx - INST_COUNT;
    }
}

static int track_source_index(Track *t) {
    if (t->sample_ref >= 0) return INST_COUNT + t->sample_ref;
    return t->inst;
}

static void cycle_track_source(int delta) {
    Track *t = cur_track();
    if (!t) return;
    set_track_source(t, track_source_index(t) + delta);
}

static int layer_source_index(PatternLayer *L) {
    if (L->sample_ref >= 0) return INST_COUNT + L->sample_ref;
    return L->inst;
}

static void set_layer_source(PatternLayer *L, int idx) {
    int total = pattern_total_sources();
    if (total <= 0) return;
    idx = ((idx % total) + total) % total;
    if (idx < INST_COUNT) {
        L->inst = idx;
        L->sample_ref = -1;
    } else {
        L->sample_ref = idx - INST_COUNT;
    }
}

static void cycle_layer_source(int layer_idx, int delta) {
    Track *t = cur_track();
    if (!t) return;
    if (layer_idx < 0 || layer_idx >= PATTERN_LAYER_COUNT) return;
    set_layer_source(&t->layers[layer_idx], layer_source_index(&t->layers[layer_idx]) + delta);
}

static void toggle_layer_active(int layer_idx) {
    Track *t = cur_track();
    if (!t) return;
    if (layer_idx < 0 || layer_idx >= PATTERN_LAYER_COUNT) return;
    t->layers[layer_idx].active = !t->layers[layer_idx].active;
}

static const char *layer_label(PatternLayer *L) {
    if (L->sample_ref >= 0 && L->sample_ref < app.sample_count) return app.samples[L->sample_ref].name;
    if (L->inst >= 0 && L->inst < INST_COUNT) return INST_NAMES[L->inst];
    return "?";
}

static SDL_Color layer_color(PatternLayer *L) {
    if (L->sample_ref >= 0) return C_GOLD;
    return INST_COLOR[L->inst];
}

static void fire_track_layers(Track *t, int step_idx, float vel, float pitch_mul) {
    for (int li = 0; li < PATTERN_LAYER_COUNT; li++) {
        PatternLayer *L = &t->layers[li];
        if (!L->active) continue;
        float lvel = vel * L->gain;
        if (L->sample_ref >= 0 && L->sample_ref < app.sample_count) {
            float lpitch = powf(2.0f, (float)(t->step_notes[step_idx] + L->note_delta) / 12.0f);
            trigger_voice_full(VK_SAMPLE, L->sample_ref, 0, 0, 0, lpitch, lvel,
                t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, 0, t->bus);
        } else {
            int n = t->note + t->step_notes[step_idx] + L->note_delta;
            trigger_voice_full(VK_SYNTH, -1, 0, 0, L->inst, midi_hz(n) * pitch_mul, lvel,
                t->lfo_rate, t->lfo_depth, t->attack_mul, t->decay_mul, 0, t->bus);
        }
    }
}

static void bind_selection_to_pattern(void) {
    Track *t = cur_track();
    if (!t) return;
    if (app.selected_type == 0 && app.selected_ref >= 0 && app.selected_ref < app.sample_count) {
        t->sample_ref = app.selected_ref;
    }
}

static SDL_Color pitch_shift_color(SDL_Color base, int delta) {
    float s = (float)delta / 12.0f;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    SDL_Color out = base;
    if (s > 0) {
        out.r = (Uint8)clampf(base.r + (255 - base.r) * s * 0.65f, 0, 255);
        out.g = (Uint8)clampf(base.g * (1.0f - s * 0.35f), 0, 255);
        out.b = (Uint8)clampf(base.b * (1.0f - s * 0.5f), 0, 255);
    } else {
        s = -s;
        out.r = (Uint8)clampf(base.r * (1.0f - s * 0.55f), 0, 255);
        out.g = (Uint8)clampf(base.g * (1.0f - s * 0.25f), 0, 255);
        out.b = (Uint8)clampf(base.b + (255 - base.b) * s * 0.7f, 0, 255);
    }
    return out;
}

static void cycle_pattern_slices(void) {
    Track *t = cur_track();
    if (!t) return;
    if (t->sample_ref < 0) return;
    int next = 0;
    switch (t->slices) {
        case 0:  next = 4;  break;
        case 4:  next = 8;  break;
        case 8:  next = 16; break;
        case 16: next = 32; break;
        case 32: next = 0;  break;
        default: next = 0;  break;
    }
    t->slices = next;
}

static void duplicate_pattern_extend(void) {
    if (app.pattern_count == 0) return;
    Pattern *pat = &app.patterns[app.step_pat];
    int new_len;
    if (pat->length < 32) new_len = 32;
    else if (pat->length < 64) new_len = 64;
    else return;
    int old = pat->length;
    for (int ti = 0; ti < pat->track_count; ti++) {
        Track *t = &pat->tracks[ti];
        for (int i = old; i < new_len; i++) {
            t->steps[i] = t->steps[i - old];
            t->step_notes[i] = t->step_notes[i - old];
            t->step_prob[i] = t->step_prob[i - old];
            t->step_rand_mode[i] = t->step_rand_mode[i - old];
        }
    }
    pat->length = new_len;
}

static void delete_pattern_at(int idx) {
    if (idx < 0 || idx >= app.pattern_count) return;
    for (int i = idx; i < app.pattern_count - 1; i++) app.patterns[i] = app.patterns[i + 1];
    app.pattern_count--;
    for (int i = 0; i < MAX_CLIPS; i++) {
        Clip *c = &app.clips[i];
        if (!c->used || c->type != 1) continue;
        if (c->ref == idx) c->used = 0;
        else if (c->ref > idx) c->ref--;
    }
    if (app.step_pat >= app.pattern_count) app.step_pat = app.pattern_count - 1;
    if (app.step_pat < 0) app.step_pat = 0;
}

const char *SESSION_PATH = "session.bukuloop";

static int save_session(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "[session]\n");
    fprintf(f, "bpm=%d\n", app.bpm);
    fprintf(f, "master_pitch=%.3f\n", (double)app.master_pitch);
    fprintf(f, "scale=%d\n", app.scale_idx);
    fprintf(f, "root=%d\n", app.scale_root);
    fprintf(f, "swing=%.3f\n", (double)app.swing);
    fprintf(f, "theme=%d\n", app.theme);
    for (int bi = 0; bi < MAX_BUSES; bi++) {
        MixBus *mb = &app.buses[bi];
        fprintf(f, "\n[bus]\n");
        fprintf(f, "volume=%.3f\n", (double)mb->volume);
        fprintf(f, "mute=%d\n", mb->mute);
        fprintf(f, "drive=%.3f\n", (double)mb->drive);
        fprintf(f, "drive_on=%d\n", mb->drive_on);
        fprintf(f, "cutoff=%.1f\n", (double)mb->cutoff);
        fprintf(f, "filter_on=%d\n", mb->filter_on);
        fprintf(f, "delay_time=%.3f\n", (double)mb->delay_time);
        fprintf(f, "delay_fb=%.3f\n", (double)mb->delay_fb);
        fprintf(f, "delay_wet=%.3f\n", (double)mb->delay_wet);
        fprintf(f, "delay_on=%d\n", mb->delay_on);
        fprintf(f, "rev_decay=%.3f\n", (double)mb->rev_decay);
        fprintf(f, "rev_wet=%.3f\n", (double)mb->rev_wet);
        fprintf(f, "reverb_on=%d\n", mb->reverb_on);
        fprintf(f, "gbeat_mode=%d\n", mb->gbeat.mode);
        fprintf(f, "gbeat_on=%d\n", mb->gbeat_on);
        fprintf(f, "reso=%.3f\n", (double)mb->reso);
        fprintf(f, "crush_rate=%.3f\n", (double)mb->crush_rate);
        fprintf(f, "crush_bits=%.3f\n", (double)mb->crush_bits);
        fprintf(f, "crush_on=%d\n", mb->crush_on);
        fprintf(f, "grain_density=%.3f\n", (double)mb->grain_density);
        fprintf(f, "grain_size=%.3f\n", (double)mb->grain_size);
        fprintf(f, "grain_pitch=%.3f\n", (double)mb->grain_pitch);
        fprintf(f, "grain_pos=%.3f\n", (double)mb->grain_pos);
        fprintf(f, "grain_on=%d\n", mb->grain_on);
    }
    for (int pi = 0; pi < app.pattern_count; pi++) {
        Pattern *pat = &app.patterns[pi];
        int len = pat->length > 0 ? pat->length : 16;
        fprintf(f, "\n[pattern]\n");
        fprintf(f, "name=%s\n", pat->name);
        fprintf(f, "length=%d\n", len);
        fprintf(f, "track_count=%d\n", pat->track_count);
        for (int ti = 0; ti < pat->track_count; ti++) {
            Track *t = &pat->tracks[ti];
            fprintf(f, "\n[track]\n");
            fprintf(f, "name=%s\n", t->name);
            fprintf(f, "inst=%d\n", t->inst);
            fprintf(f, "note=%d\n", t->note);
            fprintf(f, "sample_ref=%d\n", t->sample_ref);
            fprintf(f, "slices=%d\n", t->slices);
            fprintf(f, "chop_mode=%d\n", t->chop_mode);
            fprintf(f, "bus=%d\n", t->bus);
            fprintf(f, "lfo_rate=%.3f\n", (double)t->lfo_rate);
            fprintf(f, "lfo_depth=%.3f\n", (double)t->lfo_depth);
            fprintf(f, "attack_mul=%.3f\n", (double)t->attack_mul);
            fprintf(f, "decay_mul=%.3f\n", (double)t->decay_mul);
            if (t->note_rand_range > 0)
                fprintf(f, "note_rand_range=%d\n", t->note_rand_range);
            {
                int has_rand = 0;
                for (int ri = 0; ri < len; ri++) if (t->step_rand_mode[ri]) { has_rand = 1; break; }
                if (has_rand) {
                    fprintf(f, "rand_modes=");
                    for (int ri = 0; ri < len; ri++) fprintf(f, "%d%s", t->step_rand_mode[ri], ri + 1 < len ? "," : "\n");
                }
            }
            if (t->sample_ref >= 0 && t->sample_ref < app.sample_count) {
                fprintf(f, "sample_name=%s\n", app.samples[t->sample_ref].name);
            }
            fprintf(f, "steps=");
            for (int i = 0; i < len; i++) fprintf(f, "%d%s", t->steps[i], i + 1 < len ? "," : "\n");
            fprintf(f, "notes=");
            for (int i = 0; i < len; i++) fprintf(f, "%d%s", t->step_notes[i], i + 1 < len ? "," : "\n");
            fprintf(f, "prob=");
            for (int i = 0; i < len; i++) fprintf(f, "%d%s", t->step_prob[i], i + 1 < len ? "," : "\n");
            for (int li = 0; li < PATTERN_LAYER_COUNT; li++) {
                PatternLayer *L = &t->layers[li];
                fprintf(f, "layer=%d,%d,%d,%d,%.3f\n",
                    L->active, L->sample_ref, L->inst, L->note_delta, (double)L->gain);
            }
        }
    }
    for (int bi = 0; bi < app.beat_count; bi++) {
        Beat *bt = &app.beats[bi];
        if (!bt->active) continue;
        fprintf(f, "\n[beat]\n");
        fprintf(f, "name=%s\n", bt->name);
        fprintf(f, "span=%d\n", bt->span_bars);
        fprintf(f, "count=%d\n", bt->count);
        for (int i = 0; i < bt->count; i++) {
            fprintf(f, "entry=%d,%d,%d,%d,%d\n",
                bt->types[i], bt->refs[i], bt->lanes[i],
                bt->bar_offsets[i], bt->lens[i]);
        }
    }
    for (int ci = 0; ci < MAX_CLIPS; ci++) {
        Clip *c = &app.clips[ci];
        if (!c->used) continue;
        fprintf(f, "\n[clip]\n");
        fprintf(f, "type=%d\n", c->type);
        fprintf(f, "ref=%d\n", c->ref);
        fprintf(f, "lane=%d\n", c->lane);
        fprintf(f, "bar=%d\n", c->bar);
        fprintf(f, "len=%d\n", c->len);
        if (c->type == 0 && c->ref >= 0 && c->ref < app.sample_count) {
            fprintf(f, "sample_name=%s\n", app.samples[c->ref].name);
        }
    }
    fclose(f);
    return 1;
}

static int find_sample_by_name(const char *name) {
    for (int i = 0; i < app.sample_count; i++) {
        if (strcmp(app.samples[i].name, name) == 0) return i;
    }
    return -1;
}

static int load_session(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (app.dev) SDL_LockAudioDevice(app.dev);
    app.pattern_count = 0;
    app.beat_count = 0;
    for (int i = 0; i < MAX_CLIPS; i++) app.clips[i].used = 0;
    for (int i = 0; i < MAX_BEATS; i++) memset(&app.beats[i], 0, sizeof(Beat));
    for (int i = 0; i < MAX_VOICES; i++) app.voices[i].active = 0;
    for (int bi = 0; bi < MAX_BUSES; bi++) {
        MixBus *mb = &app.buses[bi];
        memset(mb->delay_buf, 0, sizeof(mb->delay_buf));
        mb->delay_pos = 0;
        mb->lp_state = 0; mb->bp_state = 0; mb->hp_z1 = 0; mb->hp_z2 = 0;
        mb->crush_hold = 0; mb->crush_phase = 0;
        memset(&mb->reverb, 0, sizeof(mb->reverb));
        reverb_init(&mb->reverb);
        memset(mb->gbeat.rec_buf, 0, sizeof(mb->gbeat.rec_buf));
        mb->gbeat.rec_pos = 0; mb->gbeat.read_pos = 0;
        memset(mb->grain_buf, 0, sizeof(mb->grain_buf));
        mb->grain_wpos = 0;
        for (int gi = 0; gi < 8; gi++) { mb->grain_ages[gi] = 0; mb->grain_voices[gi] = 0; }
    }
    app.selected_clip = -1;
    char line[4096];
    char section[32] = "";
    Pattern *cp = NULL;
    Track *ct = NULL;
    Clip *cc = NULL;
    Beat *cb = NULL;
    MixBus *cmb = NULL;
    int ct_layer_idx = 0;
    int bus_load_idx = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = 0;
            snprintf(section, sizeof(section), "%s", line + 1);
            if (strcmp(section, "pattern") == 0) {
                cc = NULL; cb = NULL; ct = NULL;
                if (app.pattern_count < MAX_PATTERNS) {
                    cp = &app.patterns[app.pattern_count++];
                    memset(cp, 0, sizeof(*cp));
                    cp->length = 16;
                } else cp = NULL;
            } else if (strcmp(section, "track") == 0) {
                cc = NULL; cb = NULL;
                ct_layer_idx = 0;
                if (cp && cp->track_count < MAX_TRACKS) {
                    ct = &cp->tracks[cp->track_count++];
                    memset(ct, 0, sizeof(*ct));
                    ct->sample_ref = -1;
                    ct->attack_mul = 1.0f;
                    ct->decay_mul = 1.0f;
                    for (int i = 0; i < MAX_PATTERN_STEPS; i++) ct->step_prob[i] = 100;
                    for (int li = 0; li < PATTERN_LAYER_COUNT; li++) {
                        ct->layers[li].sample_ref = -1;
                        ct->layers[li].inst = INST_808;
                        ct->layers[li].note_delta = -12;
                        ct->layers[li].gain = 0.65f;
                    }
                } else ct = NULL;
            } else if (strcmp(section, "clip") == 0) {
                cp = NULL; ct = NULL; cb = NULL;
                cc = NULL;
                for (int i = 0; i < MAX_CLIPS; i++) {
                    if (!app.clips[i].used) { cc = &app.clips[i]; memset(cc, 0, sizeof(*cc)); cc->used = 1; break; }
                }
            } else if (strcmp(section, "bus") == 0) {
                cp = NULL; ct = NULL; cc = NULL; cb = NULL;
                if (bus_load_idx < MAX_BUSES) {
                    cmb = &app.buses[bus_load_idx++];
                } else cmb = NULL;
            } else if (strcmp(section, "beat") == 0) {
                cp = NULL; ct = NULL; cc = NULL; cmb = NULL;
                if (app.beat_count < MAX_BEATS) {
                    cb = &app.beats[app.beat_count++];
                    memset(cb, 0, sizeof(*cb));
                    cb->active = 1;
                    cb->span_bars = 4;
                } else cb = NULL;
            } else {
                cp = NULL; ct = NULL; cc = NULL; cb = NULL; cmb = NULL;
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        if (strcmp(section, "session") == 0) {
            if (!strcmp(key, "bpm")) app.bpm = atoi(val);
            else if (!strcmp(key, "master_pitch")) app.master_pitch = (float)atof(val);
            else if (!strcmp(key, "scale")) app.scale_idx = atoi(val);
            else if (!strcmp(key, "root")) app.scale_root = atoi(val);
            else if (!strcmp(key, "swing")) app.swing = clampf((float)atof(val), 0.5f, 0.75f);
            else if (!strcmp(key, "theme")) { app.theme = atoi(val); apply_theme(app.theme); }
        } else if (cp && !strcmp(section, "pattern")) {
            if (!strcmp(key, "name")) snprintf(cp->name, sizeof(cp->name), "%s", val);
            else if (!strcmp(key, "length")) {
                cp->length = atoi(val);
                if (cp->length < 16) cp->length = 16;
                if (cp->length > MAX_PATTERN_STEPS) cp->length = MAX_PATTERN_STEPS;
            }
            else if (!strcmp(key, "track_count")) { /* informational */ }
        } else if (ct && !strcmp(section, "track")) {
            if (!strcmp(key, "name")) snprintf(ct->name, sizeof(ct->name), "%s", val);
            else if (!strcmp(key, "inst")) ct->inst = atoi(val);
            else if (!strcmp(key, "note")) ct->note = atoi(val);
            else if (!strcmp(key, "sample_ref")) ct->sample_ref = atoi(val);
            else if (!strcmp(key, "sample_name")) {
                int idx = find_sample_by_name(val);
                if (idx >= 0) ct->sample_ref = idx;
            }
            else if (!strcmp(key, "slices")) ct->slices = atoi(val);
            else if (!strcmp(key, "chop_mode")) ct->chop_mode = atoi(val);
            else if (!strcmp(key, "bus")) ct->bus = atoi(val);
            else if (!strcmp(key, "lfo_rate")) ct->lfo_rate = (float)atof(val);
            else if (!strcmp(key, "lfo_depth")) ct->lfo_depth = (float)atof(val);
            else if (!strcmp(key, "attack_mul")) ct->attack_mul = (float)atof(val);
            else if (!strcmp(key, "decay_mul")) ct->decay_mul = (float)atof(val);
            else if (!strcmp(key, "note_rand_range")) ct->note_rand_range = atoi(val);
            else if (!strcmp(key, "rand_modes")) {
                char *tok = strtok(val, ",");
                int i = 0;
                while (tok && i < MAX_PATTERN_STEPS) { ct->step_rand_mode[i++] = atoi(tok); tok = strtok(NULL, ","); }
            }
            else if (!strcmp(key, "steps")) {
                char *tok = strtok(val, ",");
                int i = 0;
                while (tok && i < MAX_PATTERN_STEPS) { ct->steps[i++] = atoi(tok); tok = strtok(NULL, ","); }
            }
            else if (!strcmp(key, "notes")) {
                char *tok = strtok(val, ",");
                int i = 0;
                while (tok && i < MAX_PATTERN_STEPS) { ct->step_notes[i++] = atoi(tok); tok = strtok(NULL, ","); }
            }
            else if (!strcmp(key, "prob")) {
                char *tok = strtok(val, ",");
                int i = 0;
                while (tok && i < MAX_PATTERN_STEPS) { ct->step_prob[i++] = atoi(tok); tok = strtok(NULL, ","); }
            }
            else if (!strcmp(key, "layer") && ct_layer_idx < PATTERN_LAYER_COUNT) {
                int la, sr, in, nd; float g;
                if (sscanf(val, "%d,%d,%d,%d,%f", &la, &sr, &in, &nd, &g) == 5) {
                    PatternLayer *L = &ct->layers[ct_layer_idx];
                    L->active = la; L->sample_ref = sr; L->inst = in;
                    L->note_delta = nd; L->gain = g;
                }
                ct_layer_idx++;
            }
        } else if (cb && !strcmp(section, "beat")) {
            if (!strcmp(key, "name")) snprintf(cb->name, sizeof(cb->name), "%s", val);
            else if (!strcmp(key, "span")) cb->span_bars = atoi(val);
            else if (!strcmp(key, "count")) { /* informational */ }
            else if (!strcmp(key, "entry") && cb->count < MAX_BEAT_ENTRIES) {
                int t,re,la,bo,le;
                if (sscanf(val, "%d,%d,%d,%d,%d", &t, &re, &la, &bo, &le) == 5) {
                    int i = cb->count++;
                    cb->types[i] = t; cb->refs[i] = re;
                    cb->lanes[i] = la; cb->bar_offsets[i] = bo;
                    cb->lens[i] = le;
                }
            }
        } else if (cmb && !strcmp(section, "bus")) {
            if (!strcmp(key, "volume")) cmb->volume = (float)atof(val);
            else if (!strcmp(key, "mute")) cmb->mute = atoi(val);
            else if (!strcmp(key, "drive")) cmb->drive = (float)atof(val);
            else if (!strcmp(key, "drive_on")) cmb->drive_on = atoi(val);
            else if (!strcmp(key, "cutoff")) cmb->cutoff = (float)atof(val);
            else if (!strcmp(key, "filter_on")) cmb->filter_on = atoi(val);
            else if (!strcmp(key, "delay_time")) cmb->delay_time = (float)atof(val);
            else if (!strcmp(key, "delay_fb")) cmb->delay_fb = (float)atof(val);
            else if (!strcmp(key, "delay_wet")) cmb->delay_wet = (float)atof(val);
            else if (!strcmp(key, "delay_on")) cmb->delay_on = atoi(val);
            else if (!strcmp(key, "rev_decay")) cmb->rev_decay = (float)atof(val);
            else if (!strcmp(key, "rev_wet")) cmb->rev_wet = (float)atof(val);
            else if (!strcmp(key, "reverb_on")) cmb->reverb_on = atoi(val);
            else if (!strcmp(key, "gbeat_mode")) cmb->gbeat.mode = atoi(val);
            else if (!strcmp(key, "gbeat_on")) cmb->gbeat_on = atoi(val);
            else if (!strcmp(key, "reso")) cmb->reso = (float)atof(val);
            else if (!strcmp(key, "crush_rate")) cmb->crush_rate = (float)atof(val);
            else if (!strcmp(key, "crush_bits")) cmb->crush_bits = (float)atof(val);
            else if (!strcmp(key, "crush_on")) cmb->crush_on = atoi(val);
            else if (!strcmp(key, "grain_density")) cmb->grain_density = (float)atof(val);
            else if (!strcmp(key, "grain_size")) cmb->grain_size = (float)atof(val);
            else if (!strcmp(key, "grain_pitch")) cmb->grain_pitch = (float)atof(val);
            else if (!strcmp(key, "grain_pos")) cmb->grain_pos = (float)atof(val);
            else if (!strcmp(key, "grain_on")) cmb->grain_on = atoi(val);
        } else if (cc && !strcmp(section, "clip")) {
            if (!strcmp(key, "type")) cc->type = atoi(val);
            else if (!strcmp(key, "ref")) cc->ref = atoi(val);
            else if (!strcmp(key, "lane")) cc->lane = atoi(val);
            else if (!strcmp(key, "bar")) cc->bar = atoi(val);
            else if (!strcmp(key, "len")) cc->len = atoi(val);
            else if (!strcmp(key, "sample_name") && cc->type == 0) {
                int idx = find_sample_by_name(val);
                if (idx >= 0) cc->ref = idx;
            }
        }
    }
    fclose(f);
    if (app.dev) SDL_UnlockAudioDevice(app.dev);
    return 1;
}

static void save_beat_from_cursor(int span) {
    if (app.beat_count >= MAX_BEATS) return;
    Beat *b = &app.beats[app.beat_count];
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "beat %d", app.beat_count + 1);
    b->span_bars = span;
    int start = app.cursor_bar;
    for (int i = 0; i < MAX_CLIPS && b->count < MAX_BEAT_ENTRIES; i++) {
        Clip *c = &app.clips[i];
        if (!c->used) continue;
        if (c->bar >= start && c->bar < start + span) {
            b->types[b->count] = c->type;
            b->refs[b->count] = c->ref;
            b->lanes[b->count] = c->lane;
            b->bar_offsets[b->count] = c->bar - start;
            b->lens[b->count] = c->len;
            b->count++;
        }
    }
    if (b->count == 0) return;
    b->active = 1;
    app.beat_count++;
}

static void drop_beat_at_cursor(Beat *b) {
    if (!b || !b->active) return;
    for (int i = 0; i < b->count; i++) {
        int bar = app.cursor_bar + b->bar_offsets[i];
        if (bar < 0 || bar + b->lens[i] > BARS) continue;
        add_clip(b->types[i], b->refs[i], b->lanes[i], bar, b->lens[i]);
    }
}

static void delete_beat_at(int idx) {
    if (idx < 0 || idx >= app.beat_count) return;
    for (int i = idx; i < app.beat_count - 1; i++) app.beats[i] = app.beats[i + 1];
    app.beat_count--;
    memset(&app.beats[app.beat_count], 0, sizeof(Beat));
}

static void stop_and_rewind(void) {
    app.playing = 0;
    app.song_time = 0;
    app.last_played_step = -1;
    app.last_played_half = -1;
}

static void nudge_pattern_note(int delta) {
    Track *t = cur_track();
    if (!t) return;
    int n = snap_note(t->note, delta > 0 ? 1 : -1, app.scale_idx, app.scale_root);
    if (n < 12) n = 12;
    if (n > 96) n = 96;
    t->note = n;
}

static void nudge_step_note(int delta) {
    Track *t = cur_track();
    if (!t) return;
    Pattern *pat = &app.patterns[app.step_pat];
    int col = app.step_col;
    int len = pat->length > 0 ? pat->length : 16;
    if (col < 0 || col >= len) return;
    if (t->steps[col] == 0) {
        nudge_pattern_note(delta);
        return;
    }
    int current = t->note + t->step_notes[col];
    int dir = delta > 0 ? 1 : -1;
    int snapped = snap_note(current, dir, app.scale_idx, app.scale_root);
    int new_delta = snapped - t->note;
    if (new_delta < -36) new_delta = -36;
    if (new_delta > 36) new_delta = 36;
    t->step_notes[col] = new_delta;
    preview_track_step(t, t->steps[col]);
}

static void next_page(int delta) {
    /* Performance flow: cycle STEP → PERFORM → FX. SONG (arrangement) is
     * still in the enum for save-file compat, but it's no longer in the
     * shoulder-button rotation. */
    int p = (int)app.page;
    int step = delta > 0 ? 1 : -1;
    for (int i = 0; i < PAGE_COUNT; i++) {
        p = (p + step + PAGE_COUNT) % PAGE_COUNT;
        if (p != PAGE_SONG) { app.page = (Page)p; return; }
    }
}

static void bpm_nudge(int delta) {
    app.bpm = (int)clampf((float)(app.bpm + delta), 60, 220);
}

/* ===== INPUT HANDLERS ===== */

static void on_button_song(SDL_GameControllerButton b) {
    if (app.song_pick_open) { handle_song_picker_input(b); return; }
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A:
            if (select_held) duplicate_selected_clip();
            else place_clip();
            break;
        case SDL_CONTROLLER_BUTTON_B:
            if (select_held) clear_lane();
            else delete_at_cursor();
            break;
        case SDL_CONTROLLER_BUTTON_X: stretch_selected(-1); break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (select_held) stretch_selected(1);
            else {
                app.song_pick_open = 1;
                app.song_pick_idx = 0;
            }
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (select_held) bpm_nudge(-5);
            else next_page(-1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (select_held) bpm_nudge(5);
            else next_page(1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (app.cursor_bar > 0) app.cursor_bar--;
            update_selected_from_cursor();
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (app.cursor_bar < BARS - 1) app.cursor_bar++;
            update_selected_from_cursor();
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (app.cursor_lane > 0) app.cursor_lane--;
            update_selected_from_cursor();
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (app.cursor_lane < LANES - 1) app.cursor_lane++;
            update_selected_from_cursor();
            break;
        default: break;
    }
}

static void on_button_step(SDL_GameControllerButton b) {
    if (app.piano_open) { handle_piano_input(b); return; }
    if (app.browse_open) { handle_browser_input(b); return; }
    if (app.synth_open) { handle_synth_lab_input(b); return; }
    if (app.pattern_count == 0) {
        if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) next_page(-1);
        else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) next_page(1);
        return;
    }
    Pattern *pat = &app.patterns[app.step_pat];
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A:
            if (app.step_col < 0) {
                Track *bt = cur_track();
                if (bt) { int nb = bt->bus + 1; bt->bus = nb >= MAX_BUSES ? -1 : nb; }
            } else if (select_held) toggle_accent();
            else toggle_step();
            break;
        case SDL_CONTROLLER_BUTTON_B:
            if (select_held) {
                if (pat->track_count > 1) {
                    for (int i = app.step_track; i < pat->track_count - 1; i++)
                        pat->tracks[i] = pat->tracks[i + 1];
                    pat->track_count--;
                    if (app.step_track >= pat->track_count) app.step_track = pat->track_count - 1;
                }
            } else {
                if (pat->track_count < MAX_TRACKS) {
                    char nm[32];
                    snprintf(nm, sizeof(nm), "trk %d", pat->track_count + 1);
                    add_track_to_pattern(pat, nm, INST_KICK, 36, NULL);
                    app.step_track = pat->track_count - 1;
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            if (select_held) clear_pattern_steps();
            else {
                app.piano_open = 1;
                app.piano_row = 11;
                app.piano_col = app.step_col;
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (select_held) { app.synth_open = 1; app.synth_focus = 0; }
            else {
                app.browse_open = 1;
                app.browse_panel = 0;
                rebuild_folder_index();
                Track *bt = cur_track();
                if (bt) app.browse_idx = track_source_index(bt);
            }
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (select_held) bpm_nudge(-5);
            else next_page(-1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (select_held) bpm_nudge(5);
            else next_page(1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: {
            int len = pat->length > 0 ? pat->length : 16;
            if (app.step_col <= 0) app.step_col = app.step_col < 0 ? len - 1 : -1;
            else app.step_col--;
        } break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
            int len = pat->length > 0 ? pat->length : 16;
            app.step_col++;
            if (app.step_col >= len) app.step_col = -1;
        } break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (select_held) nudge_step_note(1);
            else if (pat->track_count > 0)
                app.step_track = (app.step_track - 1 + pat->track_count) % pat->track_count;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (select_held) nudge_step_note(-1);
            else if (pat->track_count > 0)
                app.step_track = (app.step_track + 1) % pat->track_count;
            break;
        default: break;
    }
}

static void on_button_perform(SDL_GameControllerButton b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (app.perf_cursor > 0) app.perf_cursor--;
            else if (app.pattern_count > 0) app.perf_cursor = app.pattern_count - 1;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (app.perf_cursor < app.pattern_count - 1) app.perf_cursor++;
            else app.perf_cursor = 0;
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (select_held) {
                app.chain_loop = !app.chain_loop;
            } else if (app.pat_mode == 1) {
                if (app.chain_len < MAX_CHAIN)
                    app.chain[app.chain_len++] = app.perf_cursor;
            } else {
                if (app.perf_cursor != app.step_pat)
                    app.queued_pat = app.perf_cursor;
                else
                    app.queued_pat = -1;
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
            app.pat_mode = (app.pat_mode + 1) % 3;
            if (app.pat_mode == 1) { app.chain_len = 0; app.chain_pos = 0; }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            if (select_held) {
                app.chain_len = 0;
                app.chain_pos = 0;
            } else {
                app.step_pat = app.perf_cursor;
                app.queued_pat = -1;
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (select_held) {
                if (app.pattern_count < MAX_PATTERNS && app.perf_cursor >= 0 && app.perf_cursor < app.pattern_count) {
                    app.patterns[app.pattern_count] = app.patterns[app.perf_cursor];
                    Pattern *dup = &app.patterns[app.pattern_count];
                    snprintf(dup->name, sizeof(dup->name), "%.56s CP", app.patterns[app.perf_cursor].name);
                    app.pattern_count++;
                    app.perf_cursor = app.pattern_count - 1;
                }
            } else {
                add_blank_pattern();
                app.perf_cursor = app.pattern_count - 1;
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  bpm_nudge(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: bpm_nudge(1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (select_held) bpm_nudge(-5); else next_page(-1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (select_held) bpm_nudge(5); else next_page(1); break;
        default: break;
    }
}

static void on_button_fx(SDL_GameControllerButton b) {
    if (app.fx_mode == 0) {
        /* Mixer view */
        Pattern *pat = app.pattern_count > 0 ? &app.patterns[app.step_pat] : NULL;
        int tc = pat ? pat->track_count : 0;
        switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (tc > 0) app.fx_row = (app.fx_row - 1 + tc) % tc; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (tc > 0) app.fx_row = (app.fx_row + 1) % tc; break;
            case SDL_CONTROLLER_BUTTON_A:
                if (select_held) {
                    if (pat && app.fx_row < tc && pat->tracks[app.fx_row].bus >= 0)
                        app.buses[pat->tracks[app.fx_row].bus].mute = !app.buses[pat->tracks[app.fx_row].bus].mute;
                } else if (pat && app.fx_row < tc) {
                    int nb = pat->tracks[app.fx_row].bus + 1;
                    if (nb >= MAX_BUSES) nb = -1;
                    pat->tracks[app.fx_row].bus = nb;
                }
                break;
            case SDL_CONTROLLER_BUTTON_X:
                if (pat && app.fx_row < tc && pat->tracks[app.fx_row].bus >= 0) {
                    app.fx_bus = pat->tracks[app.fx_row].bus;
                    app.fx_mode = 1;
                    app.fx_row = 0;
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
                float d = (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 0.05f : -0.05f;
                if (pat && app.fx_row < tc) {
                    int bi = pat->tracks[app.fx_row].bus;
                    if (bi >= 0 && bi < MAX_BUSES)
                        app.buses[bi].volume = clampf(app.buses[bi].volume + d, 0, 1);
                }
            } break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                if (select_held) bpm_nudge(-5); else next_page(-1); break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                if (select_held) bpm_nudge(5); else next_page(1); break;
            default: break;
        }
    } else {
        /* Bus detail view */
        switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                app.fx_row = (app.fx_row - 1 + FX_DETAIL_SLOTS) % FX_DETAIL_SLOTS; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                app.fx_row = (app.fx_row + 1) % FX_DETAIL_SLOTS; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: adjust_bus_param(-1, 1.0f); break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: adjust_bus_param(1, 1.0f); break;
            case SDL_CONTROLLER_BUTTON_A:
                if (select_held) toggle_bus_fx();
                else adjust_bus_param(1, 4.0f);
                break;
            case SDL_CONTROLLER_BUTTON_B: adjust_bus_param(-1, 4.0f); break;
            case SDL_CONTROLLER_BUTTON_X:
                app.fx_mode = 0; app.fx_row = 0; break;
            case SDL_CONTROLLER_BUTTON_Y:
                app.fx_bus = (app.fx_bus + 1) % MAX_BUSES; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                if (select_held) bpm_nudge(-5); else next_page(-1); break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                if (select_held) bpm_nudge(5); else next_page(1); break;
            default: break;
        }
    }
}

static void on_axis_l2r2(int delta) {
    if (app.page == PAGE_FX && app.fx_mode == 1) {
        if (select_held) adjust_bus_param2(delta, 0.25f);
        else adjust_bus_param(delta, 0.25f);
    }
    else if (app.page == PAGE_SONG) cycle_selected_ref(delta);
    else if (app.page == PAGE_STEP) {
        if (select_held) shift_pattern(delta);
        else cycle_track_source(delta);
    }
    else if (app.page == PAGE_PERFORM) {
        /* L2/R2 on perform: scroll sequences */
        if (delta > 0 && app.perf_cursor < app.seq_count - 1) app.perf_cursor++;
        else if (delta < 0 && app.perf_cursor > 0) app.perf_cursor--;
    }
}

/* ===== DEFAULTS ===== */

static void seed_defaults(void) {
    int p_kick[16]  = {1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,0};
    int p_hat[16]   = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
    int p_snare[16] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0};
    int p_clap[16]  = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0};
    int p_808[16]   = {1,0,0,0, 0,0,0,1, 0,0,0,0, 1,0,0,0};
    int p_lead[16]  = {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0};

    if (app.pattern_count >= MAX_PATTERNS) return;
    Pattern *pat = &app.patterns[app.pattern_count++];
    memset(pat, 0, sizeof(*pat));
    snprintf(pat->name, sizeof(pat->name), "trap beat");
    pat->length = 16;
    add_track_to_pattern(pat, "kick",  INST_KICK,  36, p_kick);
    add_track_to_pattern(pat, "hat",   INST_HAT,   42, p_hat);
    add_track_to_pattern(pat, "snare", INST_SNARE, 38, p_snare);
    add_track_to_pattern(pat, "clap",  INST_CLAP,  39, p_clap);
    add_track_to_pattern(pat, "808",   INST_808,   28, p_808);
    add_track_to_pattern(pat, "lead",  INST_LEAD,  60, p_lead);
    /* All tracks default to DIRECT — send to a bus manually from mixer */

    app.step_pat = 0;
    app.step_track = 0;
    app.selected_clip = -1;
    app.cursor_lane = 0;
    app.cursor_bar = 0;
    update_selected_from_cursor();

    snprintf(app.sequences[0].name, sizeof(app.sequences[0].name), "ALL");
    for (int i = 0; i < MAX_PATTERNS; i++) app.sequences[0].pat_active[i] = 1;
    app.seq_count = 1;
    app.cur_seq = 0;
    app.queued_seq = -1;
    app.perf_cursor = 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    memset(&app, 0, sizeof(app));
    app.bpm = 140;
    app.song_bars = BARS;
    app.selected_type = -1;
    app.selected_clip = -1;
    for (int bi = 0; bi < MAX_BUSES; bi++) {
        MixBus *b = &app.buses[bi];
        b->volume = 0.85f;
        b->drive = 0.18f;
        b->cutoff = 4800.0f;
        b->delay_time = 0.18f;
        b->delay_fb = 0.28f;
        b->delay_wet = 0.14f;
        b->rev_decay = 0.5f;
        b->rev_wet = 0.25f;
        b->crush_bits = 0.5f;
        b->crush_rate = 0.5f;
        b->grain_size = 0.3f;
        reverb_init(&b->reverb);
    }
    app.page = PAGE_STEP;
    app.last_played_step = -1;
    app.last_played_half = -1;
    app.master_pitch = 1.0f;
    app.scale_idx = 0;
    app.scale_root = 0;
    app.swing = 0.5f;
    app.recording = 0;
    app.queued_seq = -1;
    app.queued_pat = -1;
    app.pat_mode = 0;
    app.chain_loop = 1;
    app.fx_mode = 0;
    app.fx_bus = 0;
    app.fx_row = 0;
    app.theme = 0;
    apply_theme(0);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            app.controller = SDL_GameControllerOpen(i);
            if (app.controller) break;
        }
    }
    make_break_sample();
    seed_defaults();
    scan_dir_recursive("samples", 0);
    load_session(SESSION_PATH);
    /* Always boot into the performance flow, even if a saved session was on SONG. */
    if (app.page == PAGE_SONG) app.page = PAGE_STEP;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_cb;
    want.userdata = &app;
    app.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!app.dev) fprintf(stderr, "audio failed: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(app.dev, 0);

    SDL_Window *win = SDL_CreateWindow("BukuLoops",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    SDL_Renderer *r = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !r) {
        fprintf(stderr, "video failed: %s\n", SDL_GetError());
        return 1;
    }

    int run = 1;
    int last_axis_l = 0, last_axis_r = 0;
    int last_rstick_y = 0;
    int last_lstick_x = 0;
    int last_rstick_x = 0;
    Uint32 last_pause_tick = 0;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = 0;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) run = 0;
                else if (k == SDLK_SPACE) app.playing = !app.playing;
                else if (k == SDLK_q) next_page(-1);
                else if (k == SDLK_e) next_page(1);
                else if (k == SDLK_TAB) next_page(1);
            }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                SDL_GameControllerButton b = (SDL_GameControllerButton)e.cbutton.button;
                if (b == SDL_CONTROLLER_BUTTON_BACK) select_held = 1;
                if (b == SDL_CONTROLLER_BUTTON_START) start_held = 1;
                if ((b == SDL_CONTROLLER_BUTTON_START && select_held) ||
                    (b == SDL_CONTROLLER_BUTTON_BACK && start_held)) { run = 0; continue; }
                if (b == SDL_CONTROLLER_BUTTON_B && start_held) {
                    if (app.file_open) {
                        app.file_open = 0;
                        app.settings_open = 0;
                    } else {
                        app.file_open = 1;
                        app.settings_open = 0;
                        app.was_playing = app.playing;
                        app.playing = 0;
                        scan_song_files();
                        app.file_idx = 0;
                    }
                    continue;
                }
                if (app.file_open) { handle_file_browser_input(b); continue; }
                if (b == SDL_CONTROLLER_BUTTON_X && start_held && app.page == PAGE_STEP) {
                    duplicate_pattern_extend();
                    continue;
                }
                /* START+Y on STEP was chop — now L3 only */
                if (b == SDL_CONTROLLER_BUTTON_Y && start_held && app.page == PAGE_SONG) {
                    save_beat_from_cursor(4);
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_A && start_held) {
                    if (save_session(SESSION_PATH)) g_save_flash = 60;
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER && start_held) {
                    app.recording = !app.recording;
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_START) {
                    Uint32 now = SDL_GetTicks();
                    if (!app.playing && last_pause_tick && now - last_pause_tick < 550) {
                        stop_and_rewind();
                        last_pause_tick = 0;
                    } else if (app.playing) {
                        app.playing = 0;
                        last_pause_tick = now;
                    } else {
                        app.playing = 1;
                    }
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_BACK) {
                    if (app.page == PAGE_SONG) toggle_selected_type();
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_LEFTSTICK && app.page == PAGE_STEP) {
                    Track *lt = cur_track();
                    if (lt) {
                        int dir = select_held ? -1 : 1;
                        if (dir > 0) {
                            if (lt->slices > 0) {
                                lt->chop_mode = (lt->chop_mode + 1) % 3;
                                if (lt->chop_mode == 0) cycle_pattern_slices();
                            } else {
                                cycle_pattern_slices();
                            }
                        } else {
                            if (lt->slices > 0 && lt->chop_mode > 0) {
                                lt->chop_mode--;
                            } else if (lt->slices > 0) {
                                int prev = 0;
                                switch (lt->slices) {
                                    case 4: prev = 0; break;
                                    case 8: prev = 4; break;
                                    case 16: prev = 8; break;
                                    case 32: prev = 16; break;
                                    default: prev = 0; break;
                                }
                                lt->slices = prev;
                                lt->chop_mode = prev > 0 ? 2 : 0;
                            }
                        }
                    }
                    continue;
                }
                if (b == SDL_CONTROLLER_BUTTON_RIGHTSTICK && app.page == PAGE_STEP) {
                    Track *rst = cur_track();
                    if (rst) {
                        Pattern *rpat = &app.patterns[app.step_pat];
                        int col = app.step_col;
                        int plen = rpat->length > 0 ? rpat->length : 16;
                        if (col >= 0 && col < plen && rst->steps[col] > 0) {
                            rst->step_rand_mode[col] = (rst->step_rand_mode[col] + 1) % 4;
                            if (rst->step_rand_mode[col] > 0 && rst->note_rand_range == 0)
                                rst->note_rand_range = 3;
                        }
                    }
                    continue;
                }
                switch (app.page) {
                    case PAGE_SONG:    on_button_song(b); break;
                    case PAGE_STEP:    on_button_step(b); break;
                    case PAGE_PERFORM: on_button_perform(b); break;
                    case PAGE_FX:      on_button_fx(b); break;
                    default: break;
                }
            }
            else if (e.type == SDL_CONTROLLERBUTTONUP) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) select_held = 0;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) start_held = 0;
            }
            else if (e.type == SDL_CONTROLLERAXISMOTION && !app.file_open) {
                int v = e.caxis.value;
                int threshold = 18000;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                    int now = v > threshold ? 1 : 0;
                    if (now && !last_axis_l) on_axis_l2r2(-1);
                    last_axis_l = now;
                } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    int now = v > threshold ? 1 : 0;
                    if (now && !last_axis_r) on_axis_l2r2(1);
                    last_axis_r = now;
                }
                /* Right stick Y-axis: adjust step probability (debounced — one nudge per deflection) */
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY && app.page == PAGE_STEP) {
                    int now_y = (v > threshold) ? 1 : (v < -threshold) ? -1 : 0;
                    if (now_y != 0 && now_y != last_rstick_y) {
                        Track *rst = cur_track();
                        if (rst) {
                            Pattern *rpat = &app.patterns[app.step_pat];
                            int col = app.step_col;
                            int plen = rpat->length > 0 ? rpat->length : 16;
                            if (col >= 0 && col < plen) {
                                if (now_y < 0)
                                    rst->step_prob[col] = rst->step_prob[col] + 5 > 100 ? 100 : rst->step_prob[col] + 5;
                                else
                                    rst->step_prob[col] = rst->step_prob[col] - 5 < 0 ? 0 : rst->step_prob[col] - 5;
                            }
                        }
                    }
                    last_rstick_y = now_y;
                }
                /* Right stick X-axis: scroll step grid horizontally */
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX && app.page == PAGE_STEP) {
                    Pattern *sp = app.pattern_count > 0 ? &app.patterns[app.step_pat] : NULL;
                    int splen = sp ? (sp->length > 0 ? sp->length : 16) : 16;
                    if (v > 8000) {
                        app.step_scroll_x += 1 + (v - 8000) / 10000;
                    } else if (v < -8000) {
                        app.step_scroll_x -= 1 + (-v - 8000) / 10000;
                    }
                    if (app.step_scroll_x < 0) app.step_scroll_x = 0;
                    if (app.step_scroll_x > splen - 16) app.step_scroll_x = splen - 16;
                    if (app.step_scroll_x < 0) app.step_scroll_x = 0;
                }
                /* Left stick X-axis: select chop slice on current step */
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX && app.page == PAGE_STEP) {
                    int now_x = (v > threshold) ? 1 : (v < -threshold) ? -1 : 0;
                    if (now_x != 0 && now_x != last_lstick_x) {
                        Track *lst = cur_track();
                        if (lst && lst->slices > 0 && lst->sample_ref >= 0) {
                            int col = app.step_col < 0 ? 0 : app.step_col;
                            Pattern *lpat = &app.patterns[app.step_pat];
                            int plen = lpat->length > 0 ? lpat->length : 16;
                            if (col >= 0 && col < plen) {
                                int si = lst->step_notes[col] + now_x;
                                if (si < 0) si = lst->slices - 1;
                                if (si >= lst->slices) si = 0;
                                lst->step_notes[col] = si;
                                if (lst->steps[col] == 0) lst->steps[col] = 1;
                                preview_track_step(lst, 1);
                            }
                        }
                    }
                    last_lstick_x = now_x;
                }
            }
        }
        render(r);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    save_session(SESSION_PATH);
    if (app.dev) SDL_CloseAudioDevice(app.dev);
    if (app.controller) SDL_GameControllerClose(app.controller);
    for (int i = 0; i < app.sample_count; i++) free(app.samples[i].data);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
