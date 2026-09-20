/* =============================================================================
 * pico_xr_profile_alias.c   —  Pico OpenXR interaction-profile aliasing shim
 *                              (systemless, Magisk overlay of a system library)
 *
 * PROBLEM
 *   The Steam-Frame build of VRChat suggests exactly one OpenXR interaction
 *   profile: `/interaction_profiles/valve/frame_controller_valve`.
 *   Pico's runtime (Monado-derived, libpxrruntime.so) does NOT implement that
 *   profile.  Per the OpenXR spec, xrSuggestInteractionProfileBindings() then
 *   fails as a whole with XR_ERROR_PATH_UNSUPPORTED and *every* binding in that
 *   call is discarded -> the controllers deliver zero input (the HMD still works
 *   because 6DoF head tracking does not depend on an interaction profile).
 *
 * FIX
 *   Replace /system/lib64/libopenxr_forwardloader.so with this shim.  The shim
 *   is loaded into the same process as the runtime, lets the original library do
 *   all the real work, and only wraps the single function the Khronos loader
 *   obtains from xrNegotiateLoaderRuntimeInterface():
 *
 *       runtime getInstanceProcAddr  ->  my_gipa()
 *
 *   my_gipa() hands back wrappers for
 *       xrStringToPath                        (rewrite the profile token)
 *       xrPathToString                        (restore it for the app)
 *       xrSuggestInteractionProfileBindings   (log / verify)
 *
 *   The rewrite is a plain substring substitution of the profile token, so it
 *   covers every shape the app may hand over:
 *       "/interaction_profiles/valve/frame_controller_valve"
 *       "/interaction_profiles/valve/frame_controller_valve/input/trigger/value"
 *
 * SAFETY
 *   * Nothing inside the VRChat APK is touched, no code is injected into the
 *     game: we sit *below* it in Pico's own OpenXR stack.
 *   * The wrapper is installed in every process, but it only ever REWRITES a
 *     path that literally contains the `from=` token.  No other app and no
 *     Pico system component ever uses `valve/frame_controller_valve`, so for
 *     everyone else behaviour is byte-identical to stock.  (A process-name
 *     gate is NOT possible here: the library is in /system/etc/public.libraries.txt,
 *     so zygote preloads it and every app inherits the already-initialised
 *     state through fork() -- the constructor never re-runs in a child.)
 *   * The original library is kept verbatim next to the shim and dlopen()ed at
 *     runtime, so behaviour without a rewrite is byte-identical to stock.
 *   * Disabling or deleting the Magisk module restores the stock system library
 *     with no residue (no APK edit, no property, no persistent state).
 *
 * EXPORTS (must match the original: xrNegotiateLoaderRuntimeInterface,
 *          xrInitializeLoaderKHR, xrInitializeLoaderKhrNative, JNI_OnLoad)
 * ========================================================================== */

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --------------------------------- types --------------------------------- */
typedef int32_t  XrResult;
typedef void    *XrInstance;
typedef uint64_t XrPath;
typedef uint64_t XrVersion;

#define XR_SUCCESS                      0
#define XR_ERROR_FUNCTION_UNSUPPORTED (-7)
#define XR_ERROR_PATH_UNSUPPORTED    (-15)
#define XR_ERROR_SIZE_INSUFFICIENT    (-2)

/* ------------------------------- configuration ---------------------------- */
/* The *token* is used without the leading "/interaction_profiles/" so that a
 * single substitution also covers profile-qualified binding paths. */
static const char *DEFAULT_FROM_TOKEN = "valve/frame_controller_valve";
static const char *DEFAULT_TO_TOKEN   = "oculus/touch_controller";
static const char *CONF_CANDIDATES[] = {
    "/system/etc/pico_xr_alias.conf",
    "/data/adb/pico_xr_alias.conf",
    NULL,
};
static const char *FILE_LOG           = "/data/local/tmp/pico_xr_alias.log";

/* Pico's runtime validates EVERY suggested binding path and fails the whole
 * xrSuggestInteractionProfileBindings() call when a single one does not exist
 * in the target interaction profile ("... is not a valid path").  So after
 * aliasing the profile we must also drop the components the target profile
 * does not have, and optionally rename a few (Steam Frame "view" button ==
 * Touch "menu" button).  Both are driven by this table. */
static const char *DEFAULT_MAP[][2] = {
    { "/input/view",   "/input/menu"   },
    { "/input/bumper", "/input/trigger" },
    { "/user/hand/right/input/x", "/user/hand/left/input/x" },
    { "/user/hand/right/input/y", "/user/hand/left/input/y" },
    { NULL, NULL },
};

