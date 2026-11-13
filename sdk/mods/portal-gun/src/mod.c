/*
 * Portal Gun, версия 0.1 (мод Bearite, API версии 1) для Super Bear Adventure.
 *
 * Что умеет: кнопка A запоминает место, где стоит медведь, кнопка B запоминает вторую точку,
 * кнопка-стрелка переносит медведя в другую точку. Видимых порталов пока нет.
 *
 * Как работает: перехватываем UpdateVelocity у контроллера игрока, чтобы знать, кто сейчас игрок.
 * Раз в кадр (через Camera.get_main) смотрим, не нажали ли кнопку: кнопки это переключатели,
 * мод считает нажатием любое изменение значения.
 * Позиция читается и ставится через KinematicCharacterMotor, запасной вариант: Transform.
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
typedef struct { void* code; void* mi; } Method;

static const BeariteApi* g_api = NULL;
static const char* g_id = "portal-gun";

/* дополнительные функции IL2CPP (в bearite_il2cpp.h их нет) */
static void* (*il2_object_get_class)(void* obj);
static void* (*il2_class_get_field)(void* klass, const char* name);
static void (*il2_field_get_value)(void* obj, void* field, void* value);

/* найденные методы игры и Unity */
static Method m_get_tpos;      /* KinematicCharacterMotor.get_TransientPosition() */
static Method m_set_pos2;      /* KinematicCharacterMotor.SetPosition(Vector3, bool) */
static Method m_set_pos1;      /* KinematicCharacterMotor.SetPosition(Vector3) */
static Method m_set_tpos;      /* KinematicCharacterMotor.SetTransientPosition(Vector3) */
static Method m_get_transform; /* Component.get_transform() */
static Method m_tr_get_pos;    /* Transform.get_position() */
static Method m_tr_set_pos;    /* Transform.set_position(Vector3) */
static Method m_is_local;      /* PlayerBearController.IsLocalPlayer */
static Method m_frame;         /* Time.get_frameCount() */

/* игрок: контроллер и кадр, когда мы его видели в последний раз */
static void* g_player = NULL;
static int g_player_frame = -1000;
static int g_warned_local = 0;

/* две точки */
static Vec3 g_pt[2];
static int g_has[2] = {0, 0};

/* прошлые значения кнопок */
static int g_last[3] = {0, 0, 0};
static int g_init = 0;
static int g_last_tick_frame = -1;

static void logmsg(const char* m) { if (g_api && g_api->log) g_api->log(g_id, m); }
static void logfmt(const char* fmt, double a, double b, double c) {
    char buf[200];
    snprintf(buf, sizeof buf, fmt, a, b, c);
    logmsg(buf);
}

static Method find_m(const char* ns, const char* cls, const char* name, int argc) {
    Method r = {NULL, NULL};
    void* mi = bearite_find_method(ns, cls, name, argc);
    if (mi) { r.mi = mi; r.code = bearite_method_code(mi); }
    return r;
}

/* Мотор может лежать в пространстве имён KinematicCharacterController или без него */
static Method find_motor(const char* name, int argc) {
    Method r = find_m("KinematicCharacterController", "KinematicCharacterMotor", name, argc);
    if (!r.code) r = find_m("", "KinematicCharacterMotor", name, argc);
    return r;
}

static int cur_frame(void) {
    if (!m_frame.code) return 0;
    return ((int (*)(const void*))m_frame.code)(m_frame.mi);
}

/* Мотор персонажа из контроллера (поле Motor или _motor) */
static void* get_motor(void* ctrl) {
    if (!ctrl || !il2_object_get_class || !il2_class_get_field || !il2_field_get_value) return NULL;
    void* k = il2_object_get_class(ctrl);
    if (!k) return NULL;
    static const char* names[] = {"Motor", "_motor", "motor", NULL};
    for (int i = 0; names[i]; ++i) {
        void* f = il2_class_get_field(k, names[i]);
        if (!f) continue;
        void* m = NULL;
        il2_field_get_value(ctrl, f, &m);
        if (m) return m;
    }
    return NULL;
}

static void* get_transform(void* ctrl) {
    if (!m_get_transform.code) return NULL;
    return ((void* (*)(void*, const void*))m_get_transform.code)(ctrl, m_get_transform.mi);
}

