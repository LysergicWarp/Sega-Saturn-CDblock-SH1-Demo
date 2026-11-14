/*
 * beetle-runner: runs a libretro Saturn core headless for a number of frames and reports the run as
 * JSON on standard output. check.py runs the demo's discs with it. --help lists the options.
 */

#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <ftw.h>
#include <unistd.h>
#endif

#include <libretro.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef BEETLE_REVISION
#define BEETLE_REVISION "unknown"
#endif

#define RUNNER_SCHEMA_VERSION 2
#define MAX_STOP_BYTES 64
#define MAX_CAPTURE_BYTES 4096
#define MAX_CAPTURES 16
#define MAX_INPUT_EVENTS 512
#define MAX_SCREENSHOTS 16
#define MAX_OPTIONS 96
#define MAX_SYSTEM_FILES 8
#define MAX_SERIALIZED_STATE_SIZE (256U * 1024U * 1024U)
#define OPTION_KEY_LENGTH 128
#define OPTION_VALUE_LENGTH 256
#define WORK_RAM_BANK_SIZE (1024U * 1024U)
#define FNV1A64_OFFSET UINT64_C(14695981039346656037)
#define FNV1A64_PRIME UINT64_C(1099511628211)

enum runner_exit_code
{
    RUNNER_EXIT_OK = 0,
    RUNNER_EXIT_STOP_NOT_REACHED = 2,
    RUNNER_EXIT_USAGE = 64,
    RUNNER_EXIT_INPUT = 65,
    RUNNER_EXIT_CORE = 70
};

enum runner_region
{
    RUNNER_REGION_NA,
    RUNNER_REGION_EU,
    RUNNER_REGION_JP
};

struct runner_config
{
    char core_path[PATH_MAX];
    char image_path[PATH_MAX];
    char bios_path[PATH_MAX];
    char system_files[MAX_SYSTEM_FILES][PATH_MAX];
    size_t system_file_count;
    uint64_t frame_limit;
    enum runner_region region;
    bool keep_state;
    bool has_serialize_path;
    char serialize_path[PATH_MAX];
    bool has_stop_address;
    bool has_stop_value;
    uint32_t stop_address;
    uint8_t stop_value[MAX_STOP_BYTES];
    size_t stop_value_length;
    size_t legacy_capture_length;
    struct runner_capture
    {
        uint32_t address;
        size_t length;
        size_t offset;
        uint8_t data[MAX_CAPTURE_BYTES];
        bool captured;
    } captures[MAX_CAPTURES];
    size_t capture_count;
    struct runner_input_event
    {
        uint64_t frame;
        unsigned port;
        unsigned id;
        int16_t value;
    } input_events[MAX_INPUT_EVENTS];
    size_t input_event_count;
    struct runner_option_override
    {
        char key[OPTION_KEY_LENGTH];
        char value[OPTION_VALUE_LENGTH];
    } option_overrides[MAX_OPTIONS];
    size_t option_override_count;
    struct runner_screenshot
    {
        uint64_t frame;
        char path[PATH_MAX];
        bool written;
        bool failed;
    } screenshots[MAX_SCREENSHOTS];
    size_t screenshot_count;
};

struct runner_option
{
    char key[OPTION_KEY_LENGTH];
    char value[OPTION_VALUE_LENGTH];
};

struct core_api
{
    void *handle;
    unsigned (*api_version)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*init)(void);
    void (*deinit)(void);
    void (*get_system_info)(struct retro_system_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
};

static struct runner_config g_config;
static struct runner_option g_options[MAX_OPTIONS];
static size_t g_option_count;
static char g_system_directory[PATH_MAX];
static char g_save_directory[PATH_MAX];
static unsigned g_last_video_width;
static unsigned g_last_video_height;
static size_t g_last_video_pitch;
static uint64_t g_video_frames;
static uint64_t g_video_bytes;
static uint64_t g_video_hash = FNV1A64_OFFSET;
static uint64_t g_audio_frames;
static uint64_t g_audio_samples;
static uint64_t g_audio_hash = FNV1A64_OFFSET;
static uint64_t g_input_polls;
static uint64_t g_input_state_calls;
static uint64_t g_current_frame;

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: beetle-runner --core PATH --image PATH --bios PATH [options]\n"
            "Options:\n"
            "  --frames COUNT          Maximum frames to execute, default 600\n"
            "  --region na|eu|jp       Fixed Saturn region, default na\n"
            "  --stop-address ADDRESS  Saturn Work RAM address to watch\n"
            "  --stop-hex HEX          Bytes that must appear at the stop address\n"
            "  --stop-ascii TEXT       ASCII bytes that must appear at the stop address\n"
            "  --capture ADDRESS:LENGTH\n"
            "                          Guest-order Work RAM range, repeatable, maximum 16 ranges\n"
            "  --capture-length COUNT  Legacy capture beginning at the stop address\n"
            "  --input FRAME:PORT:ID:VALUE\n"
            "                          Persistent digital joypad state change, repeatable\n"
            "  --core-option KEY=VALUE Override one resolved libretro core option, repeatable\n"
            "  --system-file PATH      Copy a file into the isolated system directory, repeatable\n"
            "  --screenshot FRAME:PATH Write a one-based video frame as a 32-bit BMP, repeatable\n"
            "  --serialize-out PATH    Write libretro serialized state after the final frame\n"
            "  --keep-state            Preserve the isolated system and save directory\n"
            "  --help                  Show this help\n");
}

static bool copy_text(char *destination, size_t destination_size, const char *source)
{
    size_t length = strlen(source);
    if (length >= destination_size)
        return false;

    memcpy(destination, source, length + 1);
    return true;
}

static bool parse_uint64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return false;

    *value = (uint64_t)parsed;
    return true;
}

static bool parse_uint32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (!parse_uint64(text, &parsed) || parsed > UINT32_MAX)
        return false;

    *value = (uint32_t)parsed;
    return true;
}

static bool parse_capture_range(const char *text, uint32_t *address, size_t *length)
{
    const char *separator = strchr(text, ':');
    char address_text[32];
    uint64_t parsed_length;
    size_t address_length;

    if (separator == NULL || strchr(separator + 1, ':') != NULL)
        return false;

    address_length = (size_t)(separator - text);
    if (address_length == 0 || address_length >= sizeof(address_text))
        return false;

    memcpy(address_text, text, address_length);
    address_text[address_length] = '\0';
    if (!parse_uint32(address_text, address) ||
        !parse_uint64(separator + 1, &parsed_length) ||
        parsed_length == 0 || parsed_length > MAX_CAPTURE_BYTES)
        return false;

    *length = (size_t)parsed_length;
    return true;
}

