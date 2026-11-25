#include "dev/shader_editor.h"

#include "raylib.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_FONT_PATH    GRAMARYE_DEV_DIR "fonts/DejaVuSansMono.ttf"
#define ED_FONT_LOAD_PX 48
#define ED_TAB          4
#define ED_MAX_UNDO     256
#define ED_MAX_ERRLINES 64
#define ED_PAD          6.0f

typedef struct Snapshot {
    char   *text;
    int32_t cursor;
} Snapshot;

enum { EDIT_OTHER, EDIT_TYPE, EDIT_DELETE };

static char    *g_buf;
static int32_t  g_len, g_cap;
static int32_t *g_line_start;
static bool    *g_line_in_comment;
static int32_t  g_line_count, g_line_cap;

static int32_t  g_cursor, g_anchor;
static int32_t  g_want_col = -1;
static float    g_scroll_y;
static float    g_scroll_x;
static bool     g_focused, g_dragging, g_follow_cursor;
static uint32_t g_revision;
static double   g_last_edit, g_blink_start;
static int32_t  g_last_edit_kind = EDIT_OTHER;
static double   g_last_click;
static int32_t  g_last_click_pos = -1;

static Snapshot g_undo[ED_MAX_UNDO], g_redo[ED_MAX_UNDO];
static int32_t  g_undo_count, g_redo_count;

static Font     g_font;
static bool     g_font_loaded, g_font_tried;
static float    g_font_size = 18.0f;

static char     g_info[256];
static char     g_errors[8192];
static int32_t  g_error_lines[ED_MAX_ERRLINES];
static int32_t  g_error_line_count;

static void reserve(int32_t need) {
    if (need + 1 <= g_cap) return;
    int32_t cap = g_cap ? g_cap : 1024;
    while (cap < need + 1) cap *= 2;
    g_buf = realloc(g_buf, (size_t)cap);
    g_cap = cap;
}

static void rebuild_lines(void) {
    int32_t count = 1;
    for (int32_t i = 0; i < g_len; i++) if (g_buf[i] == '\n') count++;
    if (count > g_line_cap) {
        g_line_cap = count * 2;
        g_line_start = realloc(g_line_start, sizeof(int32_t) * (size_t)g_line_cap);
        g_line_in_comment = realloc(g_line_in_comment, sizeof(bool) * (size_t)g_line_cap);
    }
    g_line_count = 0;
    g_line_start[g_line_count] = 0;
    g_line_in_comment[g_line_count++] = false;
    bool block = false, line_comment = false;
    for (int32_t i = 0; i < g_len; i++) {
        char c = g_buf[i], n = (i + 1 < g_len) ? g_buf[i + 1] : 0;
        if (c == '\n') {
            line_comment = false;
            g_line_start[g_line_count] = i + 1;
            g_line_in_comment[g_line_count++] = block;
        } else if (block) {
            if (c == '*' && n == '/') { block = false; i++; }
        } else if (!line_comment) {
            if (c == '/' && n == '/') line_comment = true;
            else if (c == '/' && n == '*') { block = true; i++; }
        }
    }
}

