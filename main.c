/*
 * ap88 - 终端随机音乐播放器
 *
 * 用法: ap88 [-g] <音乐目录>
 *   -g  在图形窗口中显示频谱（需要编译时找到 raylib）
 * 按键: 空格 暂停/继续   n 下一首   q 退出（终端和窗口里都可以按）
 *
 * 频谱可视化移植自 musializer (https://github.com/tsoding/musializer, MIT)
 */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef AP88_GUI
#include "raylib.h"
#include "rlgl.h"
#endif

/* ---------- 播放列表 ---------- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} Playlist;

static int is_audio_file(const char *name)
{
    static const char *exts[] = {".mp3", ".flac", ".wav"};
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static void playlist_add(Playlist *pl, const char *path)
{
    if (pl->count == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 64;
        pl->items = realloc(pl->items, pl->cap * sizeof(char *));
        if (!pl->items) { perror("realloc"); exit(1); }
    }
    pl->items[pl->count++] = strdup(path);
}

/* 递归扫描目录 */
static void playlist_scan(Playlist *pl, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    char path[4096];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            playlist_scan(pl, path);
        else if (S_ISREG(st.st_mode) && is_audio_file(e->d_name))
            playlist_add(pl, path);
    }
    closedir(d);
}

/* Fisher-Yates 洗牌 */
static void playlist_shuffle(Playlist *pl)
{
    for (size_t i = pl->count; i > 1; i--) {
        size_t j = (size_t)rand() % i;
        char *tmp = pl->items[i - 1];
        pl->items[i - 1] = pl->items[j];
        pl->items[j] = tmp;
    }
}

static void playlist_free(Playlist *pl)
{
    for (size_t i = 0; i < pl->count; i++) free(pl->items[i]);
    free(pl->items);
}

/* ---------- 信号 ---------- */

/* Ctrl+C / kill / 关闭终端时置位，让主循环正常退出并释放音频设备 */
static volatile sig_atomic_t quit_requested = 0;

static void on_signal(int sig)
{
    (void)sig;
    quit_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* 不设 SA_RESTART，让 select 被打断立即返回 */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

/* ---------- 终端 ---------- */

static struct termios orig_termios;
static int fullscreen; /* 终端频谱模式：使用备用屏幕 */

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    if (fullscreen)
        /* 恢复自动换行、光标，离开备用屏幕 */
        printf("\033[0m\033[?7h\033[?25h\033[?1049l");
    else
        printf("\033[?25h\n");
    fflush(stdout);
}

static void term_raw(int full)
{
    fullscreen = full;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_restore);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    if (fullscreen)
        /* 进入备用屏幕，隐藏光标，关闭自动换行（超长行直接截断） */
        printf("\033[?1049h\033[?25l\033[?7l\033[2J");
    else
        printf("\033[?25l"); /* 隐藏光标 */
}

/* 等待最多 ms 毫秒读一个按键，无按键返回 0 */
static int read_key(int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
    }
    return 0;
}

static const char *basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---------- 频谱 (移植自 musializer) ---------- */

#define FFT_SIZE (1 << 13)

/* 音频线程写入的环形缓冲区，主线程读取 */
static float ring[FFT_SIZE];
static size_t ring_pos;
static ma_spinlock ring_lock;

static float in_raw[FFT_SIZE];
static float in_win[FFT_SIZE];
static float complex_re[FFT_SIZE], complex_im[FFT_SIZE];
static float out_log[FFT_SIZE];
static float out_smooth[FFT_SIZE];
static float out_smear[FFT_SIZE];

/* engine 每次输出后回调（音频线程），把混音后的单声道样本推入环形缓冲区 */
static void on_engine_process(void *user, float *frames, ma_uint64 count)
{
    ma_uint32 ch = *(ma_uint32 *)user;
    if (ch == 0) return; /* engine 初始化期间 channels 还没填好 */
    ma_spinlock_lock(&ring_lock);
    for (ma_uint64 i = 0; i < count; i++) {
        float s = 0;
        for (ma_uint32 c = 0; c < ch; c++) s += frames[i * ch + c];
        ring[ring_pos] = s / ch;
        ring_pos = (ring_pos + 1) % FFT_SIZE;
    }
    ma_spinlock_unlock(&ring_lock);
}

