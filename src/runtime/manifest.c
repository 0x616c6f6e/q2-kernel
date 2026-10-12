#include "cJSON.h"
#include "q2/runtime.h"
#include <string.h>
static bool text(const cJSON *o, const char *key, char *out, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !*v->valuestring || strlen(v->valuestring) >= cap)
        return false;
    strcpy(out, v->valuestring);
    return true;
}
static bool unique(const cJSON *o) {
    for (const cJSON *a = o->child; a; a = a->next) {
        if (a->string)
            for (const cJSON *b = a->next; b; b = b->next)
                if (b->string && !strcmp(a->string, b->string))
                    return false;
        if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && !unique(a))
            return false;
    }
    return true;
}
static bool keys(const cJSON *o, const char *const *allowed, size_t n) {
    if (!cJSON_IsObject(o))
        return false;
    for (const cJSON *a = o->child; a; a = a->next) {
        bool found = false;
        for (size_t i = 0; i < n; i++)
            if (a->string && !strcmp(a->string, allowed[i]))
                found = true;
        if (!found)
            return false;
    }
    return true;
}
static bool digit(char c) {
    return c >= '0' && c <= '9';
}
static bool semver(const char *s) {
    for (unsigned part = 0; part < 3; part++) {
        if (!digit(*s))
            return false;
        if (*s == '0' && digit(s[1]))
            return false;
        while (digit(*s))
            s++;
        if (part < 2 && *s++ != '.')
            return false;
    }
    /* Prerelease identifiers forbid leading zero only when entirely numeric. */
    if (*s == '-') {
        s++;
        for (;;) {
            const char *begin = s;
            bool numeric = true;
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || digit(*s) || *s == '-') {
                if (!digit(*s))
                    numeric = false;
                s++;
            }
            if (s == begin || (numeric && s - begin > 1 && *begin == '0'))
                return false;
            if (*s != '.')
                break;
            s++;
        }
    }
    if (*s == '+') {
        s++;
        for (;;) {
            const char *begin = s;
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || digit(*s) || *s == '-')
                s++;
            if (s == begin)
                return false;
            if (*s != '.')
                break;
            s++;
        }
    }
    return !*s;
}
static bool uuid(const char *s) {
    if (strlen(s) != 36 || s[14] != '4' || !strchr("89ab", s[19]))
        return false;
    for (unsigned i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else if (!digit(s[i]) && !(s[i] >= 'a' && s[i] <= 'f'))
            return false;
    }
    return true;
}
static bool caps(const cJSON *o, const char *key, char out[Q2_MANIFEST_CAPS][48], size_t *count) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > (int)Q2_MANIFEST_CAPS)
        return false;
    *count = 0;
    for (const cJSON *x = v->child; x; x = x->next) {
        if (!cJSON_IsString(x) || !*x->valuestring || strlen(x->valuestring) >= 48)
            return false;
        for (const char *p = x->valuestring; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || digit(*p) || *p == '.' || *p == '_'))
                return false;
        for (size_t i = 0; i < *count; i++)
            if (!strcmp(out[i], x->valuestring))
                return false;
        strcpy(out[(*count)++], x->valuestring);
    }
    return true;
}
/* Canonical bounded sizes prevent ambiguous directory names and numeric overflow. */
static bool resolutions(const cJSON *o, q2_manifest_t *out) {
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(o, "supported_resolutions");
    if (!values)
        return true;
    if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) < 1 ||
        cJSON_GetArraySize(values) > (int)Q2_MANIFEST_RESOLUTIONS)
        return false;
    for (const cJSON *v = values->child; v; v = v->next) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= 10)
            return false;
        const char *p = v->valuestring;
        for (unsigned axis = 0; axis < 2; axis++) {
            if (*p < '1' || *p > '9')
                return false;
            unsigned size = 0;
            while (digit(*p)) {
                size = size * 10 + (unsigned)(*p++ - '0');
                if (size > 1024)
                    return false;
            }
            if (size < 64 || (axis == 0 && *p++ != 'x'))
                return false;
        }
        if (*p)
            return false;
        for (size_t i = 0; i < out->supported_resolution_count; i++)
            if (!strcmp(out->supported_resolutions[i], v->valuestring))
                return false;
        strcpy(out->supported_resolutions[out->supported_resolution_count++], v->valuestring);
    }
    return true;
}
q2_result_t q2_manifest_parse(const char *json, size_t n, q2_manifest_t *out) {
    if (!json || !out || !n || n > Q2_MANIFEST_MAX)
        return Q2_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (memchr(json, 0, n))
        return Q2_ERR_FORMAT;
    /* Reject escaped NUL: cJSON C strings cannot preserve embedded string length.
     */
    for (size_t i = 0; i + 6 <= n; i++)
        if (!memcmp(json + i, "\\u0000", 6))
            return Q2_ERR_FORMAT;
    const char *end = NULL;
    cJSON *o = cJSON_ParseWithLengthOpts(json, n, &end, false);
    if (!o)
        return Q2_ERR_FORMAT;
    while (end < json + n && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t'))
        end++;
    bool ok = end == json + n && unique(o);
    const char *allowed[] = {"format", "id",  "name",         "description", "version", "entry",
                             "type",   "api", "capabilities", "resources",   "icon",    "dynamic_resources",
                             "ota", "supported_resolutions"};
    ok = ok && keys(o, allowed, sizeof(allowed) / sizeof(*allowed));
    ok = ok && resolutions(o, out);
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(o, "format"),
                *api = cJSON_GetObjectItemCaseSensitive(o, "api");
    char entry[32], type[16];
    ok = ok && cJSON_IsNumber(format) && format->valuedouble == 2 && cJSON_IsNumber(api) &&
         api->valuedouble == Q2_API_VERSION && text(o, "id", out->id, sizeof(out->id)) && uuid(out->id) &&
         text(o, "name", out->name, sizeof(out->name)) &&
         text(o, "version", out->version, sizeof(out->version)) && semver(out->version) &&
         text(o, "entry", entry, sizeof(entry)) && !strcmp(entry, "main.lua") &&
         text(o, "type", type, sizeof(type)) && !strcmp(type, "app");
    const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(o, "capabilities");
    const char *cap_keys[] = {"required", "optional"};
    ok = ok && keys(permissions, cap_keys, 2) &&
         caps(permissions, "required", out->required, &out->required_count) &&
         caps(permissions, "optional", out->optional, &out->optional_count);
    for (size_t i = 0; i < out->required_count; i++)
        for (size_t j = 0; j < out->optional_count; j++)
            if (!strcmp(out->required[i], out->optional[j]))
                ok = false;
    const cJSON *resources = cJSON_GetObjectItemCaseSensitive(o, "resources");
    const char *resource_keys[] = {"lua_heap_kib"};
    const cJSON *heap = cJSON_GetObjectItemCaseSensitive(resources, "lua_heap_kib");
    ok = ok && keys(resources, resource_keys, 1) && cJSON_IsNumber(heap) && heap->valuedouble >= 32 &&
         heap->valuedouble <= 384 && heap->valuedouble == (int)heap->valuedouble;
    if (ok)
        out->heap_bytes = (size_t)heap->valueint * 1024;
    /* Metadata URLs are preserved by the package tool, never silently used as
     * grants. */
    const char *urls[] = {"dynamic_resources", "ota"};
    for (unsigned i = 0; i < 2; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, urls[i]);
        if (v &&
            (!cJSON_IsString(v) || strncmp(v->valuestring, "https://", 8) || strlen(v->valuestring) > 256))
            ok = false;
    }
    const cJSON *description = cJSON_GetObjectItemCaseSensitive(o, "description");
    if (description && (!cJSON_IsString(description) || strlen(description->valuestring) > 512))
        ok = false;
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(o, "icon");
    if (icon) {
        if (!cJSON_IsString(icon) || strncmp(icon->valuestring, "assets/", 7) ||
            strlen(icon->valuestring) <= 7 || strlen(icon->valuestring) > 96 ||
            strstr(icon->valuestring, ".."))
            ok = false;
        else
            for (const char *p = icon->valuestring; *p; p++)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || digit(*p) ||
                      strchr("_./-", *p)))
                    ok = false;
    }
    if (ok && icon)
        strcpy(out->icon, icon->valuestring);
    cJSON_Delete(o);
    if (!ok)
        memset(out, 0, sizeof(*out));
    return ok ? Q2_OK : Q2_ERR_FORMAT;
}
