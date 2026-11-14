/*
 * Редактор эмоций, версия 0.1 (мод Bearite, API версии 1) для Super Bear Adventure.
 *
 * Как работает: в режиме правки отключаем Animator медведя, и кости (Transform) остаются в позе.
 * Выбранную кость двигаем и вращаем ползунками из меню мода. Нужную позу сохраняем как кадр.
 * Кадры проигрываем сами: плавно переходим от позы к позе. Анимацию можно записать в файл и загрузить.
 * Эмоция видна только тебе: другие игроки получают от игры лишь номера готовых эмоций.
 *
 * Тик берём из BearEmoteController::Update (его игра зовёт каждый кадр). Нашего медведя узнаём по
 * полю playerController и методу IsLocalPlayer.
 * Кости ищем через Transform.Find от Transform аниматора, пути взяты из разведки скелета.
 *
 * Написано без проверки на устройстве. Если чего-то нет в игре, мод пишет об этом в «Лог».
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bearite_api.h"
#include "bearite_il2cpp.h"

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Quat;
typedef struct { void* code; void* mi; } Method;

#define NB 12
#define MAXF 24

static const char* BONE_PATH[NB] = {
    "root/body", "root/head", "root/hand_l", "root/hand_r", "root/foot_l", "root/foot_r",
    "root/body/Shicka/body", "root/body/Shicka/head", "root/body/Shicka/arm_l",
    "root/body/Shicka/arm_r", "root/body/Shicka/leg_l", "root/body/Shicka/leg_r"
};

static const BeariteApi* g_api = NULL;
static const char* g_id = "emote-maker";

static void* (*il2_object_get_class)(void*);
static void* (*il2_class_get_field)(void*, const char*);
static void (*il2_field_get_value)(void*, void*, void*);
static void* (*il2_string_new)(const char*);

static Method m_find, m_get_lpos, m_set_lpos, m_get_lrot, m_set_lrot, m_get_lscale, m_set_lscale;
static Method m_set_enabled, m_get_transform, m_is_local, m_time;

/* текущий медведь */
static void* g_owner = NULL;
static void* g_anim = NULL;
static void* g_bone[NB];
static int g_anim_off = 0;

/* правка */
static int g_sel = -1;
static float g_ref[6];
static Vec3 g_base_pos;
static Quat g_base_rot;
static int g_blink_i = -1;
static float g_blink_until = 0;
static Vec3 g_blink_scale;

/* кадры */
static int g_frames = 0;
static Vec3 g_fpos[MAXF][NB];
static Quat g_frot[MAXF][NB];
static unsigned char g_fvalid[MAXF][NB];

/* проигрывание и события */
static int g_was_play = 0;
static float g_play_t = 0;
static float g_last_time = 0;
static int g_last_ev[4] = {0, 0, 0, 0}; /* frame, del, save, load */
static int g_init = 0;
static int g_warned_local = 0;

static void logmsg(const char* m) { if (g_api && g_api->log) g_api->log(g_id, m); }
static void logint(const char* fmt, int a) { char b[240]; snprintf(b, sizeof b, fmt, a); logmsg(b); }
static void logint2(const char* fmt, int a, int c) { char b[240]; snprintf(b, sizeof b, fmt, a, c); logmsg(b); }

static Method find_m(const char* ns, const char* cls, const char* name, int argc) {
    Method r = {NULL, NULL};
    void* mi = bearite_find_method(ns, cls, name, argc);
    if (mi) { r.mi = mi; r.code = bearite_method_code(mi); }
    return r;
}

static void* get_field_obj(void* obj, const char* name) {
    if (!obj || !il2_object_get_class || !il2_class_get_field || !il2_field_get_value) return NULL;
    void* k = il2_object_get_class(obj);
    if (!k) return NULL;
    void* f = il2_class_get_field(k, name);
    if (!f) return NULL;
    void* v = NULL;
    il2_field_get_value(obj, f, &v);
    return v;
}

static float now_time(void) {
    if (!m_time.code) return 0.0f;
    return ((float (*)(const void*))m_time.code)(m_time.mi);
}

/* обёртки над методами Unity */
static Vec3 lpos(void* t) { return ((Vec3 (*)(void*, const void*))m_get_lpos.code)(t, m_get_lpos.mi); }
static void set_lpos(void* t, Vec3 v) { ((void (*)(void*, Vec3, const void*))m_set_lpos.code)(t, v, m_set_lpos.mi); }
static Quat lrot(void* t) { return ((Quat (*)(void*, const void*))m_get_lrot.code)(t, m_get_lrot.mi); }
static void set_lrot(void* t, Quat q) { ((void (*)(void*, Quat, const void*))m_set_lrot.code)(t, q, m_set_lrot.mi); }
static Vec3 lscale(void* t) { return ((Vec3 (*)(void*, const void*))m_get_lscale.code)(t, m_get_lscale.mi); }
static void set_lscale(void* t, Vec3 v) { ((void (*)(void*, Vec3, const void*))m_set_lscale.code)(t, v, m_set_lscale.mi); }

