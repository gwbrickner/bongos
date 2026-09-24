/* tools/mkimage: builds build/bongos.img (ARCHITECTURE §5.1, ROADMAP M1.2). A host tool, built
 * with the host's own clang (ARCHITECTURE §0's "host-side tools" exception covers mkfs.fat and
 * mtools, which this shells out to for the FAT32 ESP; the GPT/MBR layout itself is our own code,
 * see gpt.c). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gpt.h"

#define SECTOR_SIZE 512
#define MIB         (1024ULL * 1024ULL)
#define ALIGN_LBA   2048ULL /* 1 MiB, the conventional GPT partition alignment */

typedef struct {
    const char *output;
    const char *efi;
    const char *bootCfg;
    const char *kernel;
    const char *initrd;
    uint64_t sizeMib;
    uint64_t espMib;
    uint64_t biosBootMib;
} Options;

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --output PATH --efi PATH [--boot-cfg PATH] [--kernel PATH] "
            "[--initrd PATH] [--size-mib N] [--esp-mib N] [--bios-boot-mib N]\n"
            "  --output PATH        disk image to write (default 2 GiB, GPT: BIOS boot + ESP + "
            "root, ARCHITECTURE §5.1)\n"
            "  --efi PATH            BOOTX64.EFI to place at /EFI/BOOT/BOOTX64.EFI on the ESP\n"
            "  --boot-cfg/--kernel/--initrd PATH   placed at /bong/{boot.cfg,kernel.elf,"
            "initrd.img} on the ESP (M1.3+; omit if they don't exist yet)\n",
            argv0);
}

static uint64_t parseUint(const char *s, const char *argName) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        fprintf(stderr, "mkimage: %s must be a positive integer, got \"%s\"\n", argName, s);
        exit(1);
    }
    return (uint64_t)v;
}

static int parseOptions(int argc, char **argv, Options *opt) {
    opt->output = NULL;
    opt->efi = NULL;
    opt->bootCfg = NULL;
    opt->kernel = NULL;
    opt->initrd = NULL;
    opt->sizeMib = 2048;
    opt->espMib = 256;
    opt->biosBootMib = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            opt->output = argv[++i];
        } else if (strcmp(argv[i], "--efi") == 0 && i + 1 < argc) {
            opt->efi = argv[++i];
        } else if (strcmp(argv[i], "--boot-cfg") == 0 && i + 1 < argc) {
            opt->bootCfg = argv[++i];
        } else if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
            opt->kernel = argv[++i];
        } else if (strcmp(argv[i], "--initrd") == 0 && i + 1 < argc) {
            opt->initrd = argv[++i];
        } else if (strcmp(argv[i], "--size-mib") == 0 && i + 1 < argc) {
            opt->sizeMib = parseUint(argv[++i], "--size-mib");
        } else if (strcmp(argv[i], "--esp-mib") == 0 && i + 1 < argc) {
            opt->espMib = parseUint(argv[++i], "--esp-mib");
        } else if (strcmp(argv[i], "--bios-boot-mib") == 0 && i + 1 < argc) {
            opt->biosBootMib = parseUint(argv[++i], "--bios-boot-mib");
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "mkimage: unrecognized argument \"%s\"\n", argv[i]);
            return -1;
        }
    }
    if (opt->output == NULL || opt->efi == NULL) {
        fprintf(stderr, "mkimage: --output and --efi are required\n");
        return -1;
    }
    return 0;
}

/* Runs argv[0] with the given arguments (NULL-terminated), with no shell involved, and exits the
 * whole program if it doesn't return status 0 -- every caller here is a build step where a
 * partial failure must not produce a silently-broken image. */
static void runCommand(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "mkimage: fork failed: %s\n", strerror(errno));
        exit(1);
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "mkimage: exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "mkimage: waitpid failed: %s\n", strerror(errno));
        exit(1);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "mkimage: %s failed\n", argv[0]);
        exit(1);
    }
}

/* Builds a FAT32 ESP image at `espPath`, `espSectors` sectors, containing /EFI/BOOT/BOOTX64.EFI
 * (from opt->efi) and, if given, /bong/{boot.cfg,kernel.elf,initrd.img}. */
static void buildEspImage(const Options *opt, const char *espPath, uint64_t espSectors) {
    uint64_t espKib = (espSectors * SECTOR_SIZE) / 1024;
    char kibArg[32];
    snprintf(kibArg, sizeof(kibArg), "%llu", (unsigned long long)espKib);
    char *mkfsArgv[] = {"mkfs.fat",      "-C",   "-F", "32", "-n", "EFI_SYSTEM",
                        (char *)espPath, kibArg, NULL};
    runCommand(mkfsArgv);

    char *mmdEfi[] = {"mmd", "-i", (char *)espPath, "::/EFI", NULL};
    runCommand(mmdEfi);
    char *mmdEfiBoot[] = {"mmd", "-i", (char *)espPath, "::/EFI/BOOT", NULL};
    runCommand(mmdEfiBoot);
    char *mcopyEfi[] = {"mcopy", "-i", (char *)espPath, (char *)opt->efi, "::/EFI/BOOT/BOOTX64.EFI",
                        NULL};
    runCommand(mcopyEfi);

    if (opt->bootCfg != NULL || opt->kernel != NULL || opt->initrd != NULL) {
        char *mmdBong[] = {"mmd", "-i", (char *)espPath, "::/bong", NULL};
        runCommand(mmdBong);
    }
    if (opt->bootCfg != NULL) {
        char *mcopy[] = {"mcopy", "-i", (char *)espPath, (char *)opt->bootCfg, "::/bong/boot.cfg",
                         NULL};
        runCommand(mcopy);
    }
    if (opt->kernel != NULL) {
        char *mcopy[] = {"mcopy", "-i", (char *)espPath, (char *)opt->kernel, "::/bong/kernel.elf",
                         NULL};
        runCommand(mcopy);
    }
    if (opt->initrd != NULL) {
        char *mcopy[] = {"mcopy", "-i", (char *)espPath, (char *)opt->initrd, "::/bong/initrd.img",
                         NULL};
        runCommand(mcopy);
    }
}

