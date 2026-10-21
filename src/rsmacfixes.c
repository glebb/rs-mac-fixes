// librsmacfixes: fixes for Rocksmith 2014 Remastered on macOS (Steam, x86_64; runs under
// Rosetta on Apple Silicon). Loaded by the game through a weak load command that the
// rs-mac-fixes installer adds to its executable. See README.md and docs/technical.md.
//
// Faster song enumeration at startup:
// 1. The game mounts one DLC archive per frame (0x100a0d7e0); it now mounts as many as fit in
//    a frame budget (50 ms by default, see batch_install_step).
// 2. Every file-exists check asks each mounted archive in turn, and each recomputes the MD5 of
//    the same path (0x100158b70 -> 0x100158030, which also interns a refcounted path object):
//    quadratic in the number of archives. A cache keyed by the exact path bytes hands out the
//    same interned object the game would have returned, with its refcount raised under the
//    game's lock.
// 3. The game's global table of JSON number values hashes a number by its integer part, so all
//    values in [0, 1) share one bucket. The hash is replaced by the value's bit pattern (-0 made
//    +0, so equal numbers still hash equally) in the three places that compute it (insert,
//    remove, rehash), before the table holds anything.
// 4. Mounted archives stay open for a while; with many mounted per frame the game would exceed
//    the 256 open files that launchd allows by default and skip songs, so the limit is raised.
//
// Keyboard: End in menus and space in the song search (see keys()).
//
// The game's code is encrypted on disk and decrypted by its start routine after libraries
// load, so a thread waits for the expected original bytes to appear and patches only then.
// Anything unexpected (another game build, other bytes, a non-empty table): no patch, and the
// game runs as before.
//
// Settings, in ~/Library/Application Support/rs-mac-fixes/:
//   disabled   if this file exists, nothing is patched
//   budget_ms  milliseconds per frame for mounting archives (0 = one per frame, as the game does)
// Log: ~/Library/Application Support/rs-mac-fixes/rs-mac-fixes.log
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Addresses in the unslid x86_64 image (vmaddr 0x100000000).
#define TABLE_PTR 0x101949588ULL  // the hash table: +0x08 buckets, +0x10 buckets end, +0x20 count
#define LOCK_PTR 0x101949590ULL   // its recursive pthread mutex

typedef struct {
    const char *name;
    uint64_t addr;
    const char *orig;  // hex
    const char *repl;  // hex; the rest of the site is skipped with a short jmp
} site;

static const site SITES[] = {
    // number destructor (remove): hash of 0x10(%r14) in %xmm0 -> %rax
    {"remove", 0x1000f8d59,
     "f20f100def1c2f01660f28d0f20f5cd1f2480f2cc248ba00000000000000804831c2f2480f2cc0660f2ec8480f46c2",
     "660f57d2f20f58d066480f7ed0"},
    // intern (insert/find): value at -0x30(%rbp) -> %r15 (hash), %xmm1 (value for the compares)
    {"insert", 0x1000f8f61,
     "f20f1005e71a2f01f20f1055d0660f28caf20f5cc8f2480f2cc1660f28ca48ba00000000000000804831c2f24c0f2cf9660f2ec84c0f43fa",
     "f20f104dd0660f57d2f20f58d166490f7ed7"},
    // rehash: node value in %xmm1 -> %rax
    {"rehash", 0x1000f9a35, "660f28d1f20f5cd0f2480f2cd24c31c2f2480f2cc1660f2ec1480f46c2",
     "660f57d2f20f58d166480f7ed0"},
};
#define NSITES (sizeof SITES / sizeof SITES[0])

static FILE *g_log;
static struct timespec g_t0;

static double elapsed(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void logline(const char *fmt, ...) {
    if (!g_log) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(g_log, "%s +%.3fs %s\n", ts, elapsed(), msg);
}

static size_t unhex(const char *h, uint8_t *out) {
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned b;
        sscanf(h, "%2x", &b);
        out[n++] = (uint8_t)b;
    }
    return n;
}

static intptr_t g_slide;

static int matches(const site *s) {
    uint8_t want[128];
    size_t n = unhex(s->orig, want);
    return !memcmp((const void *)(s->addr + g_slide), want, n);
}

static int write_code(uint64_t addr, const uint8_t *bytes, size_t n) {
    vm_address_t page = (vm_address_t)(addr & ~(uint64_t)(vm_page_size - 1));
    vm_size_t len = (vm_size_t)((addr + n) - page);
    // Stays executable throughout: other code on the same page may run while we write. If the
    // system refuses that, briefly writable-only (the window is microseconds, early at startup).
    if (vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_ALL | VM_PROT_COPY) &&
        vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY))
        return -1;
    memcpy((void *)addr, bytes, n);
    return vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) ? -1 : 0;
}

