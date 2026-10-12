#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "q2/log.h"
#include "q2/runtime.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct q2_lua_runtime {
    q2_kernel_t *kernel;
    lua_State *lua;
    q2_manifest_t manifest;
    q2_caller_t caller;
    size_t used;
    unsigned instructions;
    uint64_t budget_at;
    unsigned time_remaining_ms;
    bool exit_requested, in_callback;
    q2_result_t last;
    char error[128];
    const char *source;
    size_t source_size;
};
typedef union {
    max_align_t align;
    size_t bytes;
} allocation_t;
static q2_lua_runtime_t *runtime(lua_State *L) {
    return *(q2_lua_runtime_t **)lua_getextraspace(L);
}
static void *allocate(void *context, void *ptr, size_t old, size_t size) {
    (void)old;
    q2_lua_runtime_t *r = context;
    allocation_t *previous = ptr ? (allocation_t *)ptr - 1 : NULL;
    size_t prior = previous ? previous->bytes : 0;
    if (!size) {
        r->used -= prior;
        q2_kernel_release(r->kernel, previous);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(allocation_t))
        return NULL;
    size_t total = size + sizeof(allocation_t);
    if (total > r->manifest.heap_bytes || r->used > r->manifest.heap_bytes - total)
        return NULL;
    allocation_t *next = NULL;
    if (q2_kernel_allocate(r->kernel, total, (void **)&next) != Q2_OK)
        return NULL;
    next->bytes = total;
    if (previous) {
        size_t bytes = prior - sizeof(allocation_t);
        memcpy(next + 1, ptr, bytes < size ? bytes : size);
        q2_kernel_release(r->kernel, previous);
    }
    r->used = r->used - prior + total;
    return next + 1;
}
static bool granted(q2_lua_runtime_t *r, const char *name) {
    for (size_t i = 0; i < r->caller.grant_count; i++)
        if (!strcmp(r->caller.grants[i], name))
            return true;
    return false;
}
static int failure(lua_State *L, q2_result_t code) {
    const char *name;
    q2_result_name(code, &name);
    lua_pushnil(L);
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "code");
    lua_pushinteger(L, code);
    lua_setfield(L, -2, "number");
    return 2;
}
/* Charge interpreter/bridge work since the previous checkpoint. Component callbacks have
 * their own bounded-work contract; they are not Lua execution and cannot be preempted here. */