static uint64_t alignUp(uint64_t lba, uint64_t alignment) {
    return ((lba + alignment - 1) / alignment) * alignment;
}

int main(int argc, char **argv) {
    Options opt;
    if (parseOptions(argc, argv, &opt) != 0) {
        usage(argv[0]);
        return 1;
    }

    uint64_t totalSectors = (opt.sizeMib * MIB) / SECTOR_SIZE;
    uint64_t biosBootSectors = (opt.biosBootMib * MIB) / SECTOR_SIZE;
    uint64_t espSectors = (opt.espMib * MIB) / SECTOR_SIZE;

    uint64_t biosBootStart = alignUp(gptFirstUsableLba(), ALIGN_LBA);
    uint64_t biosBootEnd = biosBootStart + biosBootSectors - 1;
    uint64_t espStart = alignUp(biosBootEnd + 1, ALIGN_LBA);
    uint64_t espEnd = espStart + espSectors - 1;
    uint64_t rootStart = alignUp(espEnd + 1, ALIGN_LBA);
    uint64_t lastUsable = gptLastUsableLba(totalSectors);
    if (rootStart > lastUsable) {
        fprintf(stderr,
                "mkimage: --size-mib %llu is too small for a %llu MiB BIOS boot partition + "
                "%llu MiB ESP\n",
                (unsigned long long)opt.sizeMib, (unsigned long long)opt.biosBootMib,
                (unsigned long long)opt.espMib);
        return 1;
    }
    uint64_t rootEnd = lastUsable; /* root fills whatever is left, ARCHITECTURE §5.1 */

    char espTmpPath[4096];
    snprintf(espTmpPath, sizeof(espTmpPath), "%s.esp.tmp", opt.output);
    buildEspImage(&opt, espTmpPath, espSectors);

    FILE *espFile = fopen(espTmpPath, "rb");
    if (espFile == NULL) {
        fprintf(stderr, "mkimage: cannot reopen %s: %s\n", espTmpPath, strerror(errno));
        return 1;
    }
    uint8_t *espData = malloc((size_t)(espSectors * SECTOR_SIZE));
    if (espData == NULL || fread(espData, 1, (size_t)(espSectors * SECTOR_SIZE), espFile) !=
                               (size_t)(espSectors * SECTOR_SIZE)) {
        fprintf(stderr, "mkimage: failed to read back %s\n", espTmpPath);
        return 1;
    }
    fclose(espFile);
    unlink(espTmpPath);

    uint8_t *image = calloc((size_t)totalSectors, SECTOR_SIZE);
    if (image == NULL) {
        fprintf(stderr, "mkimage: out of memory allocating a %llu MiB image\n",
                (unsigned long long)opt.sizeMib);
        return 1;
    }

    GptGuid diskGuid, biosBootGuid, espGuid, rootGuid;
    gptRandomGuid(&diskGuid);
    gptRandomGuid(&biosBootGuid);
    gptRandomGuid(&espGuid);
    gptRandomGuid(&rootGuid);

    GptPartitionSpec partitions[3] = {
        {GPT_GUID_BIOS_BOOT, biosBootGuid, biosBootStart, biosBootEnd, "BIOS_BOOT"},
        {GPT_GUID_ESP, espGuid, espStart, espEnd, "EFI_SYSTEM"},
        {GPT_GUID_BONGFS_ROOT, rootGuid, rootStart, rootEnd, "bongOS root"},
    };
    gptWriteLayout(image, totalSectors, &diskGuid, partitions, 3);

    memcpy(image + espStart * SECTOR_SIZE, espData, (size_t)(espSectors * SECTOR_SIZE));
    free(espData);
    /* The BIOS boot and root partitions stay zeroed: BIOS stage1/stage2 land in M2.5, and root
     * has no filesystem until bongfs (M7.6-M7.7) -- ARCHITECTURE §5.1 says the initrd stands in
     * for root until then. */

    FILE *out = fopen(opt.output, "wb");
    if (out == NULL) {
        fprintf(stderr, "mkimage: cannot create %s: %s\n", opt.output, strerror(errno));
        return 1;
    }
    if (fwrite(image, 1, (size_t)(totalSectors * SECTOR_SIZE), out) !=
        (size_t)(totalSectors * SECTOR_SIZE)) {
        fprintf(stderr, "mkimage: short write to %s\n", opt.output);
        fclose(out);
        return 1;
    }
    fclose(out);
    free(image);

    printf("mkimage: wrote %s (%llu MiB): BIOS boot [%llu-%llu], ESP [%llu-%llu], root "
           "[%llu-%llu]\n",
           opt.output, (unsigned long long)opt.sizeMib, (unsigned long long)biosBootStart,
           (unsigned long long)biosBootEnd, (unsigned long long)espStart,
           (unsigned long long)espEnd, (unsigned long long)rootStart, (unsigned long long)rootEnd);
    return 0;
}