static bool parse_input_event(const char *text, struct runner_input_event *input_event)
{
    uint64_t frame;
    unsigned port;
    unsigned id;
    int value;
    char trailing;

    if (sscanf(text, "%" SCNu64 ":%u:%u:%d%c", &frame, &port, &id, &value, &trailing) != 4 ||
        port > 1 || id > RETRO_DEVICE_ID_JOYPAD_R3 || value < 0 || value > 1)
        return false;

    input_event->frame = frame;
    input_event->port = port;
    input_event->id = id;
    input_event->value = (int16_t)value;
    return true;
}

static bool parse_core_option(const char *text, struct runner_option_override *option_override)
{
    const char *separator = strchr(text, '=');
    size_t key_length;

    if (separator == NULL || separator == text || separator[1] == '\0')
        return false;

    key_length = (size_t)(separator - text);
    if (key_length >= sizeof(option_override->key) ||
        strlen(separator + 1) >= sizeof(option_override->value))
        return false;

    memcpy(option_override->key, text, key_length);
    option_override->key[key_length] = '\0';
    return copy_text(
        option_override->value,
        sizeof(option_override->value),
        separator + 1);
}

static bool parse_screenshot(const char *text, struct runner_screenshot *screenshot)
{
    const char *separator = strchr(text, ':');
    char frame_text[32];
    size_t frame_length;

    if (separator == NULL || separator == text || separator[1] == '\0')
        return false;

    frame_length = (size_t)(separator - text);
    if (frame_length >= sizeof(frame_text))
        return false;

    memcpy(frame_text, text, frame_length);
    frame_text[frame_length] = '\0';
    return parse_uint64(frame_text, &screenshot->frame) &&
           screenshot->frame > 0 &&
           copy_text(screenshot->path, sizeof(screenshot->path), separator + 1);
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static bool parse_hex_bytes(const char *text, uint8_t *bytes, size_t *length)
{
    size_t text_length = strlen(text);
    size_t index;

    if (text_length == 0 || (text_length % 2) != 0 || text_length > MAX_STOP_BYTES * 2)
        return false;

    for (index = 0; index < text_length; index += 2)
    {
        int high = hex_digit(text[index]);
        int low = hex_digit(text[index + 1]);
        if (high < 0 || low < 0)
            return false;

        bytes[index / 2] = (uint8_t)((high << 4) | low);
    }

    *length = text_length / 2;
    return true;
}

static bool next_argument(int argc, char **argv, int *index, const char **value)
{
    if (*index + 1 >= argc)
        return false;

    *index += 1;
    *value = argv[*index];
    return true;
}

static int parse_arguments(int argc, char **argv)
{
    int index;
    bool has_core = false;
    bool has_image = false;
    bool has_bios = false;

    memset(&g_config, 0, sizeof(g_config));
    g_config.frame_limit = 600;
    g_config.region = RUNNER_REGION_NA;

    for (index = 1; index < argc; index++)
    {
        const char *argument = argv[index];
        const char *value = NULL;

        if (strcmp(argument, "--help") == 0)
        {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(argument, "--keep-state") == 0)
        {
            g_config.keep_state = true;
            continue;
        }
        if (strcmp(argument, "--core") == 0)
        {
            if (!next_argument(argc, argv, &index, &value) ||
                !copy_text(g_config.core_path, sizeof(g_config.core_path), value))
                return -1;
            has_core = true;
            continue;
        }
        if (strcmp(argument, "--image") == 0)
        {
            if (!next_argument(argc, argv, &index, &value) ||
                !copy_text(g_config.image_path, sizeof(g_config.image_path), value))
                return -1;
            has_image = true;
            continue;
        }
        if (strcmp(argument, "--bios") == 0)
        {
            if (!next_argument(argc, argv, &index, &value) ||
                !copy_text(g_config.bios_path, sizeof(g_config.bios_path), value))
                return -1;
            has_bios = true;
            continue;
        }
        if (strcmp(argument, "--frames") == 0)
        {
            if (!next_argument(argc, argv, &index, &value) ||
                !parse_uint64(value, &g_config.frame_limit) || g_config.frame_limit == 0)
                return -1;
            continue;
        }
        if (strcmp(argument, "--region") == 0)
        {
            if (!next_argument(argc, argv, &index, &value))
                return -1;
            if (strcmp(value, "na") == 0)
                g_config.region = RUNNER_REGION_NA;
            else if (strcmp(value, "eu") == 0)
                g_config.region = RUNNER_REGION_EU;
            else if (strcmp(value, "jp") == 0)
                g_config.region = RUNNER_REGION_JP;
            else
                return -1;
            continue;
        }
        if (strcmp(argument, "--stop-address") == 0)
        {
            if (!next_argument(argc, argv, &index, &value) ||
                !parse_uint32(value, &g_config.stop_address))
                return -1;
            g_config.has_stop_address = true;
            continue;
        }
        if (strcmp(argument, "--stop-hex") == 0)
        {
            if (g_config.has_stop_value || !next_argument(argc, argv, &index, &value) ||
                !parse_hex_bytes(value, g_config.stop_value, &g_config.stop_value_length))
                return -1;
            g_config.has_stop_value = true;
            continue;
        }
        if (strcmp(argument, "--stop-ascii") == 0)
        {
            size_t length;
            if (g_config.has_stop_value || !next_argument(argc, argv, &index, &value))
                return -1;
            length = strlen(value);
            if (length == 0 || length > MAX_STOP_BYTES)
                return -1;
            memcpy(g_config.stop_value, value, length);
            g_config.stop_value_length = length;
            g_config.has_stop_value = true;
            continue;
        }
        if (strcmp(argument, "--capture-length") == 0)
        {
            uint64_t length;
            if (!next_argument(argc, argv, &index, &value) ||
                !parse_uint64(value, &length) || length == 0 || length > MAX_CAPTURE_BYTES)
                return -1;
            g_config.legacy_capture_length = (size_t)length;
            continue;
        }
        if (strcmp(argument, "--capture") == 0)
        {
            struct runner_capture *capture;
            if (g_config.capture_count >= MAX_CAPTURES ||
                !next_argument(argc, argv, &index, &value))
                return -1;

            capture = &g_config.captures[g_config.capture_count];
            if (!parse_capture_range(value, &capture->address, &capture->length))
                return -1;
            g_config.capture_count++;
            continue;
        }
        if (strcmp(argument, "--input") == 0)
        {
            struct runner_input_event *input_event;
            if (g_config.input_event_count >= MAX_INPUT_EVENTS ||
                !next_argument(argc, argv, &index, &value))
                return -1;

            input_event = &g_config.input_events[g_config.input_event_count];
            if (!parse_input_event(value, input_event))
                return -1;
            g_config.input_event_count++;
            continue;
        }
        if (strcmp(argument, "--screenshot") == 0)
        {
            struct runner_screenshot *screenshot;
            if (g_config.screenshot_count >= MAX_SCREENSHOTS ||
                !next_argument(argc, argv, &index, &value))
                return -1;

            screenshot = &g_config.screenshots[g_config.screenshot_count];
            if (!parse_screenshot(value, screenshot))
                return -1;
            g_config.screenshot_count++;
            continue;
        }
        if (strcmp(argument, "--serialize-out") == 0)
        {
            if (g_config.has_serialize_path ||
                !next_argument(argc, argv, &index, &value) ||
                !copy_text(g_config.serialize_path, sizeof(g_config.serialize_path), value))
                return -1;
            g_config.has_serialize_path = true;
            continue;
        }
        if (strcmp(argument, "--core-option") == 0)
        {
            struct runner_option_override *option_override;
            if (g_config.option_override_count >= MAX_OPTIONS ||
                !next_argument(argc, argv, &index, &value))
                return -1;

            option_override = &g_config.option_overrides[g_config.option_override_count];
            if (!parse_core_option(value, option_override))
                return -1;
            g_config.option_override_count++;
            continue;
        }
        if (strcmp(argument, "--system-file") == 0)
        {
            if (g_config.system_file_count >= MAX_SYSTEM_FILES ||
                !next_argument(argc, argv, &index, &value) ||
                !copy_text(g_config.system_files[g_config.system_file_count],
                           sizeof(g_config.system_files[g_config.system_file_count]), value))
                return -1;
            g_config.system_file_count++;
            continue;
        }
        return -1;
    }

    if (!has_core || !has_image || !has_bios ||
        g_config.has_stop_address != g_config.has_stop_value ||
        (g_config.legacy_capture_length > 0 && !g_config.has_stop_address) ||
        (g_config.legacy_capture_length > 0 && g_config.capture_count >= MAX_CAPTURES))
        return -1;

    if (g_config.legacy_capture_length > 0)
    {
        struct runner_capture *capture = &g_config.captures[g_config.capture_count++];
        capture->address = g_config.stop_address;
        capture->length = g_config.legacy_capture_length;
    }

    for (size_t screenshot_index = 0;
         screenshot_index < g_config.screenshot_count;
         screenshot_index++)
    {
        if (g_config.screenshots[screenshot_index].frame > g_config.frame_limit)
            return -1;
    }

    return 0;
}

static bool resolve_file_path(char *path, size_t path_size)
{
    char resolved[PATH_MAX];
    struct stat info;

#ifdef _WIN32
    DWORD resolved_length = GetFullPathNameA(path, (DWORD)sizeof(resolved), resolved, NULL);
    DWORD attributes;
    if (resolved_length == 0 || resolved_length >= sizeof(resolved))
        return false;

    attributes = GetFileAttributesA(resolved);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        stat(resolved, &info) != 0)
        return false;
#else
    if (realpath(path, resolved) == NULL || stat(resolved, &info) != 0 || !S_ISREG(info.st_mode))
        return false;
#endif

    return copy_text(path, path_size, resolved);
}