static void patch_all(void) {
    for (size_t i = 0; i < NSITES; i++) {
        const site *s = &SITES[i];
        uint8_t orig[128], code[128];
        size_t len = unhex(s->orig, orig), n = unhex(s->repl, code);
        if (n + 2 > len) {
            logline("patch %s does not fit; skipped", s->name);
            continue;
        }
        // jmp rel8 over the rest of the site
        code[n] = 0xEB;
        code[n + 1] = (uint8_t)(len - n - 2);
        for (size_t k = n + 2; k < len; k++) code[k] = 0xCC;  // never executed
        int r = write_code(s->addr + g_slide, code, len);
        logline("patched %s at 0x%llx: %s", s->name, s->addr, r ? "FAILED" : "ok");
    }
}

// ---------------------------------------------------------------- path MD5 cache

#define PATH_LOCK_PTR 0x1018e6c10ULL  // recursive mutex of the interned path table
#define PATH_TABLE_PTR 0x1018e6c00ULL // the interned path table itself
#define FN_PATH_MD5 0x100158b70ULL     // void (void **out, const char *path)
#define FN_INTERN 0x100158030ULL       // void *(const char *s, int len /* -1 = strlen */)
#define FN_RELEASE 0x100158840ULL      // int (void *obj): drops a reference
#define OBJ_REFS 0x28                  // int refcount in the interned object
static const char PATH_MD5_ORIG[] = "554889e553504889f04889fbbeffffffff4889c7e8a7f4ffff4889034883c4085b5dc3";

typedef void *(*intern_fn)(const char *, int);
typedef int (*release_fn)(void *);

#define SLOTS 1024
static struct {
    char *s;
    size_t n;
    void *obj;
} g_cache[SLOTS];
static void *g_cache_table;  // the path table the cached objects belong to
static unsigned long g_hits, g_misses;
static unsigned long g_installed, g_frames;  // packages per frame (below)
#define COUNT(x) __atomic_fetch_add(&(x), 1, __ATOMIC_RELAXED)

// The game's own lock protocol: lock, owner + depth fields at 0x40/0x48, unlock.
// Also drops the cache if the game has created a new path table since it was filled: the old
// objects went with the old table (they are forgotten, not released).
static pthread_mutex_t *path_lock(void) {
    pthread_mutex_t *m = *(pthread_mutex_t **)(PATH_LOCK_PTR + g_slide);
    pthread_mutex_lock(m);
    *(pthread_t *)((char *)m + 0x40) = pthread_self();
    (*(int *)((char *)m + 0x48))++;
    void *table = *(void **)(PATH_TABLE_PTR + g_slide);
    if (table != g_cache_table) {
        for (unsigned i = 0; i < SLOTS; i++) {
            free(g_cache[i].s);
            g_cache[i].s = NULL;
            g_cache[i].obj = NULL;
        }
        if (g_cache_table) logline("path table recreated; MD5 cache dropped");
        g_cache_table = table;
    }
    return m;
}

static void path_unlock(pthread_mutex_t *m) {
    (*(int *)((char *)m + 0x48))--;
    pthread_mutex_unlock(m);
}

// Returns the object too: the original leaves it in %rax.
static void *hook_path_md5(void **out, const char *s) {
    if (!*(pthread_mutex_t **)(PATH_LOCK_PTR + g_slide)) return *out = ((intern_fn)(FN_INTERN + g_slide))(s, -1);
    size_t n = strlen(s);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    unsigned k = h % SLOTS;
    pthread_mutex_t *m = path_lock();
    if (g_cache[k].obj && g_cache[k].n == n && !memcmp(g_cache[k].s, s, n)) {
        void *obj = g_cache[k].obj;
        (*(int *)((char *)obj + OBJ_REFS))++;
        path_unlock(m);
        COUNT(g_hits);
        return *out = obj;
    }
    path_unlock(m);
    COUNT(g_misses);
    void *obj = ((intern_fn)(FN_INTERN + g_slide))(s, -1);  // +1 reference, for the caller
    char *copy = malloc(n + 1);
    void *evicted = NULL;
    if (obj && copy) {
        memcpy(copy, s, n + 1);
        m = path_lock();
        evicted = g_cache[k].obj;
        free(g_cache[k].s);
        g_cache[k].s = copy;
        g_cache[k].n = n;
        g_cache[k].obj = obj;
        (*(int *)((char *)obj + OBJ_REFS))++;  // the cache's own reference
        path_unlock(m);
    } else {
        free(copy);
    }
    if (evicted) ((release_fn)(FN_RELEASE + g_slide))(evicted);
    return *out = obj;
}

