/*
 * Рэгдолл, версия 0.2 (мод Bearite, API версии 1) для Super Bear Adventure.
 *
 * Что делает:
 *  1) Пока медведь в воздухе (IsGrounded() == false или быстрое падение дольше air_delay),
 *     он «обмякает»: родная анимация отключается, руки, ноги и голова болтаются на пружинках.
 *  2) Пока медведь обмякший, горизонтальная скорость ограничена (слабое перемещение).
 *  3) На земле обмякший медведь лежит, пока не нажат прыжок. Нажатие прыжка только поднимает его:
 *     толчок вверх с земли в UpdateVelocity гасится, ещё 0.3 секунды прыжок заблокирован.
 *
 * Что нового в 0.2 по сравнению с 0.1:
 *  - в воде, на транспорте и в особых состояниях игрока рэгдолла нет;
 *  - перед каждым обращением к костям проверяется, что объекты не уничтожены (иначе исключение Unity);
 *  - время берётся из системных часов, а не из Unity (Bearite перехватывает Time.*);
 *  - падение определяется ещё и по скорости, поэтому чит air_jump не мешает;
 *  - убраны перехваты ввода прыжка, прыжок отменяется через скорость;
 *  - защита от NaN, стабильный выбор «своего» медведя, подробный лог.
 *
 * Это имитация на пружинах, а не настоящая физика Unity с Rigidbody. Другие игроки её не видят.
 * Тик берём из BearEmoteController::Update: не включай одновременно «Редактор эмоций»,
 * «Студию эмоций» и «Разведку эмоций», они перехватывают тот же метод.
 *
 * Написано без проверки на устройстве. Если чего-то нет в игре, мод пишет об этом в «Лог».
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "bearite_api.h"
#include "bearite_il2cpp.h"

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Quat;
typedef struct { void* code; void* mi; } Method;

#define NL 5 /* голова, рука Л, рука П, нога Л, нога П */
static const char* LIMB_PATH[NL] = { "root/head", "root/hand_l", "root/hand_r", "root/foot_l", "root/foot_r" };

enum { S_NORMAL = 0, S_RAG = 1, S_GETUP = 2 };
#define JUMP_VY 4.0f     /* толчок вверх быстрее этого считаем нажатием прыжка */
#define FALL_VY (-3.5f)  /* падение быстрее этого считаем воздухом, даже если IsGrounded врёт */

static const BeariteApi* g_api = NULL;
static const char* g_id = "ragdoll";

static void* (*il2_object_get_class)(void*);
static void* (*il2_class_get_field)(void*, const char*);
static void (*il2_field_get_value)(void*, void*, void*);
static void* (*il2_string_new)(const char*);
static int (*il2_type_get_type)(void*);
static const char* (*c_get_name)(void*);
static void* (*c_get_methods)(void*, void**);
static const char* (*m_get_name_fn)(void*);
static uint32_t (*m_param_count)(void*);
static void* (*m_param)(void*, uint32_t);
static void* (*m_ret)(void*);
static char* (*t_get_name)(void*);

static Method m_find, m_get_lpos, m_set_lpos, m_get_lrot, m_set_lrot, m_get_pos, m_inv_dir;
static Method m_set_enabled, m_get_transform, m_alive;
static Method m_is_local, m_is_grounded, m_is_swim, m_in_vehicle, m_shallow, m_knocked, m_get_state, m_pt_water;
static int g_state_is_class = 0;

/* текущий медведь */
static void* g_owner = NULL;   /* BearEmoteController */
static void* g_pc = NULL;      /* PlayerBearController */
static void* g_anim = NULL;
static void* g_vis = NULL;     /* Transform "root": весь видимый медведь */
static void* g_limb[NL];
static int g_anim_off = 0;
static int g_calls = 0, g_owner_seen = 0;

/* состояние рэгдолла */
static int g_state = S_NORMAL;
static float g_air_t = 0, g_getup_t = 0;
static double g_block_until = 0, g_last_time = 0;
static int g_grounded = 1;      /* с поправкой на скорость падения */
static volatile int g_standup_req = 0;
static int g_ready = 0;

/* движение корпуса */
static Vec3 g_last_pos, g_last_vel, g_acc;
static float g_vy = 0;
static int g_have_pos = 0;

/* пружинки */
static Vec3 g_rest[NL], g_d[NL], g_vd[NL];
static Vec3 g_root_base_pos;
static Quat g_root_base_rot;
static float g_tilt = 0, g_wg = 0;