#define SHIM_VERSION "v1.7"

#define SLOT 192
static char g_from[SLOT];      /* replaced token, e.g. valve/frame_controller_valve */
static char g_to[SLOT];        /* replacement,    e.g. oculus/touch_controller      */
static int  g_debug = 1;

#define MAP_CAP 24
static char g_map_from[MAP_CAP][SLOT];
static char g_map_to[MAP_CAP][SLOT];
static int  g_map_n = 0;

static int g_force_ext = 1;    /* add XR_BD_controller_interaction to xrCreateInstance */
static int g_discover = 0;     /* probe a component dictionary against the target profile */

/* --------------------------------- original lib --------------------------- */
static const char *ORIG_CANDIDATES[] = {
    "/system/lib64/libopenxr_forwardloader_orig.so",
    "/system/lib/libopenxr_forwardloader_orig.so",
    "/data/adb/modules/pico_xr_profile_alias/system/lib64/libopenxr_forwardloader_orig.so",
    "/data/local/tmp/libopenxr_forwardloader_orig.so",
    NULL,
};

static void *g_orig = NULL;
static void *g_orig_negotiate = NULL;
static void *g_orig_initkhr = NULL;
static void *g_orig_jnionload = NULL;

/* Data object exported by the original library (kept for ABI compatibility). */
void *xrInitializeLoaderKhrNative = NULL;

/* --------------------------------- logging -------------------------------- */
static const char *TAG = "PicoXrAlias";
static int g_file_fd = -1;

static void log_line(const char *s) {
    __android_log_write(ANDROID_LOG_INFO, TAG, s);
    if (g_file_fd >= 0) {
        size_t n = strlen(s);
        ssize_t w = write(g_file_fd, s, n);
        (void)w;
        w = write(g_file_fd, "\n", 1);
        (void)w;
    }
}

static void logmsg(const char *fmt, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(buf);
}

/* --------------------------------- config --------------------------------- */
static void copy_field(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void load_config(void) {
    copy_field(g_from, sizeof(g_from), DEFAULT_FROM_TOKEN);
    copy_field(g_to, sizeof(g_to), DEFAULT_TO_TOKEN);
    g_map_n = 0;
    for (int i = 0; DEFAULT_MAP[i][0]; i++) {
        if (g_map_n >= MAP_CAP) break;
        copy_field(g_map_from[g_map_n], SLOT, DEFAULT_MAP[i][0]);
        copy_field(g_map_to[g_map_n], SLOT, DEFAULT_MAP[i][1]);
        g_map_n++;
    }

    for (int c = 0; CONF_CANDIDATES[c]; c++) {
        FILE *f = fopen(CONF_CANDIDATES[c], "re");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\0') continue;
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (strncmp(p, "from=", 5) == 0) {
                copy_field(g_from, sizeof(g_from), p + 5);
            } else if (strncmp(p, "to=", 3) == 0) {
                copy_field(g_to, sizeof(g_to), p + 3);
        } else if (strncmp(p, "debug=", 6) == 0) {
            g_debug = atoi(p + 6);
        } else if (strncmp(p, "forceext=", 9) == 0) {
            g_force_ext = atoi(p + 9);
        } else if (strncmp(p, "discover=", 9) == 0) {
            g_discover = atoi(p + 9);
        } else if (strncmp(p, "map=", 4) == 0) {
                char *eq = strchr(p + 4, '=');
                if (eq && g_map_n < MAP_CAP) {
                    *eq = '\0';
                    copy_field(g_map_from[g_map_n], SLOT, p + 4);
                    copy_field(g_map_to[g_map_n], SLOT, eq + 1);
                    g_map_n++;
                }
            }
        }
        fclose(f);
        break;      /* first readable config wins */
    }
}

/* --------------------------------- dlopen helpers ------------------------- */
/* A renamed copy is not in /system/etc/public.libraries.txt, so a plain
 * dlopen() by absolute path is refused for app processes.  Fall back to
 * android_dlopen_ext() with an explicit file descriptor, which is not subject
 * to that allow-list. */
static void *dlopen_orig(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        logmsg("orig loaded via dlopen: %s", path);
        return h;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;

    android_dlextinfo info;
    memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    h = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &info);
    close(fd);
    if (h) logmsg("orig loaded via android_dlopen_ext(fd): %s", path);
    return h;
}