static void hook_path_md5_install(void) {
    // movabs $hook, %rax; jmp *%rax
    uint8_t code[12] = {0x48, 0xB8};
    uint64_t target = (uint64_t)(uintptr_t)&hook_path_md5;
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    int r = write_code(FN_PATH_MD5 + g_slide, code, sizeof code);
    logline("hooked path MD5 at 0x%llx: %s", FN_PATH_MD5, r ? "FAILED" : "ok");
}

static void *stats(void *arg) {
    (void)arg;
    for (int i = 0; i < 30; i++) {
        sleep(10);
        logline("path MD5 cache: %lu hits, %lu misses; packages: %lu in %lu batched frames", g_hits,
                g_misses, g_installed, g_frames);
    }
    return NULL;
}

// ---------------------------------------------------------------- packages per frame

// 0x100a0d7e0 (once per frame, from 0x100a0d742): while flag 0xa is set, takes one path off the
// pending-package list and installs it (InstallContentPackage); with the list empty it clears
// the flag and marks enumeration done. The game thus mounts one DLC archive per frame. The call
// is redirected here, to repeat that step until the list is empty or the frame budget is used.
#define FN_INSTALL_STEP 0x100a0d7e0ULL
#define CALL_INSTALL_STEP 0x100a0d742ULL
#define INSTALLING_FLAG 0xa
static const char CALL_INSTALL_STEP_ORIG[] = "e899000000";
static double g_budget = 0.050;  // seconds per frame; file budget_ms overrides

typedef void (*step_fn)(void *);

static void batch_install_step(void *self) {
    step_fn step = (step_fn)(FN_INSTALL_STEP + g_slide);
    if (!*((volatile uint8_t *)self + INSTALLING_FLAG)) {
        step(self);
        return;
    }
    COUNT(g_frames);
    double t0 = elapsed();
    do {
        step(self);
        COUNT(g_installed);
    } while (*((volatile uint8_t *)self + INSTALLING_FLAG) && elapsed() - t0 < g_budget);
}

static void batch_install_step_install(void) {
    uint64_t at = CALL_INSTALL_STEP + g_slide;
    int64_t rel = (int64_t)(uintptr_t)&batch_install_step - (int64_t)(at + 5);
    if (rel != (int32_t)rel) {
        logline("packages per frame: library too far from the game for a near call; not patching");
        return;
    }
    uint8_t code[5] = {0xE8};
    int32_t r32 = (int32_t)rel;
    memcpy(code + 1, &r32, 4);
    int r = write_code(at, code, sizeof code);
    logline("packages per frame: call at 0x%llx redirected (budget %.0f ms): %s", CALL_INSTALL_STEP,
            g_budget * 1000, r ? "FAILED" : "ok");
}

// ---------------------------------------------------------------- keyboard tables

// Two key tables that the game fills at startup lack entries a Mac keyboard needs:
// - 0x101909f20: macOS key code -> Scaleform key code (menus). kVK_Home (0x73) has Home, but
//   kVK_End (0x77) is missing, so End does nothing in the song list.
// - 0x101921c60: typed character -> game KEY_* (the keyboard the search box reads). It holds
//   only 0-9, A-Z and a-z, so a typed space never reaches the search text.
// Each is added once the game has filled its table (checked by a known entry), and re-added
// should the table be rebuilt.
typedef struct {
    const char *name;
    uint64_t table, index;
    uint32_t value;
    uint64_t check_index;  // an entry the game sets when it fills the table...
    uint32_t check_value;  // ...and its value
} key_fix;

static const key_fix KEY_FIXES[] = {
    {"End key (menus)", 0x101909f20, 0x77, 0x23, 0x73, 0x24},
    {"space in typed text", 0x101921c60, ' ', 0x39, 'a', 0x1e},
};
#define NKEYFIXES (sizeof KEY_FIXES / sizeof KEY_FIXES[0])

static void *keys(void *arg) {
    (void)arg;
    int noted[NKEYFIXES] = {0};
    for (;;) {
        for (size_t i = 0; i < NKEYFIXES; i++) {
            const key_fix *f = &KEY_FIXES[i];
            volatile uint32_t *t = (volatile uint32_t *)(f->table + g_slide);
            if (t[f->check_index] != f->check_value || t[f->index] == f->value) continue;
            if (t[f->index] != 0) {  // something else is there: leave it
                if (!noted[i]) logline("keys: %s: entry holds 0x%x; leaving it", f->name, t[f->index]);
                noted[i] = 1;
                continue;
            }
            t[f->index] = f->value;
            logline("keys: %s added", f->name);
        }
        usleep(100000);
    }
    return NULL;
}

