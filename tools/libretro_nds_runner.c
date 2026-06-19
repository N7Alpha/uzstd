/*
 * Minimal libretro runner for the uzstd Nintendo DS benchmark ROM.
 *
 * This intentionally forward-declares only the libretro API surface used here.
 * Video, audio, and input callbacks are wired because cores require them, but
 * their output is ignored. The benchmark result is read from DS system RAM.
 */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3u
#define RETRO_ENVIRONMENT_SET_MESSAGE 6u
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9u
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10u
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS 11u
#define RETRO_ENVIRONMENT_SET_HW_RENDER 14u
#define RETRO_ENVIRONMENT_GET_VARIABLE 15u
#define RETRO_ENVIRONMENT_SET_VARIABLES 16u
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17u
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME 18u
#define RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK 22u
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27u
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31u
#define RETRO_ENVIRONMENT_GET_LANGUAGE 39u
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52u
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS 53u
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL 54u
#define RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER 56u
#define RETRO_ENVIRONMENT_SET_MESSAGE_EXT 60u
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 67u
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL 68u

#define RETRO_LANGUAGE_ENGLISH 0u
#define RETRO_MEMORY_SYSTEM_RAM 2u

#define BENCH_MAGIC "UZNDST1"
#define BENCH_CASE_COUNT 5u

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
};

typedef bool (*retro_environment_t)(unsigned cmd, void *data);
typedef void (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);
typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_variable {
    const char *key;
    const char *value;
};

struct retro_log_callback {
    retro_log_printf_t log;
};

struct BenchResult {
    char name[16];
    uint32_t src_bytes;
    uint32_t comp_bytes;
    uint32_t compress_ticks;
    uint32_t decompress_ticks;
    uint32_t src_hash;
    uint32_t comp_hash;
    uint32_t dec_hash;
    uint32_t status;
};

struct BenchBlock {
    char magic[8];
    uint32_t version;
    uint32_t done;
    uint32_t case_count;
    uint32_t failures;
    uint32_t result_bytes;
    struct BenchResult results[BENCH_CASE_COUNT];
};

typedef void (*retro_set_environment_fn)(retro_environment_t cb);
typedef void (*retro_set_video_refresh_fn)(retro_video_refresh_t cb);
typedef void (*retro_set_audio_sample_fn)(retro_audio_sample_t cb);
typedef void (*retro_set_audio_sample_batch_fn)(retro_audio_sample_batch_t cb);
typedef void (*retro_set_input_poll_fn)(retro_input_poll_t cb);
typedef void (*retro_set_input_state_fn)(retro_input_state_t cb);
typedef void (*retro_init_fn)(void);
typedef void (*retro_deinit_fn)(void);
typedef void (*retro_get_system_info_fn)(struct retro_system_info *info);
typedef bool (*retro_load_game_fn)(const struct retro_game_info *game);
typedef void (*retro_unload_game_fn)(void);
typedef void (*retro_run_fn)(void);
typedef void *(*retro_get_memory_data_fn)(unsigned id);
typedef size_t (*retro_get_memory_size_fn)(unsigned id);

struct Core {
    void *handle;
    retro_set_environment_fn set_environment;
    retro_set_video_refresh_fn set_video_refresh;
    retro_set_audio_sample_fn set_audio_sample;
    retro_set_audio_sample_batch_fn set_audio_sample_batch;
    retro_set_input_poll_fn set_input_poll;
    retro_set_input_state_fn set_input_state;
    retro_init_fn init;
    retro_deinit_fn deinit;
    retro_get_system_info_fn get_system_info;
    retro_load_game_fn load_game;
    retro_unload_game_fn unload_game;
    retro_run_fn run;
    retro_get_memory_data_fn get_memory_data;
    retro_get_memory_size_fn get_memory_size;
};

static const char *g_system_dir = ".";
static const char *g_save_dir = ".";