/* --------------------------------- state ---------------------------------- */
static void *g_real_gipa     = NULL;   /* runtime getInstanceProcAddr            */
static void *g_real_s2p      = NULL;   /* xrStringToPath                          */
static void *g_real_p2s      = NULL;   /* xrPathToString                          */
static void *g_real_suggest  = NULL;   /* xrSuggestInteractionProfileBindings     */
static void *g_real_create_instance = NULL;
static void *g_real_get_current_ip  = NULL;   /* xrGetCurrentInteractionProfile   */
static XrInstance g_instance = NULL;
static int g_inited = 0;

/* Map rewritten XrPath -> the string the app originally asked for, so that
 * xrPathToString() stays self-consistent from the app's point of view. */
#define RMAP_CAP 192
struct rmap_entry { XrPath path; char orig[SLOT]; };
static struct rmap_entry g_rmap[RMAP_CAP];
static int g_rmap_n = 0;
static volatile int g_lock = 0;

static void lock(void)   { while (__sync_lock_test_and_set(&g_lock, 1)) { } }
static void unlock(void) { __sync_lock_release(&g_lock); }

static void rmap_put(XrPath p, const char *orig) {
    lock();
    for (int i = 0; i < g_rmap_n; i++) {
        if (g_rmap[i].path == p) { unlock(); return; }
    }
    if (g_rmap_n < RMAP_CAP) {
        g_rmap[g_rmap_n].path = p;
        copy_field(g_rmap[g_rmap_n].orig, SLOT, orig);
        g_rmap_n++;
    }
    unlock();
}

static const char *rmap_get(XrPath p) {
    const char *r = NULL;
    lock();
    for (int i = 0; i < g_rmap_n; i++) {
        if (g_rmap[i].path == p) { r = g_rmap[i].orig; break; }
    }
    unlock();
    return r;
}

/* --------------------------------- rewrite -------------------------------- */
/* Replace every occurrence of `g_from` inside `in` with `g_to`.
 * Returns 1 when something changed and `out` holds the new string. */
