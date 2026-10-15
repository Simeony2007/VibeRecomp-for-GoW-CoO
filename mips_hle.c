#include "mips_hle.h"
#include "mips_video.h"
#include "mips_audio.h"
#include "mips_ge.h"
#include "sceKernelThread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

// NOVO: pasta real do jogo, definida em main_v35.c a partir de argv[1].
extern char g_game_root[512];
extern char g_umd_root[512];

#define SCE_KERNEL_ERROR_UNKNOWN_SEMID 0x80020199u
#define SCE_KERNEL_ERROR_WAIT_DELETE 0x800201B5u
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID 0x8002019Au
#define SCE_KERNEL_ERROR_UNKNOWN_THID 0x80020198u
#define SCE_KERNEL_ERROR_SEMA_ZERO 0x800201ADu
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT 0x800201BDu
#define SCE_KERNEL_ERROR_SEMA_OVF 0x800201AEu
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT 0x800201A8u
#define SCE_AUDIO_ERROR_CHANNEL_BUSY 0x80260002u
#define SCE_AUDIO_ERROR_CHANNEL_NOT_INIT 0x80260001u
#define SCE_AUDIO_ERROR_INVALID_CHANNEL 0x80260003u
#define SCE_AUDIO_ERROR_INVALID_VOLUME 0x8026000Bu
#define SCE_SAS_ERROR_INVALID_GRAIN 0x80420001u
#define SCE_SAS_ERROR_INVALID_MAX_VOICES 0x80420002u
#define SCE_SAS_ERROR_INVALID_OUTPUT_MODE 0x80420003u
#define SCE_SAS_ERROR_INVALID_SAMPLE_RATE 0x80420004u
#define SCE_SAS_ERROR_BAD_ADDRESS 0x80420005u
#define SCE_SAS_ERROR_INVALID_PARAMETER 0x80420014u
#define SCE_SAS_ERROR_INVALID_VOICE 0x80420010u
#define SCE_SAS_ERROR_INVALID_LOOP_POS 0x80420015u
#define SCE_SAS_ERROR_REV_INVALID_TYPE 0x80420020u
#define SCE_SAS_ERROR_REV_INVALID_FEEDBACK 0x80420021u
#define SCE_SAS_ERROR_REV_INVALID_DELAY 0x80420022u
#define SCE_SAS_ERROR_REV_INVALID_VOLUME 0x80420023u
#define SCE_KERNEL_ERROR_BAD_ARGUMENT 0x80000004u
#define SCE_KERNEL_ERROR_FILE_NOT_FOUND 0x80010002u
#define SCE_KERNEL_ERROR_IO_ERROR 0x80010005u
#define SCE_KERNEL_ERROR_NO_PERMISSION 0x8001000Du
#define SCE_KERNEL_ERROR_NOT_DIRECTORY 0x80010014u
#define SCE_KERNEL_ERROR_INVALID_ARGUMENT 0x80010016u
#define SCE_KERNEL_ERROR_TOO_MANY_FILES 0x80010018u
#define SCE_KERNEL_ERROR_NO_FREE_SPACE 0x8001001Cu
#define SCE_KERNEL_ERROR_NO_MEDIA 0x8001007Bu
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY 0x80020193u
#define SCE_KERNEL_ERROR_NOASYNC 0x8002032Au
#define SCE_KERNEL_ERROR_ILLEGAL_MODE 0x80020195u
#define SCE_KERNEL_ERROR_EVF_MULTI 0x800201B0u
#define SCE_KERNEL_ERROR_EVF_ILPAT 0x800201B1u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR 0x800200D3u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE 0x800201BCu
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201A7u
#define SCE_AVCODEC_ERROR_INVALID_DATA 0x807F00FDu
#define AUDIOCODEC_CONTEXT_SIZE 0x80u

enum {
    PSP_IO_STAT_SIZE = 0x58,
    PSP_S_IFDIR = 0x1000,
    PSP_S_IFREG = 0x2000,
    PSP_STM_FDIR = 0x10,
    PSP_STM_FREG = 0x20,
    PSP_SECTOR_SIZE = 2048,
};

static bool g_psp_interrupts_enabled = true;
static int g_async_default_priority = 0x20;
static unsigned int g_raw_sector_open_count = 0;

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int psp_open_error(int error) {
    switch (error) {
        case ENOENT: return (int32_t)SCE_KERNEL_ERROR_FILE_NOT_FOUND;
        case EEXIST: return (int32_t)0x80010011u;
        case EACCES:
        case EPERM: return (int32_t)SCE_KERNEL_ERROR_NO_PERMISSION;
        case ENOTDIR: return (int32_t)SCE_KERNEL_ERROR_NOT_DIRECTORY;
        case EISDIR: return (int32_t)0x80010015u;
        case ENOTEMPTY: return (int32_t)0x8001005Au;
        case EINVAL: return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        case EMFILE:
        case ENFILE: return (int32_t)SCE_KERNEL_ERROR_TOO_MANY_FILES;
        case ENOSPC: return (int32_t)SCE_KERNEL_ERROR_NO_FREE_SPACE;
        case EIO: return (int32_t)SCE_KERNEL_ERROR_IO_ERROR;
        default: return (int32_t)SCE_KERNEL_ERROR_IO_ERROR;
    }
}

/*
 * PSP path normalization with a case-insensitive host fallback.  The exact
 * spelling is tried first so filenames containing spaces and capitalization
 * are preserved whenever the host supports them.
 */
static int path_prefix(const char *path, const char *prefix, size_t length) {
    return path != NULL && strlen(path) > length &&
           strncasecmp(path, prefix, length) == 0 && path[length] == ':';
}

static void sanitize_path(char *dest, const char *src) {
    char input[512];
    char combined[1024];
    char normalized[1024];
    const char *p;
    size_t len;

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    /*
     * PSP APIs commonly pass fixed-width strings padded with spaces.  Only
     * trailing padding is removed; spaces in the middle of a filename are
     * significant and must be preserved.
     */
    len = strnlen(src, sizeof(input) - 1);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) {
        len--;
    }
    memcpy(input, src, len);
    input[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\\') input[i] = '/';
    }

    /*
     * PSP titles sometimes pad every fixed-width path component with spaces,
     * not only the complete string. Remove padding next to separators and
     * dot-components while preserving spaces inside real filenames.
     */
    {
        char compact[sizeof(input)];
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            if (input[i] == ' ' || input[i] == '\t') {
                size_t next = i + 1;
                while (next < len && (input[next] == ' ' || input[next] == '\t')) {
                    next++;
                }
                bool adjacent_to_separator =
                    (out > 0 && (compact[out - 1] == '/' || compact[out - 1] == '.')) ||
                    (next < len && (input[next] == '/' || input[next] == '.'));
                if (adjacent_to_separator) {
                    i = next - 1;
                    continue;
                }
            }
            if (out + 1 < sizeof(compact)) compact[out++] = input[i];
        }
        compact[out] = '\0';
        memcpy(input, compact, out + 1);
        len = out;
    }

    p = input;
    if (path_prefix(p, "umd0", 4) || path_prefix(p, "disc0", 5)) {
        p = strchr(p, ':');
        if (p != NULL) p++;
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/%s", g_umd_root, p);
    } else if (path_prefix(p, "ms0", 3)) {
        p = strchr(p, ':');
        if (p != NULL) p++;
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/ms0/%s", g_game_root, p);
    } else {
        while (*p == '/') p++;
        snprintf(combined, sizeof(combined), "%s/%s", g_game_root, p);
    }

    /*
     * Collapse dot components without calling realpath(): the target may not
     * exist yet (for O_CREAT), and paths must remain relative to the game
     * root.  A leading ".." is ignored rather than escaping that root.
     */
    normalized[0] = '\0';
    {
        char work[sizeof(combined)];
        char *save = NULL;
        char *part;
        snprintf(work, sizeof(work), "%s", combined);
        part = strtok_r(work, "/", &save);
        while (part != NULL) {
            size_t used = strlen(normalized);
            if (strcmp(part, ".") != 0 && part[0] != '\0') {
                if (strcmp(part, "..") == 0) {
                    char *slash = strrchr(normalized, '/');
                    if (slash != NULL) *slash = '\0';
                } else if (used + strlen(part) + 2 < sizeof(normalized)) {
                    if (used != 0) normalized[used++] = '/';
                    memcpy(normalized + used, part, strlen(part) + 1);
                }
            }
            part = strtok_r(NULL, "/", &save);
        }
    }
    snprintf(dest, 512, "%s", normalized);
}