static void log_printf(enum retro_log_level level, const char *fmt, ...) {
    static const char *levels[] = {"debug", "info", "warn", "error"};
    va_list ap;
    if (level < RETRO_LOG_ERROR)
        return;
    fprintf(stderr, "[core:%s] ", levels[(unsigned)level < 4u ? (unsigned)level : 1u]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const char *core_option_value(const char *key) {
    if (strcmp(key, "melonds_boot_mode") == 0)
        return "direct";
    if (strcmp(key, "melonds_console_mode") == 0)
        return "ds";
    if (strcmp(key, "melonds_sysfile_mode") == 0)
        return "builtin";
    if (strcmp(key, "melonds_render_mode") == 0)
        return "software";
    if (strcmp(key, "melonds_threaded_renderer") == 0)
        return "disabled";
    if (strcmp(key, "melonds_jit_enable") == 0)
        return "disabled";
    if (strcmp(key, "melonds_homebrew_sdcard") == 0)
        return "disabled";
    if (strcmp(key, "melonds_homebrew_readonly") == 0)
        return "enabled";
    return NULL;
}

static bool environment_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2u;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        const char *value = var && var->key ? core_option_value(var->key) : NULL;
        if (!value)
            return false;
        var->value = value;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        return false;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    (void)data;
    (void)width;
    (void)height;
    (void)pitch;
}

static void audio_sample_cb(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    (void)data;
    return frames;
}

static void input_poll_cb(void) {
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

static void *load_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    long len;
    void *data;

    if (!f) {
        fprintf(stderr, "open failed: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "stat failed: %s\n", path);
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)len ? (size_t)len : 1u);
    if (!data) {
        fprintf(stderr, "malloc failed for %ld bytes\n", len);
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed: %s\n", path);
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)len;
    return data;
}

static void *sym(void *handle, const char *name) {
    void *ptr = dlsym(handle, name);
    if (!ptr)
        fprintf(stderr, "missing symbol: %s\n", name);
    return ptr;
}

static bool load_core(const char *path, struct Core *core) {
    memset(core, 0, sizeof(*core));
    core->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!core->handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return false;
    }

#define LOAD(name) do { core->name = (retro_##name##_fn)sym(core->handle, "retro_" #name); } while (0)
    LOAD(set_environment);
    LOAD(set_video_refresh);
    LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch);
    LOAD(set_input_poll);
    LOAD(set_input_state);
    LOAD(init);
    LOAD(deinit);
    LOAD(get_system_info);
    LOAD(load_game);
    LOAD(unload_game);
    LOAD(run);
    LOAD(get_memory_data);
    LOAD(get_memory_size);
#undef LOAD

    return core->set_environment && core->set_video_refresh && core->set_audio_sample &&
           core->set_audio_sample_batch && core->set_input_poll && core->set_input_state &&
           core->init && core->deinit && core->get_system_info && core->load_game &&
           core->unload_game && core->run && core->get_memory_data && core->get_memory_size;
}

static const struct BenchBlock *find_bench_block(const uint8_t *ram, size_t ram_size, size_t *offset_out) {
    size_t i;
    if (ram_size < sizeof(struct BenchBlock))
        return NULL;
    for (i = 0; i <= ram_size - sizeof(struct BenchBlock); i += 4) {
        const struct BenchBlock *block = (const struct BenchBlock *)(const void *)(ram + i);
        if (memcmp(block->magic, BENCH_MAGIC, 8) == 0 &&
            block->version == 1u &&
            block->case_count == BENCH_CASE_COUNT &&
            block->result_bytes == sizeof(struct BenchBlock)) {
            *offset_out = i;
            return block;
        }
    }
    return NULL;
}

static void print_json_string(const char *s) {
    putchar('"');
    while (*s) {
        if (*s == '"' || *s == '\\')
            putchar('\\');
        putchar((unsigned char)*s);
        s++;
    }
    putchar('"');
}