static bool validate_inputs(char *message, size_t message_size)
{
    struct stat bios_info;

    if (!resolve_file_path(g_config.core_path, sizeof(g_config.core_path)))
    {
        snprintf(message, message_size, "Core file does not exist or is not a regular file.");
        return false;
    }
    if (!resolve_file_path(g_config.image_path, sizeof(g_config.image_path)))
    {
        snprintf(message, message_size, "Image file does not exist or is not a regular file.");
        return false;
    }
    if (!resolve_file_path(g_config.bios_path, sizeof(g_config.bios_path)) ||
        stat(g_config.bios_path, &bios_info) != 0)
    {
        snprintf(message, message_size, "BIOS file does not exist or is not a regular file.");
        return false;
    }
    if (bios_info.st_size != 524288)
    {
        snprintf(message, message_size, "Saturn BIOS must be exactly 524288 bytes.");
        return false;
    }

    for (size_t file_index = 0; file_index < g_config.system_file_count; file_index++)
    {
        if (!resolve_file_path(g_config.system_files[file_index],
                               sizeof(g_config.system_files[file_index])))
        {
            snprintf(message, message_size,
                     "System file %zu does not exist or is not a regular file.", file_index + 1);
            return false;
        }
    }

    return true;
}

static bool copy_file(const char *source_path, const char *destination_path)
{
    FILE *source = fopen(source_path, "rb");
    FILE *destination = NULL;
    uint8_t buffer[16384];
    bool success = false;

    if (source == NULL)
        return false;

    destination = fopen(destination_path, "wb");
    if (destination == NULL)
        goto cleanup;

    while (!feof(source))
    {
        size_t read_count = fread(buffer, 1, sizeof(buffer), source);
        if (read_count > 0 && fwrite(buffer, 1, read_count, destination) != read_count)
            goto cleanup;
        if (ferror(source))
            goto cleanup;
    }

    success = fflush(destination) == 0;

cleanup:
    if (destination != NULL && fclose(destination) != 0)
        success = false;
    fclose(source);
    return success;
}

static bool write_smpc_seed(const char *state_directory)
{
    static const uint8_t seed[] = {
        0x01,
        0x20, 0x26, 0x67, 0x04, 0x21, 0x45, 0x28,
        0x00, 0x00, 0x00, 0x00
    };
    const char *forward_separator = strrchr(g_config.image_path, '/');
    const char *backward_separator = strrchr(g_config.image_path, '\\');
    const char *base_name;
    char image_stem[256];
    char seed_path[PATH_MAX];
    char *extension;
    FILE *seed_file;
    bool success;

    base_name = forward_separator;
    if (backward_separator != NULL &&
        (base_name == NULL || backward_separator > base_name))
        base_name = backward_separator;
    base_name = base_name == NULL ? g_config.image_path : base_name + 1;
    if (!copy_text(image_stem, sizeof(image_stem), base_name))
        return false;

    extension = strrchr(image_stem, '.');
    if (extension != NULL)
        *extension = '\0';
    if (image_stem[0] == '\0' ||
        snprintf(seed_path, sizeof(seed_path), "%s/save/%s.smpc", state_directory, image_stem) >=
            (int)sizeof(seed_path))
        return false;

    seed_file = fopen(seed_path, "wb");
    if (seed_file == NULL)
        return false;
    success = fwrite(seed, 1, sizeof(seed), seed_file) == sizeof(seed);
    if (fclose(seed_file) != 0)
        success = false;

    return success;
}