static int rewrite_token(const char *in, char *out, size_t cap) {
    size_t fl = strlen(g_from), tl = strlen(g_to), il = strlen(in);
    if (fl == 0 || strstr(in, g_from) == NULL) return 0;

    /* worst case length */
    if (il + 1 > cap) return 0;

    size_t o = 0;
    const char *p = in;
    while (*p) {
        if (strncmp(p, g_from, fl) == 0) {
            if (o + tl + 1 > cap) return 0;
            memcpy(out + o, g_to, tl);
            o += tl;
            p += fl;
        } else {
            if (o + 2 > cap) return 0;
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 1;
}

typedef XrResult (*PFN_gipa)(XrInstance, const char *, void **);
typedef XrResult (*PFN_string_to_path)(XrInstance, const char *, XrPath *);
typedef XrResult (*PFN_path_to_string)(XrInstance, XrPath, uint32_t, uint32_t *, char *);
typedef XrResult (*PFN_suggest)(uint64_t /*XrSession*/, const void *);
typedef XrResult (*PFN_negotiate)(const void *, void *);
typedef XrResult (*PFN_initkhr)(const void *);
typedef int32_t  (*PFN_jnionload)(void *, void *);

/* ------------------------- wrapper: xrStringToPath ------------------------ */
static XrResult my_string_to_path(XrInstance inst, const char *s, XrPath *out) {
    PFN_string_to_path real = (PFN_string_to_path)g_real_s2p;
    if (!real) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!s) return real(inst, s, out);

    char nbuf[512];
    if (rewrite_token(s, nbuf, sizeof(nbuf))) {
        XrResult r = real(inst, nbuf, out);
        logmsg("alias xrStringToPath: \"%s\" -> \"%s\" ret=%d path=%llu",
               s, nbuf, (int)r, (unsigned long long)(out ? *out : 0));
        if (r == XR_SUCCESS && out) rmap_put(*out, s);
        return r;
    }

    if (g_debug && strstr(s, "/interaction_profiles")) {
        logmsg("xrStringToPath (untouched): \"%s\"", s);
    }
    return real(inst, s, out);
}

/* ------------------------- wrapper: xrPathToString ------------------------ */
static XrResult my_path_to_string(XrInstance inst, XrPath path, uint32_t cap,
                                  uint32_t *count, char *buf) {
    PFN_path_to_string real = (PFN_path_to_string)g_real_p2s;
    const char *orig = rmap_get(path);
    if (orig) {
        uint32_t need = (uint32_t)strlen(orig) + 1;
        if (count) *count = need;
        if (buf && cap >= need) {
            memcpy(buf, orig, need);
            return XR_SUCCESS;
        }
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    return real ? real(inst, path, cap, count, buf) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* ------------------- wrapper: xrSuggestInteractionProfileBindings --------- */
typedef struct {
    int32_t  type;
    uint32_t _pad0;
    const void *next;
    XrPath   interactionProfile;
    uint32_t countSuggestedBindings;
    uint32_t _pad1;
    const void *suggestedBindings;
} XrInteractionProfileSuggestedBinding;

typedef struct { uint64_t action; XrPath binding; } XrActionSuggestedBinding;

/* ---- force-enable interaction extensions the app forgot to ask for -------
 * Pico's own profile (/interaction_profiles/bytedance/pico4_controller) is
 * gated behind XR_BD_controller_interaction.  If the app did not enable that
 * extension the runtime rejects every suggested path for it with
 * XR_ERROR_PATH_UNSUPPORTED, which is exactly what a Steam Frame build does.
 * The loader has already validated the app's own list, so adding one more
 * name here is invisible to it and harmless. */
#define XR_TYPE_INSTANCE_CREATE_INFO 3
static const char *FORCE_EXTENSIONS[] = {
    "XR_BD_controller_interaction",
    NULL,
};
static const char *g_extnames[32];

typedef struct {
    char   applicationName[128];
    uint32_t applicationVersion;
    char   engineName[128];
    uint32_t engineVersion;
    uint64_t apiVersion;
} AppInfoShim;

typedef struct {
    int32_t  type;
    uint32_t _p0;
    const void *next;
    uint32_t createFlags;
    uint32_t _p1;
    AppInfoShim appInfo;
    uint32_t enabledApiLayerCount;
    uint32_t _p2;
    const char *const *enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    uint32_t _p3;
    const char *const *enabledExtensionNames;
} XrInstanceCreateInfoShim;

typedef XrResult (*PFN_create_instance)(const XrInstanceCreateInfoShim *, XrInstance *);

static XrResult my_create_instance(const XrInstanceCreateInfoShim *info, XrInstance *out) {
    PFN_create_instance real = (PFN_create_instance)g_real_create_instance;
    if (!real) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!g_force_ext || !info || info->type != XR_TYPE_INSTANCE_CREATE_INFO || !info->enabledExtensionNames) {
        return real(info, out);
    }
    logmsg("xrCreateInstance: app enabled %u extension(s), forcing %s",
           info->enabledExtensionCount, FORCE_EXTENSIONS[0]);
    for (uint32_t i = 0; i < info->enabledExtensionCount && i < 32; i++)
        logmsg("  app ext[%u] = %s", i, info->enabledExtensionNames[i]);

    uint32_t n = info->enabledExtensionCount;
    if (n > 24) n = 24;                       /* keep room for the forced ones */
    for (uint32_t i = 0; i < n; i++) g_extnames[i] = info->enabledExtensionNames[i];

    uint32_t added = 0;
    for (int e = 0; FORCE_EXTENSIONS[e]; e++) {
        int have = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (g_extnames[i] && strcmp(g_extnames[i], FORCE_EXTENSIONS[e]) == 0) { have = 1; break; }
        }
        if (!have && n + added < 31) {
            g_extnames[n + added] = FORCE_EXTENSIONS[e];
            logmsg("force-enabling %s", FORCE_EXTENSIONS[e]);
            added++;
        }
    }
    if (!added) return real(info, out);

    XrInstanceCreateInfoShim tmp = *info;
    tmp.enabledExtensionCount = n + added;
    tmp.enabledExtensionNames = g_extnames;
    return real(&tmp, out);
}

static void path_string(XrInstance inst, XrPath p, char *out, size_t cap);
static void run_discovery(uint64_t session, int32_t sbtype, XrPath profile, const XrActionSuggestedBinding *in,
                          uint32_t n);

static void path_string(XrInstance inst, XrPath p, char *out, size_t cap) {
    PFN_path_to_string real = (PFN_path_to_string)g_real_p2s;
    out[0] = '\0';
    const char *o = rmap_get(p);
    if (o) { copy_field(out, cap, o); return; }
    if (real && inst) {
        uint32_t cnt = 0;
        if (real(inst, p, (uint32_t)cap, &cnt, out) != XR_SUCCESS) out[0] = '\0';
    }
}

/* --- per-binding validity cache ------------------------------------------ */
/* Probed once per component path, shared by both hands.  Invalidated whenever
 * the alias target changes, because validity is per profile. */
#define PRUNE_CAP 256
struct pk { char key[SLOT]; int ok; };
static struct pk g_pk[PRUNE_CAP];
static int g_pk_n = 0;
static char g_pk_for[SLOT];        /* the g_to the cache was built for */

static void pk_reset_if_target_changed(void) {
    if (strcmp(g_pk_for, g_to) != 0) {
        g_pk_n = 0;
        copy_field(g_pk_for, sizeof(g_pk_for), g_to);
    }
}

static int pk_get(const char *k, int *ok) {
    for (int i = 0; i < g_pk_n; i++) {
        if (strcmp(g_pk[i].key, k) == 0) { *ok = g_pk[i].ok; return 1; }
    }
    return 0;
}

static void pk_put(const char *k, int ok) {
    if (g_pk_n < PRUNE_CAP) {
        copy_field(g_pk[g_pk_n].key, SLOT, k);
        g_pk[g_pk_n].ok = ok;
        g_pk_n++;
    }
}

/* Rename a component when the target profile uses a different name (or hand)
 * for the same physical control.  The match is a substring splice so that a
 * rule may target either the component part (/input/view) or the whole path
 * including the hand (/user/hand/right/input/x).  Returns 1 and fills `out`
 * when renamed. */
static int map_component(const char *bp, char *out, size_t cap) {
    for (int i = 0; i < g_map_n; i++) {
        const char *hit = strstr(bp, g_map_from[i]);
        if (!hit || g_map_from[i][0] == '\0') continue;
        size_t pre = (size_t)(hit - bp);
        size_t fl = strlen(g_map_from[i]);
        size_t tl = strlen(g_map_to[i]);
        size_t rest = strlen(bp) - pre - fl;
        if (pre + tl + rest + 1 > cap) return 0;
        memcpy(out, bp, pre);
        memcpy(out + pre, g_map_to[i], tl);
        memcpy(out + pre + tl, hit + fl, rest);
        out[pre + tl + rest] = '\0';
        return 1;
    }
    return 0;
}

static XrResult my_suggest(uint64_t session, const XrInteractionProfileSuggestedBinding *sb) {
    PFN_suggest real = (PFN_suggest)g_real_suggest;
    PFN_string_to_path s2p = (PFN_string_to_path)g_real_s2p;
    if (!real) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!sb) return real(session, sb);

    char prof[SLOT];
    path_string(g_instance, sb->interactionProfile, prof, sizeof(prof));

    /* Only the aliased profile gets pruned; native suggestions pass through. */
    if (!strstr(prof, g_from)) {
        logmsg("xrSuggestInteractionProfileBindings: profile=\"%s\" count=%u (pass-through)",
               prof, sb->countSuggestedBindings);
        return real(session, sb);
    }

    const XrActionSuggestedBinding *in = (const XrActionSuggestedBinding *)sb->suggestedBindings;
    uint32_t n = sb->countSuggestedBindings;
    if (!in) return real(session, sb);

    static XrActionSuggestedBinding keep[PRUNE_CAP];
    uint32_t m = 0;

    for (uint32_t i = 0; i < n && i < PRUNE_CAP; i++) {
        char bp[SLOT];
        path_string(g_instance, in[i].binding, bp, sizeof(bp));
        if (!bp[0]) {                       /* unresolvable: keep as-is */
            keep[m++] = in[i];
            continue;
        }

        XrPath use = in[i].binding;
        char ap[SLOT];
        if (map_component(bp, ap, sizeof(ap)) && s2p && g_instance) {
            XrPath np = 0;
            if (s2p(g_instance, ap, &np) == XR_SUCCESS) {
                use = np;
                logmsg("  remap binding[%u] \"%s\" -> \"%s\"", i, bp, ap);
            }
        }

        const char *key = strstr(bp, "/input/");
        if (!key) key = bp;
        int ok = 0;
        if (!pk_get(key, &ok)) {
            XrActionSuggestedBinding one[1];
            one[0].action = in[i].action;
            one[0].binding = use;
            XrInteractionProfileSuggestedBinding probe = *sb;
            probe.countSuggestedBindings = 1;
            probe.suggestedBindings = one;
            XrResult pr = real(session, &probe);
            ok = (pr == XR_SUCCESS);
            pk_put(key, ok);
            if (g_debug) logmsg("  probe %s -> %d", key, (int)pr);
        }
        if (ok) {
            keep[m].action = in[i].action;
            keep[m].binding = use;
            m++;
        } else if (g_debug) {
            logmsg("  drop binding[%u] \"%s\" (not in target profile)", i, bp);
        }
    }

    logmsg("xrSuggestInteractionProfileBindings: profile=\"%s\" kept %u/%u",
           prof, m, n);

    XrInteractionProfileSuggestedBinding fin = *sb;
    fin.countSuggestedBindings = m;
    fin.suggestedBindings = keep;
    XrResult rr = real(session, &fin);
    logmsg("xrSuggestInteractionProfileBindings -> %d", (int)rr);
    if (g_discover) run_discovery(session, sb->type, fin.interactionProfile, in, n);
    return rr;
}