static bool validate_and_print(const struct BenchBlock *block, size_t offset, unsigned frame) {
    bool ok = block->done == 1u && block->failures == 0u && block->case_count == BENCH_CASE_COUNT;
    unsigned i;

    printf("{\n");
    printf("  \"status\": %s,\n", ok ? "\"passed\"" : "\"failed\"");
    printf("  \"frame\": %u,\n", frame);
    printf("  \"ram_offset\": %zu,\n", offset);
    printf("  \"case_count\": %" PRIu32 ",\n", block->case_count);
    printf("  \"failures\": %" PRIu32 ",\n", block->failures);
    printf("  \"results\": [\n");
    for (i = 0; i < block->case_count && i < BENCH_CASE_COUNT; i++) {
        const struct BenchResult *r = &block->results[i];
        bool case_ok = r->status == 1u && r->src_hash == r->dec_hash && r->src_bytes > 0u && r->comp_bytes > 0u;
        ok = ok && case_ok;
        printf("    {\"name\": ");
        print_json_string(r->name);
        printf(", \"src_bytes\": %" PRIu32 ", \"comp_bytes\": %" PRIu32
               ", \"compress_ticks\": %" PRIu32 ", \"decompress_ticks\": %" PRIu32
               ", \"src_hash\": %" PRIu32 ", \"comp_hash\": %" PRIu32
               ", \"dec_hash\": %" PRIu32 ", \"status\": %" PRIu32
               ", \"roundtrip\": %s}%s\n",
               r->src_bytes, r->comp_bytes, r->compress_ticks, r->decompress_ticks,
               r->src_hash, r->comp_hash, r->dec_hash, r->status,
               case_ok ? "true" : "false",
               i + 1u == block->case_count ? "" : ",");
    }
    printf("  ]\n");
    printf("}\n");
    return ok;
}

int main(int argc, char **argv) {
    struct Core core;
    struct retro_system_info info;
    struct retro_game_info game;
    void *rom_data;
    size_t rom_size;
    unsigned max_frames = 3600u;
    unsigned frame;
    bool saw_block = false;
    size_t last_offset = 0;
    const struct BenchBlock *last_block = NULL;
    const char *system_dir_env;
    const char *save_dir_env;
    bool loaded = false;
    int rc = 1;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s CORE ROM [MAX_FRAMES]\n", argv[0]);
        return 2;
    }
    if (argc == 4)
        max_frames = (unsigned)strtoul(argv[3], NULL, 10);

    system_dir_env = getenv("LIBRETRO_SYSTEM_DIR");
    save_dir_env = getenv("LIBRETRO_SAVE_DIR");
    if (system_dir_env && *system_dir_env)
        g_system_dir = system_dir_env;
    if (save_dir_env && *save_dir_env)
        g_save_dir = save_dir_env;

    rom_data = load_file(argv[2], &rom_size);
    if (!rom_data)
        return 1;

    if (!load_core(argv[1], &core)) {
        free(rom_data);
        return 1;
    }

    core.set_environment(environment_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);

    memset(&info, 0, sizeof(info));
    core.get_system_info(&info);
    fprintf(stderr, "core=%s version=%s extensions=%s need_fullpath=%d\n",
            info.library_name ? info.library_name : "",
            info.library_version ? info.library_version : "",
            info.valid_extensions ? info.valid_extensions : "",
            info.need_fullpath ? 1 : 0);

    core.init();

    memset(&game, 0, sizeof(game));
    game.path = argv[2];
    game.data = rom_data;
    game.size = rom_size;
    loaded = core.load_game(&game);
    if (!loaded) {
        fprintf(stderr, "retro_load_game failed\n");
        goto done;
    }

    for (frame = 0; frame < max_frames; frame++) {
        uint8_t *ram;
        size_t ram_size;
        size_t offset = 0;
        const struct BenchBlock *block;

        core.run();

        ram = (uint8_t *)core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        ram_size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        block = find_bench_block(ram, ram_size, &offset);
        if (block) {
            saw_block = true;
            last_offset = offset;
            last_block = block;
            if (block->done == 1u) {
                rc = validate_and_print(block, offset, frame + 1u) ? 0 : 1;
                goto done;
            }
        }
    }

    fprintf(stderr, "benchmark did not finish within %u frames", max_frames);
    if (saw_block) {
        fprintf(stderr, " (saw result block at RAM offset %zu, done=0)", last_offset);
        if (last_block) {
            unsigned i;
            fprintf(stderr, "\npartial statuses:");
            for (i = 0; i < last_block->case_count && i < BENCH_CASE_COUNT; i++) {
                const struct BenchResult *r = &last_block->results[i];
                fprintf(stderr, " [%u:%s src=%" PRIu32 " comp=%" PRIu32 " status=%" PRIu32 "]",
                        i, r->name[0] ? r->name : "-", r->src_bytes, r->comp_bytes, r->status);
            }
        }
    } else {
        fprintf(stderr, " (result block magic not found)");
    }
    fprintf(stderr, "\n");

done:
    if (loaded)
        core.unload_game();
    core.deinit();
    if (core.handle)
        dlclose(core.handle);
    free(rom_data);
    return rc;
}