static bool create_state_directory(char *state_directory,
                                   size_t state_directory_size,
                                   char *message,
                                   size_t message_size)
{
#ifdef _WIN32
    char temporary_root[PATH_MAX];
    char temporary_file[PATH_MAX];
#else
    char template_path[] = "/tmp/beetle-runner-XXXXXX";
#endif
    char bios_destination[PATH_MAX];
    char system_destination[PATH_MAX];
    const char *bios_name = g_config.region == RUNNER_REGION_JP ? "sega_101.bin" : "mpr-17933.bin";

#ifdef _WIN32
    DWORD temporary_root_length = GetTempPathA((DWORD)sizeof(temporary_root), temporary_root);
    if (temporary_root_length == 0 ||
        temporary_root_length >= sizeof(temporary_root) ||
        GetTempFileNameA(temporary_root, "btl", 0, temporary_file) == 0 ||
        !DeleteFileA(temporary_file) ||
        !CreateDirectoryA(temporary_file, NULL) ||
        !copy_text(state_directory, state_directory_size, temporary_file))
    {
        snprintf(message,
                 message_size,
                 "Could not create isolated state directory: Win32 error %lu",
                 (unsigned long)GetLastError());
        return false;
    }
#else
    if (mkdtemp(template_path) == NULL ||
        !copy_text(state_directory, state_directory_size, template_path))
    {
        snprintf(message, message_size, "Could not create isolated state directory: %s", strerror(errno));
        return false;
    }
#endif

    if (snprintf(g_system_directory, sizeof(g_system_directory), "%s/system", state_directory) >=
            (int)sizeof(g_system_directory) ||
        snprintf(g_save_directory, sizeof(g_save_directory), "%s/save", state_directory) >=
            (int)sizeof(g_save_directory) ||
#ifdef _WIN32
        _mkdir(g_system_directory) != 0 || _mkdir(g_save_directory) != 0)
#else
        mkdir(g_system_directory, 0700) != 0 || mkdir(g_save_directory, 0700) != 0)
#endif
    {
        snprintf(message, message_size, "Could not create isolated system and save directories: %s", strerror(errno));
        return false;
    }

    if (snprintf(bios_destination, sizeof(bios_destination), "%s/%s", g_system_directory, bios_name) >=
        (int)sizeof(bios_destination))
    {
        snprintf(message, message_size, "BIOS destination path is too long.");
        return false;
    }

    if (!copy_file(g_config.bios_path, bios_destination))
    {
        snprintf(message, message_size, "Could not copy BIOS into isolated system directory.");
        return false;
    }

    for (size_t file_index = 0; file_index < g_config.system_file_count; file_index++)
    {
        const char *source_path = g_config.system_files[file_index];
        const char *forward_separator = strrchr(source_path, '/');
        const char *backward_separator = strrchr(source_path, '\\');
        const char *base_name = source_path;

        if (forward_separator != NULL)
            base_name = forward_separator + 1;
        if (backward_separator != NULL && backward_separator + 1 > base_name)
            base_name = backward_separator + 1;

        if (*base_name == '\0')
        {
            snprintf(message, message_size, "System file %zu has no file name.", file_index + 1);
            return false;
        }
        if (snprintf(system_destination, sizeof(system_destination), "%s/%s",
                     g_system_directory, base_name) >= (int)sizeof(system_destination))
        {
            snprintf(message, message_size, "System file destination path is too long.");
            return false;
        }
        if (!copy_file(source_path, system_destination))
        {
            snprintf(message, message_size,
                     "Could not copy system file %zu into isolated system directory.", file_index + 1);
            return false;
        }
    }

    if (!write_smpc_seed(state_directory))
    {
        snprintf(message, message_size, "Could not create deterministic SMPC seed.");
        return false;
    }

    return true;
}

#ifdef _WIN32
static bool remove_state_directory_entry(const char *path)
{
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[PATH_MAX];
    char child[PATH_MAX];
    bool success = true;

    if (snprintf(pattern, sizeof(pattern), "%s/*", path) >= (int)sizeof(pattern))
        return false;

    search = FindFirstFileA(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0)
                continue;
            if (snprintf(child, sizeof(child), "%s/%s", path, entry.cFileName) >=
                (int)sizeof(child))
            {
                success = false;
                continue;
            }

            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                success = remove_state_directory_entry(child) && success;
            else
                success = DeleteFileA(child) != 0 && success;
        } while (FindNextFileA(search, &entry));
        FindClose(search);
    }

    return RemoveDirectoryA(path) != 0 && success;
}
#else
static int remove_tree_entry(const char *path,
                             const struct stat *info,
                             int type,
                             struct FTW *tree)
{
    (void)info;
    (void)type;
    (void)tree;
    return remove(path);
}
#endif

static void remove_state_directory(const char *state_directory)
{
    if (state_directory[0] != '\0')
#ifdef _WIN32
        (void)remove_state_directory_entry(state_directory);
#else
        (void)nftw(state_directory, remove_tree_entry, 32, FTW_DEPTH | FTW_PHYS);
#endif
}

static const char *region_option_value(void)
{
    switch (g_config.region)
    {
        case RUNNER_REGION_EU:
            return "Europe";
        case RUNNER_REGION_JP:
            return "Japan";
        case RUNNER_REGION_NA:
        default:
            return "North America";
    }
}

