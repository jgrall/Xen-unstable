/******************************************************************************
 * xc_private.c
 * 
 * Helper functions for the rest of the library.
 */

#include "xc_private.h"

int init_pfn_mapper(void)
{
    return open("/dev/mem", O_RDWR);
}

int close_pfn_mapper(int pm_handle)
{
    return close(pm_handle);
}

void *map_pfn_writeable(int pm_handle, unsigned long pfn)
{
    void *vaddr = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
                       MAP_SHARED, pm_handle, pfn << PAGE_SHIFT);
    if ( vaddr == MAP_FAILED )
        return NULL;
    return vaddr;
}

void *map_pfn_readonly(int pm_handle, unsigned long pfn)
{
    void *vaddr = mmap(NULL, PAGE_SIZE, PROT_READ,
                       MAP_SHARED, pm_handle, pfn << PAGE_SHIFT);
    if ( vaddr == MAP_FAILED )
        return NULL;
    return vaddr;
}

void unmap_pfn(int pm_handle, void *vaddr)
{
    (void)munmap(vaddr, PAGE_SIZE);
}