/* лог */
static int g_warned_local = 0, g_cheat_warned = 0, g_state_logs = 0, g_water_logs = 0, g_water_prev = -1;
static float g_cheat_t = 0;
static char g_last_state[64] = "";

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

/* системные часы: Time.* у Bearite перехвачены, поэтому Unity-время не используем */
static double now_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
static int call_bool(Method m, void* self) {
    if (!m.code || !self) return 0;
    return ((uint8_t (*)(void*, const void*))m.code)(self, m.mi) != 0;
}
/* Объект Unity ещё жив? (UnityEngine.Object.op_Implicit). NULL считаем «нечего проверять» */
static int alive(void* o) {
    if (!o) return 1;
    if (!m_alive.code) return 1;
    return ((uint8_t (*)(void*, const void*))m_alive.code)(o, m_alive.mi) != 0;
}

static int finite3(Vec3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float vlen(Vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

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

/* Имя класса текущего состояния игрока (если GetPlayerState возвращает объект, а не число) */
static const char* state_name(void* pc) {
    if (!g_state_is_class || !m_get_state.code || !il2_object_get_class || !c_get_name) return NULL;
    void* o = ((void* (*)(void*, const void*))m_get_state.code)(pc, m_get_state.mi);
    if (!o) return NULL;
    void* k = il2_object_get_class(o);
    return k ? c_get_name(k) : NULL;
}
static int has(const char* s, const char* sub) { return s && strstr(s, sub) != NULL; }
static int bad_state(const char* n) {
    return has(n, "Swim") || has(n, "Water") || has(n, "Climb") || has(n, "Vehicle") || has(n, "Dead") ||
           has(n, "Death") || has(n, "Hid") || has(n, "Grab") || has(n, "Knock") || has(n, "Dive") ||
           has(n, "Cannon") || has(n, "Emote") || has(n, "Cinematic") || has(n, "Frozen") || has(n, "Ladder");
}

static void resolve(void* emote, void* pc) {
    /* старые указатели могли устареть (смена уровня), поэтому их не трогаем */
    memset(g_limb, 0, sizeof g_limb);
    g_owner = emote;
    g_pc = pc;
    g_state = S_NORMAL;
    g_anim_off = 0;
    g_have_pos = 0;
    g_standup_req = 0;
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
    if (!g_vis) logmsg("Рэгдолл: не найден корень root, наклона корпуса не будет");
}

/* Все ли нужные объекты ещё живы. Если нет, ищем кости заново и пропускаем кадр */
static int validate(void) {
    int ok = alive(g_anim) && alive(g_vis);
    for (int i = 0; i < NL && ok; ++i) ok = alive(g_limb[i]);
    if (!ok) { logmsg("Рэгдолл: объекты медведя уничтожены, ищу заново"); resolve(g_owner, g_pc); return 0; }
    return 1;
}

static void set_anim(int off) {
    if (g_anim_off == off) return;
    if (g_anim && m_set_enabled.code) {
        ((void (*)(void*, uint8_t, const void*))m_set_enabled.code)(g_anim, off ? 0 : 1, m_set_enabled.mi);
        g_anim_off = off;
    }
}

static int can_ragdoll(void) {
    if (g_vis) return 1;
    for (int i = 0; i < NL; ++i) if (g_limb[i]) return 1;
    return 0;
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
    if (validate()) {
        for (int i = 0; i < NL; ++i) if (g_limb[i]) set_lpos(g_limb[i], g_rest[i]);
        if (g_vis) { set_lpos(g_vis, g_root_base_pos); set_lrot(g_vis, g_root_base_rot); }
        set_anim(0);
    }
    g_state = S_NORMAL;
    g_air_t = 0;
}

/* Скорость и ускорение корпуса по смещению. Возвращает 0, если был телепорт или возрождение */
static int update_motion(float dt) {
    if (!g_vis) return 1;
    Vec3 p = wpos(g_vis);
    if (!finite3(p)) return 1;
    int ok = 1;
    if (g_have_pos && dt > 0.0001f) {
        Vec3 d = { p.x - g_last_pos.x, p.y - g_last_pos.y, p.z - g_last_pos.z };
        if (vlen(d) > 6.0f) { ok = 0; g_have_pos = 0; memset(&g_last_vel, 0, sizeof g_last_vel); memset(&g_acc, 0, sizeof g_acc); g_vy = 0; }
        else {
            Vec3 v = { d.x / dt, d.y / dt, d.z / dt };
            Vec3 a = { (v.x - g_last_vel.x) / dt, (v.y - g_last_vel.y) / dt, (v.z - g_last_vel.z) / dt };
            float al = vlen(a);
            if (al > 60.0f) { float s = 60.0f / al; a.x *= s; a.y *= s; a.z *= s; }
            g_last_vel = v; g_acc = a; g_vy = v.y;
        }
    } else { memset(&g_last_vel, 0, sizeof g_last_vel); memset(&g_acc, 0, sizeof g_acc); g_vy = 0; }
    g_last_pos = p;
    g_have_pos = ok ? 1 : 0;
    return ok;
}

/* Один шаг имитации; w = 1 полностью обмякший, 0 в норме (при вставании плавно убывает) */
static void simulate(float dt, float w) {
    float floppy = (float)g_api->get_float(g_id, "floppy", 1.0);
    float tilt_set = (float)g_api->get_float(g_id, "tilt", 75.0);
    if (floppy < 0.2f) floppy = 0.2f;
    if (dt > 0.033f) dt = 0.033f;
    if (dt < 0.0001f) return;

    Vec3 gdir = {0, -1, 0};
    Vec3 acc_l = g_acc;
    if (g_vis && m_inv_dir.code) { gdir = inv_dir(g_vis, gdir); acc_l = inv_dir(g_vis, g_acc); }
    if (!finite3(gdir)) { gdir.x = 0; gdir.y = -1; gdir.z = 0; }
    if (!finite3(acc_l)) memset(&acc_l, 0, sizeof acc_l);

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
        if (!finite3(g_d[i]) || !finite3(g_vd[i])) { memset(&g_d[i], 0, sizeof(Vec3)); memset(&g_vd[i], 0, sizeof(Vec3)); }
        float dl = vlen(g_d[i]);
        if (dl > 0.45f) { float s = 0.45f / dl; g_d[i].x *= s; g_d[i].y *= s; g_d[i].z *= s; }
        /* на земле руки и ноги раскидываются в стороны */
        float side = g_rest[i].x >= 0 ? 1.0f : -1.0f;
        float sx = (i == 0) ? 0.0f : side * 0.22f * g_wg;
        Vec3 p = { g_rest[i].x + (g_d[i].x + sx) * w, g_rest[i].y + g_d[i].y * w, g_rest[i].z + g_d[i].z * w };
        set_lpos(g_limb[i], p);
    }

    /* наклон корпуса: на земле заваливается, в воздухе кувыркается по скорости падения */
    if (g_vis) {
        float target = g_grounded ? tilt_set : clampf(-g_vy * 4.0f, -45.0f, 45.0f);
        if (!isfinite(g_tilt)) g_tilt = 0;
        g_tilt += (target - g_tilt) * clampf(dt * 6.0f, 0, 1);
        set_lrot(g_vis, qmul(g_root_base_rot, qaxis(1, 0, 0, g_tilt * w)));
    }
}

static void tick(void* emote, void* pc) {
    if (emote != g_owner) resolve(emote, pc);
    g_pc = pc;
    double now = now_time();
    float dt = (float)(now - g_last_time);
    g_last_time = now;
    if (dt < 0 || dt > 0.2f) dt = 0;

    if (!g_api->get_bool(g_id, "enable", 1)) { if (g_state != S_NORMAL) leave_all(); g_ready = 0; return; }
    g_ready = 1;
    if (dt <= 0) return;
    if (!validate()) return;
    if (!update_motion(dt)) { if (g_state != S_NORMAL) leave_all(); g_standup_req = 0; return; }

    int raw = call_bool(m_is_grounded, pc);
    g_grounded = raw && g_vy > FALL_VY;
    /* IsGrounded всегда «да», хотя медведь падает: скорее всего включён чит air_jump */
    if (raw && g_vy < -6.0f) { g_cheat_t += dt; if (g_cheat_t > 0.3f && !g_cheat_warned) { g_cheat_warned = 1; logmsg("Рэгдолл: IsGrounded говорит «на земле» при падении, выключи чит air_jump"); } }
    else g_cheat_t = 0;

    const char* sname = state_name(pc);
    if (sname && strcmp(sname, g_last_state) != 0 && g_state_logs < 60) {
        g_state_logs++;
        snprintf(g_last_state, sizeof g_last_state, "%s", sname);
        logs3("Рэгдолл: состояние игрока: %s", sname, NULL, NULL);
    }
    int water = 0;
    if (m_pt_water.code && g_have_pos && g_api->get_bool(g_id, "water_check", 1))
        water = ((uint8_t (*)(void*, Vec3, const void*))m_pt_water.code)(pc, g_last_pos, m_pt_water.mi) != 0;
    if (water != g_water_prev && g_water_logs < 30) { g_water_logs++; g_water_prev = water; logmsg(water ? "Рэгдолл: медведь в воде" : "Рэгдолл: медведь вне воды"); }
    int busy = water || call_bool(m_is_swim, pc) || call_bool(m_in_vehicle, pc) || call_bool(m_shallow, pc) ||
               call_bool(m_knocked, pc) || bad_state(sname);

    if (g_state == S_NORMAL) {
        if (!g_grounded && !busy) g_air_t += dt; else g_air_t = 0;
        float need = (float)g_api->get_float(g_id, "air_delay", 0.1);
        if (g_air_t >= need && g_air_t > 0 && can_ragdoll()) { enter_rag(); logmsg("Рэгдолл: обмяк"); }
        return;
    }
    if (g_state == S_RAG) {
        if (busy) { g_standup_req = 0; g_state = S_GETUP; g_getup_t = 0; logmsg("Рэгдолл: особое состояние, встаю"); }
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
    if (w <= 0 || busy) { leave_all(); g_block_until = now + 0.3; return; }
    simulate(dt, w);
}

/* ---- слабое перемещение и отмена прыжка при вставании ---- */
static void (*o_uv)(void*, Vec3*, float, const void*);
static void h_uv(void* self, Vec3* vel, float dt, const void* mi) {
    o_uv(self, vel, dt, mi);
    if (!g_ready || self != g_pc || !vel) return;
    if (!finite3(*vel)) return;
    if (g_state == S_NORMAL) {
        /* сразу после вставания прыжок заблокирован */
        if (g_grounded && now_time() < g_block_until && vel->y > JUMP_VY) vel->y = 0;
        return;
    }
    float lim = (float)g_api->get_float(g_id, "speed_limit", 3.0);
    float hl = sqrtf(vel->x * vel->x + vel->z * vel->z);
    if (hl > lim && hl > 0.0001f) { float s = lim / hl; vel->x *= s; vel->z *= s; }
    /* резкий толчок вверх с земли = нажат прыжок: встаём, а сам прыжок отменяем */
    if (g_grounded && vel->y > JUMP_VY) {
        vel->y = 0;
        if (g_state == S_RAG) g_standup_req = 1;
    } else if (g_state == S_GETUP && g_grounded && vel->y > 0.5f) {
        vel->y = 0;
    }
}

static void (*o_update)(void*, const void*);
static void h_update(void* self, const void* mi) {
    o_update(self, mi);
    if (!g_api) return;
    void* pc = get_field_obj(self, "playerController");
    if (!pc) return;
    int mine = 1;
    if (m_is_local.code) mine = call_bool(m_is_local, pc);
    else if (!g_warned_local) { g_warned_local = 1; logmsg("Рэгдолл: IsLocalPlayer не найден"); }
    if (!mine) return;
    g_calls++;
    if (self == g_owner) { g_owner_seen = g_calls; tick(self, pc); }
    else if (g_owner == NULL || g_calls - g_owner_seen > 90) tick(self, pc); /* прежний владелец пропал */
}

/* Подписи нужных методов в лог: если что-то не совпадёт, по ним легко поправить мод */
static void dump_sigs(const char* cls, const char* const* pats, int np) {
    void* k = bearite_find_class("", cls);
    if (!k || !c_get_methods) { logs3("Рэгдолл: класс %s не найден", cls, NULL, NULL); return; }
    void* it = NULL;
    void* m;
    while ((m = c_get_methods(k, &it)) != NULL) {
        const char* nm = m_get_name_fn ? m_get_name_fn(m) : NULL;
        if (!nm) continue;
        int hit = 0;
        for (int i = 0; i < np; ++i) if (strstr(nm, pats[i])) hit = 1;
        if (!hit) continue;
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

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { logmsg("Рэгдолл: не найдены функции IL2CPP"); return; }

    il2_object_get_class = (void* (*)(void*))api->il2cpp_symbol("il2cpp_object_get_class");
    il2_class_get_field = (void* (*)(void*, const char*))api->il2cpp_symbol("il2cpp_class_get_field_from_name");
    il2_field_get_value = (void (*)(void*, void*, void*))api->il2cpp