static const char *option_override(const char *key)
{
    size_t index;
    for (index = 0; index < g_config.option_override_count; index++)
    {
        if (strcmp(key, g_config.option_overrides[index].key) == 0)
            return g_config.option_overrides[index].value;
    }

    if (strcmp(key, "beetle_saturn_region") == 0)
        return region_option_value();
    if (strcmp(key, "beetle_saturn_autortc") == 0)
        return "disabled";
    if (strcmp(key, "beetle_saturn_autortc_lang") == 0)
        return "english";
    if (strcmp(key, "beetle_saturn_cdimagecache") == 0)
        return "disabled";
    if (strcmp(key, "beetle_saturn_shared_int") == 0)
        return "disabled";
    if (strcmp(key, "beetle_saturn_shared_ext") == 0)
        return "disabled";
    if (strcmp(key, "beetle_saturn_save_method") == 0)
        return "libretro";
    if (strcmp(key, "beetle_saturn_cart") == 0)
        return "None";
    return NULL;
}

static void register_legacy_options(const struct retro_variable *variables)
{
    const struct retro_variable *variable = variables;
    g_option_count = 0;

    while (variable != NULL && variable->key != NULL && g_option_count < MAX_OPTIONS)
    {
        const char *separator = variable->value == NULL ? NULL : strstr(variable->value, "; ");
        const char *default_value = separator == NULL ? "" : separator + 2;
        const char *end = strchr(default_value, '|');
        size_t value_length = end == NULL ? strlen(default_value) : (size_t)(end - default_value);

        if (strlen(variable->key) < OPTION_KEY_LENGTH && value_length < OPTION_VALUE_LENGTH)
        {
            struct runner_option *option = &g_options[g_option_count++];
            copy_text(option->key, sizeof(option->key), variable->key);
            memcpy(option->value, default_value, value_length);
            option->value[value_length] = '\0';
        }

        variable++;
    }
}

static const char *get_option_value(const char *key)
{
    size_t index;
    const char *override = option_override(key);
    if (override != NULL)
        return override;

    for (index = 0; index < g_option_count; index++)
    {
        if (strcmp(g_options[index].key, key) == 0)
            return g_options[index].value;
    }

    return NULL;
}

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *format, ...)
{
    va_list arguments;
    const char *label = "info";

    if (level == RETRO_LOG_DEBUG)
        label = "debug";
    else if (level == RETRO_LOG_WARN)
        label = "warn";
    else if (level == RETRO_LOG_ERROR)
        label = "error";

    fprintf(stderr, "[beetle:%s] ", label);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

static bool RETRO_CALLCONV environment_callback(unsigned command, void *data)
{
    switch (command)
    {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = g_system_directory;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = g_save_directory;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = core_log;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            struct retro_variable *variable = data;
            variable->value = get_option_value(variable->key);
            return variable->value != NULL;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            register_legacy_options((const struct retro_variable *)data);
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned *)data = 0;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *(int *)data = 3;
            return true;
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
            *(unsigned *)data = 1;
            return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
            return false;
        default:
            return false;
    }
}

static void hash_bytes(uint64_t *hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t index;
    for (index = 0; index < length; index++)
    {
        *hash ^= bytes[index];
        *hash *= FNV1A64_PRIME;
    }
}

static bool write_little_endian_u16(FILE *stream, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)(value >> 8)
    };
    return fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes);
}

static bool write_little_endian_u32(FILE *stream, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)(value >> 24)
    };
    return fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes);
}

static bool write_bmp_frame(const void *data,
                            unsigned width,
                            unsigned height,
                            size_t pitch,
                            const char *path)
{
    FILE *stream = NULL;
    size_t row_bytes;
    uint64_t pixel_bytes;
    unsigned row;
    bool success = false;

    if (data == NULL || width == 0 || height == 0 ||
        width > INT32_MAX || height > INT32_MAX)
        return false;

    row_bytes = (size_t)width * 4U;
    pixel_bytes = (uint64_t)row_bytes * height;
    if (row_bytes > pitch || pixel_bytes > UINT32_MAX - 54U)
        return false;

    stream = fopen(path, "wb");
    if (stream == NULL)
        return false;

    success = fwrite("BM", 1, 2, stream) == 2 &&
              write_little_endian_u32(stream, (uint32_t)pixel_bytes + 54U) &&
              write_little_endian_u16(stream, 0) &&
              write_little_endian_u16(stream, 0) &&
              write_little_endian_u32(stream, 54U) &&
              write_little_endian_u32(stream, 40U) &&
              write_little_endian_u32(stream, width) &&
              write_little_endian_u32(stream, (uint32_t)(-(int32_t)height)) &&
              write_little_endian_u16(stream, 1U) &&
              write_little_endian_u16(stream, 32U) &&
              write_little_endian_u32(stream, 0U) &&
              write_little_endian_u32(stream, (uint32_t)pixel_bytes) &&
              write_little_endian_u32(stream, 2835U) &&
              write_little_endian_u32(stream, 2835U) &&
              write_little_endian_u32(stream, 0U) &&
              write_little_endian_u32(stream, 0U);

    for (row = 0; success && row < height; row++)
    {
        const uint8_t *source = (const uint8_t *)data + (size_t)row * pitch;
        unsigned column;
        for (column = 0; column < width; column++)
        {
            uint32_t pixel;
            const uint8_t bgrx[4] = {0, 0, 0, 0};
            uint8_t output[4];
            memcpy(&pixel, source + (size_t)column * 4U, sizeof(pixel));
            memcpy(output, bgrx, sizeof(output));
            output[0] = (uint8_t)(pixel & 0xFFU);
            output[1] = (uint8_t)((pixel >> 8) & 0xFFU);
            output[2] = (uint8_t)((pixel >> 16) & 0xFFU);
            if (fwrite(output, 1, sizeof(output), stream) != sizeof(output))
            {
                success = false;
                break;
            }
        }
    }

    if (fflush(stream) != 0)
        success = false;
    if (fclose(stream) != 0)
        success = false;
    if (!success)
        remove(path);
    return success;
}

static void RETRO_CALLCONV video_callback(const void *data,
                                          unsigned width,
                                          unsigned height,
                                          size_t pitch)
{
    size_t row_length;
    unsigned row;
    g_video_frames++;
    g_last_video_width = width;
    g_last_video_height = height;
    g_last_video_pitch = pitch;
    if (data == NULL || height == 0)
        return;
    row_length = (size_t)width * 4U;
    if (row_length > pitch)
        return;

    for (row = 0; row < height; row++)
        hash_bytes(&g_video_hash, (const uint8_t *)data + row * pitch, row_length);
    g_video_bytes += (uint64_t)row_length * height;

    for (size_t screenshot_index = 0;
         screenshot_index < g_config.screenshot_count;
         screenshot_index++)
    {
        struct runner_screenshot *screenshot = &g_config.screenshots[screenshot_index];
        if (g_current_frame == screenshot->frame && !screenshot->written && !screenshot->failed)
        {
            screenshot->written = write_bmp_frame(data, width, height, pitch, screenshot->path);
            screenshot->failed = !screenshot->written;
        }
    }
}