/* кватернионы */
static Quat qmul(Quat a, Quat b) {
    Quat r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}
static Quat qaxis(float ax, float ay, float az, float deg) {
    float h = deg * 0.0174532925f * 0.5f, s = sinf(h);
    Quat q = { ax * s, ay * s, az * s, cosf(h) };
    return q;
}
static Quat qnorm(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-6f) { Quat id = {0, 0, 0, 1}; return id; }
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    return q;
}
static Quat qnlerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    Quat r = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    return qnorm(r);
}

static void resolve_bones(void* emote) {
    memset(g_bone, 0, sizeof g_bone);
    g_owner = emote;
    g_anim_off = 0;
    g_sel = -1;
    g_blink_i = -1;
    g_anim = get_field_obj(emote, "animator");
    void* root = (g_anim && m_get_transform.code)
        ? ((void* (*)(void*, const void*))m_get_transform.code)(g_anim, m_get_transform.mi) : NULL;
    if (!root || !m_find.code || !il2_string_new) { logmsg("Редактор эмоций: не нашёл Transform аниматора"); return; }
    int n = 0;
    for (int i = 0; i < NB; ++i) {
        void* s = il2_string_new(BONE_PATH[i]);
        g_bone[i] = ((void* (*)(void*, void*, const void*))m_find.code)(root, s, m_find.mi);
        if (g_bone[i]) n++;
    }
    logint("Редактор эмоций: костей найдено %d из 12", n);
}

static void set_anim(int off) {
    if (g_anim_off == off) return;
    if (g_anim && m_set_enabled.code) {
        ((void (*)(void*, uint8_t, const void*))m_set_enabled.code)(g_anim, off ? 0 : 1, m_set_enabled.mi);
        g_anim_off = off;
    }
}

static void blink_end(void) {
    if (g_blink_i >= 0 && g_bone[g_blink_i]) set_lscale(g_bone[g_blink_i], g_blink_scale);
    g_blink_i = -1;
}
static void blink_start(int i, float now) {
    blink_end();
    if (!g_bone[i]) return;
    g_blink_scale = lscale(g_bone[i]);
    Vec3 s = g_blink_scale;
    s.x *= 1.6f; s.y *= 1.6f; s.z *= 1.6f;
    set_lscale(g_bone[i], s);
    g_blink_i = i;
    g_blink_until = now + 0.7f;
}

static void read_sliders(float v[6]) {
    static const char* K[6] = {"px", "py", "pz", "rx", "ry", "rz"};
    for (int i = 0; i < 6; ++i) v[i] = (float)g_api->get_float(g_id, K[i], 0.0);
}

static void rebase(void) {
    void* t = g_bone[g_sel];
    if (!t) return;
    g_base_pos = lpos(t);
    g_base_rot = lrot(t);
    read_sliders(g_ref);
}

static void apply_edit(void) {
    void* t = g_bone[g_sel];
    if (!t) return;
    float v[6];
    read_sliders(v);
    Vec3 p = { g_base_pos.x + (v[0] - g_ref[0]), g_base_pos.y + (v[1] - g_ref[1]), g_base_pos.z + (v[2] - g_ref[2]) };
    /* порядок поворотов как в Unity Euler: Z, затем X, затем Y */
    Quat d = qmul(qaxis(0, 1, 0, v[4] - g_ref[4]), qmul(qaxis(1, 0, 0, v[3] - g_ref[3]), qaxis(0, 0, 1, v[5] - g_ref[5])));
    set_lpos(t, p);
    set_lrot(t, qnorm(qmul(g_base_rot, d)));
}

static void capture_frame(void) {
    if (g_frames >= MAXF) { logmsg("Редактор эмоций: уже 24 кадра, больше нельзя"); return; }
    for (int i = 0; i < NB; ++i) {
        g_fvalid[g_frames][i] = g_bone[i] != NULL;
        if (g_bone[i]) { g_fpos[g_frames][i] = lpos(g_bone[i]); g_frot[g_frames][i] = lrot(g_bone[i]); }
    }
    g_frames++;
    logint("Редактор эмоций: сохранён кадр %d", g_frames);
}