static void fft_clean_input(void)
{
    ma_spinlock_lock(&ring_lock);
    memset(ring, 0, sizeof(ring));
    ma_spinlock_unlock(&ring_lock);
}

static void fft_clean(void)
{
    fft_clean_input();
    memset(out_smooth, 0, sizeof(out_smooth));
    memset(out_smear, 0, sizeof(out_smear));
}

/* https://cp-algorithms.com/algebra/fft.html */
static void fft(const float in[], float re[], float im[], size_t n)
{
    for (size_t i = 0; i < n; i++) { re[i] = in[i]; im[i] = 0; }

    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = 2 * (float)M_PI / len;
        float wl_re = cosf(ang), wl_im = sinf(ang);
        for (size_t i = 0; i < n; i += len) {
            float w_re = 1, w_im = 0;
            for (size_t j = 0; j < len / 2; j++) {
                size_t a = i + j, b = i + j + len / 2;
                float v_re = re[b] * w_re - im[b] * w_im;
                float v_im = re[b] * w_im + im[b] * w_re;
                re[b] = re[a] - v_re; im[b] = im[a] - v_im;
                re[a] += v_re;        im[a] += v_im;
                float t = w_re * wl_re - w_im * wl_im;
                w_im = w_re * wl_im + w_im * wl_re;
                w_re = t;
            }
        }
    }
}

/* 返回对数频段数量 m，结果在 out_smooth / out_smear 的 [0, m) */
static size_t fft_analyze(float dt)
{
    /* 按时间顺序取出最近 FFT_SIZE 个样本 */
    ma_spinlock_lock(&ring_lock);
    memcpy(in_raw, ring + ring_pos, (FFT_SIZE - ring_pos) * sizeof(float));
    memcpy(in_raw + (FFT_SIZE - ring_pos), ring, ring_pos * sizeof(float));
    ma_spinlock_unlock(&ring_lock);

    /* Hann 窗 */
    for (size_t i = 0; i < FFT_SIZE; i++) {
        float t = (float)i / (FFT_SIZE - 1);
        in_win[i] = in_raw[i] * (0.5f - 0.5f * cosf(2 * (float)M_PI * t));
    }

    fft(in_win, complex_re, complex_im, FFT_SIZE);

    /* 压缩到对数频率刻度 */
    float step = 1.06f;
    size_t m = 0;
    float max_amp = 1.0f;
    for (float f = 1.0f; (size_t)f < FFT_SIZE / 2; f = ceilf(f * step)) {
        float f1 = ceilf(f * step);
        float a = 0.0f;
        for (size_t q = (size_t)f; q < FFT_SIZE / 2 && q < (size_t)f1; q++) {
            float b = logf(complex_re[q] * complex_re[q] + complex_im[q] * complex_im[q]);
            if (b > a) a = b;
        }
        if (max_amp < a) max_amp = a;
        out_log[m++] = a;
    }

    /* 归一化到 0..1，再做平滑和拖尾 */
    float ks = fminf(8 * dt, 1), kt = fminf(3 * dt, 1);
    for (size_t i = 0; i < m; i++) {
        out_log[i] /= max_amp;
        out_smooth[i] += (out_log[i] - out_smooth[i]) * ks;
        out_smear[i] += (out_smooth[i] - out_smear[i]) * kt;
    }
    return m;
}

