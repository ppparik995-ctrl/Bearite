/*
 * Рэгдолл, версия 0.1 (мод Bearite, API версии 1) для Super Bear Adventure.
 *
 * Что делает:
 *  1) Пока медведь в воздухе (IsGrounded() == false дольше air_delay), он «обмякает»:
 *     родная анимация отключается, руки, ноги и голова болтаются на пружинках, корпус наклоняется.
 *  2) Пока медведь обмякший, горизонтальная скорость ограничена (слабое перемещение).
 *  3) На земле обмякший медведь лежит, пока не нажат прыжок. Нажатие прыжка только поднимает
 *     его (сам прыжок «съедается»), потом 0.3 секунды прыжок заблокирован, чтобы не подпрыгнул.
 *
 * Это не настоящая физика Unity с Rigidbody, а имитация на пружинах: так надёжнее для такого скелета.
 * Другие игроки этого не видят.
 *
 * Тик берём из BearEmoteController::Update (его игра зовёт каждый кадр), как в моде «Редактор эмоций»:
 * не включай их одновременно, они перехватывают один и тот же метод и правят одни и те же кости.
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

#define NL 5 /* голова, рука Л, рука П, нога Л, нога П */
static const char* LIMB_PATH[NL] = { "root/head", "root/hand_l", "root/hand_r", "root/foot_l", "root/foot_r" };

enum { S_NORMAL = 0, S_RAG = 1, S_GETUP = 2 };

static const BeariteApi* g_api = NULL;
static const char* g_id = "ragdoll";

static void* (*il2_object_get_class)(void*);
static void* (*il2_class_get_field)(void*, const char*);
static void (*il2_field_get_value)(void*, void*, void*);
static void* (*il2_string_new)(const char*);
static const char* (*c_get_name)(void*);
static void* (*c_get_methods)(void*, void**);
static const char* (*m_get_name_fn)(void*);
static uint32_t (*m_param_count)(void*);
static void* (*m_param)(void*, uint32_t);
static void* (*m_ret)(void*);
static char* (*t_get_name)(void*);

static Method m_find, m_get_lpos, m_set_lpos, m_get_lrot, m_set_lrot, m_get_pos, m_inv_dir, m_get_root;
static Method m_set_enabled, m_get_transform, m_time;
static Method m_is_local, m_is_grounded, m_is_swim, m_in_vehicle;

/* текущий медведь */
static void* g_owner = NULL;   /* BearEmoteController */
static void* g_pc = NULL;      /* PlayerBearController */
static void* g_anim = NULL;
static void* g_vis = NULL;     /* Transform "root": весь видимый медведь */
static void* g_limb[NL];
static int g_anim_off = 0;

/* состояние рэгдолла */
static int g_state = S_NORMAL;
static float g_air_t = 0, g_getup_t = 0, g_block_until = 0, g_last_time = 0;
static int g_grounded = 1;
static volatile int g_standup_req = 0;
static int g_ready = 0;

/* пружинки */
static Vec3 g_rest[NL], g_d[NL], g_vd[NL];
static Vec3 g_root_base_pos;
static Quat g_root_base_rot;
static float g_tilt = 0, g_wg = 0;
static Vec3 g_last_pos, g_last_vel;
static int g_have_pos = 0;

/* нажатия прыжка */
static void* g_inp_ptr = NULL;
static int g_inp_local = 0;
static int g_inp_calls = 0, g_inp_matched = 0, g_inp_warned = 0;
static int g_warned_local = 0;

static void logmsg(const char* m) { if (g_api && g_api->log) g_api->log(g_id, m); }
static void logint(const char* fmt, int a) { char b[240]; snprintf(b, sizeof b, fmt, a); logmsg(b); }
static void logs3(const char* fmt, const char* a, const char* b, const char* c) {
    char buf[400];
    snprintf(buf, sizeof buf, fmt, a ? a : "?", b ? b : "?", c ? c : "?");
    logmsg(buf);
}

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

static Vec3 lpos(void* t) { return ((Vec3 (*)(void*, const void*))m_get_lpos.code)(t, m_get_lpos.mi); }
static void set_lpos(void* t, Vec3 v) { ((void (*)(void*, Vec3, const void*))m_set_lpos.code)(t, v, m_set_lpos.mi); }
static Quat lrot(void* t) { return ((Quat (*)(void*, const void*))m_get_lrot.code)(t, m_get_lrot.mi); }
static void set_lrot(void* t, Quat q) { ((void (*)(void*, Quat, const void*))m_set_lrot.code)(t, q, m_set_lrot.mi); }
static Vec3 wpos(void* t) { return ((Vec3 (*)(void*, const void*))m_get_pos.code)(t, m_get_pos.mi); }
static Vec3 inv_dir(void* t, Vec3 d) { return ((Vec3 (*)(void*, Vec3, const void*))m_inv_dir.code)(t, d, m_inv_dir.mi); }