static void play_step(float dt) {
    if (g_frames < 2) return;
    float dur = (float)g_api->get_float(g_id, "frame_sec", 0.5);
    if (dur < 0.05f) dur = 0.05f;
    int loop = g_api->get_bool(g_id, "loop", 1) != 0;
    g_play_t += dt;
    int nseg = loop ? g_frames : g_frames - 1;
    float pt = g_play_t / dur;
    int i0, i1;
    float f;
    if (!loop && pt >= (float)nseg) { i0 = i1 = g_frames - 1; f = 0; }
    else {
        while (pt >= (float)nseg) pt -= (float)nseg;
        i0 = (int)pt; f = pt - (float)i0; i1 = (i0 + 1) % g_frames;
        f = f * f * (3.0f - 2.0f * f);
    }
    for (int b = 0; b < NB; ++b) {
        if (!g_bone[b] || !g_fvalid[i0][b] || !g_fvalid[i1][b]) continue;
        Vec3 a = g_fpos[i0][b], c = g_fpos[i1][b];
        Vec3 p = { a.x + (c.x - a.x) * f, a.y + (c.y - a.y) * f, a.z + (c.z - a.z) * f };
        set_lpos(g_bone[b], p);
        set_lrot(g_bone[b], qnlerp(g_frot[i0][b], g_frot[i1][b], f));
    }
}

static int file_path(char* out, size_t n) {
    const char* dir = g_api->data_dir ? g_api->data_dir() : NULL;
    if (!dir) return 0;
    int slot = (int)(g_api->get_float(g_id, "slot", 1.0) + 0.5);
    if (slot < 1) slot = 1;
    if (slot > 5) slot = 5;
    snprintf(out, n, "%s/emote_%d.txt", dir, slot);
    return slot;
}

static void save_file(void) {
    char path[512];
    int slot = file_path(path, sizeof path);
    if (!slot) { logmsg("Редактор эмоций: нет папки для файлов"); return; }
    FILE* f = fopen(path, "w");
    if (!f) { logmsg("Редактор эмоций: не удалось открыть файл для записи"); return; }
    fprintf(f, "EM1 %d %d\n", g_frames, NB);
    for (int i = 0; i < g_frames; ++i)
        for (int b = 0; b < NB; ++b)
            fprintf(f, "%d %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n", g_fvalid[i][b],
                    g_fpos[i][b].x, g_fpos[i][b].y, g_fpos[i][b].z,
                    g_frot[i][b].x, g_frot[i][b].y, g_frot[i][b].z, g_frot[i][b].w);
    fclose(f);
    logint2("Редактор эмоций: слот %d, записано кадров %d", slot, g_frames);
}

static void load_file(void) {
    char path[512];
    int slot = file_path(path, sizeof path);
    if (!slot) { logmsg("Редактор эмоций: нет папки для файлов"); return; }
    FILE* f = fopen(path, "r");
    if (!f) { logint("Редактор эмоций: слот %d пуст", slot); return; }
    int nf = 0, nb = 0;
    if (fscanf(f, "EM1 %d %d", &nf, &nb) != 2 || nf < 0 || nf > MAXF || nb != NB) {
        fclose(f); logmsg("Редактор эмоций: файл повреждён или от другой версии"); return;
    }
    int ok = 1;
    for (int i = 0; i < nf && ok; ++i)
        for (int b = 0; b < NB && ok; ++b) {
            int v = 0;
            Vec3 p; Quat q;
            if (fscanf(f, "%d %f %f %f %f %f %f %f", &v, &p.x, &p.y, &p.z, &q.x, &q.y, &q.z, &q.w) != 8) { ok = 0; break; }
            g_fvalid[i][b] = (unsigned char)v; g_fpos[i][b] = p; g_frot[i][b] = q;
        }
    fclose(f);
    if (!ok) { logmsg("Редактор эмоций: файл оборван"); return; }
    g_frames = nf;
    logint2("Редактор эмоций: слот %d, загружено кадров %d", slot, nf);
}