static void RETRO_CALLCONV audio_sample_callback(int16_t left, int16_t right)
{
    const int16_t samples[2] = {left, right};
    hash_bytes(&g_audio_hash, samples, sizeof(samples));
    g_audio_frames++;
    g_audio_samples += 2;
}

static size_t RETRO_CALLCONV audio_batch_callback(const int16_t *data, size_t frames)
{
    if (data != NULL && frames <= SIZE_MAX / (2U * sizeof(int16_t)))
        hash_bytes(&g_audio_hash, data, frames * 2U * sizeof(int16_t));
    g_audio_frames += (uint64_t)frames;
    g_audio_samples += (uint64_t)frames * 2U;
    return frames;
}

static void RETRO_CALLCONV input_poll_callback(void)
{
    g_input_polls++;
}

static int16_t RETRO_CALLCONV input_state_callback(unsigned port,
                                                   unsigned device,
                                                   unsigned index,
                                                   unsigned id)
{
    size_t event_index;
    uint64_t selected_frame = 0;
    int16_t selected_value = 0;
    g_input_state_calls++;
    if (device != RETRO_DEVICE_JOYPAD || index != 0)
        return 0;

    for (event_index = 0; event_index < g_config.input_event_count; event_index++)
    {
        const struct runner_input_event *input_event = &g_config.input_events[event_index];
        if (input_event->port == port &&
            input_event->id == id &&
            input_event->frame <= g_current_frame &&
            input_event->frame >= selected_frame)
        {
            selected_frame = input_event->frame;
            selected_value = input_event->value;
        }
    }

    return selected_value;
}

static bool load_symbol(void *handle,
                        const char *name,
                        void *function_storage,
                        size_t function_storage_size,
                        char *message,
                        size_t message_size)
{
#ifdef _WIN32
    FARPROC symbol = GetProcAddress((HMODULE)handle, name);
    if (symbol == NULL)
    {
        snprintf(message,
                 message_size,
                 "Core does not export %s: Win32 error %lu",
                 name,
                 (unsigned long)GetLastError());
        return false;
    }
#else
    void *symbol;
    const char *error;

    dlerror();
    symbol = dlsym(handle, name);
    error = dlerror();
    if (error != NULL || symbol == NULL)
    {
        snprintf(message, message_size, "Core does not export %s: %s", name, error == NULL ? "missing symbol" : error);
        return false;
    }
#endif
    if (function_storage_size != sizeof(symbol))
    {
        snprintf(message, message_size, "Platform function pointer size is unsupported.");
        return false;
    }

    memcpy(function_storage, &symbol, function_storage_size);
    return true;
}

#define LOAD_CORE_SYMBOL(api, member, symbol_name, message, message_size) \
    load_symbol((api)->handle,                                              \
                (symbol_name),                                             \
                &(api)->member,                                            \
                sizeof((api)->member),                                     \
                (message),                                                 \
                (message_size))

static bool load_core_api(struct core_api *api,
                          const char *core_path,
                          char *message,
                          size_t message_size)
{
    memset(api, 0, sizeof(*api));
#ifdef _WIN32
    api->handle = (void *)LoadLibraryA(core_path);
    if (api->handle == NULL)
    {
        snprintf(message,
                 message_size,
                 "Could not load core: Win32 error %lu",
                 (unsigned long)GetLastError());
        return false;
    }
#else
    api->handle = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (api->handle == NULL)
    {
        snprintf(message, message_size, "Could not load core: %s", dlerror());
        return false;
    }
#endif

    return LOAD_CORE_SYMBOL(api, api_version, "retro_api_version", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_environment, "retro_set_environment", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_video_refresh, "retro_set_video_refresh", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_audio_sample, "retro_set_audio_sample", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_audio_sample_batch, "retro_set_audio_sample_batch", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_input_poll, "retro_set_input_poll", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_input_state, "retro_set_input_state", message, message_size) &&
           LOAD_CORE_SYMBOL(api, set_controller_port_device, "retro_set_controller_port_device", message, message_size) &&
           LOAD_CORE_SYMBOL(api, init, "retro_init", message, message_size) &&
           LOAD_CORE_SYMBOL(api, deinit, "retro_deinit", message, message_size) &&
           LOAD_CORE_SYMBOL(api, get_system_info, "retro_get_system_info", message, message_size) &&
           LOAD_CORE_SYMBOL(api, load_game, "retro_load_game", message, message_size) &&
           LOAD_CORE_SYMBOL(api, unload_game, "retro_unload_game", message, message_size) &&
           LOAD_CORE_SYMBOL(api, run, "retro_run", message, message_size) &&
           LOAD_CORE_SYMBOL(api, serialize_size, "retro_serialize_size", message, message_size) &&
           LOAD_CORE_SYMBOL(api, serialize, "retro_serialize", message, message_size) &&
           LOAD_CORE_SYMBOL(api, get_memory_data, "retro_get_memory_data", message, message_size) &&
           LOAD_CORE_SYMBOL(api, get_memory_size, "retro_get_memory_size", message, message_size);
}

static bool write_serialized_state(const struct core_api *core,
                                   const char *path,
                                   char *message,
                                   size_t message_size)
{
    size_t state_size = core->serialize_size();
    uint8_t *state;
    FILE *file;
    bool success;

    if (state_size == 0 || state_size > MAX_SERIALIZED_STATE_SIZE)
    {
        snprintf(message, message_size, "Core reported an invalid serialized state size of %zu bytes.", state_size);
        return false;
    }

    state = (uint8_t *)malloc(state_size);
    if (state == NULL)
    {
        snprintf(message, message_size, "Could not allocate %zu bytes for serialized state.", state_size);
        return false;
    }

    if (!core->serialize(state, state_size))
    {
        free(state);
        snprintf(message, message_size, "Core failed to serialize state.");
        return false;
    }

    file = fopen(path, "wb");
    if (file == NULL)
    {
        free(state);
        snprintf(message, message_size, "Could not open serialized state output: %s", strerror(errno));
        return false;
    }

    success = fwrite(state, 1, state_size, file) == state_size;
    if (fclose(file) != 0)
        success = false;
    free(state);

    if (!success)
    {
        snprintf(message, message_size, "Could not write serialized state output.");
        return false;
    }

    return true;
}