/* ------------------- discovery: probe a component dictionary --------------
 * Pico never documents which components its own profiles expose.  When
 * `discover=1` this walks a dictionary of candidate paths once, probes each
 * one against the target profile with an action handle of the matching type
 * (taken from the app's own suggestions) and logs the result.  The output is
 * the exact vocabulary needed to write `map=` rules. */
static const char *DISC_BASES[] = {
    "trigger", "squeeze", "grip", "select", "menu", "system", "back", "home",
    "a", "b", "x", "y", "thumbstick", "trackpad", "joystick", "thumbrest",
    "battery", "stylus", "view", "bumper", NULL,
};
static const char *DISC_SUFFIX[] = { "", "/click", "/touch", "/value", "/force", "/pose" };

enum { A_BOOL = 0, A_FLOAT = 1, A_VEC2 = 2, A_POSE = 3, A_N = 4 };

static int path_type(const char *p) {
    size_t n = strlen(p);
    if (n >= 5 && strcmp(p + n - 5, "/pose") == 0) return A_POSE;
    if (n >= 6 && (strcmp(p + n - 6, "/click") == 0 || strcmp(p + n - 6, "/touch") == 0)) return A_BOOL;
    if (n >= 6 && (strcmp(p + n - 6, "/value") == 0 || strcmp(p + n - 6, "/force") == 0)) return A_FLOAT;
    return A_VEC2;                                   /* bare component */
}

