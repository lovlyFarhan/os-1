#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "arch.h"
#include "array.h"
#include "assert.h"
#include "bits.h"
#include "compiler.h"
#include "mmu.h"
#include "object-cache.h"
#include "once.h"
#include "tree-map.h"
#include "vm.h"

#define ARM_MMU_ENABLED_BIT             0
#define ARM_MMU_EXCEPTION_VECTOR_BIT    13

static struct SecondlevelTable * secondlevel_table_alloc ();
static void SecondlevelTableFree (struct SecondlevelTable *);

static inline uint32_t GetTTBR0 ()
{
    uint32_t val;

    asm volatile(
        "mrc p15, 0, %[ttbr0], c2, c2, 0"
        : [ttbr0] "=r" (val)
    );

    return val;
}

static inline void SetTTBR0 (uint32_t val)
{
    asm volatile(
        "mcr p15, 0, %[ttbr0], c2, c2, 0"
        :
        : [ttbr0] "r" (val)
    );
}

static inline uint32_t GetTTBR1 ()
{
    uint32_t val;

    asm volatile(
        "mrc p15, 0, %[ttbr1], c2, c2, 1"
        : [ttbr1] "=r" (val)
    );

    return val;
}

static inline void SetTTBR1 (uint32_t val)
{
    asm volatile(
        "mcr p15, 0, %[ttbr1], c2, c2, 1"
        :
        : [ttbr1] "r" (val)
    );
}

static inline uint32_t GetTTBC ()
{
    uint32_t val;

    asm volatile(
        "mrc p15, 0, %[reg], c2, c2, 2"
        : [reg] "=r" (val)
    );

    return val;
}

static inline void SetTTBC (uint32_t val)
{
    asm volatile(
        "mcr p15, 0, %[reg], c2, c2, 2"
        :
        : [reg] "r" (val)
    );
}

int MmuGetEnabled (void)
{
    uint32_t cp15_r1;

    /*
    p15: coprocessor number
    0: opcode 1; generally 0b000
    %0: destination ARM register
    c1: primary coprocessor register name
    c0: further specification of the coprocessor register name.
        "c0" is a placeholder here and is ignored for cp15 c1
    */
    asm volatile(
        "mrc p15, 0, %0, c1, c0"
        : "=r"(cp15_r1)
        :
        :
    );

    return TESTBIT(cp15_r1, ARM_MMU_ENABLED_BIT);
}

void MmuSetEnabled ()
{
    /* Control Register */
    uint32_t cp15_r1;

    /* Domain access control register */
    uint32_t cp15_r3;

    /*
    The number of leading bits which must be 0 for an address to fall into
    the user (as opposed to kernel) address range.
    */
    int n;

    /* Allow full access to everything in the default domain. */
    cp15_r3 = PT_DOMAIN_ACCESS_LEVEL_ALL << (2 * PT_DOMAIN_DEFAULT);

    asm volatile(
        "mcr p15, 0, %[cp15_r3], c3, c0, 0"
        :
        : [cp15_r3] "r" (cp15_r3)
    );

    /*
    Turn on VMSAv6's support for dual translation-table bases.
    */
    #define TTBC_N_MASK     0b111

    /*
    Enfoce that kernel-reserved address range starts on an even power of 2
    */
    assert(1 << (ffs(KERNEL_MODE_OFFSET) - 1) == KERNEL_MODE_OFFSET);

    n = 32 - (ffs(KERNEL_MODE_OFFSET) - 1);
    uint32_t ttbc = GetTTBC();
    ttbc &= ~TTBC_N_MASK;
    ttbc |= (n & TTBC_N_MASK);
    SetTTBC(ttbc);

    /* Read/modify/write on MMU control register. */
    asm volatile(
        "mrc p15, 0, %[cp15_r1], c1, c0"
        : [cp15_r1] "=r" (cp15_r1)
    );

    /* Turn on MMU-enable bit */
    cp15_r1 |= SETBIT(ARM_MMU_ENABLED_BIT);

    /* Turn on high-vector enable bit */
    cp15_r1 |= SETBIT(ARM_MMU_EXCEPTION_VECTOR_BIT);

    asm volatile(
        "mcr p15, 0, %[cp15_r1], c1, c0"
        :
        : [cp15_r1] "r" (cp15_r1)
    );
}

