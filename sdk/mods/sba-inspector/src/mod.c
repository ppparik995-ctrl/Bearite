/*
 * Разведка скелета медведя для Super Bear Adventure (мод Bearite, API версии 1), версия 0.2.
 * Пишет в «Лог»: поля и методы класса BearTransformReferences и дерево костей медведя
 * (от Animator нашего медведя вниз). Ничего в игре не меняет.
 * Дерево пишется один раз после входа в уровень и заново при нажатии кнопки «Кости».
 * Написано без проверки на устройстве.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bearite_api.h"
#include "bearite_il2cpp.h"

typedef struct { void* code; void* mi; } Method;

static const BeariteApi* g_api = NULL;
static const char* g_id = "sba-inspector";

static void* (*il2_object_get_class)(void* obj);
static void* (*il2_class_get_field)(void* klass, const char* name);
static void (*il2_field_get_value)(void* obj, void* field, void* value);
static const char* (*c_get_name)(void*);
static void* (*c_get_methods)(void*, void**);
static void* (*c_get_fields)(void*, void**);
static const char* (*m_get_name_fn)(void*);
static uint32_t (*m_param_count)(void*);
static void* (*m_param)(void*, uint32_t);
static const char* (*m_param_name)(void*, uint32_t);
static void* (*m_ret)(void*);
static const char* (*f_get_name)(void*);
static void* (*f_get_type)(void*);
static char* (*t_get_name)(void*);

static Method m_obj_name, m_get_transform, m_child_count, m_get_child, m_is_local;

static int g_done = 0, g_last = -1, g_lines = 0;

static Method find_m(const char* ns, const char* cls, const char* name, int argc) {
    Method r = {NULL, NULL};
    void* mi = bearite_find_method(ns, cls, name, argc);
    if (mi) { r.mi = mi; r.code = bearite_method_code(mi); }
    return r;
}

static void logs(const char* fmt, const char* a, const char* b) {
    char buf[400];
    snprintf(buf, sizeof buf, fmt, a ? a : "?", b ? b : "?");
    g_api->log(g_id, buf);
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

/* Имя Unity-объекта (строка IL2CPP: длина по смещению 16, символы UTF-16 с 20) */
static void name_of(void* obj, char* out, size_t n) {
    out[0] = 0;
    if (!obj || !m_obj_name.code || n < 2) return;
    void* s = ((void* (*)(void*, const void*))m_obj_name.code)(obj, m_obj_name.mi);
    if (!s) return;
    int32_t len = *(int32_t*)((char*)s + 16);
    const uint16_t* ch = (const uint16_t*)((char*)s + 20);
    size_t i = 0;
    for (; i + 1 < n && (int32_t)i < len; ++i) out[i] = ch[i] < 128 ? (char)ch[i] : '?';
    out[i] = 0;
}

static void walk(void* tr, int depth) {
    if (!tr || g_lines >= 300) return;
    char nm[96], pad[40];
    name_of(tr, nm, sizeof nm);
    int d = depth < 19 ? depth : 19;
    memset(pad, ' ', (size_t)d * 2);
    pad[d * 2] = 0;
    char buf[200];
    snprintf(buf, sizeof buf, "  %s[%d] %s", pad, g_lines, nm);
    g_api->log(g_id, buf);
    g_lines++;
    int n = ((int (*)(void*, const void*))m_child_count.code)(tr, m_child_count.mi);
    for (int i = 0; i < n && g_lines < 300; ++i) {
        void* c = ((void* (*)(void*, int32_t, const void*))m_get_child.code)(tr, i, m_get_child.mi);
        walk(c, depth + 1);
    }
}

static void dump_class(const char* name) {
    void* k = bearite_find_class("", name);
    if (!k) { logs("Класс %s не найден", name, NULL); return; }
    logs("=== %s ===", name, NULL);
    void* it = NULL;
    void* f;
    while (c_get_fields && (f = c_get_fields(k, &it)) != NULL) {
        char* tn = (f_get_type && t_get_name) ? t_get_name(f_get_type(f)) : NULL;
        logs("  поле %s : %s", f_get_name ? f_get_name(f) : NULL, tn);
    }
    it = NULL;
    void* m;
    while (c_get_methods && (m = c_get_methods(k, &it)) != NULL) {
        char sig[300];
        size_t n = 0;
        uint32_t pc = m_param_count ? m_param_count(m) : 0;
        n += (size_t)snprintf(sig + n, sizeof sig - n, "(");
        for (uint32_t i = 0; i < pc && n < sizeof sig - 60; ++i) {
            char* tn = (m_param && t_get_name) ? t_get_name(m_param(m, i)) : NULL;
            const char* pn = m_param_name ? m_param_name(m, i) : NULL;
            n += (size_t)snprintf(sig + n, sizeof sig - n, "%s%s %s", i ? ", " : "", tn ? tn : "?", pn ? pn : "?");
        }
        snprintf(sig + n, sizeof sig - n, ")");
        char* rt = (m_ret && t_get_name) ? t_get_name(m_ret(m)) : NULL;
        char buf[420];
        snprintf(buf, sizeof buf, "  %s %s%s", rt ? rt : "?", m_get_name_fn ? m_get_name_fn(m) : "?", sig);
        g_api->log(g_id, buf);
    }
}