static int iso_read_exact(int fd, uint64_t offset, void *buffer, size_t size) {
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, out + done, size - done, (off_t)(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static bool iso_name_matches(const uint8_t *name, size_t name_len,
                             const char *component) {
    size_t component_len = strlen(component);
    const uint8_t *version = memchr(name, ';', name_len);
    if (version != NULL) name_len = (size_t)(version - name);
    if (name_len != component_len) return false;
    for (size_t i = 0; i < name_len; i++) {
        if (toupper(name[i]) != toupper((unsigned char)component[i])) return false;
    }
    return true;
}

static int iso_find_path(MIPS_HLE_Kernel *kernel, const char *guest_path,
                         uint32_t *extent_out, uint32_t *size_out,
                         bool *is_directory_out) {
    if (kernel->umd_image_fd < 0 || guest_path == NULL) return -1;
    const char *relative = guest_path;
    const char *colon = strchr(relative, ':');
    if (colon != NULL) relative = colon + 1;
    while (*relative == '/') relative++;

    uint8_t pvd[PSP_SECTOR_SIZE];
    if (iso_read_exact(kernel->umd_image_fd, 16ULL * PSP_SECTOR_SIZE,
                       pvd, sizeof(pvd)) != 0 ||
        pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        return -1;
    }

    const uint8_t *root = pvd + 156;
    if (root[0] < 34 || (root[25] & 2) == 0) return -1;
    uint32_t directory_extent = read_le32(root + 2);
    uint32_t directory_size = read_le32(root + 10);
    if (directory_size == 0 || directory_size > 64U * 1024U * 1024U) return -1;
    if (*relative == '\0') {
        if (extent_out != NULL) *extent_out = directory_extent;
        if (size_out != NULL) *size_out = directory_size;
        if (is_directory_out != NULL) *is_directory_out = true;
        return 0;
    }

    while (*relative != '\0') {
        char component[256];
        size_t component_len = 0;
        while (relative[component_len] != '\0' &&
               relative[component_len] != '/' &&
               component_len + 1 < sizeof(component)) {
            component_len++;
        }
        if (component_len == 0 ||
            (relative[component_len] != '\0' && relative[component_len] != '/')) {
            return -1;
        }
        memcpy(component, relative, component_len);
        component[component_len] = '\0';
        relative += component_len;
        while (*relative == '/') relative++;
        bool final_component = *relative == '\0';

        uint8_t *directory = (uint8_t *)malloc(directory_size);
        if (directory == NULL) return -1;
        uint64_t dir_offset = (uint64_t)directory_extent * PSP_SECTOR_SIZE;
        if (iso_read_exact(kernel->umd_image_fd, dir_offset,
                           directory, directory_size) != 0) {
            free(directory);
            return -1;
        }

        bool found = false;
        size_t offset = 0;
        while (offset < directory_size) {
            uint8_t record_len = directory[offset];
            if (record_len == 0) {
                offset = ((offset / PSP_SECTOR_SIZE) + 1) * PSP_SECTOR_SIZE;
                continue;
            }
            if (record_len < 34 || offset + record_len > directory_size) break;
            uint8_t name_len = directory[offset + 32];
            if ((size_t)33 + name_len <= record_len &&
                !(name_len == 1 && (directory[offset + 33] == 0 ||
                                    directory[offset + 33] == 1)) &&
                iso_name_matches(directory + offset + 33, name_len, component)) {
                directory_extent = read_le32(directory + offset + 2);
                directory_size = read_le32(directory + offset + 10);
                bool is_directory = (directory[offset + 25] & 2) != 0;
                found = true;
                if (final_component) {
                    if (extent_out != NULL) *extent_out = directory_extent;
                    if (size_out != NULL) *size_out = directory_size;
                    if (is_directory_out != NULL) *is_directory_out = is_directory;
                } else if (!is_directory || directory_size == 0 ||
                           directory_size > 64U * 1024U * 1024U) {
                    found = false;
                }
                break;
            }
            offset += record_len;
        }
        free(directory);
        if (!found) return -1;
    }
    return 0;
}

static bool parse_raw_sector_path(const char *path, uint64_t *offset_out,
                                  uint64_t *size_out) {
    const char *marker = strstr(path, "sce_lbn");
    if (marker == NULL) return false;
    const char *lbn_text = marker + strlen("sce_lbn");
    if (lbn_text[0] == '0' &&
        (lbn_text[1] == 'x' || lbn_text[1] == 'X')) lbn_text += 2;
    errno = 0;
    char *end = NULL;
    unsigned long lbn = strtoul(lbn_text, &end, 16);
    if (errno != 0 || end == lbn_text || strncmp(end, "_size", 5) != 0 ||
        lbn > UINT32_MAX) return false;
    const char *size_text = end + 5;
    if (size_text[0] == '0' &&
        (size_text[1] == 'x' || size_text[1] == 'X')) size_text += 2;
    errno = 0;
    unsigned long size = strtoul(size_text, &end, 16);
    if (errno != 0 || end == size_text || *end != '\0' || size > UINT32_MAX) {
        return false;
    }
    *offset_out = (uint64_t)lbn * PSP_SECTOR_SIZE;
    *size_out = size;
    return true;
}

static int psp_open_flags(int flags) {
    int access_mode = flags & 0x3;
    int posix_flags = access_mode == 0x2 ? O_WRONLY :
                      access_mode == 0x3 ? O_RDWR : O_RDONLY;
    if (flags & 0x0200) posix_flags |= O_CREAT;
    if (flags & 0x0400) posix_flags |= O_TRUNC;
    if (flags & 0x0100) posix_flags |= O_APPEND;
    return posix_flags;
}

static int open_psp_file(MIPS_HLE_Kernel *kernel, HLE_File *file,
                         const char *file_path, int flags, int mode) {
    (void)mode;
    uint64_t raw_offset = 0;
    uint64_t raw_size = 0;
    if (parse_raw_sector_path(file_path, &raw_offset, &raw_size)) {
        if ((flags & 0x3) != 0x1 || (flags & (0x0100 | 0x0200 | 0x0400))) {
            return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        }
        if (kernel->umd_image_fd < 0) {
            fprintf(stderr,
                    "[HLE IO Error] sce_lbn requer uma ISO carregada; "
                    "defina MIPS_UMD_IMAGE.\n");
            return (int32_t)SCE_KERNEL_ERROR_NO_MEDIA;
        }
        if (raw_offset > kernel->umd_image_size ||
            raw_size > kernel->umd_image_size - raw_offset) {
            return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        }
        file->linux_fd = -1;
        file->raw_image_fd = kernel->umd_image_fd;
        file->is_raw_sector = true;
        file->raw_offset = raw_offset;
        file->raw_length = raw_size;
        file->raw_position = 0;
        unsigned int raw_open_number =
            __sync_add_and_fetch(&g_raw_sector_open_count, 1);
        if (raw_open_number <= 32 || (raw_open_number % 1024) == 0) {
            printf("[HLE IO] sce_lbn open LBN=0x%llX size=0x%llX bytes.\n",
                   (unsigned long long)(raw_offset / PSP_SECTOR_SIZE),
                   (unsigned long long)raw_size);
        }
        return 0;
    }

    char host_path[512];
    sanitize_path(host_path, file_path);
    int fd = open(host_path, psp_open_flags(flags), 0666);
    int open_error = fd < 0 ? errno : 0;
    if (fd < 0 && open_error == ENOENT) {
        char resolved_path[512];
        if (mips_hle_resolve_case_insensitive(resolved_path,
                                              sizeof(resolved_path),
                                              host_path) == 0) {
            fd = open(resolved_path, psp_open_flags(flags), 0666);
            if (fd < 0) open_error = errno;
        }
    }
    if (fd < 0) return psp_open_error(open_error);
    file->linux_fd = fd;
    file->raw_image_fd = -1;
    file->is_raw_sector = false;
    file->raw_offset = file->raw_length = file->raw_position = 0;
    return 0;
}

/* Resolve a PSP path using the host's actual spelling only when exact open
 * failed.  This keeps case intact for normal paths while tolerating the
 * PSP's case-insensitive filesystem on a case-sensitive host. */
int mips_hle_resolve_case_insensitive(char *resolved, size_t resolved_size,
                                      const char *path) {
    char current[1024] = "";
    const char *p = path;
    char component[256];

    if (path[0] == '/') {
        snprintf(current, sizeof(current), "/");
        while (*p == '/') p++;
    } else {
        snprintf(current, sizeof(current), ".");
    }

    while (*p != '\0') {
        size_t n = 0;
        DIR *dir;
        struct dirent *entry;
        const char *chosen = NULL;

        while (*p == '/') p++;
        while (p[n] != '\0' && p[n] != '/' && n + 1 < sizeof(component)) n++;
        if (n == 0) break;
        memcpy(component, p, n);
        component[n] = '\0';
        p += n;
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            size_t used = strlen(current);
            char *slash = used > 1 ? strrchr(current, '/') : NULL;
            if (slash != NULL) *slash = '\0';
            continue;
        }

        dir = opendir(current);
        if (dir == NULL) return -1;
        while ((entry = readdir(dir)) != NULL) {
            if (strcasecmp(entry->d_name, component) == 0) {
                chosen = entry->d_name;
                break;
            }
        }
        if (chosen == NULL) {
            closedir(dir);
            return -1;
        }
        {
            size_t used = strlen(current);
            if (used > 0 && current[used - 1] != '/') {
                if (used + 1 >= sizeof(current)) {
                    closedir(dir);
                    return -1;
                }
                current[used++] = '/';
                current[used] = '\0';
            }
            if (used + strlen(chosen) + 1 >= sizeof(current)) {
                closedir(dir);
                return -1;
            }
            strcat(current, chosen);
        }
        closedir(dir);
    }
    snprintf(resolved, resolved_size, "%s", current);
    return 0;
}

int mips_hle_init(MIPS_HLE_Kernel *kernel) {
    if (kernel == NULL) return -1;
    memset(kernel, 0, sizeof(MIPS_HLE_Kernel));
    kernel->umd_image_fd = -1;
    g_async_default_priority = 0x20;
    g_raw_sector_open_count = 0;
    pthread_mutex_init(&kernel->kernel_mutex, NULL);
    for (int i = 0; i < MAX_FILES; i++) {
        pthread_mutex_init(&kernel->files[i].async_mutex, NULL);
        pthread_cond_init(&kernel->files[i].async_cond, NULL);
    }

    const char *umd_image_path = getenv("MIPS_UMD_IMAGE");
    if (umd_image_path != NULL && umd_image_path[0] != '\0') {
        struct stat image_info;
        uint8_t pvd_header[7];
        kernel->umd_image_fd = open(umd_image_path, O_RDONLY);
        if (kernel->umd_image_fd < 0 ||
            fstat(kernel->umd_image_fd, &image_info) != 0 ||
            image_info.st_size < 17 * PSP_SECTOR_SIZE ||
            iso_read_exact(kernel->umd_image_fd, 16ULL * PSP_SECTOR_SIZE,
                           pvd_header, sizeof(pvd_header)) != 0 ||
            pvd_header[0] != 1 || memcmp(pvd_header + 1, "CD001", 5) != 0) {
            fprintf(stderr,
                    "[HLE IO Error] MIPS_UMD_IMAGE nao e uma ISO9660 valida: %s\n",
                    umd_image_path);
            if (kernel->umd_image_fd >= 0) close(kernel->umd_image_fd);
            kernel->umd_image_fd = -1;
            return -1;
        }
        kernel->umd_image_size = (uint64_t)image_info.st_size;
        printf("[HLE IO] ISO UMD habilitada: %s (%llu bytes).\n",
               umd_image_path, (unsigned long long)kernel->umd_image_size);
    }
    
    // Initialize the Graphics Engine state
    mips_ge_init(&kernel->ge);
    kernel->video_system = NULL;
    MIPS_AudioSystem *audio = (MIPS_AudioSystem *)calloc(1, sizeof(*audio));
    if (audio == NULL) {
        fprintf(stderr, "[HLE Kernel Error] Could not allocate the PSP audio system.\n");
        if (kernel->umd_image_fd >= 0) {
            close(kernel->umd_image_fd);
            kernel->umd_image_fd = -1;
        }
        return -1;
    }
    if (mips_audio_init(audio) != 0) {
        free(audio);
        fprintf(stderr, "[HLE Kernel Error] Could not initialize the PSP audio system.\n");
        if (kernel->umd_image_fd >= 0) {
            close(kernel->umd_image_fd);
            kernel->umd_image_fd = -1;
        }
        return -1;
    }
    kernel->audio_system = audio;

    MIPS_VideoSystem *video = (MIPS_VideoSystem *)calloc(1, sizeof(*video));
    if (video == NULL) {
        fprintf(stderr, "[HLE Video Warning] Could not allocate the video system; continuing headless.\n");
    } else if (mips_video_init(video) != 0) {
        fprintf(stderr, "[HLE Video Warning] Video initialization failed; continuing headless.\n");
        free(video);
    } else {
        kernel->video_system = video;
    }
    
    // Create local mock directories for storage integrity
    mkdir("./umd0", 0777);
    mkdir("./ms0", 0777);
    
    printf("[HLE Kernel] Sub-system Virtual File System and thread controllers initialized.\n");
    return 0;
}

static void trace_ge_lifecycle(const char *event, const MIPS_CPU *cpu,
                               const MIPS_GraphicsEngine *ge,
                               uint32_t a0, uint32_t a1, uint32_t a2,
                               uint32_t a3) {
    static int enabled = -1;
    static unsigned int count;
    if (enabled < 0) {
        const char *setting = getenv("MIPS_GE_TRACE_LIST_LIFECYCLE");
        enabled = setting != NULL && strcmp(setting, "1") == 0;
    }
    if (!enabled || count >= 256) return;
    count++;
    fprintf(stderr,
            "[HLE GE LIFECYCLE] event=%s cpu_pc=%08X ra=%08X "
            "args=%08X,%08X,%08X,%08X ge_pc=%08X stall=%08X "
            "running=%u finished=%u\n",
            event, cpu != NULL ? cpu->pc : 0,
            cpu != NULL ? cpu->gpr[31] : 0, a0, a1, a2, a3,
            ge != NULL ? ge->pc : 0, ge != NULL ? ge->stall_pc : 0,
            ge != NULL && ge->running ? 1u : 0u,
            ge != NULL && ge->finished ? 1u : 0u);
}

void mips_hle_free(MIPS_HLE_Kernel *kernel) {
    if (kernel == NULL) return;

    if (kernel->video_system != NULL) {
        MIPS_VideoSystem *video = (MIPS_VideoSystem *)kernel->video_system;
        mips_video_free(video);
        free(video);
        kernel->video_system = NULL;
    }

    if (kernel->audio_system != NULL) {
        MIPS_AudioSystem *audio = (MIPS_AudioSystem *)kernel->audio_system;
        mips_audio_free(audio);
        free(audio);
        kernel->audio_system = NULL;
    }
    
    pthread_mutex_lock(&kernel->kernel_mutex);
    // Cleanup active files, threads, and sync objects
    for (int i = 0; i < MAX_FILES; i++) {
        if (kernel->files[i].is_active) {
            if (kernel->files[i].async_thread_valid) {
                pthread_join(kernel->files[i].async_thread, NULL);
                kernel->files[i].async_thread_valid = false;
            }
            if (kernel->files[i].linux_fd >= 0) close(kernel->files[i].linux_fd);
        }
        pthread_mutex_destroy(&kernel->files[i].async_mutex);
        pthread_cond_destroy(&kernel->files[i].async_cond);
    }
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (kernel->directories[i].is_active &&
            kernel->directories[i].linux_dir != NULL) {
            closedir(kernel->directories[i].linux_dir);
            kernel->directories[i].linux_dir = NULL;
            kernel->directories[i].is_active = false;
        }
    }
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (kernel->semaphores[i].is_active) {
            pthread_mutex_destroy(&kernel->semaphores[i].mutex);
            pthread_cond_destroy(&kernel->semaphores[i].cond);
        }
    }
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (kernel->events[i].is_active) {
            pthread_mutex_destroy(&kernel->events[i].mutex);
            pthread_cond_destroy(&kernel->events[i].cond);
        }
    }
    if (kernel->umd_image_fd >= 0) {
        close(kernel->umd_image_fd);
        kernel->umd_image_fd = -1;
    }
    pthread_mutex_unlock(&kernel->kernel_mutex);
    pthread_mutex_destroy(&kernel->kernel_mutex);
}