void MmuFlushTlb (void)
{
    int ignored_register = ignored_register;

    asm volatile(
        "mcr p15, 0, %[ignored_register], c8, c7, 0"
        :
        : [ignored_register] "r" (ignored_register)
    );
}

/* Allocates translation_table's */
struct ObjectCache translation_table_cache;

/* Allocates secondlevel_table's */
struct ObjectCache secondlevel_table_cache;

/* Allocates secondlevel_ptes's */
struct ObjectCache secondlevel_ptes_cache;

static Once_t mmu_init_control = ONCE_INIT;

static void mmu_static_init (void * ignored)
{
    ObjectCacheInit(&translation_table_cache, sizeof(struct TranslationTable));
    ObjectCacheInit(&secondlevel_table_cache, sizeof(struct SecondlevelTable));
    ObjectCacheInit(&secondlevel_ptes_cache, sizeof(struct SecondlevelPtes));
}

static struct SecondlevelTable * secondlevel_table_alloc ()
{
    struct SecondlevelTable * table;

    table = ObjectCacheAlloc(&secondlevel_table_cache);

    if (table)
    {
        unsigned int i;

        INIT_LIST_HEAD(&table->link);
        table->ptes = ObjectCacheAlloc(&secondlevel_ptes_cache);

        if (!table->ptes)
        {
            ObjectCacheFree(&secondlevel_table_cache, table);
            table = NULL;
        }

        assert(N_ELEMENTS(table->ptes->ptes) == 256);

        for (i = 0; i < N_ELEMENTS(table->ptes->ptes); i++) {
            table->ptes->ptes[i] = PT_SECONDLEVEL_MAPTYPE_UNMAPPED;
        }

        table->refcount = 0;
    }

    return table;
}

static void SecondlevelTableFree (struct SecondlevelTable * table)
{
    if (!list_empty(&table->link)) {
        list_del(&table->link);
    }

    ObjectCacheFree(&secondlevel_ptes_cache, &table->ptes);
    ObjectCacheFree(&secondlevel_table_cache, table);
}

struct TranslationTable * TranslationTableAlloc (void)
{
    enum {
        /**
         * log_2 of the number of pages required to hold
         * the hardware translation-table.
         *
         * I.e., the translation table is
         *
         *     PAGE_SIZE * (2^^TRANSLATION_TABLE_PAGES_ORDER)
         *
         * bytes long.
         */
        TRANSLATION_TABLE_PAGES_ORDER = 2,

        TRANSLATION_TABLE_SIZE = PAGE_SIZE * (1 << TRANSLATION_TABLE_PAGES_ORDER),
    };

    COMPILER_ASSERT(TRANSLATION_TABLE_SIZE == 4096 * 4);

    struct TranslationTable * table;
    unsigned int i;

    Once(&mmu_init_control, mmu_static_init, NULL);

    table = ObjectCacheAlloc(&translation_table_cache);

    if (!table) {
        return NULL;
    }

    /* Translation table is 16-KB aligned and 4 pages long. */
    table->firstlevel_ptes_pages = VmPagesAlloc(TRANSLATION_TABLE_PAGES_ORDER);

    if (!table->firstlevel_ptes_pages) {
        goto cleanup_table;
    }

    table->firstlevel_ptes = (pt_firstlevel_t *)table->firstlevel_ptes_pages->base_address;

    table->sparse_secondlevel_map = TreeMapAlloc(TreeMapAddressCompareFunc);

    if (!table->sparse_secondlevel_map) {
        goto cleanup_pages;
    }

    /* Initially make all sections unmapped */
    for (i = 0; i < TRANSLATION_TABLE_SIZE / sizeof(pt_firstlevel_t); i++) {
        table->firstlevel_ptes[i] = PT_FIRSTLEVEL_MAPTYPE_UNMAPPED;
    }