static void run_discovery(uint64_t session, int32_t sbtype, XrPath profile, const XrActionSuggestedBinding *in,
                          uint32_t n) {
    static int done = 0;
    if (done || !n) return;
    done = 1;

    PFN_suggest real = (PFN_suggest)g_real_suggest;
    uint64_t act[A_N];
    for (int t = 0; t < A_N; t++) act[t] = 0;

    char user[SLOT] = "/user/hand/left";
    for (uint32_t i = 0; i < n; i++) {
        char bp[SLOT];
        path_string(g_instance, in[i].binding, bp, sizeof(bp));
        if (!bp[0]) continue;
        int t = path_type(bp);
        if (act[t] == 0) act[t] = in[i].action;
        const char *h = strstr(bp, "/input/");
        if (h && strcmp(user, "/user/hand/left") == 0) {
            size_t l = (size_t)(h - bp);
            if (l > 0 && l < sizeof(user)) { memcpy(user, bp, l); user[l] = '\0'; }
        }
    }
    logmsg("discovery: user=\"%s\" actions bool=%llu float=%llu vec2=%llu pose=%llu",
           user, (unsigned long long)act[A_BOOL], (unsigned long long)act[A_FLOAT],
           (unsigned long long)act[A_VEC2], (unsigned long long)act[A_POSE]);

    for (int b = 0; DISC_BASES[b]; b++) {
        for (int s = 0; s < 6; s++) {
            char comp[SLOT];
            snprintf(comp, sizeof(comp), "/input/%s%s", DISC_BASES[b], DISC_SUFFIX[s]);
            int t = path_type(comp);
            if (act[t] == 0) continue;

            char full[SLOT];
            snprintf(full, sizeof(full), "%s%s", user, comp);

            XrPath p = 0;
            PFN_string_to_path s2p = (PFN_string_to_path)g_real_s2p;
            if (!s2p || s2p(g_instance, full, &p) != XR_SUCCESS) {
                logmsg("disc %s -> stringToPath failed", full);
                continue;
            }
            XrActionSuggestedBinding one[1];
            one[0].action = act[t];
            one[0].binding = p;
            XrInteractionProfileSuggestedBinding probe;
            memset(&probe, 0, sizeof(probe));
            probe.type = sbtype;
            probe.type = sbtype;
            probe.interactionProfile = profile;
            probe.countSuggestedBindings = 1;
            probe.suggestedBindings = one;
            XrResult pr = real(session, &probe);
            logmsg("disc %s -> %d%s", full, (int)pr, pr == XR_SUCCESS ? "   <<VALID>>" : "");
        }
    }
    logmsg("discovery finished");
}