static bool resolve_work_ram_range(uint32_t address,
                                   size_t length,
                                   size_t work_ram_size,
                                   size_t *resolved_offset,
                                   char *message,
                                   size_t message_size)
{
    size_t offset;
    size_t bank_end;

    if (address >= 0x00200000U && address <= 0x003FFFFFU)
        offset = (size_t)((address - 0x00200000U) & 0x000FFFFFU);
    else if (address >= 0x06000000U && address <= 0x07FFFFFFU)
        offset = WORK_RAM_BANK_SIZE + (size_t)((address - 0x06000000U) & 0x000FFFFFU);
    else
    {
        snprintf(message, message_size, "Requested address is outside Saturn Low or High Work RAM.");
        return false;
    }

    bank_end = offset < WORK_RAM_BANK_SIZE ? WORK_RAM_BANK_SIZE : 2U * WORK_RAM_BANK_SIZE;

    if (offset > work_ram_size ||
        length > work_ram_size - offset ||
        length > bank_end - offset)
    {
        snprintf(message, message_size, "Requested range crosses a Work RAM bank boundary.");
        return false;
    }

    *resolved_offset = offset;
    return true;
}

static uint8_t read_guest_byte(const uint8_t *work_ram, size_t offset)
{
    const uint16_t endian_probe = 1;
    const bool little_endian = *(const uint8_t *)&endian_probe == 1;
    return work_ram[little_endian ? (offset ^ 1U) : offset];
}

static bool stop_condition_matches(const uint8_t *work_ram, size_t stop_offset)
{
    size_t index;

    for (index = 0; index < g_config.stop_value_length; index++)
    {
        if (read_guest_byte(work_ram, stop_offset + index) != g_config.stop_value[index])
            return false;
    }

    return true;
}

static void copy_guest_bytes(const uint8_t *work_ram,
                             size_t offset,
                             uint8_t *destination,
                             size_t length)
{
    size_t index;
    for (index = 0; index < length; index++)
        destination[index] = read_guest_byte(work_ram, offset + index);
}

static void write_json_string(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    putchar('"');
    while (*cursor != 0)
    {
        unsigned char value = *cursor++;
        if (value == '"' || value == '\\')
            printf("\\%c", value);
        else if (value == '\b')
            printf("\\b");
        else if (value == '\f')
            printf("\\f");
        else if (value == '\n')
            printf("\\n");
        else if (value == '\r')
            printf("\\r");
        else if (value == '\t')
            printf("\\t");
        else if (value < 0x20)
            printf("\\u%04x", value);
        else
            putchar((int)value);
    }
    putchar('"');
}

static void write_hex_bytes(const uint8_t *bytes, size_t length)
{
    size_t index;
    putchar('"');
    for (index = 0; index < length; index++)
        printf("%02x", bytes[index]);
    putchar('"');
}

static void write_capture_json(const struct runner_capture *capture)
{
    printf("{\"address\":\"0x%08" PRIx32 "\",\"length\":%zu,\"encoding\":\"hex\",\"data\":",
           capture->address,
           capture->length);
    write_hex_bytes(capture->data, capture->length);
    printf("}");
}