    /* Successful case */
    return table;

    /* Failure cases */
cleanup_pages:
    VmPageFree(table->firstlevel_ptes_pages);

cleanup_table:
    ObjectCacheFree(&translation_table_cache, table);
    return NULL;
}

void TranslationTableFree (struct TranslationTable * table)
{
    /* Aggregates any individual secondlevel_table's that need freed */
    struct list_head head;

    Once(&mmu_init_control, mmu_static_init, NULL);

    /* Clean out any individual second-level translation tables */
    INIT_LIST_HEAD(&head);

    /*
    Use a foreach on the tree to collect any existing nodes into a
    list, then blow away the list's elements afterward.
    */
    void func (TreeMapKey_t key, TreeMapValue_t value, void * user_data)
    {
        struct list_head *          head;
        struct SecondlevelTable *  secondlevel_table;

        head                = (struct list_head *)user_data;
        secondlevel_table   = (struct SecondlevelTable *)value;

        list_add(&secondlevel_table->link, head);
    }

    /* Go ahead and gather up all the nodes */
    TreeMapForeach(table->sparse_secondlevel_map, func, &head);

    /* Deallocate everything we found in there */
    while (!list_empty(&head))
    {
        struct SecondlevelTable * secondlevel_table = list_first_entry(
                &head,
                struct SecondlevelTable,
                link
                );

        SecondlevelTableFree(secondlevel_table);
    }

    table->firstlevel_ptes = NULL;
    TreeMapFree(table->sparse_secondlevel_map);
    VmPageFree(table->firstlevel_ptes_pages);
    ObjectCacheFree(&translation_table_cache, table);
}

static struct TranslationTable * kernel_translation_table = NULL;

struct TranslationTable * MmuGetKernelTranslationTable ()
{
    return kernel_translation_table;
}

void MmuSetKernelTranslationTable (struct TranslationTable * table)
{
    uint32_t    ttbr1;
    PhysAddr_t  table_phys = V2P((VmAddr_t)&table->firstlevel_ptes[0]);

    /* Sanity check */
    assert((table_phys & 0xffffc000) == table_phys);

    /*
    Only bits 14 through 31 (that is, the high 18 bits) of the translation
    table base register are usable. Because the hardware requires the
    translation table to start on a 16KB boundary.
    */

    /* Fetch translation base register */
    ttbr1 = GetTTBR1();

    /* Set the top 18 bits to encode our translation base address */
    ttbr1 &= 0x00003fff;
    ttbr1 |= (table_phys & 0xffffc000);

    /* Install modified register back */
    SetTTBR1(ttbr1);
    kernel_translation_table = table;
}

static struct TranslationTable * user_translation_table = NULL;

struct TranslationTable * MmuGetUserTranslationTable ()
{
    return user_translation_table;
}

void MmuSetUserTranslationTable (struct TranslationTable * table)
{
    uint32_t    ttbr0;
    PhysAddr_t  table_phys;

    table_phys = table != NULL
            ? V2P((VmAddr_t)&table->firstlevel_ptes[0])
            : 0;

    /* Sanity check */
    assert((table_phys & 0xffffc000) == table_phys);

    /*
    Only bits 14 through 31 (that is, the high 18 bits) of the translation
    table base register are usable. Because the hardware requires the
    translation table to start on a 16KB boundary.
    */

    /* Fetch translation base register */
    ttbr0 = GetTTBR0();

    /* Set the top 18 bits to encode our translation base address */
    ttbr0 &= 0x00003fff;
    ttbr0 |= (table_phys & 0xffffc000);

    /* Install modified register back */
    SetTTBR0(ttbr0);
    user_translation_table = table;
}