typedef struct {
    MIPS_HLE_Kernel *kernel;
    HLE_File *file;
    char path[256];
    int flags;
    int mode;
    int slot;
} AsyncOpenRequest;

static void *async_io_open_worker(void *arg) {
    AsyncOpenRequest *request = (AsyncOpenRequest *)arg;
    HLE_File *file = request->file;
    int result = open_psp_file(request->kernel, file, request->path,
                               request->flags, request->mode);
    if (result == 0) {
        snprintf(file->psp_path, sizeof(file->psp_path), "%s", request->path);
        result = request->slot + 1;
        printf("[HLE IO] sceIoOpenAsync concluido: '%s' -> fd=%d.\n",
               request->path, result);
    } else {
        fprintf(stderr,
                "[HLE IO Error] sceIoOpenAsync('%s') concluido com 0x%08X.\n",
                request->path, (uint32_t)result);
    }
    free(request);

    pthread_mutex_lock(&file->async_mutex);
    file->async_result = result;
    file->is_async = false;
    file->async_done = true;
    pthread_cond_broadcast(&file->async_cond);
    pthread_mutex_unlock(&file->async_mutex);
    return NULL;
}

/*
 * Background Async Thread for sceIoReadAsync execution [Async I/O Real]
 */
static void *async_io_read_worker(void *arg) {
    HLE_File *file = (HLE_File *)arg;

    uint8_t *temp_buf = file->async_size != 0 ?
        (uint8_t *)malloc(file->async_size) : NULL;
    ssize_t read_bytes = -1;
    int result = -1;
    if (temp_buf != NULL || file->async_size == 0) {
        if (file->is_raw_sector) {
            uint64_t remaining = file->raw_length - file->raw_position;
            size_t request_size = file->async_size;
            if ((uint64_t)request_size > remaining) request_size = (size_t)remaining;
            if (iso_read_exact(file->raw_image_fd,
                               file->raw_offset + file->raw_position,
                               temp_buf, request_size) == 0) {
                read_bytes = (ssize_t)request_size;
                file->raw_position += request_size;
            }
        } else {
            read_bytes = read(file->linux_fd, temp_buf, file->async_size);
        }
    }

    if (read_bytes >= 0) {
        void *dst = mips_get_ptr_range(file->async_cpu, file->async_buffer,
                                       (size_t)read_bytes);
        if (dst != NULL) {
            if (read_bytes > 0) memcpy(dst, temp_buf, (size_t)read_bytes);
            result = (int)read_bytes;
        } else {
            result = -2;
        }
    }

    free(temp_buf);

    pthread_mutex_lock(&file->async_mutex);
    __sync_synchronize();
    file->async_result = result;
    file->is_async = false;
    file->async_done = true;
    pthread_cond_broadcast(&file->async_cond);
    pthread_mutex_unlock(&file->async_mutex);
    printf("[HLE IO] Async read worker completed: read %d bytes.\n", (int)read_bytes);
    return NULL;
}

/*
 * HLE API Implementations
 */

// sceIoOpen(const char* file, int flags, int mode)
static int hle_sceIoOpen(MIPS_HLE_Kernel *kernel, const char *file_path, int flags, int mode) {
    if (file_path == NULL || strnlen(file_path, 256) >= 256) return -1;
    pthread_mutex_lock(&kernel->kernel_mutex);

    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!kernel->files[i].is_active) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1;
    }
    HLE_File *file = &kernel->files[slot];
    file->linux_fd = -1;
    file->raw_image_fd = -1;
    file->is_active = true;
    file->is_async = false;
    file->is_raw_sector = false;
    file->async_priority = g_async_default_priority;
    file->async_open = false;
    int open_result = open_psp_file(kernel, file, file_path, flags, mode);
    if (open_result != 0) {
        file->is_active = false;
        pthread_mutex_unlock(&kernel->kernel_mutex);
        printf("[HLE IO Error] Failed to open path: %s result=0x%08X\n",
               file_path, (uint32_t)open_result);
        return open_result;
    }
    snprintf(file->psp_path, sizeof(file->psp_path), "%s", file_path);
    pthread_mutex_unlock(&kernel->kernel_mutex);
    printf("[HLE IO] Opened file '%s' successfully at slot %d.\n", file_path, slot);
    return slot + 1;
}

static int hle_sceIoOpenAsync(MIPS_HLE_Kernel *kernel, const char *file_path,
                              int flags, int mode) {
    if (file_path == NULL) return -1;
    AsyncOpenRequest *request = (AsyncOpenRequest *)calloc(1, sizeof(*request));
    if (request == NULL) return -1;
    request->kernel = kernel;
    request->path[0] = '\0';
    size_t path_len = strnlen(file_path, sizeof(request->path));
    if (path_len == 0 || path_len >= sizeof(request->path)) {
        free(request);
        return -1;
    }
    memcpy(request->path, file_path, path_len + 1);
    request->flags = flags;
    request->mode = mode;

    pthread_mutex_lock(&kernel->kernel_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!kernel->files[i].is_active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        free(request);
        return -1;
    }

    HLE_File *file = &kernel->files[slot];
    file->linux_fd = -1;
    file->raw_image_fd = -1;
    file->is_active = true;
    file->is_raw_sector = false;
    file->async_priority = g_async_default_priority;
    file->async_open = true;
    request->file = file;
    request->slot = slot;

    pthread_mutex_lock(&file->async_mutex);
    file->is_async = true;
    file->async_done = false;
    file->async_result = 0;
    int create_result = pthread_create(&file->async_thread, NULL,
                                       async_io_open_worker, request);
    if (create_result != 0) {
        file->is_async = false;
        file->async_open = false;
        file->is_active = false;
        pthread_mutex_unlock(&file->async_mutex);
        pthread_mutex_unlock(&kernel->kernel_mutex);
        free(request);
        return -1;
    }
    file->async_thread_valid = true;
    pthread_mutex_unlock(&file->async_mutex);
    pthread_mutex_unlock(&kernel->kernel_mutex);
    return slot + 1;
}

// sceIoRead(int fd, void *data, int size)
static int hle_sceIoRead(MIPS_HLE_Kernel *kernel, int fd, MIPS_CPU *cpu, uint32_t ram_addr, int size) {
    int slot = fd - 1;
    if (size < 0 || slot < 0 || slot >= MAX_FILES ||
        !kernel->files[slot].is_active) return -1;

    void *ptr = mips_get_ptr_range(cpu, ram_addr, (size_t)size);
    if (ptr == NULL) return -2; // Invalid RAM target

    HLE_File *file = &kernel->files[slot];
    ssize_t read_bytes;
    if (file->is_raw_sector) {
        uint64_t remaining = file->raw_length - file->raw_position;
        size_t request_size = (uint64_t)size > remaining ?
                              (size_t)remaining : (size_t)size;
        if (iso_read_exact(file->raw_image_fd,
                           file->raw_offset + file->raw_position,
                           ptr, request_size) != 0) return -1;
        file->raw_position += request_size;
        read_bytes = (ssize_t)request_size;
    } else {
        read_bytes = read(file->linux_fd, ptr, (size_t)size);
    }
    return (int)read_bytes;
}

static int hle_sceIoGetstat(MIPS_HLE_Kernel *kernel, MIPS_CPU *cpu,
                            uint32_t path_addr, uint32_t stat_addr) {
    const char *path = (const char *)mips_get_ptr_range(cpu, path_addr, 256);
    void *stat_buf = mips_get_ptr_range(cpu, stat_addr, PSP_IO_STAT_SIZE);
    if (path == NULL || memchr(path, '\0', 256) == NULL || stat_buf == NULL) {
        return -1;
    }

    char host_path[512];
    sanitize_path(host_path, path);
    struct stat info;
    uint32_t iso_extent = 0;
    uint32_t iso_size = 0;
    bool iso_is_directory = false;
    bool have_iso_entry =
        iso_find_path(kernel, path, &iso_extent, &iso_size, &iso_is_directory) == 0;
    uint64_t raw_offset = 0;
    uint64_t raw_size = 0;
    if (parse_raw_sector_path(path, &raw_offset, &raw_size)) {
        if (kernel->umd_image_fd < 0) return (int32_t)SCE_KERNEL_ERROR_NO_MEDIA;
        if (raw_offset > kernel->umd_image_size ||
            raw_size > kernel->umd_image_size - raw_offset) {
            return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
        }
        iso_extent = (uint32_t)(raw_offset / PSP_SECTOR_SIZE);
        iso_size = (uint32_t)raw_size;
        iso_is_directory = false;
        have_iso_entry = true;
    }
    if (stat(host_path, &info) != 0) {
        char resolved[1024];
        if (mips_hle_resolve_case_insensitive(resolved, sizeof(resolved), host_path) != 0 ||
            stat(resolved, &info) != 0) {
            if (!have_iso_entry) return -1;
            memset(&info, 0, sizeof(info));
            info.st_mode = (iso_is_directory ? S_IFDIR : S_IFREG) | 0555;
            info.st_size = iso_size;
        }
    }
    if (path_prefix(path, "disc0", 5) && have_iso_entry) {
        info.st_mode = (iso_is_directory ? S_IFDIR : S_IFREG) | 0555;
        info.st_size = iso_size;
    }

    memset(stat_buf, 0, 0x40);
    uint32_t mode = S_ISDIR(info.st_mode) ? PSP_S_IFDIR : PSP_S_IFREG;
    uint32_t attributes = S_ISDIR(info.st_mode) ? PSP_STM_FDIR : PSP_STM_FREG;
    mips_write32(cpu, stat_addr, mode | ((uint32_t)info.st_mode & 0777));
    mips_write32(cpu, stat_addr + 4, attributes);
    uint64_t file_size = (uint64_t)info.st_size;
    mips_write32(cpu, stat_addr + 8, (uint32_t)file_size);
    mips_write32(cpu, stat_addr + 12, (uint32_t)(file_size >> 32));
    if (path_prefix(path, "disc0", 5)) {
        if (have_iso_entry) {
            mips_write32(cpu, stat_addr + 0x40, iso_extent);
        } else if (kernel->umd_image_fd < 0) {
            static unsigned int missing_image_warnings;
            if (missing_image_warnings++ < 8) {
                fprintf(stderr,
                        "[HLE IO WARN] sceIoGetstat('%s'): defina MIPS_UMD_IMAGE "
                        "para preencher st_private[0] com o LBN real.\n", path);
            }
        }
    }
    return 0;
}

static int hle_sceIoDopen(MIPS_HLE_Kernel *kernel, MIPS_CPU *cpu,
                          uint32_t path_addr) {
    const char *guest_path =
        (const char *)mips_get_ptr_range(cpu, path_addr, 256);
    if (guest_path == NULL || memchr(guest_path, '\0', 256) == NULL) {
        return -1;
    }

    char host_path[512];
    sanitize_path(host_path, guest_path);
    DIR *directory = opendir(host_path);
    if (directory == NULL) {
        char resolved_path[512];
        if (mips_hle_resolve_case_insensitive(resolved_path, sizeof(resolved_path),
                                              host_path) == 0) {
            directory = opendir(resolved_path);
            if (directory != NULL) {
                snprintf(host_path, sizeof(host_path), "%s", resolved_path);
            }
        }
    }
    if (directory == NULL) {
        printf("[HLE IO Error] sceIoDopen: nao foi possivel abrir '%s' (mapeado: %s).\n",
               guest_path, host_path);
        return -1;
    }

    pthread_mutex_lock(&kernel->kernel_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_DIRECTORIES; i++) {
        if (!kernel->directories[i].is_active) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        kernel->directories[slot].linux_dir = directory;
        snprintf(kernel->directories[slot].host_path,
                 sizeof(kernel->directories[slot].host_path), "%s", host_path);
        snprintf(kernel->directories[slot].psp_path,
                 sizeof(kernel->directories[slot].psp_path), "%s", guest_path);
        kernel->directories[slot].is_active = true;
    }
    pthread_mutex_unlock(&kernel->kernel_mutex);

    if (slot < 0) {
        closedir(directory);
        return -1;
    }

    int handle = 0x10000 + slot;
    printf("[HLE IO] sceIoDopen('%s') -> %d.\n", guest_path, handle);
    return handle;
}