static void* get_transform(void* comp) {
    if (!comp || !m_get_transform.code) return NULL;
    return ((void* (*)(void*, const void*))m_get_transform.code)(comp, m_get_transform.mi);
}
static void* root_of(void* comp) {
    void* t = get_transform(comp);
    if (!t || !m_get_root.code) return NULL;
    return ((void* (*)(void*, const void*))m_get_root.code)(t, m_get_root.mi);
}
static int call_bool(Method m, void* self) {
    if (!m.code || !self) return 0;
    return ((uint8_t (*)(void*, const void*))m.code)(self, m.mi) != 0;
}

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
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float vlen(Vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

static void resolve(void* emote, void* pc) {
    memset(g_limb, 0, sizeof g_limb);
    g_owner = emote;
    g_pc = pc;
    g_state = S_NORMAL;
    g_anim_off = 0;
    g_have_pos = 0;
    g_inp_ptr = NULL;
    g_anim = get_field_obj(emote, "animator");
    void* root = g_anim ? get_transform(g_anim) : NULL;
    g_vis = NULL;
    if (!root || !m_find.code || !il2_string_new) { logmsg("Рэгдолл: не нашёл Transform аниматора"); return; }
    g_vis = ((void* (*)(void*, void*, const void*))m_find.code)(root, il2_string_new("root"), m_find.mi);
    int n = 0;
    for (int i = 0; i < NL; ++i) {
        g_limb[i] = ((void* (*)(void*, void*, const void*))m_find.code)(root, il2_string_new(LIMB_PATH[i]), m_find.mi);
        if (g_limb[i]) n++;
    }
    logint("Рэгдолл: найдено конечностей %d из 5", n);
    if (!g_vis) logmsg("Рэгдолл: не найден корень root, наклон корпуса не будет работать");
}

static void set_anim(int off) {
    if (g_anim_off == off) return;
    if (g_anim && m_set_enabled.code) {
        ((void (*)(void*, uint8_t, const void*))m_set_enabled.code)(g_anim, off ? 0 : 1, m_set_enabled.mi);
        g_anim_off = off;
    }
}

static void enter_rag(void) {
    for (int i = 0; i < NL; ++i) {
        if (g_limb[i]) g_rest[i] = lpos(g_limb[i]);
        memset(&g_d[i], 0, sizeof(Vec3));
        memset(&g_vd[i], 0, sizeof(Vec3));
    }
    if (g_vis) { g_root_base_pos = lpos(g_vis); g_root_base_rot = lrot(g_vis); }
    g_tilt = 0; g_wg = 0;
    set_anim(1);
    g_state = S_RAG;
}

static void leave_all(void) {
    for (int i = 0; i < NL; ++i) if (g_limb[i]) set_lpos(g_limb[i], g_rest[i]);
    if (g_vis) { set_lpos(g_vis, g_root_base_pos); set_lrot(g_vis, g_root_base_rot); }
    set_anim(0);
    g_state = S_NORMAL;
    g_air_t = 0;
}

/* Один шаг имитации; w = 1 полностью обмякший, 0 в норме (при вставании плавно убывает) */
static void simulate(float dt, float w) {
    float floppy = (float)g_api->get_float(g_id, "floppy", 1.0);
    float tilt_set = (float)g_api->get_float(g_id, "tilt", 75.0);
    if (floppy < 0.2f) floppy = 0.2f;
    if (dt > 0.033f) dt = 0.033f;
    if (dt < 0.0001f) return;

    /* скорость и ускорение корпуса в мире */
    Vec3 acc = {0, 0, 0};
    float vy = 0;
    if (g_vis) {
        Vec3 p = wpos(g_vis);
        if (g_have_pos) {
            Vec3 v = { (p.x - g_last_pos.x) / dt, (p.y - g_last_pos.y) / dt, (p.z - g_last_pos.z) / dt };
            acc.x = (v.x - g_last_vel.x) / dt; acc.y = (v.y - g_last_vel.y) / dt; acc.z = (v.z - g_last_vel.z) / dt;
            float al = vlen(acc);
            if (al > 60.0f) { float s = 60.0f / al; acc.x *= s; acc.y *= s; acc.z *= s; }
            g_last_vel = v;
            vy = v.y;
        } else { memset(&g_last_vel, 0, sizeof g_last_vel); }
        g_last_pos = p;
        g_have_pos = 1;
    }
    Vec3 gdir = {0, -1, 0};
    Vec3 acc_l = acc;
    if (g_vis && m_inv_dir.code) { gdir = inv_dir(g_vis, gdir); acc_l = inv_dir(g_vis, acc); }

    float k = 60.0f / floppy, c = 5.5f;
    float droop = 0.12f * floppy;
    g_wg += ((g_grounded ? 1.0f : 0.0f) - g_wg) * clampf(dt * 5.0f, 0, 1);

    for (int i = 0; i < NL; ++i) {
        if (!g_limb[i]) continue;
        Vec3 a;
        a.x = -k * g_d[i].x - c * g_vd[i].x + k * droop * gdir.x - 0.5f * floppy * acc_l.x;
        a.y = -k * g_d[i].y - c * g_vd[i].y + k * droop * gdir.y - 0.5f * floppy * acc_l.y;
        a.z = -k * g_d[i].z - c * g_vd[i].z + k * droop * gdir.z - 0.5f * floppy * acc_l.z;
        g_vd[i].x += a.x * dt; g_vd[i].y += a.y * dt; g_vd[i].z += a.z * dt;
        g_d[i].x += g_vd[i].x * dt; g_d[i].y += g_vd[i].y * dt; g_d[i].z += g_vd[i].z * dt;
        float dl = vlen(g_d[i]);
        if (dl > 0.45f) { float s = 0.45f / dl; g_d[i].x *= s; g_d[i].y *= s; g_d[i].z *= s; }
        /* на земле руки и ноги раскидываются в стороны */
        float side = g_rest[i].x >= 0 ? 1.0f : -1.0f;
        Vec3 sprawl = { side * 0.22f * g_wg, 0, 0 };
        if (i == 0) memset(&sprawl, 0, sizeof sprawl);
        Vec3 p = { g_rest[i].x + (g_d[i].x + sprawl.x) * w, g_rest[i].y + (g_d[i].y + sprawl.y) * w,
                   g_rest[i].z + (g_d[i].z + sprawl.z) * w };
        set_lpos(g_limb[i], p);
    }

    /* наклон корпуса: на земле заваливается, в воздухе кувыркается по скорости падения */
    if (g_vis) {
        float target = g_grounded ? tilt_set : clampf(-vy * 4.0f, -45.0f, 45.0f);
        g_tilt += (target - g_tilt) * clampf(dt * 6.0f, 0, 1);
        set_lrot(g_vis, qmul(g_root_base_rot, qaxis(1, 0, 0, g_tilt * w)));
    }
}

static void tick(void* emote, void* pc) {
    if (emote != g_owner || pc != g_pc) resolve(emote, pc);
    float now = now_time();
    float dt = now - g_last_time;
    g_last_time = now;
    if (dt < 0 || dt > 0.5f) dt = 0;

    if (!g_api->get_bool(g_id, "enable", 1)) { if (g_state != S_NORMAL) leave_all(); g_ready = 0; return; }
    g_ready = 1;

    g_grounded = call_bool(m_is_grounded, pc);
    int busy = call_bool(m_is_swim, pc) || call_bool(m_in_vehicle, pc);

    /* телепорт или возрождение: сбрасываем рэгдолл */
    if (g_vis && g_have_pos) {
        Vec3 p = wpos(g_vis);
        Vec3 d = { p.x - g_last_pos.x, p.y - g_last_pos.y, p.z - g_last_pos.z };
        if (vlen(d) > 6.0f) { if (g_state != S_NORMAL) leave_all(); g_have_pos = 0; g_standup_req = 0; return; }
    }

    if (g_state == S_NORMAL) {
        if (!g_grounded && !busy) g_air_t += dt; else g_air_t = 0;
        float need = (float)g_api->get_float(g_id, "air_delay", 0.1);
        if (g_air_t >= need && g_air_t > 0) { enter_rag(); logmsg("Рэгдолл: обмяк"); }
        return;
    }
    if (g_state == S_RAG) {
        if (busy) { g_standup_req = 0; g_state = S_GETUP; g_getup_t = 0; }
        else if (g_standup_req && g_grounded) { g_standup_req = 0; g_state = S_GETUP; g_getup_t = 0; logmsg("Рэгдолл: встаёт"); }
        else if (!g_grounded) g_standup_req = 0;
        simulate(dt, 1.0f);
        return;
    }
    /* S_GETUP */
    float dur = (float)g_api->get_float(g_id, "getup_sec", 0.35);
    if (dur < 0.05f) dur = 0.05f;
    g_getup_t += dt;
    if (!g_grounded && !busy) { g_state = S_RAG; return; }
    float w = 1.0f - g_getup_t / dur;
    if (w <= 0) { leave_all(); g_block_until = now + 0.3f; return; }
    simulate(dt, w);
}

/* ---- ограничение скорости (слабое перемещение) ---- */
static void (*o_uv)(void*, Vec3*, float, const void*);
static void h_uv(void* self, Vec3* vel, float dt, const void* mi) {
    o_uv(self, vel, dt, mi);
    if (!g_ready || g_state == S_NORMAL || self != g_pc || !vel) return;
    float lim = (float)g_api->get_float(g_id, "speed_limit", 3.0);
    float hx = vel->x, hz = vel->z;
    float hl = sqrtf(hx * hx + hz * hz);
    if (hl > lim && hl > 0.0001f) { float s = lim / hl; vel->x *= s; vel->z *= s; }
}

/* ---- прыжок: в обмякшем состоянии он только поднимает ---- */
static int is_local_input(void* self) {
    if (!g_pc) return 0;
    g_inp_calls++;
    if (self == g_inp_ptr) return g_inp_local;
    g_inp_ptr = self;
    void* a = root_of(self);
    void* b = root_of(g_pc);
    g_inp_local = (a && a == b);
    if (g_inp_local) g_inp_matched = 1;
    return g_inp_local;
}

static uint8_t jump_filter(void* self, uint8_t r, int press_event) {
    if (!g_ready) return r;
    int local = is_local_input(self);
    /* если по корню совпадения нет вообще, считаем нашим любой ввод (одиночная игра) */
    if (!local && !g_inp_matched && g_inp_calls > 200) {
        local = 1;
        if (!g_inp_warned) { g_inp_warned = 1; logmsg("Рэгдолл: ввод прыжка не привязан к медведю по корню, считаю любой ввод своим"); }
    }
    if (!local) return r;
    if (g_state != S_NORMAL) {
        if (r && press_event && g_grounded && g_state == S_RAG) g_standup_req = 1;
        return 0;
    }
    if (now_time() < g_block_until) return 0;
    return r;
}

static uint8_t (*o_jp)(void*, const void*);
static uint8_t h_jp(void* self, const void* mi) { return jump_filter(self, o_jp(self, mi), 1); }
static uint8_t (*o_jpb)(void*, const void*);
static uint8_t h_jpb(void* self, const void* mi) { return jump_filter(self, o_jpb(self, mi), 1); }
static uint8_t (*o_jh)(void*, const void*);
static uint8_t h_jh(void* self, const void* mi) { return jump_filter(self, o_jh(self, mi), 0); }

static void (*o_update)(void*, const void*);
static void h_update(void* self, const void* mi) {
    o_update(self, mi);
    if (!g_api) return;
    void* pc = get_field_obj(self, "playerController");
    if (!pc) return;
    int mine = 1;
    if (m_is_local.code) mine = call_bool(m_is_local, pc);
    else if (!g_warned_local) { g_warned_local = 1; logmsg("Рэгдолл: IsLocalPlayer не найден"); }
    if (mine) tick(self, pc);
}

/* Подписи нужных методов в лог: если что-то не совпадёт, по ним легко поправить мод */
static void dump_sigs(const char* cls, const char* a, const char* b, const char* c, const char* d) {
    void* k = bearite_find_class("", cls);
    if (!k || !c_get_methods) { logs3("Рэгдолл: класс %s не найден", cls, NULL, NULL); return; }
    void* it = NULL;
    void* m;
    while ((m = c_get_methods(k, &it)) != NULL) {
        const char* nm = m_get_name_fn ? m_get_name_fn(m) : NULL;
        if (!nm) continue;
        if (!(strstr(nm, a) || strstr(nm, b) || strstr(nm, c) || strstr(nm, d))) continue;
        char sig[200];
        size_t n = 0;
        uint32_t pc = m_param_count ? m_param_count(m) : 0;
        for (uint32_t i = 0; i < pc && n < sizeof sig - 60; ++i) {
            char* tn = (m_param && t_get_name) ? t_get_name(m_param(m, i)) : NULL;
            n += (size_t)snprintf(sig + n, sizeof sig - n, "%s%s", i ? ", " : "", tn ? tn : "?");
        }
        sig[n] = 0;
        char* rt = (m_ret && t_get_name) ? t_get_name(m_ret(m)) : NULL;
        char buf[400];
        snprintf(buf, sizeof buf, "Рэгдолл: %s: %s %s(%s)", cls, rt ? rt : "?", nm, sig);
        logmsg(buf);
    }
}

static void hook_bool(const char* name, void* repl, void** orig) {
    if (bearite_hook_method(g_api, g_id, "", "PlayerInputController", name, 0, repl, orig) != 0)
        logs3("Рэгдолл: не удалось перехватить PlayerInputController.%s (смотри подписи выше)", name, NULL, NULL);
}

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { logmsg("Рэгдолл: не найдены функции IL2CPP"); return; }

    il2_object_get_class = (void* (*)(void*))api->il2cpp_symbol("il2cpp_object_get_class");
    il2_class_get_field = (void* (*)(void*, const char*))api->il2cpp_symbol("il2cpp_class_get_field_from_name");
    il2_field_get_value = (void (*)(void*, void*, void*))api->il2cpp_symbol("il2cpp_field_get_value");
    il2_string_new = (void* (*)(const char*))api->il2cpp_symbol("il2cpp_string_new");
    c_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_class_get_name");
    c_get_methods = (void* (*)(void*, void**))api->il2cpp_symbol("il2cpp_class_get_methods");
    m_get_name_fn = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_name");
    m_param_count = (uint32_t (*)(void*))api->il2cpp_symbol("il2cpp_method_get_param_count");
    m_param = (void* (*)(void*, uint32_t))api->il2cpp_symbol("il2cpp_method_get_param");
    m_ret = (void* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_return_type");
    t_get_name = (char* (*)(void*))api->il2cpp_symbol("il2cpp_type_get_name");
    (void)c_get_name;

    m_find = find_m("UnityEngine", "Transform", "Find", 1);
    m_get_lpos = find_m("UnityEngine", "Transform", "get_localPosition", 0);
    m_set_lpos = find_m("UnityEngine", "Transform", "set_localPosition", 1);
    m_get_lrot = find_m("UnityEngine", "Transform", "get_localRotation", 0);
    m_set_lrot = find_m("UnityEngine", "Transform", "set_localRotation", 1);
    m_get_pos = find_m("UnityEngine", "Transform", "get_position", 0);
    m_inv_dir = find_m("UnityEngine", "Transform", "InverseTransformDirection", 1);
    m_get_root = find_m("UnityEngine", "Transform", "get_root", 0);
    m_set_enabled = find_m("UnityEngine", "Behaviour", "set_enabled", 1);
    m_get_transform = find_m("UnityEngine", "Component", "get_transform", 0);
    m_time = find_m("UnityEngine", "Time", "get_unscaledTime", 0);
    m_is_local = find_m("", "PlayerBearController", "get_IsLocalPlayer", 0);
    if (!m_is_local.code) m_is_local = find_m("", "PlayerBearController", "IsLocalPlayer", 0);
    m_is_grounded = find_m("", "PlayerBearController", "IsGrounded", 0);
    m_is_swim = find_m("", "PlayerBearController", "IsSwimmingUnderwater", 0);
    m_in_vehicle = find_m("", "PlayerBearController", "IsInVehicle", 0);

    dump_sigs("PlayerInputController", "Jump", "Jump", "Jump", "Jump");
    dump_sigs("PlayerBearController", "IsGrounded", "IsSwimmingUnderwater", "IsInVehicle", "IsLocalPlayer");

    int ok = m_find.code && m_get_lpos.code && m_set_lpos.code && m_get_lrot.code && m_set_lrot.code &&
             m_get_pos.code && m_set_enabled.code && m_get_transform.code && m_is_grounded.code && il2_string_new;
    logmsg(ok ? "Рэгдолл: все нужные методы найдены" : "Рэгдолл: НЕ все нужные методы найдены, пришли лог");
    if (!m_inv_dir.code) logmsg("Рэгдолл: InverseTransformDirection не найден, гравитация конечностей будет упрощённой");
    if (!m_get_root.code) logmsg("Рэгдолл: Transform.get_root не найден, привязка прыжка пойдёт через запасной путь");
    if (!ok) return;

    if (bearite_hook_method(api, g_id, "", "PlayerBearController", "UpdateVelocity", 2, (void*)h_uv, (void**)&o_uv) != 0)
        logmsg("Рэгдолл: не удалось перехватить UpdateVelocity, слабого перемещения не будет");
    hook_bool("IsJumpInputPressed", (void*)h_jp, (void**)&o_jp);
    hook_bool("IsJumpInputPressedOrBuffered", (void*)h_jpb, (void**)&o_jpb);
}
