Early allocator, buddy pmm, slab/kmalloc, vmalloc, vmm, page cache, swap. Arrives starting M2.2.

No-recursion rule (M2.4, D-092): the pmm (`pmm.c`/`buddy.c`/`early.c`) and the vmm (`vmm.c`/
`kva.c`) never call `kmalloc`/`vmalloc`/`slabAlloc` -- the slab allocator and vmalloc are built on
top of the pmm/vmm, not the other way around, and calling back down would be circular at boot
(`slabInit()`/`vmallocInit()` run after both are already up). A cache's constructor/destructor
must not allocate from its own cache either.