static void dump_bear(void* emote_ctrl) {
    g_api->log(g_id, "Кости: начало");
    void* anim = get_field_obj(emote_ctrl, "animator");
    void* tr = (anim && m_get_transform.code)
        ? ((void* (*)(void*, const void*))m_get_transform.code)(anim, m_get_transform.mi) : NULL;
    if (!tr) { g_api->log(g_id, "Кости: Animator или его Transform не найден"); return; }
    g_lines = 0;
    walk(tr, 0);
    g_api->log(g_id, "Кости: конец");
}

static void (*o_update)(void*, const void*);
static void h_update(void* self, const void* mi) {
    o_update(self, mi);
    if (!g_api) return;
    int now = g_api->get_bool(g_id, "dump", 0) != 0;
    int want = 0;
    if (g_last < 0) g_last = now;
    else if (now != g_last) { g_last = now; want = 1; }
    if (g_done && !want) return;
    void* pc = get_field_obj(self, "playerController");
    int mine = pc && (!m_is_local.code ||
        ((uint8_t (*)(void*, const void*))m_is_local.code)(pc, m_is_local.mi) != 0);
    if (mine) { g_done = 1; dump_bear(self); }
}

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { api->log(g_id, "не найдены функции IL2CPP"); return; }

    il2_object_get_class = (void* (*)(void*))api->il2cpp_symbol("il2cpp_object_get_class");
    il2_class_get_field = (void* (*)(void*, const char*))api->il2cpp_symbol("il2cpp_class_get_field_from_name");
    il2_field_get_value = (void (*)(void*, void*, void*))api->il2cpp_symbol("il2cpp_field_get_value");
    c_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_class_get_name");
    c_get_methods = (void* (*)(void*, void**))api->il2cpp_symbol("il2cpp_class_get_methods");
    c_get_fields = (void* (*)(void*, void**))api->il2cpp_symbol("il2cpp_class_get_fields");
    m_get_name_fn = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_name");
    m_param_count = (uint32_t (*)(void*))api->il2cpp_symbol("il2cpp_method_get_param_count");
    m_param = (void* (*)(void*, uint32_t))api->il2cpp_symbol("il2cpp_method_get_param");
    m_param_name = (const char* (*)(void*, uint32_t))api->il2cpp_symbol("il2cpp_method_get_param_name");
    m_ret = (void* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_return_type");
    f_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_field_get_name");
    f_get_type = (void* (*)(void*))api->il2cpp_symbol("il2cpp_field_get_type");
    t_get_name = (char* (*)(void*))api->il2cpp_symbol("il2cpp_type_get_name");

    m_obj_name = find_m("UnityEngine", "Object", "get_name", 0);
    m_get_transform = find_m("UnityEngine", "Component", "get_transform", 0);
    m_child_count = find_m("UnityEngine", "Transform", "get_childCount", 0);
    m_get_child = find_m("UnityEngine", "Transform", "GetChild", 1);
    m_is_local = find_m("", "PlayerBearController", "get_IsLocalPlayer", 0);
    if (!m_is_local.code) m_is_local = find_m("", "PlayerBearController", "IsLocalPlayer", 0);

    api->log(g_id, "Разведка 0.2: начало");
    dump_class("BearTransformReferences");
    api->log(g_id, "Разведка 0.2: классы записаны, кости появятся после входа в уровень");

    if (!m_obj_name.code || !m_get_transform.code || !m_child_count.code || !m_get_child.code) {
        api->log(g_id, "не найдены методы Unity для обхода костей"); return;
    }
    if (bearite_hook_method(api, g_id, "", "BearEmoteController", "Update", 0,
                            (void*)h_update, (void**)&o_update) != 0)
        api->log(g_id, "не удалось перехватить BearEmoteController.Update");
}