static int hle_sceIoDread(MIPS_HLE_Kernel *kernel, MIPS_CPU *cpu, int handle,
                          uint32_t dirent_addr) {
    enum {
        PSP_IO_DIRENT_SIZE = PSP_IO_STAT_SIZE + 256 + 4,
    };
    void *guest_dirent =
        mips_get_ptr_range(cpu, dirent_addr, PSP_IO_DIRENT_SIZE);
    if (guest_dirent == NULL) return -1;

    int slot = handle - 0x10000;
    if (slot < 0 || slot >= MAX_DIRECTORIES) return -1;

    pthread_mutex_lock(&kernel->kernel_mutex);
    HLE_Directory *directory = &kernel->directories[slot];
    if (!directory->is_active || directory->linux_dir == NULL) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1;
    }

    struct dirent *entry;
    do {
        errno = 0;
        entry = readdir(directory->linux_dir);
    } while (entry != NULL &&
             (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0));

    if (entry == NULL) {
        int read_error = errno;
        pthread_mutex_unlock(&kernel->kernel_mutex);
        if (read_error != 0) {
            printf("[HLE IO Error] sceIoDread(handle=%d): readdir falhou (%d).\n",
                   handle, read_error);
            return -1;
        }
        ((char *)guest_dirent)[PSP_IO_STAT_SIZE] = '\0';
        return 0;
    }

    char entry_name[256];
    snprintf(entry_name, sizeof(entry_name), "%s", entry->d_name);
    char directory_path[512];
    snprintf(directory_path, sizeof(directory_path), "%s", directory->host_path);
    char psp_directory_path[256];
    snprintf(psp_directory_path, sizeof(psp_directory_path), "%s", directory->psp_path);
    pthread_mutex_unlock(&kernel->kernel_mutex);

    char entry_path[1024];
    int path_length = snprintf(entry_path, sizeof(entry_path), "%s/%s",
                               directory_path, entry_name);
    struct stat info;
    bool have_stat = path_length > 0 && (size_t)path_length < sizeof(entry_path) &&
                     stat(entry_path, &info) == 0;

    if (!have_stat) {
        printf("[HLE IO Error] sceIoDread(handle=%d): stat falhou para '%s'.\n",
               handle, entry_name);
        return -1;
    }

    uint32_t mode = S_ISDIR(info.st_mode) ? PSP_S_IFDIR : PSP_S_IFREG;
    uint32_t attributes = S_ISDIR(info.st_mode) ? PSP_STM_FDIR : PSP_STM_FREG;
    uint32_t permissions = (uint32_t)info.st_mode & 0777;
    uint64_t entry_size = (uint64_t)info.st_size;
    uint32_t extent = 0;
    if (path_prefix(psp_directory_path, "disc0", 5)) {
        char psp_entry_path[512];
        int psp_path_length = snprintf(psp_entry_path, sizeof(psp_entry_path),
                                       "%s/%s", psp_directory_path, entry_name);
        uint32_t iso_size = 0;
        bool iso_is_directory = false;
        if (psp_path_length > 0 &&
            (size_t)psp_path_length < sizeof(psp_entry_path) &&
            iso_find_path(kernel, psp_entry_path, &extent, &iso_size,
                          &iso_is_directory) == 0) {
            mode = iso_is_directory ? PSP_S_IFDIR : PSP_S_IFREG;
            attributes = iso_is_directory ? PSP_STM_FDIR : PSP_STM_FREG;
            permissions = 0555;
            entry_size = iso_size;
        } else {
            extent = 0;
        }
    }

    memset(guest_dirent, 0, 0x40);
    mips_write32(cpu, dirent_addr, mode | permissions);
    mips_write32(cpu, dirent_addr + 4, attributes);
    mips_write32(cpu, dirent_addr + 8, (uint32_t)entry_size);
    mips_write32(cpu, dirent_addr + 12,
                 (uint32_t)(entry_size >> 32));
    mips_write32(cpu, dirent_addr + 0x40, extent);
    strncpy((char *)guest_dirent + PSP_IO_STAT_SIZE, entry_name, 256);
    ((char *)guest_dirent)[PSP_IO_STAT_SIZE + 255] = '\0';
    return 1;
}

static int hle_sceIoDclose(MIPS_HLE_Kernel *kernel, int handle) {
    int slot = handle - 0x10000;
    if (slot < 0 || slot >= MAX_DIRECTORIES) return -1;

    pthread_mutex_lock(&kernel->kernel_mutex);
    HLE_Directory *directory = &kernel->directories[slot];
    if (!directory->is_active || directory->linux_dir == NULL) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1;
    }
    DIR *linux_dir = directory->linux_dir;
    directory->linux_dir = NULL;
    directory->host_path[0] = '\0';
    directory->psp_path[0] = '\0';
    directory->is_active = false;
    pthread_mutex_unlock(&kernel->kernel_mutex);

    return closedir(linux_dir) == 0 ? 0 : -1;
}

// sceIoReadAsync(int fd, void *data, int size) [Async I/O Real]
static int hle_sceIoReadAsync(MIPS_HLE_Kernel *kernel, int fd, MIPS_CPU *cpu, uint32_t ram_addr, int size) {
    int slot = fd - 1;
    if (size < 0 || slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;
    
    void *ptr = mips_get_ptr_range(cpu, ram_addr, (size_t)size);
    if (ptr == NULL) return -2;
    
    HLE_File *file = &kernel->files[slot];
    pthread_mutex_lock(&file->async_mutex);
    if (file->is_async || file->async_done) {
        pthread_mutex_unlock(&file->async_mutex);
        return -3;
    }
    if (file->async_thread_valid) {
        pthread_t previous_thread = file->async_thread;
        file->async_thread_valid = false;
        pthread_mutex_unlock(&file->async_mutex);
        pthread_join(previous_thread, NULL);
        pthread_mutex_lock(&file->async_mutex);
    }
    file->is_async = true;
    file->async_done = false;
    file->async_cpu = cpu;
    file->async_buffer = ram_addr;
    file->async_size = size;
    file->async_result = 0;
    int create_result = pthread_create(&file->async_thread, NULL,
                                       async_io_read_worker, file);
    if (create_result != 0) {
        file->is_async = false;
        pthread_mutex_unlock(&file->async_mutex);
        return -4;
    }
    file->async_thread_valid = true;
    pthread_mutex_unlock(&file->async_mutex);
    sched_yield(); 
    return 0; // Async read requested successfully
}

static int hle_sceIoWaitAsync(MIPS_HLE_Kernel *kernel, MIPS_CPU *cpu,
                              int fd, uint32_t result_addr, bool poll) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) return -1;

    HLE_File *file = &kernel->files[slot];
    pthread_mutex_lock(&file->async_mutex);
    if (poll && file->is_async) {
        pthread_mutex_unlock(&file->async_mutex);
        return 1;
    }
    while (!poll && file->is_async) {
        pthread_cond_wait(&file->async_cond, &file->async_mutex);
    }
    if (!file->async_done) {
        pthread_mutex_unlock(&file->async_mutex);
        return (int32_t)SCE_KERNEL_ERROR_NOASYNC;
    }

    int32_t result = (int32_t)file->async_result;
    bool failed_open = file->async_open && result < 0;
    file->async_open = false;
    file->async_done = false;
    bool join_thread = file->async_thread_valid;
    pthread_t async_thread = file->async_thread;
    file->async_thread_valid = false;
    pthread_mutex_unlock(&file->async_mutex);

    if (join_thread) pthread_join(async_thread, NULL);

    if (failed_open) {
        pthread_mutex_lock(&kernel->kernel_mutex);
        if (file->is_active) {
            if (file->linux_fd >= 0) close(file->linux_fd);
            file->linux_fd = -1;
            file->raw_image_fd = -1;
            file->is_active = false;
        }
        pthread_mutex_unlock(&kernel->kernel_mutex);
    }

    if (result_addr != 0 && (result_addr & 3) == 0) {
        uint32_t *result_words =
            (uint32_t *)mips_get_ptr_range(cpu, result_addr, 2 * sizeof(uint32_t));
        if (result_words != NULL) {
            result_words[0] = (uint32_t)result;
            result_words[1] = result < 0 ? UINT32_MAX : 0;
        }
    }
    return 0;
}

static int psp_mutable_path(MIPS_CPU *cpu, uint32_t path_addr,
                            char *host_path, size_t host_path_size) {
    const char *guest_path = (const char *)mips_get_ptr_range(cpu, path_addr, 256);
    if (guest_path == NULL || memchr(guest_path, '\0', 256) == NULL ||
        path_prefix(guest_path, "disc0", 5) ||
        path_prefix(guest_path, "umd0", 4)) {
        return -1;
    }
    char normalized[512];
    sanitize_path(normalized, guest_path);
    if (normalized[0] == '\0' ||
        strnlen(normalized, sizeof(normalized)) >= host_path_size) {
        return -1;
    }
    snprintf(host_path, host_path_size, "%s", normalized);
    return 0;
}