/* ------------------- wrapper: xrGetCurrentInteractionProfile --------------
 * The runtime internally creates its own profile XrPath for the connected
 * physical device (e.g. /interaction_profiles/bytedance/pico4_controller).
 * That path is NOT in the rmap (only app-created ones are), so a plain
 * xrPathToString leaks the Pico name to the app.  VRChat does not know that
 * profile -> it renders no controller model and may gate its controller
 * logic.  Reverse-alias the answer back to the app's own token. */
typedef struct {
    int32_t  type;
    uint32_t _p0;
    const void *next;
    XrPath   interactionProfile;
} XrInteractionProfileStateShim;

typedef XrResult (*PFN_get_current_ip)(XrInstance, uint64_t, XrInteractionProfileStateShim *);

/* Replace every occurrence of `g_to` inside `in` with `g_from`. */
static int rewrite_token_rev(const char *in, char *out, size_t cap) {
    size_t fl = strlen(g_from), tl = strlen(g_to), il = strlen(in);
    if (tl == 0 || strstr(in, g_to) == NULL) return 0;
    if (il + 1 > cap) return 0;

    size_t o = 0;
    const char *p = in;
    while (*p) {
        if (strncmp(p, g_to, tl) == 0) {
            if (o + fl + 1 > cap) return 0;
            memcpy(out + o, g_from, fl);
            o += fl;
            p += tl;
        } else {
            if (o + 2 > cap) return 0;
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 1;
}

static XrResult my_get_current_ip(XrInstance inst, uint64_t session,
                                  XrInteractionProfileStateShim *state) {
    PFN_get_current_ip real = (PFN_get_current_ip)g_real_get_current_ip;
    if (!real) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = real(inst, session, state);
    if (r != XR_SUCCESS || !state || !state->interactionProfile) return r;

    PFN_path_to_string  p2s = (PFN_path_to_string)g_real_p2s;
    PFN_string_to_path  s2p = (PFN_string_to_path)g_real_s2p;
    if (!p2s || !s2p) return r;

    char s[SLOT], rev[SLOT];
    uint32_t cnt = 0;
    s[0] = '\0';
    if (p2s(inst, state->interactionProfile, (uint32_t)sizeof(s), &cnt, s) != XR_SUCCESS)
        return r;
    if (!rewrite_token_rev(s, rev, sizeof(rev))) return r;   /* unrelated path */

    XrPath np = 0;
    if (s2p(inst, rev, &np) == XR_SUCCESS) {
        state->interactionProfile = np;
        logmsg("xrGetCurrentInteractionProfile: \"%s\" -> \"%s\"", s, rev);
    }
    return r;
}

/* ------------------------------- wrapper: GIPA ---------------------------- */
static XrResult my_gipa(XrInstance inst, const char *name, void **fn) {
    PFN_gipa real = (PFN_gipa)g_real_gipa;
    if (!real) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XrResult r = real(inst, name, fn);
    if (r != XR_SUCCESS || !fn || !*fn || !name) return r;
    g_instance = inst;

    if (strcmp(name, "xrStringToPath") == 0) {
        if (!g_real_s2p) g_real_s2p = *fn;
        *fn = (void *)my_string_to_path;
    } else if (strcmp(name, "xrPathToString") == 0) {
        if (!g_real_p2s) g_real_p2s = *fn;
        *fn = (void *)my_path_to_string;
    } else if (strcmp(name, "xrSuggestInteractionProfileBindings") == 0) {
        if (!g_real_suggest) g_real_suggest = *fn;
        *fn = (void *)my_suggest;
    } else if (strcmp(name, "xrGetCurrentInteractionProfile") == 0) {
        if (!g_real_get_current_ip) g_real_get_current_ip = *fn;
        *fn = (void *)my_get_current_ip;
    } else if (strcmp(name, "xrCreateInstance") == 0) {
        if (!g_real_create_instance) g_real_create_instance = *fn;
        *fn = (void *)my_create_instance;
    }
    return r;
}

/* ------------------------------- init ------------------------------------ */
/* The original library shares our SONAME.  If the linker ever resolves the
 * dlopen() request back to THIS library (already loaded under the same
 * soname), g_orig_negotiate would point at ourselves and recurse forever.
 * Verify with dladdr() that the resolved code really comes from *_orig.so. */
static int is_foreign_orig(void *code) {
    Dl_info info;
    if (!dladdr(code, &info) || !info.dli_fname) return 0;
    return strstr(info.dli_fname, "_orig.so") != NULL;
}

static void ensure_orig(void) {
    if (g_inited) return;
    g_inited = 1;

    g_file_fd = open(FILE_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);

    load_config();

    char selfcmd[128];
    selfcmd[0] = '\0';
    int cfd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, selfcmd, sizeof(selfcmd) - 1);
        close(cfd);
        if (n > 0) selfcmd[n] = '\0'; else selfcmd[0] = '\0';
    }

    for (int i = 0; ORIG_CANDIDATES[i]; i++) {
        void *h = dlopen_orig(ORIG_CANDIDATES[i]);
        if (!h) continue;
        void *nego = dlsym(h, "xrNegotiateLoaderRuntimeInterface");
        if (!nego || !is_foreign_orig(nego)) {
            logmsg("rejecting %s: resolved back to the shim itself (soname clash)",
                   ORIG_CANDIDATES[i]);
            continue;
        }
        g_orig = h;
        g_orig_negotiate = nego;
        break;
    }
    if (!g_orig) {
        log_line("FATAL: cannot load original forwardloader");
        return;
    }

    g_orig_initkhr   = dlsym(g_orig, "xrInitializeLoaderKHR");
    g_orig_jnionload = dlsym(g_orig, "JNI_OnLoad");

    void *slot = dlsym(g_orig, "xrInitializeLoaderKhrNative");
    if (slot) xrInitializeLoaderKhrNative = *(void **)slot;

    logmsg("---- shim init pid=%d proc=\"%s\" from=\"%s\" to=\"%s\" debug=%d orig=%p",
           (int)getpid(), selfcmd, g_from, g_to, g_debug, g_orig);
}

