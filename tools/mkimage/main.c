/* tools/mkimage: builds build/bongos.img (ARCHITECTURE §5.1, ROADMAP M1.2). A host tool, built
 * with the host's own clang (ARCHITECTURE §0's "host-side tools" exception covers mkfs.fat and
 * mtools, which this shells out to for the FAT32 ESP; the GPT/MBR layout itself is our own code,
 * see gpt.c). */
/* -std=c17 alone hides glibc's POSIX declarations (setenv, pwrite, ftruncate); this is host
 * tooling, not the freestanding kernel/loader C17 subset ARCHITECTURE §4 restricts. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "branding.h"
#include "gpt.h"

#define SECTOR_SIZE 512
#define MIB         (1024ULL * 1024ULL)
#define ALIGN_LBA   2048ULL /* 1 MiB, the conventional GPT partition alignment */
/* A degenerate (near-zero) root partition is a config error worth catching here rather than
 * shipping an image whose root can't hold a boot.cfg-sized initrd, let alone bongfs later. */
#define MIN_ROOT_SECTORS 2048ULL /* 1 MiB */
/* FAT32 needs >= 65525 data clusters (else mkfs.fat legally formats it as FAT16 instead, which a
 * strict UEFI implementation may refuse to boot from). This is a coarse safety floor, not an
 * exact bound -- mkfs.fat picks the cluster size itself and its own error is the real check --
 * but it catches an obviously-too-small --esp-mib before spending a build cycle on mkfs.fat's
 * error message. ARCHITECTURE §5.1's 256 MiB default stays comfortably above it. */