static void hsv_to_rgb(float h, float s, float v, int rgb[3])
{
    float c = v * s, x = c * (1 - fabsf(fmodf(h * 6, 2) - 1)), mm = v - c;
    float r = 0, g = 0, b = 0;
    switch ((int)(h * 6) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    rgb[0] = (int)((r + mm) * 255);
    rgb[1] = (int)((g + mm) * 255);
    rgb[2] = (int)((b + mm) * 255);
}

/* 帧缓冲，攒齐一整帧再一次性写出，避免闪烁 */
static char *frame_buf;
static size_t frame_len, frame_cap;

static void fb_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fb_printf(const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(frame_buf + frame_len, frame_cap - frame_len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (frame_len + (size_t)n < frame_cap) { frame_len += n; return; }
        frame_cap = (frame_cap + n) * 2;
        frame_buf = realloc(frame_buf, frame_cap);
        if (!frame_buf) { perror("realloc"); exit(1); }
    }
}

static void draw_frame(const char *name, float pos, float len, int paused, float dt)
{
    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }

    size_t m = fft_analyze(dt);

    frame_len = 0;
    fb_printf("\033[H\033[0m%s %s  [%02d:%02d / %02d:%02d]\033[K\r\n",
              paused ? "||" : "> ", name,
              (int)pos / 60, (int)pos % 60, (int)len / 60, (int)len % 60);
    fb_printf("\033[2m[空格 暂停  n 下一首  q 退出]\033[0m\033[K\r\n");

    int h = rows - 3; /* 柱状图高度（行），最后一行留空 */
    if (h < 1 || cols < 2 || m == 0) {
        fb_printf("\033[J");
        fwrite(frame_buf, 1, frame_len, stdout);
        fflush(stdout);
        return;
    }

    /* 频段合并到柱子：每根柱子占 slot 列，其中 1 列留作间隔 */
    size_t nbars = (size_t)cols / 2 < m ? (size_t)cols / 2 : m;
    int slot = cols / (int)nbars;
    int pad = (cols - slot * (int)nbars) / 2;

    static float bar[FFT_SIZE], trail[FFT_SIZE];
    static int color[FFT_SIZE][3], dim[FFT_SIZE][3];
    for (size_t i = 0; i < nbars; i++) {
        size_t a = i * m / nbars, b = (i + 1) * m / nbars;
        bar[i] = trail[i] = 0;
        for (size_t k = a; k < b; k++) {
            if (out_smooth[k] > bar[i]) bar[i] = out_smooth[k];
            if (out_smear[k] > trail[i]) trail[i] = out_smear[k];
        }
        hsv_to_rgb((float)i / nbars, 0.75f, 1.0f, color[i]);
        hsv_to_rgb((float)i / nbars, 0.75f, 0.35f, dim[i]);
    }

    /* 用 1/8 方块字符获得 8 倍纵向精度；拖尾（smear 高于当前值）用暗色画出 */
    static const char *eighths[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (int r = 0; r < h; r++) {
        int y = (h - 1 - r) * 8; /* 本行底部，以 1/8 行为单位 */
        fb_printf("\033[0m%*s", pad, "");
        int last = -1; /* 上次输出的颜色：柱子编号*2 + 是否暗色 */
        for (size_t i = 0; i < nbars; i++) {
            int lb = (int)(bar[i] * h * 8) - y;
            int lt = (int)(trail[i] * h * 8) - y;
            const char *ch = " ";
            int want = -1;
            if (lb > 0) {
                ch = eighths[lb > 8 ? 8 : lb];
                want = (int)i * 2;
            } else if (lt > 0) {
                ch = eighths[lt > 8 ? 8 : lt];
                want = (int)i * 2 + 1;
            }
            if (want >= 0 && want != last) {
                int *c = (want & 1) ? dim[i] : color[i];
                fb_printf("\033[38;2;%d;%d;%dm", c[0], c[1], c[2]);
                last = want;
            }
            for (int w = 0; w < slot - 1; w++) fb_printf("%s", ch);
            fb_printf(" ");
        }
        fb_printf("\033[0m\033[K\r\n");
    }
    fb_printf("\033[J");

    fwrite(frame_buf, 1, frame_len, stdout);
    fflush(stdout);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#ifdef AP88_GUI
/* ---------- 图形窗口 (移植自 musializer 的 fft_render) ---------- */

static const char *circle_fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform float radius;\n"
    "uniform float power;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    float r = radius;\n"
    "    vec2 p = fragTexCoord - vec2(0.5);\n"
    "    if (length(p) <= 0.5) {\n"
    "        float s = length(p) - r;\n"
    "        if (s <= 0) {\n"
    "            finalColor = fragColor*1.5;\n"
    "        } else {\n"
    "            float t = 1 - s / (0.5 - r);\n"
    "            finalColor = mix(vec4(fragColor.xyz, 0), fragColor*1.5, pow(t, power));\n"
    "        }\n"
    "    } else {\n"
    "        finalColor = vec4(0);\n"
    "    }\n"
    "}\n";

static Shader circle;
static int circle_radius_loc, circle_power_loc;

/* 歌名字体：raylib 默认字体只有 ASCII，需要一个带中文的 TTF/OTF（不支持 .ttc 合集） */
static const char *font_paths[] = {
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
};
#define FONT_LOAD_SIZE 64 /* 按大字号栅格化再缩小画，高分屏上也清晰 */

static unsigned char *font_data;
static int font_data_size;
static const char *font_ext;
static Font title_font;
static int title_font_loaded;

static void font_data_load(void)
{
    const char *env = getenv("AP88_FONT");
    size_t n = sizeof(font_paths) / sizeof(font_paths[0]);
    for (size_t i = 0; i <= n && !font_data; i++) {
        const char *path = i == 0 ? env : font_paths[i - 1];
        if (!path || !FileExists(path)) continue;
        font_data = LoadFileData(path, &font_data_size);
        font_ext = GetFileExtension(path);
    }
}

/* 只为当前歌名里出现的字符（外加 ASCII）生成字形 */
static void title_font_load(const char *name)
{
    if (title_font_loaded) UnloadFont(title_font);
    title_font_loaded = 0;
    if (!font_data) return;

    int count = 0;
    int *cps = LoadCodepoints(name, &count);
    int *all = malloc((count + 95) * sizeof(int));
    int n = 0;
    for (int c = 32; c < 127; c++) all[n++] = c;
    for (int i = 0; i < count; i++) {
        int dup = cps[i] < 127;
        for (int j = 95; j < n && !dup; j++) dup = all[j] == cps[i];
        if (!dup) all[n++] = cps[i];
    }
    UnloadCodepoints(cps);

    title_font = LoadFontFromMemory(font_ext, font_data, font_data_size, FONT_LOAD_SIZE, all, n);
    free(all);
    if (title_font.texture.id == 0 || title_font.texture.id == GetFontDefault().texture.id) return;
    SetTextureFilter(title_font.texture, TEXTURE_FILTER_BILINEAR);
    title_font_loaded = 1;
}

static void gui_init(void)
{
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1000, 600, "ap88");
    SetExitKey(KEY_NULL); /* 退出统一走 q / 关闭窗口 */
    SetTargetFPS(60);
    circle = LoadShaderFromMemory(NULL, circle_fs);
    circle_radius_loc = GetShaderLocation(circle, "radius");
    circle_power_loc = GetShaderLocation(circle, "power");
    font_data_load();
    if (!font_data)
        fprintf(stderr, "没有找到中文字体，窗口里的歌名将无法显示中文（可用 AP88_FONT 指定 .ttf/.otf）\n");
}

static void gui_close(void)
{
    UnloadShader(circle);
    if (title_font_loaded) UnloadFont(title_font);
    if (font_data) UnloadFileData(font_data);
    CloseWindow();
}

static void fft_render(Rectangle boundary, size_t m)
{
    float cell_width = boundary.width / m;
    float saturation = 0.75f;
    float value = 1.0f;

    /* 柱子 */
    for (size_t i = 0; i < m; i++) {
        float t = out_smooth[i];
        Color color = ColorFromHSV((float)i / m * 360, saturation, value);
        Vector2 start = {
            boundary.x + i * cell_width + cell_width / 2,
            boundary.y + boundary.height - boundary.height * 2 / 3 * t,
        };
        Vector2 end = {
            boundary.x + i * cell_width + cell_width / 2,
            boundary.y + boundary.height,
        };
        DrawLineEx(start, end, cell_width / 3 * sqrtf(t), color);
    }

    Texture2D texture = {rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

    /* 拖尾 */
    SetShaderValue(circle, circle_radius_loc, (float[1]){0.3f}, SHADER_UNIFORM_FLOAT);
    SetShaderValue(circle, circle_power_loc, (float[1]){3.0f}, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(circle);
    for (size_t i = 0; i < m; i++) {
        float start = out_smear[i];
        float end = out_smooth[i];
        Color color = ColorFromHSV((float)i / m * 360, saturation, value);
        float x = boundary.x + i * cell_width + cell_width / 2;
        float y0 = boundary.y + boundary.height - boundary.height * 2 / 3 * start;
        float y1 = boundary.y + boundary.height - boundary.height * 2 / 3 * end;
        float radius = cell_width * 3 * sqrtf(end);
        if (y1 >= y0) {
            Rectangle dest = {x - radius / 2, y0, radius, y1 - y0};
            DrawTexturePro(texture, (Rectangle){0, 0, 1, 0.5f}, dest, (Vector2){0}, 0, color);
        } else {
            Rectangle dest = {x - radius / 2, y1, radius, y0 - y1};
            DrawTexturePro(texture, (Rectangle){0, 0.5f, 1, 0.5f}, dest, (Vector2){0}, 0, color);
        }
    }
    EndShaderMode();

    /* 顶端光点 */
    SetShaderValue(circle, circle_radius_loc, (float[1]){0.07f}, SHADER_UNIFORM_FLOAT);
    SetShaderValue(circle, circle_power_loc, (float[1]){5.0f}, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(circle);
    for (size_t i = 0; i < m; i++) {
        float t = out_smooth[i];
        Color color = ColorFromHSV((float)i / m * 360, saturation, value);
        Vector2 center = {
            boundary.x + i * cell_width + cell_width / 2,
            boundary.y + boundary.height - boundary.height * 2 / 3 * t,
        };
        float radius = cell_width * 6 * sqrtf(t);
        DrawTextureEx(texture, (Vector2){center.x - radius, center.y - radius}, 0, 2 * radius, color);
    }
    EndShaderMode();
}

/* 画一帧窗口并收集按键；窗口字体不含中文，歌名放在窗口标题和终端状态行里 */
static int gui_frame(const char *name, float pos, float len, int paused, float dt)
{
    printf("\r\033[K%s %s  [%02d:%02d / %02d:%02d]",
           paused ? "||" : "> ", name,
           (int)pos / 60, (int)pos % 60, (int)len / 60, (int)len % 60);
    fflush(stdout);

    size_t m = fft_analyze(dt);
    float w = (float)GetScreenWidth(), h = (float)GetScreenHeight();
    float bar_h = 6;

    BeginDrawing();
    ClearBackground(GetColor(0x151515FF));
    fft_render((Rectangle){0, 0, w, h - 40}, m);

    /* 底部进度条 + 时间 */
    const char *time_str = TextFormat("%02d:%02d / %02d:%02d",
                                      (int)pos / 60, (int)pos % 60, (int)len / 60, (int)len % 60);
    int text_w = MeasureText(time_str, 20);
    float track_w = w - 60 - text_w;
    DrawRectangleRec((Rectangle){20, h - 25, track_w, bar_h}, GetColor(0x303030FF));
    if (len > 0)
        DrawRectangleRec((Rectangle){20, h - 25, track_w * pos / len, bar_h}, GetColor(0xBBBBBBFF));
    DrawText(time_str, (int)w - 20 - text_w, (int)h - 32, 20, GetColor(0xBBBBBBFF));
    /* 左上角歌名，太长就缩小字号 */
    Font font = title_font_loaded ? title_font : GetFontDefault();
    float size = 32, spacing = 1;
    float tw = MeasureTextEx(font, name, size, spacing).x;
    if (tw > w - 40) {
        size = fmaxf(size * (w - 40) / tw, 14);
        spacing = size / 32;
    }
    DrawTextEx(font, name, (Vector2){20, 20}, size, spacing, GetColor(0xDDDDDDFF));
    if (paused) DrawTextEx(font, "PAUSED", (Vector2){20, 20 + size + 10}, 24, 1, GetColor(0x888888FF));
    EndDrawing(); /* SetTargetFPS 在这里限速 */

    if (WindowShouldClose() || IsKeyPressed(KEY_Q)) return 'q';
    if (IsKeyPressed(KEY_N)) return 'n';
    if (IsKeyPressed(KEY_SPACE)) return ' ';
    return read_key(0);
}
#endif

/* ---------- 播放 ---------- */

enum { PLAY_NEXT, PLAY_QUIT };

/* 播放单个文件，直到结束或用户操作 */
static int play_file(ma_engine *engine, const char *path, int gui)
{
    ma_sound sound;
    if (ma_sound_init_from_file(engine, path, MA_SOUND_FLAG_STREAM, NULL, NULL, &sound) != MA_SUCCESS) {
        fprintf(stderr, "\r\033[K无法播放: %s\n", path);
        sleep(1); /* 备用屏幕下错误信息会被下一帧覆盖，留点时间看 */
        return PLAY_NEXT;
    }

    const char *name = basename_of(path);
    float len = 0;
    ma_sound_get_length_in_seconds(&sound, &len);
    fft_clean();
#ifdef AP88_GUI
    if (gui) {
        SetWindowTitle(name);
        title_font_load(name);
    }
#endif
    ma_sound_start(&sound);

    int paused = 0;
    double last = now_seconds();
    int result = PLAY_NEXT;
    while (!ma_sound_at_end(&sound)) {
        if (quit_requested) { result = PLAY_QUIT; break; }

        float pos = 0;
        ma_sound_get_cursor_in_seconds(&sound, &pos);
        double t = now_seconds();
        float dt = (float)(t - last);
        last = t;

        int key;
#ifdef AP88_GUI
        if (gui) key = gui_frame(name, pos, len, paused, dt);
        else
#endif
        {
            (void)gui;
            draw_frame(name, pos, len, paused, dt);
            key = read_key(33); /* 约 30 fps */
        }
        if (key == 'q') { result = PLAY_QUIT; break; }
        if (key == 'n') break;
        if (key == ' ') {
            paused = !paused;
            /* 暂停时连底层设备一起停掉，否则会一直向耳机输出静音、占着设备 */
            if (paused) {
                ma_sound_stop(&sound);
                ma_engine_stop(engine);
                fft_clean_input(); /* 设备停了不再有新样本，清空输入让柱子自然落下 */
            } else {
                ma_engine_start(engine);
                ma_sound_start(&sound);
            }
        }
    }

    ma_sound_uninit(&sound);
    if (paused && result == PLAY_NEXT) ma_engine_start(engine); /* 切下一首前恢复设备 */
    if (gui) printf("\n"); /* 窗口模式下终端保留每首歌的记录 */
    return result;
}

int main(int argc, char **argv)
{
    int gui = 0;
    if (argc == 3 && strcmp(argv[1], "-g") == 0) {
        gui = 1;
        argv++;
        argc--;
    }
    if (argc != 2) {
        fprintf(stderr, "用法: %s [-g] <音乐目录>\n", argv[0]);
        return 1;
    }
#ifndef AP88_GUI
    if (gui) {
        fprintf(stderr, "编译时没有找到 raylib，不支持 -g\n");
        return 1;
    }
#endif

    Playlist pl = {0};
    playlist_scan(&pl, argv[1]);
    if (pl.count == 0) {
        fprintf(stderr, "目录中没有找到音乐文件: %s\n", argv[1]);
        return 1;
    }

    static ma_uint32 channels;
    ma_engine_config cfg = ma_engine_config_init();
    cfg.onProcess = on_engine_process;
    cfg.pProcessUserData = &channels;

    ma_engine engine;
    if (ma_engine_init(&cfg, &engine) != MA_SUCCESS) {
        fprintf(stderr, "音频设备初始化失败\n");
        playlist_free(&pl);
        return 1;
    }

    channels = ma_engine_get_channels(&engine);
    install_signal_handlers();
    srand((unsigned)time(NULL));
#ifdef AP88_GUI
    if (gui) {
        gui_init();
        printf("共 %zu 首  [空格 暂停  n 下一首  q 退出]\n", pl.count);
    }
#endif
    term_raw(!gui);

    /* 列表播完后重新洗牌，无限循环 */
    for (;;) {
        playlist_shuffle(&pl);
        for (size_t i = 0; i < pl.count; i++)
            if (play_file(&engine, pl.items[i], gui) == PLAY_QUIT) goto done;
    }

done:
#ifdef AP88_GUI
    if (gui) gui_close();
#endif
    ma_engine_uninit(&engine);
    playlist_free(&pl);
    free(frame_buf);
    return 0;
}
