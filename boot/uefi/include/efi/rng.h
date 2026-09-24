/* EFI_RNG_PROTOCOL, UEFI Spec 2.10 §37.6 (formerly the separate "UEFI RNG Protocol" spec). Used
 * from M1.3 on to seed BootInfo.randomSeed (ARCHITECTURE §5.5 step 7), falling back to
 * RDSEED/RDRAND/TSC jitter when a platform doesn't implement it. */
#ifndef EFI_RNG_H
#define EFI_RNG_H

#include "base.h"

#define EFI_RNG_PROTOCOL_GUID                                                                      \
    {                                                                                              \
        0x3152bca5, 0xeade, 0x433d, {                                                              \
            0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44                                         \
        }                                                                                          \
    }

/* RNG algorithm GUIDs the loader recognizes; GetRNG(NULL, ...) asks for the platform default,
 * which is all M1.3 needs. */
#define EFI_RNG_ALGORITHM_RAW                                                                      \
    {                                                                                              \
        0xe43176d7, 0xb6e8, 0x4827, {                                                              \
            0xb7, 0x84, 0x7f, 0xfd, 0xc4, 0xb6, 0x85, 0x61                                         \
        }                                                                                          \
    }

typedef EFI_GUID EFI_RNG_ALGORITHM;

typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_RNG_GET_INFO)(IN EFI_RNG_PROTOCOL *This,
                                             IN OUT UINTN *RNGAlgorithmListSize,
                                             OUT EFI_RNG_ALGORITHM *RNGAlgorithmList);
typedef EFI_STATUS(EFIAPI *EFI_RNG_GET_RNG)(IN EFI_RNG_PROTOCOL *This,
                                            IN EFI_RNG_ALGORITHM *RNGAlgorithm OPTIONAL,
                                            IN UINTN RNGValueLength, OUT UINT8 *RNGValue);

struct EFI_RNG_PROTOCOL {
    EFI_RNG_GET_INFO GetInfo;
    EFI_RNG_GET_RNG GetRNG;
};

#endif