bool TranslationTableMapSection (
        struct TranslationTable * table,
        VmAddr_t virt,
        PhysAddr_t phys
        )
{
    unsigned int virt_idx;
    unsigned int phys_idx;

    assert(virt % SECTION_SIZE == 0);
    assert(phys % SECTION_SIZE == 0);

    virt_idx = virt >> MEGABYTE_SHIFT;
    phys_idx = phys >> MEGABYTE_SHIFT;

    /* Make sure no individual page mappings exist for the VM range */
    if ((table->firstlevel_ptes[virt_idx] & PT_FIRSTLEVEL_MAPTYPE_MASK) != PT_FIRSTLEVEL_MAPTYPE_UNMAPPED) {
        return false;
    }

    /* Install the mapping */
    table->firstlevel_ptes[virt_idx] =
            PT_FIRSTLEVEL_MAPTYPE_SECTION |
            (PT_DOMAIN_DEFAULT << PT_FIRSTLEVEL_DOMAIN_SHIFT) |
            PT_FIRSTLEVEL_SECTION_AP_FULL |
            ((phys_idx << MEGABYTE_SHIFT) & PT_FIRSTLEVEL_SECTION_BASE_ADDR_MASK);

    return true;
}

bool TranslationTableUnmapSection (
        struct TranslationTable * table,
        VmAddr_t virt
        )
{
    unsigned int virt_idx;

    assert(virt % SECTION_SIZE == 0);

    virt_idx = virt >> MEGABYTE_SHIFT;

    switch (table->firstlevel_ptes[virt_idx] & PT_FIRSTLEVEL_MAPTYPE_MASK)
    {
        case PT_FIRSTLEVEL_MAPTYPE_SECTION:
            table->firstlevel_ptes[virt_idx] = PT_FIRSTLEVEL_MAPTYPE_UNMAPPED;
            return true;
            break;

        case PT_FIRSTLEVEL_MAPTYPE_COARSE:
        case PT_FIRSTLEVEL_MAPTYPE_UNMAPPED:
            return false;
            break;

        /* There are no other defined mapping types */
        default:
            assert(false);
            return false;
            break;
    }
}

bool TranslationTableMapPage (
        struct TranslationTable * table,
        VmAddr_t virt,
        PhysAddr_t phys
        )
{
    /* VM address rounded down to nearest megabyte */
    uintptr_t virt_mb_rounded;

    /* Page index (within the containing megabyte) of the VM address */
    uintptr_t virt_pg_idx;

    /* Data structure representing secondlevel table */
    struct SecondlevelTable * secondlevel_table = NULL;

    assert(virt % PAGE_SIZE == 0);
    assert(phys % PAGE_SIZE == 0);

    virt_mb_rounded = virt & MEGABYTE_MASK;
    virt_pg_idx = (virt & ~MEGABYTE_MASK) >> PAGE_SHIFT;

    assert(virt_pg_idx < (SECTION_SIZE / PAGE_SIZE));

    /* Make sure no previous mapping exists for the page */
    switch (table->firstlevel_ptes[virt_mb_rounded >> MEGABYTE_SHIFT] & PT_FIRSTLEVEL_MAPTYPE_MASK)
    {
        case PT_FIRSTLEVEL_MAPTYPE_UNMAPPED:
            break;

        case PT_FIRSTLEVEL_MAPTYPE_SECTION:
            /* This virtual address range already used by a 1MB section map */
            return false;

        case PT_FIRSTLEVEL_MAPTYPE_COARSE:
        {
            secondlevel_table = TreeMapLookup(
                    table->sparse_secondlevel_map,
                    (TreeMapKey_t)virt_mb_rounded
                    );

            if (!secondlevel_table) {
                /* No pages exist in the section. Why's it mapped then?? */
                assert(false);
            }
            else {
                assert(secondlevel_table->ptes != NULL);

                if ((secondlevel_table->ptes->ptes[virt_pg_idx] & PT_SECONDLEVEL_MAPTYPE_MASK) != PT_SECONDLEVEL_MAPTYPE_UNMAPPED) {
                    /* Page already mapped. */
                    return false;
                }
            }

            break;
        }

        default:
            /* No other mapping type should be used */
            assert(false);
            return false;
    }

    /* In case a table didn't exist yet, make it */
    if (!secondlevel_table) {

        if ((secondlevel_table = secondlevel_table_alloc()) == NULL) {
            return false;
        }

        TreeMapInsert(
                table->sparse_secondlevel_map,
                (TreeMapKey_t)virt_mb_rounded,
                secondlevel_table);

        table->firstlevel_ptes[virt_mb_rounded >> MEGABYTE_SHIFT] =
                PT_FIRSTLEVEL_MAPTYPE_COARSE |
                (PT_DOMAIN_DEFAULT << PT_FIRSTLEVEL_DOMAIN_SHIFT) |
                (V2P((VmAddr_t)&secondlevel_table->ptes->ptes[0]) & PT_FIRSTLEVEL_COARSE_BASE_ADDR_MASK);
    }

    /* Insert the new page into the secondlevel TT */
    secondlevel_table->ptes->ptes[virt_pg_idx] =
            PT_SECONDLEVEL_MAPTYPE_SMALL_PAGE |
            PT_SECONDLEVEL_AP_FULL |
            (phys & PT_SECONDLEVEL_SMALL_PAGE_BASE_ADDR_MASK);

    secondlevel_table->refcount++;

    return true;
}