static void tick(void* emote) {
    if (emote != g_owner) resolve_bones(emote);
    float now = now_time();
    float dt = now - g_last_time;
    g_last_time = now;
    if (dt < 0 || dt > 0.5f) dt = 0;

    int ev[4];
    ev[0] = g_api->get_bool(g_id, "frame", 0) != 0;
    ev[1] = g_api->get_bool(g_id, "del", 0) != 0;
    ev[2] = g_api->get_bool(g_id, "save", 0) != 0;
    ev[3] = g_api->get_bool(g_id, "load", 0) != 0;
    if (!g_init) { for (int i = 0; i < 4; ++i) g_last_ev[i] = ev[i]; g_init = 1; return; }
    if (ev[0] != g_last_ev[0]) { g_last_ev[0] = ev[0]; capture_frame(); }
    if (ev[1] != g_last_ev[1]) { g_last_ev[1] = ev[1]; if (g_frames > 0) { g_frames--; logint("Редактор эмоций: осталось кадров %d", g_frames); } }
    if (ev[2] != g_last_ev[2]) { g_last_ev[2] = ev[2]; save_file(); }
    if (ev[3] != g_last_ev[3]) { g_last_ev[3] = ev[3]; load_file(); }

    int edit = g_api->get_bool(g_id, "edit", 0) != 0;
    int play = g_api->get_bool(g_id, "play", 0) != 0;
    int want_off = edit || play;
    if (want_off && !g_anim_off) { set_anim(1); g_sel = -1; }
    if (!want_off && g_anim_off) { blink_end(); set_anim(0); g_was_play = 0; return; }
    if (!want_off) return;

    if (g_blink_i >= 0 && now > g_blink_until) blink_end();

    if (play) {
        if (!g_was_play) { g_was_play = 1; g_play_t = 0; blink_end(); if (g_frames < 2) logmsg("Редактор эмоций: для проигрывания нужно минимум 2 кадра"); }
        play_step(dt);
        return;
    }
    if (g_was_play) { g_was_play = 0; g_sel = -1; }

    int sel = (int)(g_api->get_float(g_id, "bone", 0.0) + 0.5);
    if (sel < 0) sel = 0;
    if (sel >= NB) sel = NB - 1;
    if (sel != g_sel) { g_sel = sel; rebase(); blink_start(sel, now); }
    apply_edit();
}

static void (*o_update)(void*, const void*);
static void h_update(void* self, const void* mi) {
    o_update(self, mi);
    if (!g_api) return;
    void* pc = get_field_obj(self, "playerController");
    if (!pc) return;
    int mine = 1;
    if (m_is_local.code) mine = ((uint8_t (*)(void*, const void*))m_is_local.code)(pc, m_is_local.mi) != 0;
    else if (!g_warned_local) { g_warned_local = 1; logmsg("Редактор эмоций: IsLocalPlayer не найден"); }
    if (mine) tick(self);
}

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { logmsg("Редактор эмоций: не найдены функции IL2CPP"); return; }

    il2_object_get_class = (void* (*)(void*))api->il2cpp_symbol("il2cpp_object_get_class");
    il2_class_get_field = (void* (*)(void*, const char*))api->il2cpp_symbol("il2cpp_class_get_field_from_name");
    il2_field_get_value = (void (*)(void*, void*, void*))api->il2cpp_symbol("il2cpp_field_get_value");
    il2_string_new = (void* (*)(const char*))api->il2cpp_symbol("il2cpp_string_new");

    m_find = find_m("UnityEngine", "Transform", "Find", 1);
    m_get_lpos = find_m("UnityEngine", "Transform", "get_localPosition", 0);
    m_set_lpos = find_m("UnityEngine", "Transform", "set_localPosition", 1);
    m_get_lrot = find_m("UnityEngine", "Transform", "get_localRotation", 0);
    m_set_lrot = find_m("UnityEngine", "Transform", "set_localRotation", 1);
    m_get_lscale = find_m("UnityEngine", "Transform", "get_localScale", 0);
    m_set_lscale = find_m("UnityEngine", "Transform", "set_localScale", 1);
    m_set_enabled = find_m("UnityEngine", "Behaviour", "set_enabled", 1);
    m_get_transform = find_m("UnityEngine", "Component", "get_transform", 0);
    m_time = find_m("UnityEngine", "Time", "get_unscaledTime", 0);
    m_is_local = find_m("", "PlayerBearController", "get_IsLocalPlayer", 0);
    if (!m_is_local.code) m_is_local = find_m("", "PlayerBearController", "IsLocalPlayer", 0);

    int ok = m_find.code && m_get_lpos.code && m_set_lpos.code && m_get_lrot.code && m_set_lrot.code &&
             m_get_lscale.code && m_set_lscale.code && m_set_enabled.code && m_get_transform.code && il2_string_new;
    logmsg(ok ? "Редактор эмоций: все методы Unity найдены" : "Редактор эмоций: НЕ все методы Unity найдены, пришли лог");
    if (!ok) return;

    if (bearite_hook_method(api, g_id, "", "BearEmoteController", "Update", 0,
                            (void*)h_update, (void**)&o_update) != 0) {
        logmsg("Редактор эмоций: не удалось перехватить BearEmoteController.Update (выключи Студию эмоций, если она установлена)");
        return;
    }
    logmsg("Редактор эмоций 0.1: готов. Кнопки слева и справа на экране, ползунки в меню мода");
}
