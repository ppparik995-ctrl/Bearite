/*
 * Разведка классов для Super Bear Adventure (мод Bearite, API версии 1).
 * Пишет в «Лог» поля и методы выбранных классов (с типами аргументов), чтобы можно было
 * писать моды без дампа игры. Ничего в игре не меняет.
 * Написано без проверки на устройстве.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bearite_api.h"
#include "bearite_il2cpp.h"

static const BeariteApi* g_api = NULL;
static const char* g_id = "sba-inspector";

static const char* (*c_get_name)(void*);
static void* (*c_get_parent)(void*);
static void* (*c_get_methods)(void*, void**);
static void* (*c_get_fields)(void*, void**);
static const char* (*m_get_name)(void*);
static uint32_t (*m_param_count)(void*);
static void* (*m_param)(void*, uint32_t);
static const char* (*m_param_name)(void*, uint32_t);
static void* (*m_ret)(void*);
static uint32_t (*m_flags)(void*, uint32_t*);
static const char* (*f_get_name)(void*);
static void* (*f_get_type)(void*);
static char* (*t_get_name)(void*);

/* Классы, которые разведываем. Пространство имён пустое, как у остальных классов игры. */
static const char* CLASSES[] = {
    "BearEmoteController", "EmoteManager", "EmoteData", "EmoteBehaviour", "EmoteButton", NULL
};

static void line(const char* fmt, const char* a, const char* b, const char* c) {
    char buf[480];
    snprintf(buf, sizeof buf, fmt, a ? a : "?", b ? b : "?", c ? c : "?");
    g_api->log(g_id, buf);
}

static void dump_class(const char* name) {
    void* k = bearite_find_class("", name);
    if (!k) { line("Класс %s не найден", name, NULL, NULL); return; }
    void* parent = c_get_parent ? c_get_parent(k) : NULL;
    line("=== %s (родитель: %s) ===", name, parent && c_get_name ? c_get_name(parent) : "нет", NULL);

    void* it = NULL;
    void* f;
    while (c_get_fields && (f = c_get_fields(k, &it)) != NULL) {
        char* tn = (f_get_type && t_get_name) ? t_get_name(f_get_type(f)) : NULL;
        line("  поле %s : %s", f_get_name ? f_get_name(f) : NULL, tn, NULL);
    }

    it = NULL;
    void* m;
    while (c_get_methods && (m = c_get_methods(k, &it)) != NULL) {
        char sig[360];
        size_t n = 0;
        uint32_t iflags = 0;
        int is_static = m_flags ? (m_flags(m, &iflags) & 0x10) != 0 : 0;
        uint32_t pc = m_param_count ? m_param_count(m) : 0;
        n += (size_t)snprintf(sig + n, sizeof sig - n, "(");
        for (uint32_t i = 0; i < pc && n < sizeof sig - 60; ++i) {
            char* tn = (m_param && t_get_name) ? t_get_name(m_param(m, i)) : NULL;
            const char* pn = m_param_name ? m_param_name(m, i) : NULL;
            n += (size_t)snprintf(sig + n, sizeof sig - n, "%s%s %s", i ? ", " : "", tn ? tn : "?", pn ? pn : "?");
        }
        snprintf(sig + n, sizeof sig - n, ")");
        char* rt = (m_ret && t_get_name) ? t_get_name(m_ret(m)) : NULL;
        char buf[480];
        snprintf(buf, sizeof buf, "  %s%s %s%s", is_static ? "static " : "", rt ? rt : "?",
                 m_get_name ? m_get_name(m) : "?", sig);
        g_api->log(g_id, buf);
    }
}

static void dump_all(void) {
    g_api->log(g_id, "Разведка: начало");
    for (int i = 0; CLASSES[i]; ++i) dump_class(CLASSES[i]);
    g_api->log(g_id, "Разведка: конец");
}

/* Раз в кадр через Camera.get_main смотрим, не переключили ли кнопку «Лог» */
static void* (*o_get_main)(const void*);
static int g_last = -1;
static void* h_get_main(const void* mi) {
    void* cam = o_get_main(mi);
    if (g_api) {
        int now = g_api->get_bool(g_id, "dump", 0) != 0;
        if (g_last < 0) g_last = now;
        else if (now != g_last) { g_last = now; dump_all(); }
    }
    return cam;
}

__attribute__((visibility("default")))
void bearite_mod_init(const BeariteApi* api, const char* mod_id) {
    if (api->version != BEARITE_API_VERSION) return;
    g_api = api;
    if (mod_id) g_id = mod_id;
    if (!bearite_il2cpp_init(api)) { api->log(g_id, "не найдены функции IL2CPP"); return; }

    c_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_class_get_name");
    c_get_parent = (void* (*)(void*))api->il2cpp_symbol("il2cpp_class_get_parent");
    c_get_methods = (void* (*)(void*, void**))api->il2cpp_symbol("il2cpp_class_get_methods");
    c_get_fields = (void* (*)(void*, void**))api->il2cpp_symbol("il2cpp_class_get_fields");
    m_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_name");
    m_param_count = (uint32_t (*)(void*))api->il2cpp_symbol("il2cpp_method_get_param_count");
    m_param = (void* (*)(void*, uint32_t))api->il2cpp_symbol("il2cpp_method_get_param");
    m_param_name = (const char* (*)(void*, uint32_t))api->il2cpp_symbol("il2cpp_method_get_param_name");
    m_ret = (void* (*)(void*))api->il2cpp_symbol("il2cpp_method_get_return_type");
    m_flags = (uint32_t (*)(void*, uint32_t*))api->il2cpp_symbol("il2cpp_method_get_flags");
    f_get_name = (const char* (*)(void*))api->il2cpp_symbol("il2cpp_field_get_name");
    f_get_type = (void* (*)(void*))api->il2cpp_symbol("il2cpp_field_get_type");
    t_get_name = (char* (*)(void*))api->il2cpp_symbol("il2cpp_type_get_name");

    dump_all(); /* сразу при запуске */
    bearite_hook_method(api, g_id, "UnityEngine", "Camera", "get_main", 0, (void*)h_get_main, (void**)&o_get_main);
}