static void charge_time(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    uint64_t now = 0;
    q2_kernel_time(r->kernel, &now);
    if (now < r->budget_at || now - r->budget_at > r->time_remaining_ms)
        luaL_error(L, "execution budget exceeded (time)");
    r->time_remaining_ms -= (unsigned)(now - r->budget_at);
    r->budget_at = now;
}
static void native_complete(q2_lua_runtime_t *r) {
    /* Exclude only the just-completed trusted call; never replenish time already spent in Lua
     * or reset the instruction count. This also applies when a provider reports an error. */
    q2_kernel_time(r->kernel, &r->budget_at);
}
static void hook(lua_State *L, lua_Debug *debug) {
    (void)debug;
    q2_lua_runtime_t *r = runtime(L);
    if (!r->instructions)
        luaL_error(L, "execution budget exceeded (instructions)");
    r->instructions--;
    charge_time(L);
}
static void budget(q2_lua_runtime_t *r) {
    q2_kernel_time(r->kernel, &r->budget_at);
    r->time_remaining_ms = 30;
    r->instructions = 100;
    lua_sethook(r->lua, hook, LUA_MASKCOUNT, 1000);
}
static int method(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    const q2_method_t *m = lua_touserdata(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    if (n > 4)
        return failure(L, Q2_ERR_ARGUMENT);
    q2_value_t args[4] = {0}, value = {0};
    for (int i = 0; i < n; i++) {
        if (lua_isboolean(L, i + 1)) {
            args[i].kind = Q2_BOOLEAN;
            args[i].as.boolean = lua_toboolean(L, i + 1);
        } else if (lua_isinteger(L, i + 1)) {
            args[i].kind = Q2_INTEGER;
            args[i].as.integer = lua_tointeger(L, i + 1);
        } else if (lua_type(L, i + 1) == LUA_TSTRING) {
            size_t len;
            const char *s = lua_tolstring(L, i + 1, &len);
            if (len >= Q2_TEXT_MAX || memchr(s, 0, len))
                return failure(L, Q2_ERR_ARGUMENT);
            args[i].kind = Q2_STRING;
            memcpy(args[i].as.string, s, len + 1);
        } else
            return failure(L, Q2_ERR_ARGUMENT);
    }
    charge_time(L);
    q2_result_t e = q2_kernel_call(r->kernel, &r->caller, m->module, m->name, args, (size_t)n, &value);
    native_complete(r);
    if (e != Q2_OK)
        return failure(L, e);
    switch (value.kind) {
    case Q2_BOOLEAN:
        lua_pushboolean(L, value.as.boolean);
        break;
    case Q2_INTEGER:
        lua_pushinteger(L, value.as.integer);
        break;
    case Q2_STRING:
        lua_pushlstring(L, value.as.string, strlen(value.as.string));
        break;
    default:
        lua_pushboolean(L, true);
        break;
    }
    return 1;
}
static int info(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    if (!granted(r, "system.info"))
        return failure(L, Q2_ERR_PERMISSION);
    lua_newtable(L);
    lua_pushliteral(L, "Q2-Kernel");
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, Q2_API_VERSION);
    lua_setfield(L, -2, "api");
    lua_pushstring(L, r->caller.app_id);
    lua_setfield(L, -2, "app_id");
    lua_pushinteger(L, (lua_Integer)r->caller.instance);
    lua_setfield(L, -2, "instance");
    return 1;
}
static int available(lua_State *L) {
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return failure(L, Q2_ERR_ARGUMENT);
    size_t n;
    const char *cap = luaL_checklstring(L, 1, &n);
    if (n >= 48 || memchr(cap, 0, n))
        return failure(L, Q2_ERR_ARGUMENT);
    bool found = false;
    q2_capability_available(runtime(L)->kernel, cap, &found);
    lua_pushboolean(L, found);
    lua_pushboolean(L, granted(runtime(L), cap));
    return 2;
}
static int exit_app(lua_State *L) {
    runtime(L)->exit_requested = true;
    lua_pushboolean(L, true);
    return 1;
}
static int app_log(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    if (!granted(r, "system.log"))
        return failure(L, Q2_ERR_PERMISSION);
    if (lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING)
        return failure(L, Q2_ERR_ARGUMENT);
    size_t n, m;
    const char *level = lua_tolstring(L, 1, &n), *message = lua_tolstring(L, 2, &m);
    if (n > 5 || memchr(level, 0, n) || m >= Q2_TEXT_MAX || memchr(message, 0, m))
        return failure(L, Q2_ERR_ARGUMENT);
    const char *names[] = {"", "error", "warn", "info", "debug"};
    q2_log_level_t severity = Q2_LOG_OFF;
    for (unsigned i = 1; i <= Q2_LOG_DEBUG; i++)
        if (!strcmp(level, names[i]))
            severity = (q2_log_level_t)i;
    if (severity == Q2_LOG_OFF)
        return failure(L, Q2_ERR_ARGUMENT);
    /* APP supplies data, never a printf format or logger configuration. */
    charge_time(L);
    q2_result_t e = q2_log_write(r->kernel, severity, "lua.app", "%s: %s", r->caller.app_id, message);
    native_complete(r);
    if (e != Q2_OK)
        return failure(L, e);
    lua_pushboolean(L, true);
    return 1;
}
static int require_q2(lua_State *L) {
    size_t n;
    const char *name = luaL_checklstring(L, 1, &n);
    if (n != 2 || memcmp(name, "q2", 2))
        return luaL_error(L, "only the q2 system module is available in this profile");
    lua_getfield(L, LUA_REGISTRYINDEX, "q2.api");
    return 1;
}
static int setup(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    lua_pop(L, 1);
    /* Remove file/loader access and protected calls that could repeatedly hide
     * quota errors. */
    const char *remove[] = {"dofile", "loadfile", "load",         "collectgarbage", "pcall",
                            "xpcall", "print",    "setmetatable", "getmetatable"};
    for (size_t i = 0; i < sizeof(remove) / sizeof(*remove); i++) {
        lua_pushnil(L);
        lua_setglobal(L, remove[i]);
    }
    const char *str_remove[] = {"rep", "format", "find", "match", "gmatch", "gsub"};
    lua_getglobal(L, "string");
    for (size_t i = 0; i < sizeof(str_remove) / sizeof(*str_remove); i++) {
        lua_pushnil(L);
        lua_setfield(L, -2, str_remove[i]);
    }
    lua_pop(L, 1);
    lua_getglobal(L, "table");
    lua_pushnil(L);
    lua_setfield(L, -2, "sort");
    lua_pushnil(L);
    lua_setfield(L, -2, "move");
    lua_pop(L, 1);
    lua_newtable(L);
    int api = lua_gettop(L);
    lua_newtable(L);
    lua_pushcfunction(L, info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, available);
    lua_setfield(L, -2, "capability");
    lua_pushcfunction(L, exit_app);
    lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, app_log);
    lua_setfield(L, -2, "log");
    lua_setfield(L, api, "os");
    const q2_method_t *m;
    for (size_t i = 0; q2_method_at(r->kernel, i, &m) == Q2_OK; i++) {
        lua_getfield(L, api, m->module);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, api, m->module);
        }
        lua_pushlightuserdata(L, (void *)m);
        lua_pushcclosure(L, method, 1);
        lua_setfield(L, -2, m->name);
        lua_pop(L, 1);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "q2.api");
    lua_pushcfunction(L, require_q2);
    lua_setglobal(L, "require");
    if (luaL_loadbufferx(L, r->source, r->source_size, "main.lua", "t") != LUA_OK)
        return lua_error(L);
    /* Library setup and bounded text compilation can be slow on MCU/PSRAM. Start the
     * execution quota only when entering APP bytecode; top-level and on_start share it. */
    budget(r);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1))
        return luaL_error(L, "main.lua must return a lifecycle table");
    lua_setfield(L, LUA_REGISTRYINDEX, "q2.app");
    lua_getfield(L, LUA_REGISTRYINDEX, "q2.app");
    lua_getfield(L, -1, "on_start");
    if (!lua_isnil(L, -1)) {
        if (!lua_isfunction(L, -1))
            return luaL_error(L, "on_start must be a function");
        lua_call(L, 0, 0);
    }
    return 0;
}
static int deliver(lua_State *L) {
    const q2_event_t *e = lua_touserdata(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "q2.app");
    lua_getfield(L, -1, "on_event");
    if (lua_isnil(L, -1))
        return 0;
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "on_event must be a function");
    lua_newtable(L);
    lua_pushstring(L, e->topic);
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, e->value);
    lua_setfield(L, -2, "value");
    lua_pushstring(L, e->text);
    lua_setfield(L, -2, "text");
    if (e->has_data) {
        lua_pushstring(L, e->key);
        lua_setfield(L, -2, "key");
        lua_pushinteger(L, (lua_Integer)e->subscription);
        lua_setfield(L, -2, "subscription");
        lua_pushinteger(L, (lua_Integer)e->sequence);
        lua_setfield(L, -2, "sequence");
        lua_pushinteger(L, (lua_Integer)e->timestamp_ms);
        lua_setfield(L, -2, "timestamp_ms");
        const char *status;
        q2_result_name(e->data_status, &status);
        lua_pushstring(L, status);
        lua_setfield(L, -2, "status");
        lua_pushboolean(L, e->data_status == Q2_OK);
        lua_setfield(L, -2, "valid");
        if (e->data_status != Q2_OK)
            lua_pushnil(L);
        else if (e->data.kind == Q2_BOOLEAN)
            lua_pushboolean(L, e->data.as.boolean);
        else if (e->data.kind == Q2_INTEGER)
            lua_pushinteger(L, e->data.as.integer);
        else if (e->data.kind == Q2_STRING)
            lua_pushstring(L, e->data.as.string);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "value");
    }
    lua_call(L, 1, 0);
    return 0;
}
static q2_result_t checked(q2_lua_runtime_t *r, int result) {
    if (result == LUA_OK) {
        lua_settop(r->lua, 0);
        return Q2_OK;
    }
    /* assert(q.ui.*(...)) may raise the unified error table; preserve its code for diagnostics. */
    if (lua_type(r->lua, -1) == LUA_TTABLE) {
        lua_pushliteral(r->lua, "code");
        lua_rawget(r->lua, -2);
    }
    const char *message =
        lua_type(r->lua, -1) == LUA_TSTRING ? lua_tostring(r->lua, -1) : "non-string Lua error";
    snprintf(r->error, sizeof(r->error), "%s", message ? message : "Lua failure");
    r->last = result == LUA_ERRMEM ? Q2_ERR_MEMORY : Q2_ERR_RUNTIME;
    Q2_LOGE(r->kernel, "lua", "APP %s failed result=%d: %s", r->caller.app_id, r->last, r->error);
    q2_result_t e = q2_lua_stop(r);
    return e == Q2_OK ? r->last : e;
}
static q2_result_t event(void *context, const q2_event_t *e) {
    q2_lua_runtime_t *r = context;
    if (!r->lua || r->exit_requested || (e->instance && e->instance != r->caller.instance))
        return Q2_OK;
    /* Only instance-addressed app events are exposed; system events need an
     * explicit component API. */
    if (!e->instance)
        return Q2_OK;
    budget(r);
    r->in_callback = true;
    lua_pushcfunction(r->lua, deliver);
    lua_pushlightuserdata(r->lua, (void *)e);
    int code = lua_pcall(r->lua, 1, 0, 0);
    r->in_callback = false;
    return checked(r, code);
}
static int tick(lua_State *L) {
    q2_lua_runtime_t *r = runtime(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "q2.app");
    lua_getfield(L, -1, "on_tick");
    if (lua_isnil(L, -1))
        return 0;
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "on_tick must be a function");
    uint64_t now = 0;
    q2_kernel_time(r->kernel, &now);
    lua_pushinteger(L, (lua_Integer)now);
    lua_call(L, 1, 0);
    return 0;
}
static q2_result_t poll(void *context) {
    q2_lua_runtime_t *r = context;
    if (r->exit_requested)
        return q2_lua_stop(r);
    if (!r->lua)
        return Q2_OK;
    /* Every VM receives its own bounded cooperative tick; never reenter another Lua callback. */
    budget(r);
    r->in_callback = true;
    lua_pushcfunction(r->lua, tick);
    int code = lua_pcall(r->lua, 0, 0, 0);
    r->in_callback = false;
    return checked(r, code);
}
static q2_result_t stop(void *context) {
    return q2_lua_stop(context);
}
q2_result_t q2_lua_create(q2_kernel_t *k, q2_lua_runtime_t **out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    q2_lua_runtime_t *r = NULL;
    q2_result_t e = q2_kernel_allocate(k, sizeof(*r), (void **)&r);
    if (e != Q2_OK)
        return e;
    memset(r, 0, sizeof(*r));
    r->kernel = k;
    q2_runtime_driver_t driver = {1, sizeof(driver), r, event, poll, stop};
    e = q2_runtime_attach(k, &driver);
    if (e != Q2_OK) {
        q2_kernel_release(k, r);
        return e;
    }
    *out = r;
    return Q2_OK;
}
q2_result_t q2_lua_start(q2_lua_runtime_t *r, const char *json, size_t json_size, const char *source,
                         size_t size, const char *const *grants, size_t count) {
    if (!r || !source || !size || size > Q2_SOURCE_MAX || count > Q2_CAPABILITY_MAX || (count && !grants))
        return Q2_ERR_ARGUMENT;
    if (r->lua || r->caller.instance)
        return Q2_ERR_BUSY;
    q2_result_t e = q2_manifest_parse(json, json_size, &r->manifest);
    if (e != Q2_OK) {
        Q2_LOGW(r->kernel, "lua", "manifest rejected result=%d", e);
        return e;
    }
    memset(&r->caller, 0, sizeof(r->caller));
    r->error[0] = 0;
    r->last = Q2_OK;
    r->exit_requested = false;
    strcpy(r->caller.app_id, r->manifest.id);
    for (size_t group = 0; group < 2; group++) {
        size_t n = group ? r->manifest.optional_count : r->manifest.required_count;
        for (size_t i = 0; i < n; i++) {
            const char *cap = group ? r->manifest.optional[i] : r->manifest.required[i];
            bool permitted = false, found = false;
            for (size_t g = 0; g < count; g++)
                if (grants[g] && !strcmp(cap, grants[g]))
                    permitted = true;
            q2_capability_available(r->kernel, cap, &found);
            if (!group && (!permitted || !found)) {
                Q2_LOGW(r->kernel, "lua", "APP %s cannot acquire %s", r->manifest.id, cap);
                return !permitted ? Q2_ERR_PERMISSION : Q2_ERR_UNSUPPORTED;
            }
            if (permitted && found)
                r->caller.grants[r->caller.grant_count++] = cap;
        }
    }
    e = q2_kernel_next_instance(r->kernel, &r->caller.instance);
    if (e != Q2_OK)
        return e;
    r->lua = lua_newstate(allocate, r, 0);
    if (!r->lua) {
        Q2_LOGE(r->kernel, "lua", "VM allocation failed");
        r->caller.instance = 0;
        return Q2_ERR_MEMORY;
    }
    *(q2_lua_runtime_t **)lua_getextraspace(r->lua) = r;
    r->source = source;
    r->source_size = size;
    budget(r);
    lua_pushcfunction(r->lua, setup);
    r->in_callback = true;
    int code = lua_pcall(r->lua, 0, 0, 0);
    r->in_callback = false;
    e = checked(r, code);
    if (e == Q2_OK)
        Q2_LOGI(r->kernel, "lua", "APP %s started instance=%llu", r->caller.app_id,
                (unsigned long long)r->caller.instance);
    r->source = NULL;
    r->source_size = 0;
    return e;
}
q2_result_t q2_lua_stop(q2_lua_runtime_t *r) {
    if (!r)
        return Q2_ERR_ARGUMENT;
    if (r->in_callback)
        return Q2_ERR_BUSY;
    /* Close VM first so finalizers cannot observe already freed component
     * resources. */
    if (r->lua) {
        lua_close(r->lua);
        r->lua = NULL;
    }
    if (r->caller.instance) {
        q2_result_t e = q2_kernel_release_app(r->kernel, r->caller.instance);
        if (e != Q2_OK) {
            Q2_LOGE(r->kernel, "lua", "APP cleanup failed result=%d", e);
            return e;
        }
        Q2_LOGI(r->kernel, "lua", "APP %s stopped instance=%llu", r->caller.app_id,
                (unsigned long long)r->caller.instance);
    }
    r->caller.instance = 0;
    r->exit_requested = false;
    return Q2_OK;
}
q2_result_t q2_lua_destroy(q2_lua_runtime_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    q2_lua_runtime_t *r = *ref;
    q2_result_t e = q2_runtime_detach(r->kernel, r);
    if (e != Q2_OK)
        return e;
    q2_kernel_release(r->kernel, r);
    *ref = NULL;
    return Q2_OK;
}
q2_result_t q2_lua_status(q2_lua_runtime_t *r, q2_app_status_t *out) {
    if (!r || !out)
        return Q2_ERR_ARGUMENT;
    *out = (q2_app_status_t){.running = r->lua != NULL,
                             .heap_bytes = r->used,
                             .instance = r->caller.instance,
                             .last_result = r->last};
    strcpy(out->error, r->error);
    return Q2_OK;
}