static int get_pos(void* ctrl, Vec3* out) {
    void* motor = get_motor(ctrl);
    if (motor && m_get_tpos.code) {
        *out = ((Vec3 (*)(void*, const void*))m_get_tpos.code)(motor, m_get_tpos.mi);
        return 1;
    }
    void* tr = get_transform(ctrl);
    if (tr && m_tr_get_pos.code) {
        *out = ((Vec3 (*)(void*, const void*))m_tr_get_pos.code)(tr, m_tr_get_pos.mi);
        return 1;
    }
    return 0;
}

static int set_pos(void* ctrl, Vec3 p) {
    void* motor = get_motor(ctrl);
    if (motor && m_set_pos2.code) {
        ((void (*)(void*, Vec3, uint8_t, const void*))m_set_pos2.code)(motor, p, 1, m_set_pos2.mi);
        return 1;
    }
    if (motor && m_set_pos1.code) {
        ((void (*)(void*, Vec3, const void*))m_set_pos1.code)(motor, p, m_set_pos1.mi);
        return 1;
    }
    int done = 0;
    if (motor && m_set_tpos.code) {
        ((void (*)(void*, Vec3, const void*))m_set_tpos.code)(motor, p, m_set_tpos.mi);
        done = 1;
    }
    void* tr = get_transform(ctrl);
    if (tr && m_tr_set_pos.code) {
        ((void (*)(void*, Vec3, const void*))m_tr_set_pos.code)(tr, p, m_tr_set_pos.mi);
        done = 1;
    }
    return done;
}

/* игрок считается живым, если мы видели его в последние кадры */
static void* live_player(void) {
    if (!g_player) return NULL;
    if (cur_frame() - g_player_frame > 15) return NULL;
    return g_player;
}

static void note_player(void* self) {
    if (!self) return;
    if (m_is_local.code) {
        uint8_t local = ((uint8_t (*)(void*, const void*))m_is_local.code)(self, m_is_local.mi);
        if (!local) return;
    } else if (!g_warned_local) {
        g_warned_local = 1;
        logmsg("Portal Gun: IsLocalPlayer не найден, игроком считается любой контроллер");
    }
    g_player = self;
    g_player_frame = cur_frame();
}

/* Перехваты UpdateVelocity(ref Vector3 velocity, float deltaTime) у контроллеров игрока */
static void (*o_uv_bear)(void*, void*, float, const void*);
static void h_uv_bear(void* self, void* vel, float dt, const void* mi) {
    note_player(self);
    o_uv_bear(self, vel, dt, mi);
}
static void (*o_uv_shicka)(void*, void*, float, const void*);
static void h_uv_shicka(void* self, void* vel, float dt, const void* mi) {
    note_player(self);
    o_uv_shicka(self, vel, dt, mi);
}

static void do_set_point(int i) {
    void* p = live_player();
    if (!p) { logmsg("Portal Gun: игрок не найден, зайди в уровень и подвигайся"); return; }
    Vec3 pos;
    if (!get_pos(p, &pos)) { logmsg("Portal Gun: не удалось прочитать позицию"); return; }
    g_pt[i] = pos;
    g_has[i] = 1;
    logfmt(i == 0 ? "Портал A: %.2f %.2f %.2f" : "Портал B: %.2f %.2f %.2f", pos.x, pos.y, pos.z);
}

