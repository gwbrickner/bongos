/* See bootvideo.h. */
#include "include/bootvideo.h"

#include "include/bootmem.h"

/* `mask` shifted so its lowest set bit sits at bit 0 is a contiguous run of 1s iff
 * `shifted & (shifted + 1) == 0` (e.g. 0b0111 + 1 = 0b1000, AND is 0; 0b0101 + 1 = 0b0110, AND is
 * nonzero). Requires mask != 0. */
static bool maskIsContiguous(uint32_t mask) {
    uint32_t shifted = mask >> __builtin_ctz(mask);
    return (shifted & (shifted + 1)) == 0;
}

bool bootVideoAccept(const BootVideoMode *m) {
    if (m->width == 0 || m->pitch < m->width * 4u || m->pitch % 4u != 0) {
        return false;
    }
    uint32_t r = m->redMask, g = m->greenMask, b = m->blueMask;
    if (r == 0 || g == 0 || b == 0) {
        return false;
    }
    if (!maskIsContiguous(r) || !maskIsContiguous(g) || !maskIsContiguous(b)) {
        return false;
    }
    if (__builtin_popcount(r) > 8 || __builtin_popcount(g) > 8 || __builtin_popcount(b) > 8) {
        return false;
    }
    if ((r & g) != 0 || (r & b) != 0 || (g & b) != 0) { /* D-109: channels must not overlap */
        return false;
    }
    uint32_t highest = r | g | b | m->reservedMask;
    if (highest == 0) {
        return false;
    }
    int top = 31 - __builtin_clz(highest);
    return top >= 24 && top <= 31;
}

void bootVideoPickerInit(BootVideoPicker *p, uint32_t resWidth, uint32_t resHeight) {
    bootMemset(p, 0, sizeof(*p));
    p->resWidth = resWidth;
    p->resHeight = resHeight;
}

void bootVideoPickerOffer(BootVideoPicker *p, const BootVideoMode *m) {
    if (p->resWidth != 0 && p->resHeight != 0 && m->width == p->resWidth &&
        m->height == p->resHeight) {
        if (!p->haveExact || m->id < p->bestExact.id) {
            p->bestExact = *m;
            p->haveExact = true;
        }
    }

    if (m->width > BOOT_VIDEO_MAX_WIDTH || m->height > BOOT_VIDEO_MAX_HEIGHT) {
        return;
    }
    uint64_t area = (uint64_t)m->width * (uint64_t)m->height;
    uint64_t bestArea = (uint64_t)p->bestAuto.width * (uint64_t)p->bestAuto.height;
    if (!p->haveAuto || area > bestArea ||
        (area == bestArea && (m->width > p->bestAuto.width ||
                              (m->width == p->bestAuto.width && m->id < p->bestAuto.id)))) {
        p->bestAuto = *m;
        p->haveAuto = true;
    }
}

bool bootVideoPickerResult(const BootVideoPicker *p, BootVideoMode *out, bool *exactFellBack) {
    bool wantExact = p->resWidth != 0 && p->resHeight != 0;
    if (wantExact && p->haveExact) {
        *out = p->bestExact;
        *exactFellBack = false;
        return true;
    }
    if (p->haveAuto) {
        *out = p->bestAuto;
        *exactFellBack = wantExact;
        return true;
    }
    return false;
}

void bootVideoToFramebuffer(const BootVideoMode *m, BootFramebuffer *fb) {
    bootMemset(fb, 0, sizeof(*fb));
    fb->phys = m->fbPhys;
    fb->width = m->width;
    fb->height = m->height;
    fb->pitch = m->pitch;
    fb->bpp = 32;
    fb->redShift = (uint8_t)__builtin_ctz(m->redMask);
    fb->redSize = (uint8_t)__builtin_popcount(m->redMask);
    fb->greenShift = (uint8_t)__builtin_ctz(m->greenMask);
    fb->greenSize = (uint8_t)__builtin_popcount(m->greenMask);
    fb->blueShift = (uint8_t)__builtin_ctz(m->blueMask);
    fb->blueSize = (uint8_t)__builtin_popcount(m->blueMask);
}