bool TranslationTableUnmapPage (
        struct TranslationTable * table,
        VmAddr_t virt
        )
{
    /* VM address rounded down to nearest megabyte */
    uintptr_t virt_mb_rounded;

    /* Page index (within the containing megabyte) of the VM address */
    uintptr_t virt_pg_idx;

    /* Data structure representing secondlevel table */
    struct SecondlevelTable * secondlevel_table = NULL;

    assert(virt % PAGE_SIZE == 0);

    virt_mb_rounded = virt & MEGABYTE_MASK;
    virt_pg_idx = (virt & ~MEGABYTE_MASK) >> PAGE_SHIFT;

    assert(virt_pg_idx < (SECTION_SIZE / PAGE_SIZE));

    /* Make sure no previous mapping exists for the page */
    switch (table->firstlevel_ptes[virt_mb_rounded >> MEGABYTE_SHIFT] & PT_FIRSTLEVEL_MAPTYPE_MASK)
    {
        case PT_FIRSTLEVEL_MAPTYPE_UNMAPPED:
        case PT_FIRSTLEVEL_MAPTYPE_SECTION:
            return false;
            break;

        case PT_FIRSTLEVEL_MAPTYPE_COARSE:

            secondlevel_table = TreeMapLookup(
                    table->sparse_secondlevel_map,
                    (TreeMapKey_t)virt_mb_rounded
                    );

            if (!secondlevel_table) {
                /* No pages exist in the section. Why's it mapped then?? */
                assert(false);
                return false;
            }

            break;

        /* There are no other defined mapping types */
        default:
            assert(false);
            return false;
            break;
    }

    if ((secondlevel_table->ptes->ptes[virt_pg_idx] & PT_SECONDLEVEL_MAPTYPE_MASK) != PT_SECONDLEVEL_MAPTYPE_SMALL_PAGE) {
        return false;
    }

    secondlevel_table->ptes->ptes[virt_pg_idx] = PT_SECONDLEVEL_MAPTYPE_UNMAPPED;
    secondlevel_table->refcount--;

    /* If no pages are used in the secondlevel table, clean it up */
    if (secondlevel_table->refcount < 1) {
        table->firstlevel_ptes[virt_mb_rounded >> MEGABYTE_SHIFT] &= ~PT_FIRSTLEVEL_MAPTYPE_MASK;
        table->firstlevel_ptes[virt_mb_rounded >> MEGABYTE_SHIFT] |= PT_FIRSTLEVEL_MAPTYPE_UNMAPPED;

        secondlevel_table = TreeMapRemove(
                table->sparse_secondlevel_map,
                (TreeMapKey_t)virt_mb_rounded
                );

        assert(secondlevel_table != NULL);
    }

    return true;
}