static float dist2(Vec3 a, Vec3 b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static void do_go(void) {
    if (!g_has[0] && !g_has[1]) { logmsg("Portal Gun: сначала поставь точку A или B"); return; }
    void* p = live_player();
    if (!p) { logmsg("Portal Gun: игрок не найден, зайди в уровень и подвигайся"); return; }
    Vec3 cur;
    int target;
    if (g_has[0] && g_has[1]) {
        if (!get_pos(p, &cur)) { logmsg("Portal Gun: не удалось прочитать позицию"); return; }
        target = dist2(cur, g_pt[0]) < dist2(cur, g_pt[1]) ? 1 : 0;
    } else {
        target = g_has[0] ? 0 : 1;
    }
    Vec3 dest = g_pt[target];
    dest.y += (float)g_api->get_float(g_id, "lift", 0.3);
    if (set_pos(p, dest))
        logfmt(target == 0 ? "Прыжок в A: %.2f %.2f %.2f" : "Прыжок в B: %.2f %.2f %.2f", dest.x, dest.y, dest.z);
    else
        logmsg("Portal Gun: не удалось переместить игрока");
}

/* Раз в кадр: ищем нажатия кнопок */
static void tick(void) {
    if (!g_api) return;
    int f = cur_frame();
    if (m_frame.code && f == g_last_tick_frame) return;
    g_last_tick_frame = f;

    int now[3];
    now[0] = g_api->get_bool(g_id, "set_a", 0) != 0;
    now[1] = g_api->get_bool(g_id, "set_b", 0) != 0;
    now[2] = g_api->get_bool(g_id, "go", 0) != 0;
    if (!g_init) { /* первый кадр: запоминаем состояние, нажатий нет */
        for (int i = 0; i < 3; ++i) g_last[i] = now[i];
        g_init = 1;
        return;
    }
    if (now[0] != g_last[0]) { g_last[0] = now[0]; do_set_point(0); }
    if (now[1] != g_last[1]) { g_last[1] = now[1]; do_set_point(1); }
    if (now[2] != g_last[2]) { g_last[2] = now[2]; do_go(); }
}

/* Camera.get_main игра зовёт часто, используем как «каждый кадр» (как в моде Зум) */
static void* (*o_get_main)(const void*);
static void* h_get_main(const void* mi) {
    void* cam = o_get_main(mi);
    tick();
    return cam;
}

static void report(const char* name, Method m) {
    char buf[160];
    snprintf(buf, sizeof buf, "Portal Gun: %s: %s", name, m.code ? "найден" : "НЕ найден");
    logmsg(buf);
}

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { logmsg("Portal Gun: не найдены функции IL2CPP, мод не работает"); return; }

    il2_object_get_class = (void* (*)(void*))api->il2cpp_symbol("il2cpp_object_get_class");
    il2_class_get_field = (void* (*)(void*, const char*))api->il2cpp_symbol("il2cpp_class_get_field_from_name");
    il2_field_get_value = (void (*)(void*, void*, void*))api->il2cpp_symbol("il2cpp_field_get_value");

    m_get_tpos = find_motor("get_TransientPosition", 0);
    m_set_pos2 = find_motor("SetPosition", 2);
    m_set_pos1 = find_motor("SetPosition", 1);
    m_set_tpos = find_motor("SetTransientPosition", 1);
    m_get_transform = find_m("UnityEngine", "Component", "get_transform", 0);
    m_tr_get_pos = find_m("UnityEngine", "Transform", "get_position", 0);
    m_tr_set_pos = find_m("UnityEngine", "Transform", "set_position", 1);
    m_frame = find_m("UnityEngine", "Time", "get_frameCount", 0);
    m_is_local = find_m("", "PlayerBearController", "get_IsLocalPlayer", 0);
    if (!m_is_local.code) m_is_local = find_m("", "PlayerBearController", "IsLocalPlayer", 0);

    report("motor.get_TransientPosition", m_get_tpos);
    report("motor.SetPosition(2)", m_set_pos2);
    report("motor.SetPosition(1)", m_set_pos1);
    report("motor.SetTransientPosition", m_set_tpos);
    report("Component.get_transform", m_get_transform);
    report("Transform.get_position", m_tr_get_pos);
    report("Transform.set_position", m_tr_set_pos);
    report("Time.get_frameCount", m_frame);
    report("IsLocalPlayer", m_is_local);

    int ok1 = bearite_hook_method(api, g_id, "", "PlayerBearController", "UpdateVelocity", 2,
                                  (void*)h_uv_bear, (void**)&o_uv_bear) == 0;
    int ok2 = bearite_hook_method(api, g_id, "", "PlayerShickaController", "UpdateVelocity", 2,
                                  (void*)h_uv_shicka, (void**)&o_uv_shicka) == 0;
    int ok3 = bearite_hook_method(api, g_id, "UnityEngine", "Camera", "get_main", 0,
                                  (void*)h_get_main, (void**)&o_get_main) == 0;
    if (!ok3) { logmsg("Portal Gun: без Camera.get_main кнопки не работают"); return; }
    if (!ok1 && !ok2) { logmsg("Portal Gun: не найден UpdateVelocity у игрока, пришли лог"); return; }
    logmsg("Portal Gun 0.1: готов. Кнопки A, B и стрелка слева на экране");
}