static void emit_result(const char *status,
                        int exit_code,
                        const char *message,
                        const char *core_name,
                        const char *core_version,
                        uint64_t frames_executed,
                        bool stop_condition_met,
                        const char *state_directory)
{
    size_t capture_index;
    size_t captured_count = 0;
    size_t option_index;

    printf("{\"schemaVersion\":%d,\"status\":", RUNNER_SCHEMA_VERSION);
    write_json_string(status);
    printf(",\"exitCode\":%d,\"message\":", exit_code);
    write_json_string(message);
    printf(",\"framesExecuted\":%" PRIu64, frames_executed);
    printf(",\"frameLimit\":%" PRIu64, g_config.frame_limit);
    printf(",\"stopConditionMet\":%s", stop_condition_met ? "true" : "false");
    if (g_config.has_stop_address)
    {
        printf(",\"stopAddress\":\"0x%08" PRIx32 "\",\"stopValue\":", g_config.stop_address);
        write_hex_bytes(g_config.stop_value, g_config.stop_value_length);
    }
    else
        printf(",\"stopAddress\":null,\"stopValue\":null");

    printf(",\"capture\":");
    for (capture_index = 0; capture_index < g_config.capture_count; capture_index++)
    {
        if (g_config.captures[capture_index].captured)
        {
            write_capture_json(&g_config.captures[capture_index]);
            break;
        }
    }
    if (capture_index == g_config.capture_count)
        printf("null");

    printf(",\"captures\":[");
    for (capture_index = 0; capture_index < g_config.capture_count; capture_index++)
    {
        if (!g_config.captures[capture_index].captured)
            continue;
        if (captured_count > 0)
            putchar(',');
        write_capture_json(&g_config.captures[capture_index]);
        captured_count++;
    }
    printf("]");

    printf(",\"video\":{\"frames\":%" PRIu64
           ",\"lastWidth\":%u,\"lastHeight\":%u,\"lastPitch\":%zu"
           ",\"bytes\":%" PRIu64 ",\"hash\":\"%016" PRIx64 "\"}",
           g_video_frames,
           g_last_video_width,
           g_last_video_height,
           g_last_video_pitch,
           g_video_bytes,
           g_video_hash);
    printf(",\"audio\":{\"frames\":%" PRIu64 ",\"samples\":%" PRIu64
           ",\"hash\":\"%016" PRIx64 "\"}",
           g_audio_frames,
           g_audio_samples,
           g_audio_hash);
    printf(",\"input\":{\"polls\":%" PRIu64 ",\"stateCalls\":%" PRIu64 "}",
           g_input_polls,
           g_input_state_calls);
    printf(",\"core\":{\"name\":");
    write_json_string(core_name);
    printf(",\"version\":");
    write_json_string(core_version);
    printf(",\"revision\":");
    write_json_string(BEETLE_REVISION);
    printf("}");
    printf(",\"coreOptions\":[");
    for (option_index = 0; option_index < g_option_count; option_index++)
    {
        const char *resolved_value = get_option_value(g_options[option_index].key);
        if (option_index > 0)
            putchar(',');
        printf("{\"key\":");
        write_json_string(g_options[option_index].key);
        printf(",\"value\":");
        write_json_string(resolved_value == NULL ? "" : resolved_value);
        printf("}");
    }
    printf("]");
    printf(",\"stateDirectory\":");
    if (state_directory != NULL)
        write_json_string(state_directory);
    else
        printf("null");
    printf("}\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct core_api core;
    struct retro_system_info system_info;
    struct retro_game_info game_info;
    char message[512] = "Runner failed.";
    char state_directory[PATH_MAX] = "";
    char core_name[128] = "unknown";
    char core_version[128] = "unknown";
    uint8_t *work_ram = NULL;
    size_t stop_offset = 0;
    size_t capture_index;
    uint64_t frames_executed = 0;
    bool core_initialized = false;
    bool game_loaded = false;
    bool stop_condition_met = false;
    int exit_code = RUNNER_EXIT_CORE;
    int parse_result = parse_arguments(argc, argv);

    memset(&core, 0, sizeof(core));
    memset(&system_info, 0, sizeof(system_info));
    memset(&game_info, 0, sizeof(game_info));

    if (parse_result > 0)
        return RUNNER_EXIT_OK;
    if (parse_result < 0)
    {
        print_usage(stderr);
        return RUNNER_EXIT_USAGE;
    }

    if (!validate_inputs(message, sizeof(message)))
    {
        emit_result("error", RUNNER_EXIT_INPUT, message, core_name, core_version, 0, false, NULL);
        return RUNNER_EXIT_INPUT;
    }

    if (!create_state_directory(state_directory, sizeof(state_directory), message, sizeof(message)))
    {
        emit_result("error", RUNNER_EXIT_INPUT, message, core_name, core_version, 0, false, NULL);
        remove_state_directory(state_directory);
        return RUNNER_EXIT_INPUT;
    }

    if (!load_core_api(&core, g_config.core_path, message, sizeof(message)))
        goto cleanup;

    if (core.api_version() != RETRO_API_VERSION)
    {
        snprintf(message, sizeof(message), "Core libretro API version does not match runner API version %u.", RETRO_API_VERSION);
        goto cleanup;
    }

    core.set_environment(environment_callback);
    core.set_video_refresh(video_callback);
    core.set_audio_sample(audio_sample_callback);
    core.set_audio_sample_batch(audio_batch_callback);
    core.set_input_poll(input_poll_callback);
    core.set_input_state(input_state_callback);
    core.get_system_info(&system_info);
    if (system_info.library_name != NULL)
        copy_text(core_name, sizeof(core_name), system_info.library_name);
    if (system_info.library_version != NULL)
        copy_text(core_version, sizeof(core_version), system_info.library_version);
    core.init();
    core_initialized = true;
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    game_info.path = g_config.image_path;
    if (!core.load_game(&game_info))
    {
        snprintf(message, sizeof(message), "Core rejected the disc image.");
        goto cleanup;
    }
    game_loaded = true;

    if (g_config.has_stop_address || g_config.capture_count > 0)
    {
        work_ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        size_t work_ram_size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        if (work_ram == NULL || work_ram_size < 2U * WORK_RAM_BANK_SIZE)
        {
            snprintf(message, sizeof(message), "Core did not expose the expected two MiB Work RAM buffer.");
            goto cleanup;
        }
        if (g_config.has_stop_address &&
            !resolve_work_ram_range(g_config.stop_address,
                                    g_config.stop_value_length,
                                    work_ram_size,
                                    &stop_offset,
                                    message,
                                    sizeof(message)))
            goto cleanup;

        for (capture_index = 0; capture_index < g_config.capture_count; capture_index++)
        {
            struct runner_capture *capture = &g_config.captures[capture_index];
            if (!resolve_work_ram_range(capture->address,
                                        capture->length,
                                        work_ram_size,
                                        &capture->offset,
                                        message,
                                        sizeof(message)))
                goto cleanup;
        }
    }

    while (frames_executed < g_config.frame_limit)
    {
        g_current_frame = frames_executed + 1;
        core.run();
        frames_executed++;
        if (work_ram != NULL && stop_condition_matches(work_ram, stop_offset))
        {
            stop_condition_met = true;
            break;
        }
    }

    if (g_config.has_serialize_path &&
        !write_serialized_state(&core,
                                g_config.serialize_path,
                                message,
                                sizeof(message)))
        goto cleanup;

    if (work_ram != NULL)
    {
        for (capture_index = 0; capture_index < g_config.capture_count; capture_index++)
        {
            struct runner_capture *capture = &g_config.captures[capture_index];
            copy_guest_bytes(work_ram, capture->offset, capture->data, capture->length);
            capture->captured = true;
        }
    }

    for (capture_index = 0; capture_index < g_config.screenshot_count; capture_index++)
    {
        const struct runner_screenshot *screenshot = &g_config.screenshots[capture_index];
        if (!screenshot->written)
        {
            exit_code = RUNNER_EXIT_CORE;
            snprintf(
                message,
                sizeof(message),
                screenshot->failed
                    ? "Could not write requested video frame %" PRIu64 "."
                    : "Requested video frame %" PRIu64 " did not produce a capturable callback.",
                screenshot->frame);
            goto cleanup;
        }
    }

    if (g_config.has_stop_address && !stop_condition_met)
    {
        exit_code = RUNNER_EXIT_STOP_NOT_REACHED;
        snprintf(message, sizeof(message), "Frame limit reached before the stop condition matched.");
    }
    else if (stop_condition_met)
    {
        exit_code = RUNNER_EXIT_OK;
        snprintf(message, sizeof(message), "Stop condition matched.");
    }
    else
    {
        exit_code = RUNNER_EXIT_OK;
        snprintf(message, sizeof(message), "Frame limit completed.");
    }

cleanup:
    if (game_loaded)
        core.unload_game();
    if (core_initialized)
        core.deinit();
    if (core.handle != NULL)
#ifdef _WIN32
        FreeLibrary((HMODULE)core.handle);
#else
        dlclose(core.handle);
#endif

    if (g_config.keep_state)
        fprintf(stderr, "[runner] state kept at %s\n", state_directory);
    else
        remove_state_directory(state_directory);

    emit_result(exit_code == RUNNER_EXIT_OK
                        ? (stop_condition_met ? "stop_condition_met" : "completed")
                        : (exit_code == RUNNER_EXIT_STOP_NOT_REACHED ? "frame_limit" : "error"),
                exit_code,
                message,
                core_name,
                core_version,
                frames_executed,
                stop_condition_met,
                g_config.keep_state ? state_directory : NULL);
    return exit_code;
}
