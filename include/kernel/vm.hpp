#ifndef __VM_HPP__
#define __VM_HPP__

/*! \file kernel/vm.hpp */

#include <sys/decls.h>

#include <kernel/vm-defs.h>
#include <kernel/list.hpp>

BEGIN_DECLS

/**
 * \brief   Data structure representing one physical page of
 *          RAM in the running system.
 *
 * One instance of #Page is created for each page of RAM which is
 * not occupied by static kernel memory.
 */
class Page
{
public:
    /**
     * \brief   Location in the flat high-memory map of all RAM.
     *          Always a multiple of #PAGE_SIZE.
     */
    VmAddr_t    base_address;

    /**
     * \brief   Used internally by VM to keep list of free pages, and
     *          allowed for external use by holders of allocated pages
     *          to track the ownership.
     */
    ListElement list_link;

    /**
     * \brief   Find and provision 2<sup>\em order + 1</sup> consecutive pages of
     *          virtual memory from the free-pages pool.
     *
     * \return  A pointer to the #Page structure representing the base address
     *          of 2<sup>\em order + 1</sup> consecutive page chunk of memory, or
     *          NULL if no block of consecutive free pages could be found to satisfy
     *          the request
     *
     * Requestor is responsible for releasing the pages when done by
     * using Page#Free() on the return value
     *
     * \memberof Page
     */
    static Page * Alloc (unsigned int order = 0);

    /**
     * \brief   Release the page starting at virtual memory address Page#base_address
     *
     * \param page  The page that's being returned back to the free-pages pool
     */
    static void Free (Page * page);
};

END_DECLS

#endif /* __VM_HPP__ */