static int32_t line_of(int32_t pos) {
    int32_t lo = 0, hi = g_line_count - 1;
    while (lo < hi) {
        int32_t mid = (lo + hi + 1) / 2;
        if (g_line_start[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int32_t line_end(int32_t line) {
    return (line + 1 < g_line_count) ? g_line_start[line + 1] - 1 : g_len;
}

static int32_t col_of(int32_t pos) { return pos - g_line_start[line_of(pos)]; }

static int32_t pos_at(int32_t line, int32_t col) {
    if (line < 0) return 0;
    if (line >= g_line_count) return g_len;
    int32_t p = g_line_start[line] + (col < 0 ? 0 : col);
    int32_t e = line_end(line);
    return p > e ? e : p;
}

static int32_t sel_min(void) { return g_cursor < g_anchor ? g_cursor : g_anchor; }
static int32_t sel_max(void) { return g_cursor > g_anchor ? g_cursor : g_anchor; }
static bool    has_sel(void) { return g_cursor != g_anchor; }

static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void snapshot_free(Snapshot *s) { free(s->text); s->text = NULL; }

static Snapshot snapshot_take(void) {
    Snapshot s = { malloc((size_t)g_len + 1), g_cursor };
    memcpy(s.text, g_buf, (size_t)g_len);
    s.text[g_len] = 0;
    return s;
}

static void stack_push(Snapshot *stack, int32_t *count, Snapshot s) {
    if (*count == ED_MAX_UNDO) {
        snapshot_free(&stack[0]);
        memmove(stack, stack + 1, sizeof(Snapshot) * (ED_MAX_UNDO - 1));
        (*count)--;
    }
    stack[(*count)++] = s;
}

static void clear_history(void) {
    for (int32_t i = 0; i < g_undo_count; i++) snapshot_free(&g_undo[i]);
    for (int32_t i = 0; i < g_redo_count; i++) snapshot_free(&g_redo[i]);
    g_undo_count = g_redo_count = 0;
    g_last_edit_kind = EDIT_OTHER;
}

static void begin_edit(int32_t kind) {
    double now = GetTime();
    bool merge = kind != EDIT_OTHER && kind == g_last_edit_kind && now - g_last_edit < 1.0;
    if (!merge) stack_push(g_undo, &g_undo_count, snapshot_take());
    for (int32_t i = 0; i < g_redo_count; i++) snapshot_free(&g_redo[i]);
    g_redo_count = 0;
    g_last_edit_kind = kind;
}

static void restore(Snapshot s) {
    int32_t n = (int32_t)strlen(s.text);
    reserve(n);
    memcpy(g_buf, s.text, (size_t)n + 1);
    g_len = n;
    rebuild_lines();
    g_cursor = g_anchor = s.cursor > g_len ? g_len : s.cursor;
    g_revision++;
    g_last_edit = GetTime();
    g_last_edit_kind = EDIT_OTHER;
    g_follow_cursor = true;
}

static void undo(void) {
    if (!g_undo_count) return;
    stack_push(g_redo, &g_redo_count, snapshot_take());
    Snapshot s = g_undo[--g_undo_count];
    restore(s);
    snapshot_free(&s);
}

static void redo(void) {
    if (!g_redo_count) return;
    stack_push(g_undo, &g_undo_count, snapshot_take());
    Snapshot s = g_redo[--g_redo_count];
    restore(s);
    snapshot_free(&s);
}

static void replace(int32_t a, int32_t b, const char *s, int32_t n) {
    reserve(g_len - (b - a) + n);
    memmove(g_buf + a + n, g_buf + b, (size_t)(g_len - b));
    memcpy(g_buf + a, s, (size_t)n);
    g_len += n - (b - a);
    g_buf[g_len] = 0;
    rebuild_lines();
    g_cursor = g_anchor = a + n;
    g_want_col = -1;
    g_revision++;
    g_last_edit = GetTime();
    g_follow_cursor = true;
}

static void insert(const char *s, int32_t n, int32_t kind) {
    begin_edit(has_sel() ? EDIT_OTHER : kind);
    replace(sel_min(), sel_max(), s, n);
}

static void delete_range(int32_t a, int32_t b) {
    if (a == b) return;
    begin_edit(EDIT_DELETE);
    replace(a, b, "", 0);
}

static int32_t leading_spaces(int32_t line) {
    int32_t p = g_line_start[line], e = line_end(line), n = 0;
    while (p + n < e && g_buf[p + n] == ' ') n++;
    return n;
}

static bool only_spaces(int32_t a, int32_t b) {
    for (int32_t i = a; i < b; i++) if (g_buf[i] != ' ') return false;
    return true;
}

static int32_t word_left(int32_t p) {
    while (p > 0 && isspace((unsigned char)g_buf[p - 1]) && g_buf[p - 1] != '\n') p--;
    if (p > 0 && is_word(g_buf[p - 1])) { while (p > 0 && is_word(g_buf[p - 1])) p--; }
    else if (p > 0) p--;
    return p;
}

static int32_t word_right(int32_t p) {
    if (p < g_len && is_word(g_buf[p])) { while (p < g_len && is_word(g_buf[p])) p++; }
    else if (p < g_len) p++;
    while (p < g_len && isspace((unsigned char)g_buf[p]) && g_buf[p] != '\n') p++;
    return p;
}

static void newline(void) {
    int32_t line = line_of(sel_min());
    int32_t indent = leading_spaces(line);
    int32_t col = sel_min() - g_line_start[line];
    if (indent > col) indent = col;
    int32_t before = sel_min() - 1;
    while (before >= g_line_start[line] && g_buf[before] == ' ') before--;
    bool opens = before >= g_line_start[line] && g_buf[before] == '{';
    bool closes = sel_max() < g_len && g_buf[sel_max()] == '}';

    char tmp[512];
    int32_t n = 0, inner = indent + (opens ? ED_TAB : 0);
    if (inner > 240) inner = 240;
    tmp[n++] = '\n';
    for (int32_t i = 0; i < inner; i++) tmp[n++] = ' ';
    int32_t caret = n;
    if (opens && closes) {
        tmp[n++] = '\n';
        for (int32_t i = 0; i < indent && i < 240; i++) tmp[n++] = ' ';
    }
    int32_t start = sel_min();
    insert(tmp, n, EDIT_OTHER);
    g_cursor = g_anchor = start + caret;
}

static void transform_lines(int32_t mode) {
    int32_t la = line_of(sel_min()), lb = line_of(sel_max());
    if (lb > la && sel_max() == g_line_start[lb]) lb--;
    bool had_sel = has_sel();
    int32_t old_cursor_line = line_of(g_cursor), old_col = col_of(g_cursor);

    bool all_commented = true;
    int32_t min_indent = 1 << 30;
    for (int32_t l = la; l <= lb; l++) {
        int32_t s = g_line_start[l], e = line_end(l), ind = leading_spaces(l);
        if (s + ind == e) continue;
        if (ind < min_indent) min_indent = ind;
        if (!(e - (s + ind) >= 2 && g_buf[s + ind] == '/' && g_buf[s + ind + 1] == '/')) all_commented = false;
    }
    if (min_indent == 1 << 30) { min_indent = 0; all_commented = false; }

    int32_t a = g_line_start[la], b = line_end(lb);
    char *out = malloc((size_t)(b - a) + (size_t)(lb - la + 1) * 8 + 1);
    int32_t n = 0, cursor_delta = 0;
    for (int32_t l = la; l <= lb; l++) {
        int32_t s = g_line_start[l], e = line_end(l), ind = leading_spaces(l);
        int32_t delta = 0;
        bool blank = s + ind == e;
        if (mode > 0) {
            for (int32_t i = 0; i < ED_TAB; i++) out[n++] = ' ';
            delta = ED_TAB;
            memcpy(out + n, g_buf + s, (size_t)(e - s)); n += e - s;
        } else if (mode < 0) {
            int32_t cut = ind < ED_TAB ? ind : ED_TAB;
            delta = -cut;
            memcpy(out + n, g_buf + s + cut, (size_t)(e - s - cut)); n += e - s - cut;
        } else if (blank) {
            memcpy(out + n, g_buf + s, (size_t)(e - s)); n += e - s;
        } else if (all_commented) {
            int32_t c = s + ind + 2;
            int32_t cut = (c < e && g_buf[c] == ' ') ? 3 : 2;
            memcpy(out + n, g_buf + s, (size_t)ind); n += ind;
            memcpy(out + n, g_buf + s + ind + cut, (size_t)(e - s - ind - cut)); n += e - s - ind - cut;
            delta = -cut;
        } else {
            memcpy(out + n, g_buf + s, (size_t)min_indent); n += min_indent;
            memcpy(out + n, "// ", 3); n += 3;
            memcpy(out + n, g_buf + s + min_indent, (size_t)(e - s - min_indent)); n += e - s - min_indent;
            delta = 3;
        }
        if (l == old_cursor_line) cursor_delta = delta;
        if (l < lb) out[n++] = '\n';
    }
    begin_edit(EDIT_OTHER);
    replace(a, b, out, n);
    free(out);
    if (had_sel) {
        g_anchor = a;
        g_cursor = a + n;
    } else {
        int32_t col = old_col + cursor_delta;
        g_cursor = g_anchor = pos_at(old_cursor_line, col < 0 ? 0 : col);
    }
}

static void copy_selection(void) {
    if (!has_sel()) return;
    int32_t a = sel_min(), b = sel_max();
    char *s = malloc((size_t)(b - a) + 1);
    memcpy(s, g_buf + a, (size_t)(b - a));
    s[b - a] = 0;
    SetClipboardText(s);
    free(s);
}

static char *sanitize(const char *src, int32_t start_col, int32_t *out_len) {
    int32_t cap = 64;
    for (const char *c = src; *c; c++) cap += (*c == '\t') ? ED_TAB : 1;
    char *out = malloc((size_t)cap);
    int32_t n = 0, col = start_col;
    for (const char *c = src; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == '\t') { do { out[n++] = ' '; col++; } while (col % ED_TAB); }
        else if (ch == '\n') { out[n++] = '\n'; col = 0; }
        else if (ch >= 32 && ch < 127) { out[n++] = (char)ch; col++; }
    }
    out[n] = 0;
    *out_len = n;
    return out;
}

static void paste(void) {
    const char *clip = GetClipboardText();
    if (!clip || !*clip) return;
    int32_t n;
    char *s = sanitize(clip, col_of(sel_min()), &n);
    insert(s, n, EDIT_OTHER);
    free(s);
}

void shader_editor_set_text(const char *text, bool keep_view) {
    shader_editor_text();
    int32_t n;
    char *s = sanitize(text ? text : "", 0, &n);
    int32_t keep_line = line_of(g_cursor), keep_col = g_cursor - g_line_start[keep_line];
    reserve(n);
    memcpy(g_buf, s, (size_t)n + 1);
    g_len = n;
    free(s);
    rebuild_lines();
    clear_history();
    if (keep_view) {
        g_cursor = g_anchor = pos_at(keep_line, keep_col);
    } else {
        g_cursor = g_anchor = 0;
        g_scroll_x = g_scroll_y = 0.0f;
    }
    g_want_col = -1;
    g_revision++;
}

const char *shader_editor_text(void) {
    if (!g_buf) { reserve(0); g_buf[0] = 0; rebuild_lines(); }
    return g_buf;
}

uint32_t shader_editor_revision(void)  { return g_revision; }
double   shader_editor_last_edit(void) { return g_last_edit; }
bool     shader_editor_focused(void)   { return g_focused; }
void     shader_editor_set_focus(bool f) { g_focused = f; g_dragging = false; }

void shader_editor_set_info(const char *info) {
    snprintf(g_info, sizeof(g_info), "%s", info ? info : "");
}

void shader_editor_set_errors(const char *log) {
    snprintf(g_errors, sizeof(g_errors), "%s", log ? log : "");
    g_error_line_count = 0;
    for (const char *p = g_errors; *p; p++) {
        bool start = p == g_errors || !isalnum((unsigned char)p[-1]);
        if (!(start && p[0] == '0' && (p[1] == ':' || p[1] == '(') && isdigit((unsigned char)p[2]))) continue;
        int32_t line = (int32_t)strtol(p + 2, NULL, 10);
        bool seen = false;
        for (int32_t i = 0; i < g_error_line_count; i++) if (g_error_lines[i] == line) seen = true;
        if (!seen && g_error_line_count < ED_MAX_ERRLINES) g_error_lines[g_error_line_count++] = line;
    }
}

void shader_editor_shutdown(void) {
    clear_history();
    free(g_buf); free(g_line_start); free(g_line_in_comment);
    g_buf = NULL; g_line_start = NULL; g_line_in_comment = NULL;
    g_len = g_cap = g_line_count = g_line_cap = 0;
    g_cursor = g_anchor = 0;
    g_focused = false;
    if (g_font_loaded) UnloadFont(g_font);
    g_font_loaded = g_font_tried = false;
}

static bool key(int k)  { return IsKeyPressed(k) || IsKeyPressedRepeat(k); }
static bool ctrl(void)  { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

static void move_to(int32_t p, bool extend, bool keep_col) {
    if (p < 0) p = 0;
    if (p > g_len) p = g_len;
    g_cursor = p;
    if (!extend) g_anchor = p;
    if (!keep_col) g_want_col = -1;
    g_follow_cursor = true;
}

static void move_lines(int32_t delta, bool extend) {
    if (g_want_col < 0) g_want_col = col_of(g_cursor);
    int32_t line = line_of(g_cursor) + delta;
    if (line < 0) { move_to(0, extend, false); return; }
    if (line >= g_line_count) { move_to(g_len, extend, false); return; }
    move_to(pos_at(line, g_want_col), extend, true);
}

static void handle_keys(int32_t page_lines) {
    bool c = ctrl(), s = shift();

    if (c) {
        if (IsKeyPressed(KEY_A)) { g_anchor = 0; g_cursor = g_len; }
        if (IsKeyPressed(KEY_C)) copy_selection();
        if (IsKeyPressed(KEY_X) && has_sel()) { copy_selection(); delete_range(sel_min(), sel_max()); }
        if (key(KEY_V)) paste();
        if (key(KEY_Z)) { if (s) redo(); else undo(); }
        if (key(KEY_Y)) redo();
        if (IsKeyPressed(KEY_SLASH)) transform_lines(0);
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) g_font_size = g_font_size < 40 ? g_font_size + 2 : 40;
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) g_font_size = g_font_size > 10 ? g_font_size - 2 : 10;
    } else {
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (cp < 32 || cp >= 127) continue;
            char ch = (char)cp;
            int32_t line = line_of(sel_min());
            if (ch == '}' && !has_sel() && only_spaces(g_line_start[line], g_cursor) && col_of(g_cursor) >= ED_TAB) {
                begin_edit(EDIT_OTHER);
                replace(g_cursor - ED_TAB, g_cursor, "}", 1);
            } else {
                insert(&ch, 1, EDIT_TYPE);
            }
        }
    }
    while (GetCharPressed() != 0) {}

    if (!c && (key(KEY_ENTER) || key(KEY_KP_ENTER))) newline();
    if (key(KEY_TAB)) {
        bool multi = has_sel() && line_of(sel_min()) != line_of(sel_max());
        if (s) transform_lines(-1);
        else if (multi) transform_lines(1);
        else {
            char sp[ED_TAB];
            int32_t n = ED_TAB - col_of(sel_min()) % ED_TAB;
            memset(sp, ' ', sizeof(sp));
            insert(sp, n, EDIT_TYPE);
        }
    }
    if (key(KEY_BACKSPACE)) {
        if (has_sel()) delete_range(sel_min(), sel_max());
        else if (g_cursor > 0) {
            int32_t line = line_of(g_cursor), col = col_of(g_cursor), from = g_cursor - 1;
            if (c) from = word_left(g_cursor);
            else if (col > 0 && only_spaces(g_line_start[line], g_cursor)) from = g_cursor - ((col - 1) % ED_TAB + 1);
            delete_range(from, g_cursor);
        }
    }
    if (key(KEY_DELETE)) {
        if (has_sel()) delete_range(sel_min(), sel_max());
        else if (g_cursor < g_len) delete_range(g_cursor, c ? word_right(g_cursor) : g_cursor + 1);
    }

    if (key(KEY_LEFT)) {
        if (has_sel() && !s) move_to(sel_min(), false, false);
        else move_to(c ? word_left(g_cursor) : g_cursor - 1, s, false);
    }
    if (key(KEY_RIGHT)) {
        if (has_sel() && !s) move_to(sel_max(), false, false);
        else move_to(c ? word_right(g_cursor) : g_cursor + 1, s, false);
    }
    if (key(KEY_UP))        move_lines(-1, s);
    if (key(KEY_DOWN))      move_lines(1, s);
    if (key(KEY_PAGE_UP))   move_lines(-page_lines, s);
    if (key(KEY_PAGE_DOWN)) move_lines(page_lines, s);
    if (key(KEY_HOME)) {
        if (c) move_to(0, s, false);
        else {
            int32_t line = line_of(g_cursor), first = g_line_start[line] + leading_spaces(line);
            move_to(g_cursor == first ? g_line_start[line] : first, s, false);
        }
    }
    if (key(KEY_END)) move_to(c ? g_len : line_end(line_of(g_cursor)), s, false);
    if (IsKeyPressed(KEY_ESCAPE)) g_focused = false;
}

typedef struct Metrics {
    float x, y, w, h;
    float gutter_x, gutter_w;
    float adv, line_h, size;
} Metrics;

static int32_t mouse_to_pos(const Metrics *m, Vector2 p) {
    int32_t line = (int32_t)((p.y - m->y - ED_PAD) / m->line_h + g_scroll_y);
    if (line < 0) return 0;
    if (line >= g_line_count) return g_len;
    float colf = (p.x - m->x + g_scroll_x) / m->adv;
    return pos_at(line, colf < 0 ? 0 : (int32_t)(colf + 0.5f));
}

static const char *const K_TYPES[] = {
    "void", "bool", "int", "uint", "float", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
    "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "sampler2D",
    "samplerCube", "sampler3D", NULL };
static const char *const K_WORDS[] = {
    "in", "out", "inout", "uniform", "const", "if", "else", "for", "while", "do", "return", "break",
    "continue", "discard", "struct", "precision", "highp", "mediump", "lowp", "flat", "smooth",
    "layout", "true", "false", "switch", "case", "default", NULL };
static const char *const K_FUNCS[] = {
    "sin", "cos", "tan", "asin", "acos", "atan", "pow", "exp", "log", "exp2", "log2", "sqrt",
    "inversesqrt", "abs", "sign", "floor", "ceil", "fract", "mod", "min", "max", "clamp", "mix",
    "step", "smoothstep", "length", "distance", "dot", "cross", "normalize", "reflect", "refract",
    "texture", "texelFetch", "textureSize", "textureLod", "dFdx", "dFdy", "fwidth", "radians",
    "degrees", "round", "trunc", "sinh", "cosh", "tanh", "faceforward", "transpose", "inverse",
    "determinant", "any", "all", "not", "lessThan", "greaterThan", "equal", NULL };
static const char *const K_IO[] = {
    "uTime", "uResolution", "uMouse", "uTextureSize", "texture0", "fragTexCoord", "fragColor",
    "finalColor", "gl_FragCoord", "main", NULL };

static const Color C_BG       = { 18, 18, 26, 255 };
static const Color C_GUTTER   = { 13, 13, 19, 255 };
static const Color C_LINENO   = { 80, 82, 100, 255 };
static const Color C_LINENO_C = { 200, 200, 215, 255 };
static const Color C_CURLINE  = { 26, 27, 40, 255 };
static const Color C_SEL      = { 48, 74, 122, 255 };
static const Color C_TEXT     = { 215, 218, 228, 255 };
static const Color C_KEYWORD  = { 198, 120, 221, 255 };
static const Color C_TYPE     = { 97, 175, 239, 255 };
static const Color C_FUNC     = { 86, 190, 200, 255 };
static const Color C_IO       = { 152, 200, 121, 255 };
static const Color C_NUMBER   = { 216, 160, 102, 255 };
static const Color C_COMMENT  = { 100, 110, 122, 255 };
static const Color C_PREPROC  = { 229, 192, 123, 255 };
static const Color C_PUNCT    = { 160, 165, 180, 255 };
static const Color C_ERRLINE  = { 70, 24, 30, 255 };
static const Color C_ERRMARK  = { 230, 80, 80, 255 };
static const Color C_ERRBG    = { 44, 16, 20, 255 };
static const Color C_ERRTEXT  = { 255, 190, 190, 255 };
static const Color C_STATUS   = { 24, 26, 38, 255 };
static const Color C_STATUSTX = { 150, 155, 175, 255 };

static bool in_list(const char *const *list, const char *w, int32_t n) {
    for (; *list; list++) if ((int32_t)strlen(*list) == n && memcmp(*list, w, (size_t)n) == 0) return true;
    return false;
}

static void draw_run(const Metrics *m, const char *s, int32_t n, int32_t col, float y, Color c) {
    char tmp[512];
    if (n <= 0) return;
    if (n > (int32_t)sizeof(tmp) - 1) n = (int32_t)sizeof(tmp) - 1;
    float x = m->x + col * m->adv - g_scroll_x;
    if (x > m->x + m->w || x + n * m->adv < m->x) return;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    DrawTextEx(g_font, tmp, (Vector2){ (float)(int32_t)x, (float)(int32_t)y }, m->size, 0.0f, c);
}

static void draw_line_text(const Metrics *m, int32_t line, float y) {
    const char *s = g_buf + g_line_start[line];
    int32_t n = line_end(line) - g_line_start[line], i = 0;
    bool block = g_line_in_comment[line];
    while (i < n) {
        int32_t j = i;
        Color c = C_TEXT;
        if (block) {
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            if (j < n) { j += 2; block = false; }
            c = C_COMMENT;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            j = n; c = C_COMMENT;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            block = true; j = i + 2;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            if (j < n) { j += 2; block = false; }
            c = C_COMMENT;
        } else if (s[i] == '#' && only_spaces(g_line_start[line], g_line_start[line] + i)) {
            while (j < n && !(s[j] == '/' && j + 1 < n && (s[j + 1] == '/' || s[j + 1] == '*'))) j++;
            c = C_PREPROC;
        } else if (isdigit((unsigned char)s[i]) || (s[i] == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.')) j++;
            c = C_NUMBER;
        } else if (is_word(s[i])) {
            while (j < n && is_word(s[j])) j++;
            int32_t w = j - i;
            if (in_list(K_TYPES, s + i, w))      c = C_TYPE;
            else if (in_list(K_WORDS, s + i, w)) c = C_KEYWORD;
            else if (in_list(K_FUNCS, s + i, w)) c = C_FUNC;
            else if (in_list(K_IO, s + i, w))    c = C_IO;
        } else if (s[i] == ' ') {
            while (j < n && s[j] == ' ') j++;
            i = j;
            continue;
        } else {
            j = i + 1;
            c = C_PUNCT;
        }
        draw_run(m, s + i, j - i, i, y, c);
        i = j;
    }
}

static bool is_error_line(int32_t line) {
    for (int32_t i = 0; i < g_error_line_count; i++) if (g_error_lines[i] == line + 1) return true;
    return false;
}

#define ED_ERR_ROWS 48
typedef struct ErrRow { int32_t start, len, src_line; } ErrRow;

static int32_t wrap_errors(ErrRow *rows, int32_t cols) {
    int32_t count = 0, i = 0, n = (int32_t)strlen(g_errors);
    if (cols < 8) cols = 8;
    while (i < n && count < ED_ERR_ROWS) {
        int32_t e = i;
        while (e < n && g_errors[e] != '\n') e++;
        int32_t src = -1;
        for (int32_t k = i; k + 2 < e; k++) {
            bool start = k == 0 || !isalnum((unsigned char)g_errors[k - 1]);
            if (start && g_errors[k] == '0' && (g_errors[k + 1] == ':' || g_errors[k + 1] == '(') &&
                isdigit((unsigned char)g_errors[k + 2])) { src = (int32_t)strtol(g_errors + k + 2, NULL, 10); break; }
        }
        if (e == i) { i = e + 1; continue; }
        for (int32_t p = i; p < e && count < ED_ERR_ROWS; p += cols) {
            rows[count++] = (ErrRow){ p, (e - p) < cols ? (e - p) : cols, src };
        }
        i = e + 1;
    }
    return count;
}

static void ensure_font(void) {
    if (g_font_tried) return;
    g_font_tried = true;
    if (FileExists(ED_FONT_PATH)) {
        g_font = LoadFontEx(ED_FONT_PATH, ED_FONT_LOAD_PX, NULL, 0);
        g_font_loaded = g_font.texture.id != 0;
        if (g_font_loaded) SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    }
    if (!g_font_loaded) {
        TraceLog(LOG_WARNING, "shader editor: %s missing, using default font", ED_FONT_PATH);
        g_font = GetFontDefault();
    }
}

void shader_editor_draw_in_rect(float x, float y, float w, float h) {
    ensure_font();
    shader_editor_text();
    if (w < 40.0f || h < 40.0f) return;

    float small = g_font_size * 0.8f;
    float small_lh = small * 1.3f;
    float status_h = small_lh + 6.0f;

    ErrRow rows[ED_ERR_ROWS];
    float small_adv = MeasureTextEx(g_font, "M", small, 0.0f).x;
    int32_t err_rows = g_errors[0] ? wrap_errors(rows, (int32_t)((w - 2 * ED_PAD) / small_adv)) : 0;
    float err_h = err_rows ? err_rows * small_lh + 2 * ED_PAD : 0.0f;
    if (err_h > h * 0.4f) err_h = h * 0.4f;

    Metrics m;
    m.size = g_font_size;
    m.adv = MeasureTextEx(g_font, "M", m.size, 0.0f).x;
    m.line_h = (float)(int32_t)(m.size * 1.3f);
    int32_t digits = 1;
    for (int32_t v = g_line_count; v >= 10; v /= 10) digits++;
    if (digits < 3) digits = 3;
    m.gutter_x = x;
    m.gutter_w = (digits + 2) * m.adv;
    m.x = x + m.gutter_w + ED_PAD;
    m.y = y;
    m.w = w - m.gutter_w - ED_PAD;
    m.h = h - status_h - err_h;
    int32_t visible = (int32_t)((m.h - ED_PAD) / m.line_h);
    if (visible < 1) visible = 1;

    Vector2 mouse = GetMousePosition();
    Rectangle code_rect = { x, y, w, m.h };
    Rectangle err_rect = { x, y + m.h, w, err_h };
    bool over_code = CheckCollisionPointRec(mouse, code_rect);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (over_code) {
            g_focused = true;
            int32_t p = mouse_to_pos(&m, mouse);
            double now = GetTime();
            if (p == g_last_click_pos && now - g_last_click < 0.35) {
                int32_t a = p, b = p;
                while (a > 0 && is_word(g_buf[a - 1])) a--;
                while (b < g_len && is_word(g_buf[b])) b++;
                g_anchor = a; g_cursor = b;
                g_dragging = false;
            } else {
                move_to(p, shift(), false);
                g_dragging = true;
            }
            g_last_click = now;
            g_last_click_pos = p;
            g_blink_start = now;
        } else if (err_h > 0 && CheckCollisionPointRec(mouse, err_rect)) {
            int32_t r = (int32_t)((mouse.y - err_rect.y - ED_PAD) / small_lh);
            if (r >= 0 && r < err_rows && rows[r].src_line > 0) {
                int32_t l = rows[r].src_line - 1;
                if (l >= g_line_count) l = g_line_count - 1;
                g_focused = true;
                move_to(pos_at(l, leading_spaces(l)), false, false);
            }
        } else {
            g_focused = false;
        }
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_dragging = false;
    if (g_dragging) {
        g_cursor = mouse_to_pos(&m, mouse);
        if (mouse.y < m.y) g_scroll_y -= 0.5f;
        if (mouse.y > m.y + m.h) g_scroll_y += 0.5f;
    }
    if (over_code) {
        Vector2 wheel = GetMouseWheelMoveV();
        if (shift()) { wheel.x = wheel.y; wheel.y = 0; }
        g_scroll_y -= wheel.y * 3.0f;
        g_scroll_x -= wheel.x * m.adv * 6.0f;
    }

    if (g_focused) {
        uint32_t rev = g_revision;
        int32_t cur = g_cursor;
        handle_keys(visible - 1);
        if (rev != g_revision || cur != g_cursor) g_blink_start = GetTime();
    }

    if (g_follow_cursor) {
        g_follow_cursor = false;
        int32_t cl = line_of(g_cursor);
        if (cl < g_scroll_y) g_scroll_y = (float)cl;
        if (cl > g_scroll_y + visible - 1) g_scroll_y = (float)(cl - visible + 1);
        float cx = col_of(g_cursor) * m.adv;
        if (cx < g_scroll_x) g_scroll_x = cx - m.adv * 4;
        if (cx > g_scroll_x + m.w - m.adv * 2) g_scroll_x = cx - m.w + m.adv * 6;
    }
    float max_scroll = (float)(g_line_count - 1);
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_x < 0) g_scroll_x = 0;
    g_scroll_y = (float)(int32_t)(g_scroll_y + 0.5f);

    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)m.h, C_BG);
    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)m.gutter_w, (int32_t)m.h, C_GUTTER);
    BeginScissorMode((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)m.h);

    int32_t first = (int32_t)g_scroll_y;
    int32_t cursor_line = line_of(g_cursor);
    int32_t smin = sel_min(), smax = sel_max();
    float text_dy = (m.line_h - m.size) * 0.5f;
    for (int32_t l = first; l < g_line_count && l <= first + visible; l++) {
        float ly = m.y + ED_PAD + (l - first) * m.line_h;
        bool err = is_error_line(l);
        if (err) DrawRectangle((int32_t)m.x - (int32_t)ED_PAD, (int32_t)ly, (int32_t)(m.w + ED_PAD), (int32_t)m.line_h, C_ERRLINE);
        else if (l == cursor_line && g_focused && !has_sel())
            DrawRectangle((int32_t)m.x - (int32_t)ED_PAD, (int32_t)ly, (int32_t)(m.w + ED_PAD), (int32_t)m.line_h, C_CURLINE);

        int32_t ls = g_line_start[l], le = line_end(l);
        if (smin != smax && smin <= le && smax > ls) {
            int32_t a = smin > ls ? smin - ls : 0;
            int32_t b = smax < le ? smax - ls : (le - ls) + 1;
            float sx = m.x + a * m.adv - g_scroll_x;
            float sw = (b - a) * m.adv;
            if (sx < m.x) { sw -= m.x - sx; sx = m.x; }
            if (sw > 0) DrawRectangle((int32_t)sx, (int32_t)ly, (int32_t)sw, (int32_t)m.line_h, C_SEL);
        }

        BeginScissorMode((int32_t)m.x, (int32_t)y, (int32_t)m.w, (int32_t)m.h);
        draw_line_text(&m, l, ly + text_dy);
        BeginScissorMode((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)m.h);

        const char *num = TextFormat("%*d", digits, l + 1);
        Color nc = err ? C_ERRMARK : (l == cursor_line ? C_LINENO_C : C_LINENO);
        DrawTextEx(g_font, num, (Vector2){ (float)(int32_t)(m.gutter_x + m.adv), (float)(int32_t)(ly + text_dy) }, m.size, 0.0f, nc);
        if (err) DrawRectangle((int32_t)m.gutter_x, (int32_t)ly, 3, (int32_t)m.line_h, C_ERRMARK);
    }

    if (g_focused && fmod(GetTime() - g_blink_start, 1.0) < 0.6) {
        float cx = m.x + col_of(g_cursor) * m.adv - g_scroll_x;
        float cy = m.y + ED_PAD + (cursor_line - first) * m.line_h;
        if (cx >= m.x - 1 && cursor_line >= first) DrawRectangle((int32_t)cx, (int32_t)cy + 1, 2, (int32_t)m.line_h - 2, RAYWHITE);
    }
    EndScissorMode();
    if (g_focused) DrawRectangleLinesEx((Rectangle){ x, y, w, m.h }, 1.0f, (Color){ 70, 110, 180, 255 });

    if (err_h > 0) {
        DrawRectangle((int32_t)x, (int32_t)err_rect.y, (int32_t)w, (int32_t)err_h, C_ERRBG);
        BeginScissorMode((int32_t)x, (int32_t)err_rect.y, (int32_t)w, (int32_t)err_h);
        for (int32_t r = 0; r < err_rows; r++) {
            char tmp[512];
            int32_t n = rows[r].len < 511 ? rows[r].len : 511;
            memcpy(tmp, g_errors + rows[r].start, (size_t)n);
            tmp[n] = 0;
            float ry = err_rect.y + ED_PAD + r * small_lh;
            bool hover = rows[r].src_line > 0 && mouse.y >= ry && mouse.y < ry + small_lh &&
                         CheckCollisionPointRec(mouse, err_rect);
            DrawTextEx(g_font, tmp, (Vector2){ x + ED_PAD, (float)(int32_t)ry }, small, 0.0f,
                       hover ? RAYWHITE : C_ERRTEXT);
        }
        EndScissorMode();
    }

    float sy = y + h - status_h;
    DrawRectangle((int32_t)x, (int32_t)sy, (int32_t)w, (int32_t)status_h, C_STATUS);
    const char *pos = TextFormat("Ln %d, Col %d", cursor_line + 1, col_of(g_cursor) + 1);
    float pw = MeasureTextEx(g_font, pos, small, 0.0f).x;
    BeginScissorMode((int32_t)x, (int32_t)sy, (int32_t)(w - pw - 3 * ED_PAD), (int32_t)status_h);
    DrawTextEx(g_font, g_info, (Vector2){ x + ED_PAD, (float)(int32_t)(sy + 3) }, small, 0.0f, C_STATUSTX);
    EndScissorMode();
    DrawTextEx(g_font, pos, (Vector2){ (float)(int32_t)(x + w - pw - ED_PAD), (float)(int32_t)(sy + 3) }, small, 0.0f, C_STATUSTX);
}