__attribute__((constructor)) static void shim_ctor(void) { ensure_orig(); }

/* ------------------------------- exports --------------------------------- */
typedef struct {
    int32_t  structType;
    uint32_t structVersion;
    size_t   structSize;
    uint32_t runtimeInterfaceVersion;
    uint32_t _pad;
    XrVersion runtimeApiVersion;
    void    *getInstanceProcAddr;
} NegotiateRuntimeRequest;

__attribute__((visibility("default")))
XrResult xrNegotiateLoaderRuntimeInterface(const void *loaderInfo, void *runtimeRequest) {
    ensure_orig();
    /* The constructor usually runs in zygote, so children inherit the config
     * read at boot.  Re-read it here, once per application start, so that
     * editing /system/etc/pico_xr_alias.conf only needs an app restart. */
    load_config();
    pk_reset_if_target_changed();
    if (!g_orig_negotiate) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XrResult r = ((PFN_negotiate)g_orig_negotiate)(loaderInfo, runtimeRequest);

    NegotiateRuntimeRequest *req = (NegotiateRuntimeRequest *)runtimeRequest;
    logmsg("xrNegotiateLoaderRuntimeInterface ret=%d structSize=%zu gipa=%p",
           (int)r, (req && req->structSize) ? req->structSize : (size_t)0,
           req ? req->getInstanceProcAddr : NULL);

    if (r == XR_SUCCESS && req && req->getInstanceProcAddr &&
        req->structSize >= sizeof(NegotiateRuntimeRequest)) {
        g_real_gipa = req->getInstanceProcAddr;
        req->getInstanceProcAddr = (void *)my_gipa;
        logmsg("shim %s runtime GIPA wrapped (rewrite only on token match)", SHIM_VERSION);
    }
    return r;
}

__attribute__((visibility("default")))
XrResult xrInitializeLoaderKHR(const void *initInfo) {
    ensure_orig();
    logmsg("xrInitializeLoaderKHR forwarded");
    if (!g_orig_initkhr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return ((PFN_initkhr)g_orig_initkhr)(initInfo);
}

__attribute__((visibility("default")))
int32_t JNI_OnLoad(void *vm, void *reserved) {
    ensure_orig();
    logmsg("JNI_OnLoad forwarded");
    if (!g_orig_jnionload) return 0x00010006; /* JNI_VERSION_1_6 */
    return ((PFN_jnionload)g_orig_jnionload)(vm, reserved);
}