static void *worker(void *arg) {
    (void)arg;
    // Wait for the start routine to decrypt the code (it runs right after library init).
    const site md5 = {"path MD5", FN_PATH_MD5, PATH_MD5_ORIG, ""};
    const site step = {"install step call", CALL_INSTALL_STEP, CALL_INSTALL_STEP_ORIG, ""};
    int ready = 0;
    for (int i = 0; i < 300000 && !ready; i++) {  // up to ~60 s
        ready = matches(&md5) && matches(&step);
        for (size_t k = 0; k < NSITES; k++) ready &= matches(&SITES[k]);
        if (!ready) usleep(200);
    }
    if (!ready) {
        logline("expected code not found (other game version?); not patching");
        return NULL;
    }
    // The path table and its lock are created later; the hook only runs once the game uses them.
    hook_path_md5_install();
    batch_install_step_install();
    pthread_t t;
    pthread_create(&t, NULL, stats, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, keys, NULL);
    pthread_detach(t);
    void **table = (void **)(TABLE_PTR + g_slide);
    pthread_mutex_t **lock = (pthread_mutex_t **)(LOCK_PTR + g_slide);
    logline("code decrypted; table %p lock %p", *table, (void *)*lock);
    if (!*lock) {
        // Table not created yet, so no number exists yet: patch before the first insert.
        patch_all();
        return NULL;
    }
    pthread_mutex_lock(*lock);
    uint64_t count = *table ? *(uint64_t *)((char *)*table + 0x20) : 0;
    if (count == 0) patch_all();
    else logline("table already holds %llu numbers; not patching", count);
    pthread_mutex_unlock(*lock);
    return NULL;
}

// The addresses in this file belong to one build: check its segment layout before touching any.
static int known_image(const struct mach_header *h) {
    static const struct { const char *name; uint64_t vmaddr, vmsize; } want[] = {
        {"__TEXT", 0x100000000, 0x17e7000},
        {"__DATA", 0x1017e7000, 0},  // size checked below: must cover the tables we use
    };
    if (!h || h->magic != MH_MAGIC_64) return 0;
    const struct mach_header_64 *h64 = (const struct mach_header_64 *)h;
    const uint8_t *p = (const uint8_t *)(h64 + 1);
    int found = 0;
    for (uint32_t i = 0; i < h64->ncmds; i++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *sc = (const void *)p;
        if (sc->cmd != LC_SEGMENT_64) continue;
        for (int k = 0; k < 2; k++) {
            if (strcmp(sc->segname, want[k].name) || sc->vmaddr != want[k].vmaddr) continue;
            if (k == 0 && sc->vmsize != want[k].vmsize) return 0;
            if (k == 1 && sc->vmaddr + sc->vmsize < 0x101949598) return 0;
            found |= 1 << k;
        }
    }
    return found == 3;
}

__attribute__((constructor)) static void init(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    char dir[1024], path[1100];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(dir, sizeof dir, "%s/Library/Application Support/rs-mac-fixes", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/rs-mac-fixes.log", dir);
    g_log = fopen(path, "a");
    if (g_log) setvbuf(g_log, NULL, _IONBF, 0);
    snprintf(path, sizeof path, "%s/disabled", dir);
    if (access(path, F_OK) == 0) {
        logline("disabled (%s exists); not patching", path);
        return;
    }
    snprintf(path, sizeof path, "%s/budget_ms", dir);
    FILE *bf = fopen(path, "r");
    if (bf) {
        double ms;
        if (fscanf(bf, "%lf", &ms) == 1 && ms >= 0 && ms <= 1000) g_budget = ms / 1000;
        fclose(bf);
    }
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t old = rl.rlim_cur, want = 10240;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
        if (want > old) {
            rl.rlim_cur = want;
            int r = setrlimit(RLIMIT_NOFILE, &rl);
            getrlimit(RLIMIT_NOFILE, &rl);
            logline("open-file limit %llu -> %llu%s", (unsigned long long)old, (unsigned long long)rl.rlim_cur,
                    r ? " (setrlimit failed)" : "");
        }
    }
    // Image 0 is the main executable; the addresses above only mean something in the game.
    const char *exe = _dyld_get_image_name(0);
    size_t el = exe ? strlen(exe) : 0;
    if (el < 14 || strcmp(exe + el - 14, "/Rocksmith2014")) {
        logline("not the game (%s); not patching", exe ? exe : "?");
        return;
    }
    const struct mach_header *h = _dyld_get_image_header(0);
    if (!known_image(h)) {
        logline("unknown Rocksmith2014 build (segment layout differs); not patching");
        return;
    }
    g_slide = (intptr_t)h - (intptr_t)0x100000000;
    logline("loaded; image at %p", (void *)h);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_detach(t);
}
