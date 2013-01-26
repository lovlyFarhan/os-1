#include <stdint.h>

#include <muos/arch.h>
#include <muos/spinlock.h>

#include <kernel/assert.h>
#include <kernel/list.hpp>
#include <kernel/once.h>
#include <kernel/tree-map.hpp>
#include <kernel/vm.hpp>

#include "object-cache-internal.hpp"

/*
Used by large-object caches to get slab's not hosted inside the slab
storage. Prevents terrible space waste for large (>= PAGE_SIZE / 2)
objects.
*/
struct ObjectCache slabs_cache;
Spinlock_t         slabs_cache_lock = SPINLOCK_INIT;

static Once_t init_control = ONCE_INIT;

void init_slabs_cache (void * param)
{
    ObjectCacheInit(&slabs_cache, sizeof(struct Slab));
}

static void static_init ()
{
    Once(&init_control, init_slabs_cache, NULL);
}

static void constructor (struct ObjectCache * cache)
{
    cache->bufctl_to_slab_map = 0;
}

static void destructor (struct ObjectCache * cache)
{
    if (cache->bufctl_to_slab_map) {
        delete cache->bufctl_to_slab_map;
        cache->bufctl_to_slab_map = 0;
    }
}

static struct Slab * large_objects_try_allocate_slab (struct ObjectCache * cache)
{
    Page *          new_page;
    struct Slab *   new_slab;
    unsigned int    objs_in_slab;
    unsigned int    i;

    /* Lazily allocate the auxiliary map used to record which slab owns a particular object */
    if (!cache->bufctl_to_slab_map) {
        cache->bufctl_to_slab_map = new ObjectCache::BufctlToSlabMap_t(ObjectCache::BufctlToSlabMap_t::AddressCompareFunc);
        assert(cache->bufctl_to_slab_map != 0);
    }

    new_page = Page::Alloc();

    if (!new_page) {
        return NULL;
    }

    SpinlockLock(&slabs_cache_lock);
    new_slab = (struct Slab *)ObjectCacheAlloc(&slabs_cache);
    SpinlockUnlock(&slabs_cache_lock);

    if (!new_slab) {
        Page::Free(new_page);
        return NULL;
    }

    InitSlab(new_slab);
    new_slab->page = new_page;

    objs_in_slab = PAGE_SIZE / cache->element_size;

    /* Carve out (PAGE_SIZE / element_size) individual buffers. */
    for (i = 0; i < objs_in_slab; ++i) {
        VmAddr_t        buf_base;
        struct Bufctl * new_bufctl;

        buf_base = new_page->base_address + cache->element_size * i;
        new_bufctl = (struct Bufctl *)buf_base;
        InitBufctl(new_bufctl);

        /* Record controlling slab's location in auxiliary map */
        cache->bufctl_to_slab_map->Insert(new_bufctl, new_slab);
        assert(cache->bufctl_to_slab_map->Lookup(new_bufctl) == new_slab);

        /* Now insert into freelist */
        new_slab->freelist_head.Append(new_bufctl);
    }

    return new_slab;
}

static void large_objects_free_slab (struct ObjectCache * cache, struct Slab * slab)
{
    typedef List<Bufctl, &Bufctl::freelist_link> list_t;

    if (slab->refcount == 0) {
        /* Unlink this slab from the cache's list */
        List<Slab, &Slab::cache_link>::Remove(slab);

        /*
        There's no need to deconstruct each separate bufctl object contained
        in the freelist. They all live inside the storage of the VM page
        that we're about to free anyway.

        But we do need to iterate the list and remove the bufctl-to-slab mapping
        entry for each bufctl.
        */
        for (list_t::Iterator cursor = slab->freelist_head.Begin(); cursor; cursor++) {
            
            struct Slab * removed;

            assert(cache->bufctl_to_slab_map != 0);
            removed = cache->bufctl_to_slab_map->Remove(*cursor);
            assert(removed != NULL);
        }

        /* Release the page that stored the user buffers */
        Page::Free(slab->page);

        /* Finally free the slab, which is allocated from an object cache. */
        SpinlockLock(&slabs_cache_lock);
        ObjectCacheFree(&slabs_cache, slab);
        SpinlockUnlock(&slabs_cache_lock);
    }
}

static struct Slab * large_objects_slab_from_bufctl (
        struct ObjectCache * cache,
        void * bufctl_addr
        )
{
    assert(cache->bufctl_to_slab_map != 0);
    return cache->bufctl_to_slab_map->Lookup(bufctl_addr);
}

const struct ObjectCacheOps large_objects_ops = {
    /* StaticInit       */  static_init,
    /* Constructor      */  constructor,
    /* Destructor       */  destructor,
    /* TryAllocateSlab  */  large_objects_try_allocate_slab,
    /* TryFreeSlab      */  large_objects_free_slab,
    /* MapBufctlToSlab  */  large_objects_slab_from_bufctl,
};