static int hle_sceIoMkdir(MIPS_CPU *cpu, uint32_t path_addr, uint32_t mode) {
    char host_path[512];
    if (psp_mutable_path(cpu, path_addr, host_path, sizeof(host_path)) != 0) {
        return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    if (mkdir(host_path, (mode_t)(mode & 0777u)) == 0) return 0;
    return psp_open_error(errno);
}

static int hle_sceIoRename(MIPS_CPU *cpu, uint32_t from_addr,
                           uint32_t to_addr) {
    char from_path[512];
    char to_path[512];
    if (psp_mutable_path(cpu, from_addr, from_path, sizeof(from_path)) != 0 ||
        psp_mutable_path(cpu, to_addr, to_path, sizeof(to_path)) != 0) {
        return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    struct stat destination_info;
    if (lstat(to_path, &destination_info) == 0) {
        return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    if (errno != ENOENT) return psp_open_error(errno);
    if (rename(from_path, to_path) == 0) return 0;
    return psp_open_error(errno);
}

static int hle_sceIoRemove(MIPS_CPU *cpu, uint32_t path_addr) {
    char host_path[512];
    if (psp_mutable_path(cpu, path_addr, host_path, sizeof(host_path)) != 0) {
        return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    if (unlink(host_path) == 0) return 0;
    return psp_open_error(errno);
}

static int hle_sceIoRmdir(MIPS_CPU *cpu, uint32_t path_addr) {
    char host_path[512];
    if (psp_mutable_path(cpu, path_addr, host_path, sizeof(host_path)) != 0) {
        return (int32_t)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    if (rmdir(host_path) == 0) return 0;
    return psp_open_error(errno);
}

static uint32_t hle_sceIoLseekAsync(MIPS_HLE_Kernel *kernel, int fd,
                                    uint32_t offset_low, uint32_t offset_high,
                                    int whence) {
    int slot = fd - 1;
    if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) {
        return (uint32_t)-1;
    }
    HLE_File *file = &kernel->files[slot];
    pthread_mutex_lock(&file->async_mutex);
    if (file->is_async || file->async_done) {
        pthread_mutex_unlock(&file->async_mutex);
        return (uint32_t)-1;
    }
    int64_t offset = (int64_t)(((uint64_t)offset_high << 32) | offset_low);
    off_t result = file->is_raw_sector
        ? (off_t)file->raw_position
        : lseek(file->linux_fd, (off_t)offset, whence);
    if (file->is_raw_sector && result >= 0) {
        uint64_t base = whence == SEEK_SET ? 0 :
                        whence == SEEK_CUR ? file->raw_position :
                        whence == SEEK_END ? file->raw_length : UINT64_MAX;
        uint64_t negative_offset = offset < 0
            ? (uint64_t)(-(offset + 1)) + 1 : 0;
        if (base == UINT64_MAX || base > file->raw_length ||
            (offset < 0 && negative_offset > base) ||
            (offset > 0 && (uint64_t)offset > file->raw_length - base)) {
            result = (off_t)-1;
            errno = EINVAL;
        } else {
            file->raw_position = offset < 0 ? base - negative_offset :
                                              base + (uint64_t)offset;
            result = (off_t)file->raw_position;
        }
    }
    if (result < 0) {
        int error = psp_open_error(errno);
        pthread_mutex_unlock(&file->async_mutex);
        return (uint32_t)error;
    }
    file->async_result = (int64_t)result;
    file->async_done = true;
    pthread_mutex_unlock(&file->async_mutex);
    return 0;
}

// sceKernelCreateSema(const char* name, int attr, int init_val, int max_val, void* option)
static int hle_sceKernelCreateSema(MIPS_HLE_Kernel *kernel, const char *name,
                                   uint32_t attributes, int init_val,
                                   int max_val) {
    if (max_val <= 0 || init_val < 0 || init_val > max_val) {
        return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }
    pthread_mutex_lock(&kernel->kernel_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (!kernel->semaphores[i].is_active) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&kernel->kernel_mutex);
        return -1;
    }
    
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    snprintf(sema->name, sizeof(sema->name), "%s", name);
    sema->attributes = attributes;
    sema->initial_count = init_val;
    sema->count = init_val;
    sema->max_count = max_val;
    pthread_mutex_init(&sema->mutex, NULL);
    pthread_cond_init(&sema->cond, NULL);
    sema->is_active = true;
    sema->psp_sid = slot + 1;
    
    pthread_mutex_unlock(&kernel->kernel_mutex);
    printf("[HLE Sema] Semaphore '%s' created (Init: %d, Max: %d) with ID %d.\n", name, init_val, max_val, sema->psp_sid);
    return sema->psp_sid;
}

static int hle_sceKernelDeleteSema(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel,
                                   int semaid) {
    if (semaid <= 0) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int slot = semaid - 1;
    if (slot >= MAX_SEMAPHORES || !kernel->semaphores[slot].is_active) {
        return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    }

    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    if (!sema->is_active) {
        pthread_mutex_unlock(&sema->mutex);
        return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    }
    sema->is_active = false;
    pthread_mutex_unlock(&sema->mutex);

    hle_thread_delete_semaphore_waiters(cpu, semaid,
                                        SCE_KERNEL_ERROR_WAIT_DELETE);
    pthread_cond_destroy(&sema->cond);
    pthread_mutex_destroy(&sema->mutex);
    sema->psp_sid = 0;
    sema->name[0] = '\0';
    sema->count = 0;
    sema->max_count = 0;
    printf("[HLE Sema] Semaphore ID %d deleted.\n", semaid);
    return 0;
}

// sceKernelWaitSema(int semaid, int signal, void* timeout)
static bool hle_sceKernelWaitSema(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel,
                                  int semaid, int signal, uint32_t timeout_addr,
                                  int *result) {
    if (!hle_scheduler_dispatch_enabled()) {
        *result = (int)0x800201A7; // SCE_KERNEL_ERROR_CAN_NOT_WAIT
        return false;
    }
    if (semaid <= 0) {
        *result = (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
        return false;
    }
    int slot = semaid - 1;
    if (slot < 0 || slot >= MAX_SEMAPHORES || !kernel->semaphores[slot].is_active) {
        *result = (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
        return false;
    }
    if (signal <= 0) {
        *result = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
        return false;
    }

    hle_scheduler_wake_expired(cpu);
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    if (signal > sema->max_count) {
        pthread_mutex_unlock(&sema->mutex);
        *result = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
        return false;
    }
    bool has_waiters = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_WAITING &&
            threads[i].wait_type == THREAD_WAIT_SEMAPHORE &&
            threads[i].wait_semaphore_id == semaid) {
            has_waiters = true;
            break;
        }
    }
    if (sema->count >= signal && !has_waiters) {
        sema->count -= signal;
        pthread_mutex_unlock(&sema->mutex);
        *result = 0;
        return false;
    }
    pthread_mutex_unlock(&sema->mutex);

    uint64_t deadline_us = 0;
    if (timeout_addr != 0) {
        if (mips_get_ptr_range(cpu, timeout_addr, sizeof(uint32_t)) == NULL) {
            printf("[HLE ThreadMan Error] sceKernelWaitSema: invalid timeout pointer 0x%08X\n",
                   timeout_addr);
            *result = (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
            return false;
        }
        uint32_t timeout_us = mips_read32(cpu, timeout_addr);
        if (timeout_us == 0) {
            *result = (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            return false;
        }
        if (!hle_scheduler_deadline_after_us(timeout_us, &deadline_us)) {
            *result = (int)SCE_KERNEL_ERROR_BAD_ARGUMENT;
            return false;
        }
    }
    if (!hle_thread_wait_semaphore(cpu, semaid, signal, deadline_us, timeout_addr)) {
        *result = (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
        return false;
    }
    *result = 0;
    return true;
}

static int hle_sceKernelPollSema(MIPS_HLE_Kernel *kernel, int semaid, int signal) {
    if (signal <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (semaid <= 0) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;

    int slot = semaid - 1;
    if (slot < 0 || slot >= MAX_SEMAPHORES ||
        !kernel->semaphores[slot].is_active) {
        return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    }

    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    int result = SCE_KERNEL_ERROR_SEMA_ZERO;
    bool has_waiters = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_WAITING &&
            threads[i].wait_type == THREAD_WAIT_SEMAPHORE &&
            threads[i].wait_semaphore_id == semaid) {
            has_waiters = true;
            break;
        }
    }
    if (sema->count >= signal && !has_waiters) {
        sema->count -= signal;
        result = 0;
    }
    pthread_mutex_unlock(&sema->mutex);
    return result;
}

// sceKernelSignalSema(int semaid, int signal)
static int hle_sceKernelSignalSema(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel,
                                   int semaid, int signal) {
    if (semaid <= 0) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int slot = semaid - 1;
    if (slot < 0 || slot >= MAX_SEMAPHORES || !kernel->semaphores[slot].is_active) {
        return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    }
    if (signal <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    
    HLE_Semaphore *sema = &kernel->semaphores[slot];
    pthread_mutex_lock(&sema->mutex);
    int waiter_count = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_STATE_WAITING &&
            threads[i].wait_type == THREAD_WAIT_SEMAPHORE &&
            threads[i].wait_semaphore_id == semaid) {
            waiter_count++;
        }
    }
    int64_t resulting_count = (int64_t)sema->count + signal - waiter_count;
    if (resulting_count > sema->max_count) {
        pthread_mutex_unlock(&sema->mutex);
        return (int)SCE_KERNEL_ERROR_SEMA_OVF;
    }
    sema->count += signal;
    pthread_mutex_unlock(&sema->mutex);
    hle_thread_wake_semaphore(cpu, semaid, &sema->count);
    return 0;
}

/*
 * Syscall Intercepting Routing Manager
 * Maps PSP NID hashes directly into HLE routines in POSIX
 */
void mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;
    
    // Arguments: $a0, $a1, $a2, $a3 [Allegrex Calling Convention]
    uint32_t arg0 = cpu->gpr[4];  // $a0
    uint32_t arg1 = cpu->gpr[5];  // $a1
    uint32_t arg2 = cpu->gpr[6];  // $a2
    uint32_t arg3 = cpu->gpr[7];  // $a3
    
    uint32_t return_val = 0;

    if (syscall_id == 0x22224) {
        hle_sceKernelDelayThread(cpu);
        return;
    }
    
    switch (syscall_id) {
        // --- IoFileMgrForUser ---
        case 0x11111: { // Mock: sceIoOpen
            // NOVO: diagnostico do bug de "caminho com espacos" - antes de
            // confiar no ponteiro, mostra o endereco bruto (arg0) e um dump
            // hex+ASCII da memoria ali, pra ver se e' o PONTEIRO que esta
            // errado (apontando pra um lugar aleatorio da RAM) ou se o
            // CONTEUDO da memoria naquele endereco especifico realmente tem
            // esses espacos (o que apontaria pro jogo/decompressor, nao pra
            // leitura de argumento).
            const char *path_check =
                (const char *)mips_get_ptr_range(cpu, arg0, 256);
            if (path_check != NULL && memchr(path_check, '\0', 256) == NULL) {
                path_check = NULL;
            }
            bool looks_wrong = false;
            if (path_check) {
                for (int k = 0; k < 8; k++) {
                    if (path_check[k] == ' ') { looks_wrong = true; break; }
                    if (path_check[k] == '\0') break;
                }
            }
            if (looks_wrong) {
                printf("[HLE IO DEBUG] sceIoOpen: arg0 (ponteiro do path) = 0x%08X. Dump da memoria nesse endereco:\n", arg0);
                const uint8_t *raw = (const uint8_t *)path_check;
                for (int row = 0; row < 4; row++) {
                    printf("  +0x%02X:", row * 16);
                    for (int col = 0; col < 16; col++) printf(" %02X", raw[row * 16 + col]);
                    printf("  |");
                    for (int col = 0; col < 16; col++) {
                        uint8_t b = raw[row * 16 + col];
                        putchar((b >= 32 && b < 127) ? (char)b : '.');
                    }
                    printf("|\n");
                }
                printf("[HLE IO DEBUG] Caller RA: 0x%08X\n", cpu->gpr[0x1F]);
            }
            return_val = hle_sceIoOpen(kernel, path_check, (int)arg1, (int)arg2);
            break;
        }

        case 0x11112: // Mock: sceIoRead
            return_val = hle_sceIoRead(kernel, (int)arg0, cpu, arg1, (int)arg2);
            break;

        // NOVO: sceIoLseek - ja estava mapeada pelo linker (NID 0x27EB27B8)
        // mas sem handler aqui, entao qualquer seek virava "Unknown Syscall".
        // Assinatura real: sceIoLseek(int fd, SceOff offset, int whence) ->
        // retorna a nova posicao (64-bit na PSP real, mas aqui simplificado
        // pra 32-bit ja que os arquivos de jogo nao devem passar de 4GB).
        // offset chega em $a1:$a2 (64-bit split em 2 registradores de 32,
        // little-endian: $a1=parte baixa, $a2=parte alta) e whence em $a3.
        case 0x11116: {
            int fd = (int)arg0;
            /*
             * sceIoLseek(id, s64 offset, whence) follows the PSP ABI's
             * 64-bit alignment rule: offset starts in a2/a3, while whence
             * is the next stack argument.  Reading a1/a2/a3 as three
             * independent arguments produced offsets such as 0x90000000000
             * and whence 511 in the CH trace.
             */
            int64_t offset = (int64_t)((uint64_t)arg2 | ((uint64_t)arg3 << 32));
            uint32_t stack_whence_addr = cpu->gpr[29] + 16;
            int whence = (int)mips_read32(cpu, stack_whence_addr);
            if (whence < 0 || whence > 2) {
                /* Compatibility with callers that use the simplified
                 * register-only convention used by older test binaries. */
                offset = (int64_t)((uint64_t)arg1 | ((uint64_t)arg2 << 32));
                whence = (int)arg3;
            }
            int slot = fd - 1;
            if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active) {
                return_val = (uint32_t)-1;
            } else {
                int posix_whence = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
                HLE_File *file = &kernel->files[slot];
                off_t new_pos;
                if (file->is_raw_sector) {
                    int64_t base = posix_whence == SEEK_CUR ?
                                   (int64_t)file->raw_position :
                                   posix_whence == SEEK_END ?
                                   (int64_t)file->raw_length : 0;
                    int64_t target = base + offset;
                    if (target < 0 || (uint64_t)target > file->raw_length) {
                        new_pos = -1;
                    } else {
                        file->raw_position = (uint64_t)target;
                        new_pos = (off_t)target;
                    }
                } else {
                    new_pos = lseek(file->linux_fd, (off_t)offset, posix_whence);
                }
                return_val = (uint32_t)new_pos;
                printf("[HLE IO] sceIoLseek(fd=%d, offset=%lld, whence=%d) -> nova posicao %ld\n",
                       fd, (long long)offset, whence, (long)new_pos);
            }
            break;
        }

        case 0x11117: { // Mock: sceIoLseek32(int fd, int offset, int whence)
            int fd = (int)arg0;
            int slot = fd - 1;
            int whence = (int)arg2;
            if (slot < 0 || slot >= MAX_FILES || !kernel->files[slot].is_active ||
                whence < 0 || whence > 2) {
                return_val = (uint32_t)-1;
            } else {
                int posix_whence = (whence == 1) ? SEEK_CUR :
                                   (whence == 2) ? SEEK_END : SEEK_SET;
                HLE_File *file = &kernel->files[slot];
                off_t new_pos;
                if (file->is_raw_sector) {
                    int64_t base = posix_whence == SEEK_CUR ?
                                   (int64_t)file->raw_position :
                                   posix_whence == SEEK_END ?
                                   (int64_t)file->raw_length : 0;
                    int64_t target = base + (int32_t)arg1;
                    if (target < 0 || (uint64_t)target > file->raw_length) {
                        new_pos = -1;
                    } else {
                        file->raw_position = (uint64_t)target;
                        new_pos = (off_t)target;
                    }
                } else {
                    new_pos = lseek(file->linux_fd, (off_t)(int32_t)arg1,
                                    posix_whence);
                }
                return_val = new_pos < 0 ? (uint32_t)-1 : (uint32_t)new_pos;
                printf("[HLE IO] sceIoLseek32(fd=%d, offset=%d, whence=%d) "
                       "-> nova posicao %ld\n",
                       fd, (int32_t)arg1, whence, (long)new_pos);
            }
            break;
        }
            
        case 0x11113: // Mock: sceIoReadAsync
            return_val = hle_sceIoReadAsync(kernel, (int)arg0, cpu, arg1, (int)arg2);
            break;

        case 0x11126: { // sceIoOpenAsync
            const char *path = (const char *)mips_get_ptr_range(cpu, arg0, 256);
            if (path == NULL || memchr(path, '\0', 256) == NULL) {
                return_val = (uint32_t)-1;
            } else {
                return_val = (uint32_t)hle_sceIoOpenAsync(
                    kernel, path, (int)arg1, (int)arg2);
                printf("[HLE IO] sceIoOpenAsync('%s', flags=0x%X) -> %d\n",
                       path, arg1, (int32_t)return_val);
            }
            break;
        }

        case 0x11127: { // sceIoChangeAsyncPriority
            int slot = (int)arg0 - 1;
            int priority = (int32_t)arg1;
            if (priority != -1 && (priority < 0x08 || priority > 0x77)) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
            } else if ((int32_t)arg0 == -1) {
                g_async_default_priority = priority;
                return_val = 0;
            } else if (slot < 0 || slot >= MAX_FILES ||
                       !kernel->files[slot].is_active) {
                return_val = (uint32_t)-1;
            } else {
                kernel->files[slot].async_priority = priority;
                return_val = 0;
            }
            break;
        }

        case 0x11125: // Mock: sceIoGetstat
            return_val = hle_sceIoGetstat(kernel, cpu, arg0, arg1);
            break;

        case 0x1111F: // sceIoDopen
            return_val = hle_sceIoDopen(kernel, cpu, arg0);
            break;

        case 0x11120: // sceIoDread
            return_val = hle_sceIoDread(kernel, cpu, (int)arg0, arg1);
            break;

        case 0x11121: // sceIoDclose
            return_val = hle_sceIoDclose(kernel, (int)arg0);
            break;

        case 0x1111A: // Mock: sceIoWaitAsync
            return_val = hle_sceIoWaitAsync(kernel, cpu, (int)arg0, arg1, false);
            break;

        case 0x1111B: // Mock: sceIoPollAsync
            return_val = hle_sceIoWaitAsync(kernel, cpu, (int)arg0, arg1, true);
            break;

        case 0x1112D: // sceIoWaitAsyncCB
            return_val = hle_sceIoWaitAsync(kernel, cpu, (int)arg0, arg1, false);
            break;

        case 0x1112C: // sceIoLseekAsync
            return_val = hle_sceIoLseekAsync(kernel, (int)arg0, arg1, arg2,
                                              (int)arg3);
            break;

        case 0x11128: // sceIoMkdir
            return_val = (uint32_t)hle_sceIoMkdir(cpu, arg0, arg1);
            break;

        case 0x11129: // sceIoRename
            return_val = (uint32_t)hle_sceIoRename(cpu, arg0, arg1);
            break;

        case 0x1112A: // sceIoRemove
            return_val = (uint32_t)hle_sceIoRemove(cpu, arg0);
            break;

        case 0x1112B: // sceIoRmdir
            return_val = (uint32_t)hle_sceIoRmdir(cpu, arg0);
            break;
            
        case 0x11114: // Mock: sceIoClose
            pthread_mutex_lock(&kernel->kernel_mutex);
            int file_slot = (int)arg0 - 1;
            if (file_slot >= 0 && file_slot < MAX_FILES && kernel->files[file_slot].is_active) {
                if (kernel->files[file_slot].async_thread_valid) {
                    pthread_join(kernel->files[file_slot].async_thread, NULL);
                    kernel->files[file_slot].async_thread_valid = false;
                }
                if (kernel->files[file_slot].linux_fd >= 0) {
                    close(kernel->files[file_slot].linux_fd);
                }
                kernel->files[file_slot].linux_fd = -1;
                kernel->files[file_slot].raw_image_fd = -1;
                kernel->files[file_slot].async_done = false;
                kernel->files[file_slot].is_async = false;
                kernel->files[file_slot].async_open = false;
                kernel->files[file_slot].is_active = false;
                return_val = 0;
            } else {
                return_val = -1;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        // --- StdioForUser ---
        case 0x11122: // sceKernelStdin
            return_val = 0;
            break;
        case 0x11123: // sceKernelStdout
            return_val = 1;
            break;
        case 0x11124: // sceKernelStderr
            return_val = 2;
            break;

        // --- ThreadManForUser ---
        case 0x22221: // Mock: sceKernelCreateSema
            return_val = hle_sceKernelCreateSema(
                kernel, (const char *)mips_get_ptr(cpu, arg0), arg1,
                (int)arg2, (int)arg3);
            break;
            
        case 0x22222: // Mock: sceKernelWaitSema
        {
            int wait_result = 0;
            bool blocked = hle_sceKernelWaitSema(cpu, kernel, (int)arg0,
                                                 (int)arg1, arg2, &wait_result);
            return_val = (uint32_t)wait_result;
            if (blocked) {
                cpu->gpr[2] = return_val;
                cpu->pc = cpu->gpr[31];
                cpu->next_pc = cpu->pc + 4;
                run_scheduler(cpu, THREAD_STATE_WAITING);
                return;
            }
            break;
        }
            
        case 0x22223: // Mock: sceKernelSignalSema
            return_val = hle_sceKernelSignalSema(cpu, kernel, (int)arg0, (int)arg1);
            break;

        case 0x2223D: // sceKernelDeleteSema
            return_val = (uint32_t)hle_sceKernelDeleteSema(
                cpu, kernel, (int)arg0);
            break;

        case 0x22246: { // sceKernelReferSemaStatus
            enum { PSP_SEMA_STATUS_SIZE = 56 };
            int slot = (int)arg0 - 1;
            if (slot < 0 || slot >= MAX_SEMAPHORES ||
                !kernel->semaphores[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_SEMID;
                break;
            }
            uint32_t size_addr = arg1;
            if ((size_addr & 3u) != 0 ||
                mips_get_ptr_range(cpu, size_addr, sizeof(uint32_t)) == NULL) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
                break;
            }
            uint32_t requested_size = mips_read32(cpu, size_addr);
            if (requested_size > PSP_SEMA_STATUS_SIZE) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_SIZE;
                break;
            }
            if (requested_size == 0) {
                return_val = 0;
                break;
            }
            hle_scheduler_wake_expired(cpu);
            uint8_t *status = (uint8_t *)mips_get_ptr_range(
                cpu, size_addr, PSP_SEMA_STATUS_SIZE);
            if (status == NULL) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
                break;
            }
            HLE_Semaphore *sema = &kernel->semaphores[slot];
            pthread_mutex_lock(&sema->mutex);
            uint32_t waiting = 0;
            for (int i = 0; i < MAX_THREADS; i++) {
                if (threads[i].state == THREAD_STATE_WAITING &&
                    threads[i].wait_type == THREAD_WAIT_SEMAPHORE &&
                    threads[i].wait_semaphore_id == (int)arg0) {
                    waiting++;
                }
            }
            memset(status, 0, PSP_SEMA_STATUS_SIZE);
            memcpy(status + 4, sema->name, sizeof(sema->name));
            mips_write32(cpu, size_addr, PSP_SEMA_STATUS_SIZE);
            mips_write32(cpu, size_addr + 36, sema->attributes);
            mips_write32(cpu, size_addr + 40, (uint32_t)sema->initial_count);
            mips_write32(cpu, size_addr + 44, (uint32_t)sema->count);
            mips_write32(cpu, size_addr + 48, (uint32_t)sema->max_count);
            mips_write32(cpu, size_addr + 52, waiting);
            pthread_mutex_unlock(&sema->mutex);
            return_val = 0;
            break;
        }

        case 0x22238: // sceKernelPollSema
            return_val = (uint32_t)hle_sceKernelPollSema(kernel, (int)arg0, (int)arg1);
            break;

        case 0x22243: // sceKernelGetSystemTimeLow
            hle_sceKernelGetSystemTimeLow(cpu);
            return;

        case 0x22244: // sceKernelTerminateThread
            hle_sceKernelTerminateThread(cpu, false);
            return;

        case 0x22245: // sceKernelTerminateDeleteThread
            hle_sceKernelTerminateThread(cpu, true);
            return;

        case 0x22239: // sceKernelCpuSuspendIntr
            return_val = g_psp_interrupts_enabled ? 1 : 0;
            g_psp_interrupts_enabled = false;
            break;

        case 0x2223A: // sceKernelCpuResumeIntr
        case 0x2223B: // sceKernelCpuResumeIntrWithSync
            g_psp_interrupts_enabled = arg0 != 0;
            return_val = 0;
            break;

        case 0x2223C: // sceKernelIsCpuIntrEnable
            return_val = g_psp_interrupts_enabled ? 1 : 0;
            break;
            
        case 0x22224: // Mock: sceKernelDelayThread
            hle_sceKernelDelayThread(cpu);
            return;
            break;

        // --- GraphicsEngine (sceGe) ---
        case 0x33331: // Mock: sceGeListEnqueue
            trace_ge_lifecycle("enqueue", cpu, &kernel->ge,
                               arg0, arg1, arg2, arg3);
            if (mips_get_ptr_range(cpu, arg0, sizeof(uint32_t)) == NULL) {
                static unsigned int invalid_ge_count = 0;
                invalid_ge_count++;
                if (invalid_ge_count <= 32 || (invalid_ge_count % 4096) == 0) {
                    printf("[HLE GE DEBUG] invalid list #%u start=0x%08X stall=0x%08X "
                           "pc=0x%08X ra=0x%08X t9=0x%08X sp=0x%08X\n",
                           invalid_ge_count, arg0, arg1, cpu->pc, cpu->gpr[31],
                           cpu->gpr[25], cpu->gpr[29]);
                    printf("[HLE GE DEBUG] fpr={%08X,%08X,%08X,%08X} "
                           "vfpu={%08X,%08X,%08X,%08X}\n",
                           *(uint32_t *)&cpu->fpr[0], *(uint32_t *)&cpu->fpr[1],
                           *(uint32_t *)&cpu->fpr[2], *(uint32_t *)&cpu->fpr[3],
                           *(uint32_t *)&cpu->vfpu[0], *(uint32_t *)&cpu->vfpu[1],
                           *(uint32_t *)&cpu->vfpu[2], *(uint32_t *)&cpu->vfpu[3]);
                }
                return_val = -1;
                break;
            }
            /*
             * A GE stall address is either zero (no stall) or a PSP RAM
             * address.  Some homebrew emits an uninitialized host-looking
             * value here; treating it as a real stall freezes the software
             * GE before it can process the list.
             */
            if (arg1 != 0 && mips_get_ptr_range(cpu, arg1, sizeof(uint32_t)) == NULL) {
                printf("[HLE GE] Ignoring invalid stall address 0x%08X\n", arg1);
                arg1 = 0;
            }
            printf("[HLE GE] Enqueued Display List (Start: 0x%08X, Stall: 0x%08X)\n", arg0, arg1);
            kernel->ge.video_system = kernel->video_system;
            if (kernel->video_system != NULL) {
                mips_video_begin_frame((MIPS_VideoSystem *)kernel->video_system);
                kernel->ge.draw_callback = (GEDrawCallback)mips_video_draw_prim;
            }
            mips_ge_begin_list(&kernel->ge);
            mips_ge_run_list(&kernel->ge, cpu, arg0, arg1);
            if (mips_video_present((MIPS_VideoSystem *)kernel->video_system)) {
                cpu->exit_requested = true;
            }
            return_val = 1; // List ID
            trace_ge_lifecycle("enqueue_complete", cpu, &kernel->ge,
                               arg0, arg1, arg2, (uint32_t)return_val);
            break;

        case 0x33332: // Mock: sceGeListSync
            trace_ge_lifecycle("list_sync", cpu, &kernel->ge,
                               arg0, arg1, arg2, arg3);
            return_val = 0; // Success
            break;

        case 0x33336: { // sceGeListUpdateStallAddr
            trace_ge_lifecycle("stall_update", cpu, &kernel->ge,
                               arg0, arg1, arg2, arg3);
            /*
             * mips_ge_run_list() stops with ge.pc at the stall address.
             * Updating that address is the GE equivalent of releasing the
             * list; resume parsing from the saved PC when possible.
             */
            uint32_t new_stall = arg1;
            if (new_stall != 0 &&
                mips_get_ptr_range(cpu, new_stall, sizeof(uint32_t)) == NULL) {
                printf("[HLE GE] Ignoring invalid updated stall address 0x%08X\n", new_stall);
                new_stall = 0;
            }
            kernel->ge.stall_pc = new_stall;
            if (kernel->ge.running && kernel->ge.pc != new_stall) {
                mips_ge_run_list(&kernel->ge, cpu, kernel->ge.pc, new_stall);
                if (mips_video_present((MIPS_VideoSystem *)kernel->video_system)) {
                    cpu->exit_requested = true;
                }
            }
            return_val = 0;
            trace_ge_lifecycle("stall_update_complete", cpu, &kernel->ge,
                               arg0, new_stall, arg2, (uint32_t)return_val);
            break;
        }

        case 0x33337: // sceGeSetCallback
            /*
             * The software GE does not have an interrupt queue yet, but the
             * callback descriptor is still accepted and the PSP API must
             * return a valid callback id rather than the generic zero stub.
             */
            return_val = 0;
            break;

        case 0x33338: // sceGeUnsetCallback
            return_val = 0;
            break;

        case 0x22230: { // sceKernelCreateEventFlag
            const char *name = (const char *)mips_get_ptr_range(cpu, arg0, 32);
            int slot = -1;
            for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
                if (!kernel->events[i].is_active) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0 || name == NULL || memchr(name, '\0', 32) == NULL) {
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
                break;
            }
            if ((arg1 & 0x100u) != 0 || arg1 >= 0x300u) {
                return_val = 0x80020191u;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            event->psp_evid = (uint32_t)(slot + 1);
            snprintf(event->name, sizeof(event->name), "%s", name);
            event->attributes = arg1;
            event->initial_pattern = arg2;
            event->pattern = arg2;
            pthread_mutex_init(&event->mutex, NULL);
            pthread_cond_init(&event->cond, NULL);
            event->is_active = true;
            return_val = event->psp_evid;
            break;
        }

        case 0x22240: { // sceKernelDeleteEventFlag
            int slot = (int)arg0 - 1;
            if (slot < 0 || slot >= MAX_EVENT_FLAGS ||
                !kernel->events[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_EVFID;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            hle_thread_delete_event_flag_waiters(
                cpu, (int)event->psp_evid, SCE_KERNEL_ERROR_WAIT_DELETE);
            pthread_mutex_destroy(&event->mutex);
            pthread_cond_destroy(&event->cond);
            event->is_active = false;
            event->pattern = 0;
            return_val = 0;
            break;
        }

        case 0x22231: { // sceKernelSetEventFlag
            int slot = (int)arg0 - 1;
            if (slot < 0 || slot >= MAX_EVENT_FLAGS ||
                !kernel->events[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_EVFID;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            pthread_mutex_lock(&event->mutex);
            event->pattern |= arg1;
            hle_thread_wake_event_flag(
                cpu, (int)event->psp_evid, &event->pattern,
                (event->attributes & 0x200u) != 0);
            hle_thread_update_event_flag_pattern((int)event->psp_evid,
                                                 event->pattern);
            pthread_cond_broadcast(&event->cond);
            pthread_mutex_unlock(&event->mutex);
            return_val = 0;
            break;
        }

        case 0x22232: { // sceKernelClearEventFlag
            int slot = (int)arg0 - 1;
            if (slot < 0 || slot >= MAX_EVENT_FLAGS ||
                !kernel->events[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_EVFID;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            pthread_mutex_lock(&event->mutex);
            event->pattern &= arg1;
            hle_thread_update_event_flag_pattern((int)event->psp_evid,
                                                 event->pattern);
            pthread_mutex_unlock(&event->mutex);
            return_val = 0;
            break;
        }

        case 0x22241: { // sceKernelWaitEventFlag
            const uint32_t known_modes = 0x31u;
            int slot = (int)arg0 - 1;
            uint32_t timeout_addr = 0;
            uint64_t timeout_addr64 = (uint64_t)cpu->gpr[29] + 16;
            if (timeout_addr64 <= UINT32_MAX &&
                mips_get_ptr_range(cpu, (uint32_t)timeout_addr64,
                                   sizeof(uint32_t)) != NULL) {
                timeout_addr = mips_read32(cpu, (uint32_t)timeout_addr64);
            }
            if ((arg2 & ~known_modes) != 0) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_MODE;
                break;
            }
            if (arg1 == 0) {
                return_val = SCE_KERNEL_ERROR_EVF_ILPAT;
                break;
            }
            if (!hle_scheduler_dispatch_enabled()) {
                return_val = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
                break;
            }
            if (slot < 0 || slot >= MAX_EVENT_FLAGS ||
                !kernel->events[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_EVFID;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            pthread_mutex_lock(&event->mutex);
            bool matches = (arg2 & 1u) != 0
                ? ((event->pattern & arg1) != 0)
                : ((event->pattern & arg1) == arg1);
            if (matches) {
                if (arg3 != 0 && (arg3 & 3u) == 0 &&
                    mips_get_ptr_range(cpu, arg3, sizeof(uint32_t)) != NULL) {
                    mips_write32(cpu, arg3, event->pattern);
                }
                if ((arg2 & 0x20u) != 0) event->pattern &= ~arg1;
                if ((arg2 & 0x10u) != 0) event->pattern = 0;
                hle_thread_update_event_flag_pattern(
                    (int)event->psp_evid, event->pattern);
                pthread_mutex_unlock(&event->mutex);
                return_val = 0;
                break;
            }

            uint32_t waiting_count = 0;
            for (int i = 0; i < MAX_THREADS; i++) {
                if (threads[i].state == THREAD_STATE_WAITING &&
                    threads[i].wait_type == THREAD_WAIT_EVENT_FLAG &&
                    threads[i].wait_event_flag_id == (int)event->psp_evid) {
                    waiting_count++;
                }
            }
            if (waiting_count != 0 &&
                (event->attributes & 0x200u) == 0) {
                pthread_mutex_unlock(&event->mutex);
                return_val = SCE_KERNEL_ERROR_EVF_MULTI;
                break;
            }

            uint32_t timeout_us = UINT32_MAX;
            if (timeout_addr != 0 &&
                mips_get_ptr_range(cpu, timeout_addr, sizeof(uint32_t)) != NULL) {
                timeout_us = mips_read32(cpu, timeout_addr);
            }
            if (timeout_us == 0) {
                pthread_mutex_unlock(&event->mutex);
                return_val = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                break;
            }
            uint64_t deadline_us = 0;
            if (timeout_us != UINT32_MAX &&
                !hle_scheduler_deadline_after_us(timeout_us, &deadline_us)) {
                pthread_mutex_unlock(&event->mutex);
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
                break;
            }
            uint32_t output_addr = timeout_us != 0 ? arg3 : 0;
            if (!hle_thread_wait_event_flag(
                    cpu, (int)event->psp_evid, arg1, arg2,
                    event->pattern, output_addr, timeout_addr, deadline_us)) {
                pthread_mutex_unlock(&event->mutex);
                return_val = SCE_KERNEL_ERROR_UNKNOWN_THID;
                break;
            }
            pthread_mutex_unlock(&event->mutex);
            cpu->gpr[2] = 0;
            cpu->pc = cpu->gpr[31];
            cpu->next_pc = cpu->pc + 4;
            run_scheduler(cpu, THREAD_STATE_WAITING);
            return;
        }

        case 0x22242: { // sceKernelReferEventFlagStatus
            enum { PSP_EVENT_FLAG_STATUS_SIZE = 52 };
            int slot = (int)arg0 - 1;
            if (slot < 0 || slot >= MAX_EVENT_FLAGS ||
                !kernel->events[slot].is_active) {
                return_val = SCE_KERNEL_ERROR_UNKNOWN_EVFID;
                break;
            }
            if ((arg1 & 3u) != 0 ||
                mips_get_ptr_range(cpu, arg1, sizeof(uint32_t)) == NULL) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
                break;
            }
            uint32_t requested_size = mips_read32(cpu, arg1);
            if (requested_size > PSP_EVENT_FLAG_STATUS_SIZE) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_SIZE;
                break;
            }
            if (requested_size == 0) {
                return_val = 0;
                break;
            }
            hle_scheduler_wake_expired(cpu);
            uint8_t *status = (uint8_t *)mips_get_ptr_range(
                cpu, arg1, PSP_EVENT_FLAG_STATUS_SIZE);
            if (status == NULL) {
                return_val = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
                break;
            }
            HLE_EventFlag *event = &kernel->events[slot];
            pthread_mutex_lock(&event->mutex);
            uint32_t waiting_count = 0;
            for (int i = 0; i < MAX_THREADS; i++) {
                if (threads[i].state == THREAD_STATE_WAITING &&
                    threads[i].wait_type == THREAD_WAIT_EVENT_FLAG &&
                    threads[i].wait_event_flag_id == (int)event->psp_evid) {
                    waiting_count++;
                }
            }
            memset(status, 0, PSP_EVENT_FLAG_STATUS_SIZE);
            memcpy(status + 4, event->name, sizeof(event->name));
            mips_write32(cpu, arg1, PSP_EVENT_FLAG_STATUS_SIZE);
            mips_write32(cpu, arg1 + 36, event->attributes);
            mips_write32(cpu, arg1 + 40, event->initial_pattern);
            mips_write32(cpu, arg1 + 44, event->pattern);
            mips_write32(cpu, arg1 + 48, waiting_count);
            pthread_mutex_unlock(&event->mutex);
            return_val = 0;
            break;
        }

        // NOVO: sceGeEdramGetAddr - retorna o endereco base da EDRAM de
        // video (onde framebuffers/texturas residem). Ja estava mapeada
        // pelo linker (NID 0xE47E40E4) mas sem handler, caindo em "Unknown
        // Syscall". Valor real do PSP e' fixo: 0x04000000.
        case 0x33333:
            return_val = 0x04000000;
            break;

        case 0x33339: // sceGeEdramSetAddrTranslation
            return_val = mips_ge_set_addr_translation(&kernel->ge, arg0);
            break;

        // NOVO: sceGeDrawSync - espera a GE terminar de desenhar a lista
        // atual. Como a nossa GE roda de forma sincrona dentro do proprio
        // sceGeListEnqueue (mips_ge_run_list ja processa a lista inteira
        // na hora), aqui so precisa devolver sucesso - nao ha fila
        // assincrona de verdade pra esperar.
        case 0x33335:
            trace_ge_lifecycle("draw_sync", cpu, &kernel->ge,
                               arg0, arg1, arg2, arg3);
            return_val = 0; // PSP_OK
            break;

        // --- Audio OS Library (sceAudio) ---
        case 0x40001: // Mock: sceAudioChReserve
            if (kernel->audio_system != NULL) {
                return_val = mips_audio_reserve_channel((MIPS_AudioSystem *)kernel->audio_system, (int)arg0, (int)arg1, (int)arg2);
            } else {
                return_val = -1;
            }
            break;

        case 0x40002: // Mock: sceAudioChRelease
            if (kernel->audio_system != NULL) {
                return_val = mips_audio_release_channel((MIPS_AudioSystem *)kernel->audio_system, (int)arg0);
            } else {
                return_val = -1;
            }
            break;

        case 0x40003: // sceAudioOutputPannedBlocking
        case 0x40004: // sceAudioOutputPanned
            if (kernel->audio_system != NULL) {
                static unsigned int audio_output_logs = 0;
                MIPS_AudioSystem *sys = (MIPS_AudioSystem *)kernel->audio_system;
                if ((int32_t)(arg1 | arg2) < 0 ||
                    (arg1 | arg2) > 0xFFFF) {
                    return_val = SCE_AUDIO_ERROR_INVALID_VOLUME;
                    break;
                }
                if (arg0 >= AUDIO_MAX_CHANNELS) {
                    return_val = SCE_AUDIO_ERROR_INVALID_CHANNEL;
                    break;
                }
                if (!sys->channels[arg0].active) {
                    return_val = SCE_AUDIO_ERROR_CHANNEL_NOT_INIT;
                    break;
                }
                int format = sys->channels[arg0].format;
                int sample_count = sys->channels[arg0].sample_count;
                int element_len = sample_count * (format == 1 ? 2 : 1);
                if (arg3 == 0) {
                    return_val = (uint32_t)sample_count;
                    break;
                }
                const int16_t *pcm_data = (const int16_t *)mips_get_ptr_range(
                    cpu, arg3, (size_t)element_len * sizeof(*pcm_data));
                if (pcm_data != NULL) {
                    if (sys->silent || syscall_id == 0x40004) {
                        int pushed = mips_audio_output_panned(
                            sys, (int)arg0, (int)arg1, (int)arg2, pcm_data, element_len);
                        if (pushed < 0) {
                            return_val = (uint32_t)pushed;
                        } else if (pushed == element_len) {
                            return_val = (uint32_t)sample_count;
                        } else {
                            return_val = SCE_AUDIO_ERROR_CHANNEL_BUSY;
                        }
                    } else {
                        int pushed = 0;
                        while (pushed < element_len && !cpu->exit_requested) {
                            int ret = mips_audio_output_panned(
                                sys, (int)arg0, (int)arg1, (int)arg2,
                                pcm_data + pushed, element_len - pushed);
                            if (ret > 0) {
                                pushed += ret;
                            } else {
                                usleep(2000);
                            }
                        }
                        return_val = pushed == element_len
                            ? (uint32_t)sample_count
                            : SCE_AUDIO_ERROR_CHANNEL_BUSY;
                    }
                    if (audio_output_logs < 8 ||
                        (audio_output_logs % 1024) == 0) {
                        printf("[HLE Audio] Output syscall=0x%05X channel=%u "
                               "samples=%d result=%d path=%s\n",
                               syscall_id, arg0, sample_count,
                               (int32_t)return_val,
                               sys->silent ? "silent" :
                                   syscall_id == 0x40003 ? "blocking" : "queued");
                    }
                    audio_output_logs++;
                } else {
                    return_val = -2; // Pointer violation
                }
            } else {
                return_val = -1;
            }
            break;

        case 0x40005: // sceAudioSetChannelDataLen
            if (kernel->audio_system != NULL) {
                return_val = mips_audio_set_channel_length(
                    (MIPS_AudioSystem *)kernel->audio_system,
                    (int)arg0, (int)arg1);
            } else {
                return_val = -1;
            }
            break;

        case MIPS_HLE_AUDIOCODEC_CHECK_NEED_MEM: {
            uint8_t *context = mips_get_ptr_range(
                cpu, arg0, AUDIOCODEC_CONTEXT_SIZE);
            int32_t codec = (int32_t)arg1;
            if (codec < 0x1000 || codec >= 0x1006) {
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
            } else if (context == NULL) {
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
            } else {
                uint32_t needed_mem = 0;
                bool invalid_format = false;
                switch (codec) {
                case 0x1000:
                    needed_mem = 0x7BC0;
                    if (context[0x28] != 0x28 || context[0x29] != 0x5C) {
                        mips_write32(cpu, arg0 + 0x10, needed_mem);
                        mips_write32(cpu, arg0 + 0x08, 0x20F);
                        return_val = SCE_AVCODEC_ERROR_INVALID_DATA;
                        invalid_format = true;
                        break;
                    }
                    break;
                case 0x1001:
                    needed_mem = 0x3DE0;
                    break;
                case 0x1002:
                    needed_mem = 0x3B68;
                    break;
                default:
                    break;
                }
                if (!invalid_format) {
                    if (codec <= 0x1002) {
                        mips_write32(cpu, arg0 + 0x10, needed_mem);
                    }
                    mips_write32(cpu, arg0 + 0x08, 0);
                    mips_write32(cpu, arg0, 0x05100601);
                    return_val = 0;
                }
            }
            break;
        }

        case MIPS_HLE_AUDIOCODEC_GET_EDRAM: {
            uint8_t *context = mips_get_ptr_range(
                cpu, arg0, AUDIOCODEC_CONTEXT_SIZE);
            if (context == NULL) {
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
            } else {
                uint32_t alloc_mem = arg1 == 0x1002
                    ? 0x001B3124 : 0x0018EA90;
                uint32_t edram_addr = (alloc_mem + 0x3F) & ~0x3Fu;
                mips_write32(cpu, arg0 + 0x68, alloc_mem);
                mips_write32(cpu, arg0 + 0x0C, edram_addr);
                return_val = 0;
            }
            break;
        }

        case MIPS_HLE_SAS_INIT: {
            uint64_t sample_rate_addr = (uint64_t)cpu->gpr[29] + 16;
            void *context = mips_get_ptr_range(cpu, arg0, 1);
            // This EBOOT passes sampleRate in $t0; standard callers use stack arg 5.
            uint32_t sample_rate = cpu->gpr[8];
            if (sample_rate != 44100) {
                if (sample_rate_addr > UINT32_MAX ||
                    mips_get_ptr_range(cpu, (uint32_t)sample_rate_addr,
                                       sizeof(uint32_t)) == NULL) {
                    return_val = SCE_SAS_ERROR_INVALID_PARAMETER;
                    break;
                }
                sample_rate = mips_read32(cpu, (uint32_t)sample_rate_addr);
            }
            if (context == NULL || (arg0 & 0x3F) != 0) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
            } else if (arg2 == 0 || arg2 > 32) {
                return_val = SCE_SAS_ERROR_INVALID_MAX_VOICES;
            } else if (arg1 < 0x40 || arg1 > 0x800 || (arg1 & 0x1F) != 0) {
                return_val = SCE_SAS_ERROR_INVALID_GRAIN;
            } else if (arg3 > 1) {
                return_val = SCE_SAS_ERROR_INVALID_OUTPUT_MODE;
            } else if (sample_rate != 44100) {
                return_val = SCE_SAS_ERROR_INVALID_SAMPLE_RATE;
            } else {
                pthread_mutex_lock(&kernel->kernel_mutex);
                kernel->sas.initialized = true;
                kernel->sas.context_addr = arg0;
                kernel->sas.grain_size = arg1;
                kernel->sas.output_mode = arg3;
                kernel->sas.reverb_type = -1;
                kernel->sas.reverb_delay = 0;
                kernel->sas.reverb_feedback = 0;
                kernel->sas.reverb_left_volume = 0;
                kernel->sas.reverb_right_volume = 0;
                kernel->sas.reverb_dry_enabled = false;
                kernel->sas.reverb_wet_enabled = false;
                memset(kernel->sas.voices, 0, sizeof(kernel->sas.voices));
                pthread_mutex_unlock(&kernel->kernel_mutex);
                return_val = 0;
            }
            break;
        }

        case MIPS_HLE_SAS_CORE: {
            uint32_t grain_size;
            uint32_t output_mode;
            bool initialized;
            pthread_mutex_lock(&kernel->kernel_mutex);
            initialized = kernel->sas.initialized;
            grain_size = kernel->sas.grain_size;
            output_mode = kernel->sas.output_mode;
            pthread_mutex_unlock(&kernel->kernel_mutex);
            if (!initialized) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
                break;
            }
            size_t output_size = (size_t)grain_size * (output_mode == 1 ? 8 : 4);
            void *output = mips_get_ptr_range(cpu, arg1, output_size);
            if (output == NULL) {
                return_val = SCE_SAS_ERROR_INVALID_PARAMETER;
                break;
            }
            memset(output, 0, output_size);
            pthread_mutex_lock(&kernel->kernel_mutex);
            mips_sas_advance(&kernel->sas);
            pthread_mutex_unlock(&kernel->kernel_mutex);
            uint32_t mix_time_us = (uint32_t)(((uint64_t)grain_size *
                                               1000000ULL) / 44100ULL);
            uint64_t deadline_us = 0;
            if (!hle_scheduler_deadline_after_us(mix_time_us, &deadline_us)) {
                return_val = SCE_KERNEL_ERROR_BAD_ARGUMENT;
                break;
            }
            return_val = 0;
            cpu->gpr[2] = return_val;
            cpu->pc = cpu->gpr[31];
            cpu->next_pc = cpu->pc + 4;
            if (!hle_thread_wait_delay(deadline_us)) {
                cpu->gpr[2] = SCE_KERNEL_ERROR_UNKNOWN_THID;
                return;
            }
            run_scheduler(cpu, THREAD_STATE_WAITING);
            return;
        }

        case MIPS_HLE_SAS_END_FLAG:
            pthread_mutex_lock(&kernel->kernel_mutex);
            return_val = mips_sas_get_end_flag(&kernel->sas);
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_SET_VOICE: {
            uint64_t loop_addr = (uint64_t)cpu->gpr[29] + 16;
            if (loop_addr > UINT32_MAX ||
                mips_get_ptr_range(cpu, (uint32_t)loop_addr, sizeof(uint32_t)) == NULL) {
                return_val = SCE_SAS_ERROR_INVALID_PARAMETER;
                break;
            }
            uint32_t loop = mips_read32(cpu, (uint32_t)loop_addr);
            int32_t sample_size = (int32_t)arg3;
            uint32_t address_check_size = sample_size > 0 ? (uint32_t)sample_size : 1;
            if (mips_get_ptr_range(cpu, arg2, address_check_size) == NULL) {
                return_val = SCE_SAS_ERROR_INVALID_PARAMETER;
                break;
            }
            pthread_mutex_lock(&kernel->kernel_mutex);
            return_val = mips_sas_set_voice(&kernel->sas, arg1, arg2,
                                            sample_size, loop);
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;
        }

        case MIPS_HLE_SAS_KEY_ON:
            pthread_mutex_lock(&kernel->kernel_mutex);
            return_val = mips_sas_key_on(&kernel->sas, arg1);
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_GET_OUTPUT_MODE:
            pthread_mutex_lock(&kernel->kernel_mutex);
            return_val = kernel->sas.initialized
                ? kernel->sas.output_mode : SCE_SAS_ERROR_BAD_ADDRESS;
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_REV_TYPE:
            if ((int32_t)arg1 < -1 || (int32_t)arg1 > 8) {
                return_val = SCE_SAS_ERROR_REV_INVALID_TYPE;
                break;
            }
            pthread_mutex_lock(&kernel->kernel_mutex);
            if (!kernel->sas.initialized) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
            } else {
                kernel->sas.reverb_type = (int32_t)arg1;
                return_val = 0;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_REV_PARAM:
            if (arg1 >= 128) {
                return_val = SCE_SAS_ERROR_REV_INVALID_DELAY;
                break;
            }
            if (arg2 >= 128) {
                return_val = SCE_SAS_ERROR_REV_INVALID_FEEDBACK;
                break;
            }
            pthread_mutex_lock(&kernel->kernel_mutex);
            if (!kernel->sas.initialized) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
            } else {
                kernel->sas.reverb_delay = arg1;
                kernel->sas.reverb_feedback = arg2;
                return_val = 0;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_REV_VON:
            pthread_mutex_lock(&kernel->kernel_mutex);
            if (!kernel->sas.initialized) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
            } else {
                kernel->sas.reverb_dry_enabled = arg1 != 0;
                kernel->sas.reverb_wet_enabled = arg2 != 0;
                return_val = 0;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;

        case MIPS_HLE_SAS_REV_EVOL:
            if (arg1 > 0x1000 || arg2 > 0x1000) {
                return_val = SCE_SAS_ERROR_REV_INVALID_VOLUME;
                break;
            }
            pthread_mutex_lock(&kernel->kernel_mutex);
            if (!kernel->sas.initialized) {
                return_val = SCE_SAS_ERROR_BAD_ADDRESS;
            } else {
                kernel->sas.reverb_left_volume = arg1;
                kernel->sas.reverb_right_volume = arg2;
                return_val = 0;
            }
            pthread_mutex_unlock(&kernel->kernel_mutex);
            break;
            
        default:
            printf("[HLE Warning] Unknown Syscall NID Hash 0x%05X triggered at PC: 0x%08X\n", syscall_id, cpu->pc);
            return_val = -1;
            break;
    }
    
        // Store returned result in $v0 [Allegrex Calling Convention]
    cpu->gpr[2] = return_val;
    
    // FIX: o stub gravado por mips_linker.c e' so "syscall; nop" - nao tem
    // nenhum 'jr $ra' de verdade em lugar nenhum. Avancar so +4 fazia o
    // interpretador continuar executando o nop e depois cair direto no resto
    // da tabela de stubs original (dados/NIDs, nao codigo) como se fosse
    // instrucao valida, ate um jr/jalr com registrador sujo produzir um PC
    // invalido (foi assim que "0x001D976E" apareceu logo apos o sceIoOpen).
    // O retorno tem que ser pro chamador ($ra), igual TODO outro handler HLE
    // do projeto ja faz (ver sceSysMem.c, sceKernelThread.c). Isso vale pra
    // qualquer branch do switch acima, inclusive o 'default' de syscall
    // desconhecida - nao ha motivo pra deixar essas continuarem pra frente.
    cpu->pc = cpu->gpr[0x1F]; // $ra
    cpu->next_pc = cpu->pc + 4;
}


// Global default weak implementation of link handler
__attribute__((weak)) void hle_syscall(MIPS_CPU *cpu) {
    extern MIPS_HLE_Kernel kernel;
    mips_hle_syscall(cpu, &kernel);
}