#define MIN_ESP_MIB 33ULL

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
    if (opt->biosBootMib < 1) {
        fprintf(stderr, "mkimage: --bios-boot-mib must be at least 1\n");
        return -1;
    }
    if (opt->espMib < MIN_ESP_MIB) {
        fprintf(stderr, "mkimage: --esp-mib must be at least %llu (FAT32's 65525-cluster floor)\n",
                (unsigned long long)MIN_ESP_MIB);
        return -1;
    }
    if (opt->sizeMib > (UINT64_MAX / MIB)) {
        fprintf(stderr, "mkimage: --size-mib %llu overflows a byte count\n",
                (unsigned long long)opt->sizeMib);
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
 * (from opt->efi) and, if given, /bong/{boot.cfg,kernel.elf,initrd.img}. `espStart` becomes the
 * partition's hidden-sector count (-h): some tools, and our own future BIOS FAT32 reader, read
 * that field rather than assuming 0. */
static void buildEspImage(const Options *opt, const char *espPath, uint64_t espSectors,
                          uint64_t espStart) {
    /* mtools otherwise refuses to touch a file whose apparent geometry doesn't match its BPB,
     * which our own tooling (and QEMU's raw block device) doesn't care about. */
    setenv("MTOOLS_SKIP_CHECK", "1", 1);

    /* mkfs.fat's -C creates the target file, but refuses if one already exists -- clear out a
     * stale file left by a prior interrupted run first. */
    unlink(espPath);

    uint64_t espKib = (espSectors * SECTOR_SIZE) / 1024;
    char kibArg[32];
    snprintf(kibArg, sizeof(kibArg), "%llu", (unsigned long long)espKib);
    char hiddenArg[32];
    snprintf(hiddenArg, sizeof(hiddenArg), "%llu", (unsigned long long)espStart);
    char *mkfsArgv[] = {"mkfs.fat",      "-C",   "-F",      "32", "-S",
                        "512",           "-h",   hiddenArg, "-n", "EFI_SYSTEM",
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

/* Writes `length` bytes at `offset`; exits on any short write or I/O error, since a partial
 * image is worse than no image. */
static void pwriteExact(int fd, const void *buf, size_t length, off_t offset) {
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < length) {
        ssize_t n = pwrite(fd, p + done, length - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "mkimage: pwrite failed: %s\n", strerror(errno));
            exit(1);
        }
        done += (size_t)n;
    }
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
    /* Root fills whatever is left (ARCHITECTURE §5.1), aligned down to a 1 MiB boundary like
     * every other partition edge -- an unaligned end wastes nothing functionally, but every real
     * partitioning tool (and bongfs's own 4 KiB block / XTS data-unit alignment later, ARCHITECTURE
     * §15.1/§15.2) expects it, so `sgdisk -v` stays clean. */
    uint64_t rootEnd = ((lastUsable + 1) / ALIGN_LBA) * ALIGN_LBA - 1;
    if (rootStart + MIN_ROOT_SECTORS > rootEnd + 1) {
        fprintf(stderr,
                "mkimage: --size-mib %llu is too small for a %llu MiB BIOS boot partition + "
                "%llu MiB ESP + a usable root partition\n",
                (unsigned long long)opt.sizeMib, (unsigned long long)opt.biosBootMib,
                (unsigned long long)opt.espMib);
        return 1;
    }

    char outputTmpPath[4096];
    snprintf(outputTmpPath, sizeof(outputTmpPath), "%s.tmp", opt.output);
    char espTmpPath[4096];
    snprintf(espTmpPath, sizeof(espTmpPath), "%s.esp.tmp", opt.output);
    buildEspImage(&opt, espTmpPath, espSectors, espStart);

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

    /* Build the full metadata region (protective MBR + primary header/array) plus the backup
     * region in one contiguous in-memory image, same as before; only the final write below
     * changed, to leave the (large, empty) BIOS boot and root partitions as holes on disk rather
     * than writing real zero bytes across the whole 2 GiB. */
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

    char rootName[36];
    snprintf(rootName, sizeof(rootName), "%s root", BRANDING_NAME);

    GptPartitionSpec partitions[3] = {
        {GPT_GUID_BIOS_BOOT, biosBootGuid, biosBootStart, biosBootEnd, "BIOS_BOOT"},
        {GPT_GUID_ESP, espGuid, espStart, espEnd, "EFI_SYSTEM"},
        {GPT_TYPE_GUID_ROOT, rootGuid, rootStart, rootEnd, rootName},
    };
    gptWriteLayout(image, totalSectors, &diskGuid, partitions, 3);
    memcpy(image + espStart * SECTOR_SIZE, espData, (size_t)(espSectors * SECTOR_SIZE));
    free(espData);
    /* The BIOS boot and root partitions stay zeroed: BIOS stage1/stage2 land in M2.5, and root
     * has no filesystem until bongfs (M7.6-M7.7) -- ARCHITECTURE §5.1 says the initrd stands in
     * for root until then. */

    unlink(outputTmpPath);
    int fd = open(outputTmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "mkimage: cannot create %s: %s\n", outputTmpPath, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)(totalSectors * SECTOR_SIZE)) != 0) {
        fprintf(stderr, "mkimage: ftruncate %s failed: %s\n", outputTmpPath, strerror(errno));
        return 1;
    }

    uint64_t backupArrayLba = lastUsable + 1;
    uint64_t backupRegionSectors = totalSectors - backupArrayLba; /* array + backup header */
    uint64_t primaryMetadataSectors = gptFirstUsableLba();        /* protective MBR + LBA1-33 */

    pwriteExact(fd, image, (size_t)(primaryMetadataSectors * SECTOR_SIZE), 0);
    pwriteExact(fd, image + espStart * SECTOR_SIZE, (size_t)(espSectors * SECTOR_SIZE),
                (off_t)(espStart * SECTOR_SIZE));
    pwriteExact(fd, image + backupArrayLba * SECTOR_SIZE,
                (size_t)(backupRegionSectors * SECTOR_SIZE), (off_t)(backupArrayLba * SECTOR_SIZE));
    free(image);

    if (close(fd) != 0) {
        fprintf(stderr, "mkimage: close %s failed: %s\n", outputTmpPath, strerror(errno));
        return 1;
    }
    if (rename(outputTmpPath, opt.output) != 0) {
        fprintf(stderr, "mkimage: rename %s -> %s failed: %s\n", outputTmpPath, opt.output,
                strerror(errno));
        return 1;
    }

    printf("mkimage: wrote %s (%llu MiB): BIOS boot [%llu-%llu], ESP [%llu-%llu], root "
           "[%llu-%llu]\n",
           opt.output, (unsigned long long)opt.sizeMib, (unsigned long long)biosBootStart,
           (unsigned long long)biosBootEnd, (unsigned long long)espStart,
           (unsigned long long)espEnd, (unsigned long long)rootStart, (unsigned long long)rootEnd);
    return 0;
}
