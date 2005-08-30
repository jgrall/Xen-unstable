/******************************************************************************
 * include/asm-x86/shadow_64.h
 * 
 * Copyright (c) 2005 Michael A Fetterman
 * Based on an earlier implementation by Ian Pratt et al
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * Jun Nakajima <jun.nakajima@intel.com>
 * Chengyuan Li <chengyuan.li@intel.com>
 *
 * Extended to support 64-bit guests.
 */
#ifndef _XEN_SHADOW_64_H
#define _XEN_SHADOW_64_H
#include <asm/shadow.h>

#define READ_FAULT  0
#define WRITE_FAULT 1

#define ERROR_W    2
#define ERROR_U     4
#define X86_64_SHADOW_DEBUG 0

#if X86_64_SHADOW_DEBUG
#define ESH_LOG(_f, _a...)              \
        printk(_f, ##_a)
#else
#define ESH_LOG(_f, _a...) ((void)0)
#endif

#define L4      4UL
#define L3      3UL
#define L2      2UL
#define L1      1UL
#define L_MASK  0xff

#define ROOT_LEVEL_64   L4
#define ROOT_LEVEL_32   L2

#define SHADOW_ENTRY    (2UL << 16)
#define GUEST_ENTRY     (1UL << 16)

#define GET_ENTRY   (2UL << 8)
#define SET_ENTRY   (1UL << 8)

#define PAGETABLE_ENTRIES    (1<<PAGETABLE_ORDER)

typedef struct { intpte_t lo; } pgentry_64_t;
#define shadow_level_to_type(l)    (l << 29)
#define shadow_type_to_level(t)    (t >> 29)

#define entry_get_value(_x)         ((_x).lo)
#define entry_get_pfn(_x)           \
      (((_x).lo & (PADDR_MASK&PAGE_MASK)) >> PAGE_SHIFT)
#define entry_get_paddr(_x)          (((_x).lo & (PADDR_MASK&PAGE_MASK)))
#define entry_get_flags(_x)         (get_pte_flags((_x).lo))

#define entry_empty()           ((pgentry_64_t) { 0 })
#define entry_from_pfn(pfn, flags)  \
    ((pgentry_64_t) { ((intpte_t)(pfn) << PAGE_SHIFT) | put_pte_flags(flags) })
#define entry_add_flags(x, flags)    ((x).lo |= put_pte_flags(flags))
#define entry_remove_flags(x, flags) ((x).lo &= ~put_pte_flags(flags))
#define entry_has_changed(x,y,flags) \
        ( !!(((x).lo ^ (y).lo) & ((PADDR_MASK&PAGE_MASK)|put_pte_flags(flags))) )
static inline int  table_offset_64(unsigned long va, int level)
{
    switch(level) {
        case 1:
            return  (((va) >> L1_PAGETABLE_SHIFT) & (L1_PAGETABLE_ENTRIES - 1));
        case 2:
            return  (((va) >> L2_PAGETABLE_SHIFT) & (L2_PAGETABLE_ENTRIES - 1));
        case 3:
            return  (((va) >> L3_PAGETABLE_SHIFT) & (L3_PAGETABLE_ENTRIES - 1));
#if CONFIG_PAGING_LEVELS >= 4
        case 4:
            return  (((va) >> L4_PAGETABLE_SHIFT) & (L4_PAGETABLE_ENTRIES - 1));
#endif
        default:
            //printk("<table_offset_64> level %d is too big\n", level);
            return -1;
    }
}

static inline void free_out_of_sync_state(struct domain *d)
{
    struct out_of_sync_entry *entry;

    // NB: Be careful not to call something that manipulates this list
    //     while walking it.  Remove one item at a time, and always
    //     restart from start of list.
    //
    while ( (entry = d->arch.out_of_sync) )
    {
        d->arch.out_of_sync = entry->next;
        release_out_of_sync_entry(d, entry);

        entry->next = d->arch.out_of_sync_free;
        d->arch.out_of_sync_free = entry;
    }
}

static inline pgentry_64_t *__entry(
    struct vcpu *v, u64 va, u32 flag)
{
    int i;
    pgentry_64_t *le_e;
    pgentry_64_t *le_p;
    unsigned long mfn;
    int index;
    u32 level = flag & L_MASK;
    struct domain *d = v->domain;

    index = table_offset_64(va, ROOT_LEVEL_64);
    if (flag & SHADOW_ENTRY)
        le_e = (pgentry_64_t *)&v->arch.shadow_vtable[index];
    else
        le_e = (pgentry_64_t *)&v->arch.guest_vtable[index];

    /*
     * If it's not external mode, then mfn should be machine physical.
     */
    for (i = ROOT_LEVEL_64 - level; i > 0; i--) {
        if (unlikely(!(entry_get_flags(*le_e) & _PAGE_PRESENT)))
            return NULL;
        mfn = entry_get_value(*le_e) >> PAGE_SHIFT;
        if ((flag & GUEST_ENTRY) && shadow_mode_translate(d))
            mfn = get_mfn_from_pfn(mfn);
        le_p = (pgentry_64_t *)phys_to_virt(mfn << PAGE_SHIFT);
        index = table_offset_64(va, (level + i - 1));
        le_e = &le_p[index];

    }
    return le_e;

}

static inline pgentry_64_t *__rw_entry(
    struct vcpu *ed, u64 va, void *e_p, u32 flag)
{
    pgentry_64_t *le_e = __entry(ed, va, flag);
    pgentry_64_t *e = (pgentry_64_t *)e_p;
    if (le_e == NULL)
        return NULL;

    if (e) {
        if (flag & SET_ENTRY)
            *le_e = *e;
        else
            *e = *le_e;
    }
    return le_e;
}
#define __shadow_set_l4e(v, va, value) \
  __rw_entry(v, va, value, SHADOW_ENTRY | SET_ENTRY | L4)
#define __shadow_get_l4e(v, va, sl4e) \
  __rw_entry(v, va, sl4e, SHADOW_ENTRY | GET_ENTRY | L4)
#define __shadow_set_l3e(v, va, value) \
  __rw_entry(v, va, value, SHADOW_ENTRY | SET_ENTRY | L3)
#define __shadow_get_l3e(v, va, sl3e) \
  __rw_entry(v, va, sl3e, SHADOW_ENTRY | GET_ENTRY | L3)
#define __shadow_set_l2e(v, va, value) \
  __rw_entry(v, va, value, SHADOW_ENTRY | SET_ENTRY | L2)
#define __shadow_get_l2e(v, va, sl2e) \
  __rw_entry(v, va, sl2e, SHADOW_ENTRY | GET_ENTRY | L2)
#define __shadow_set_l1e(v, va, value) \
  __rw_entry(v, va, value, SHADOW_ENTRY | SET_ENTRY | L1)
#define __shadow_get_l1e(v, va, sl1e) \
  __rw_entry(v, va, sl1e, SHADOW_ENTRY | GET_ENTRY | L1)

#define __guest_set_l4e(v, va, value) \
  __rw_entry(v, va, value, GUEST_ENTRY | SET_ENTRY | L4)
#define __guest_get_l4e(v, va, gl4e) \
  __rw_entry(v, va, gl4e, GUEST_ENTRY | GET_ENTRY | L4)
#define __guest_set_l3e(v, va, value) \
  __rw_entry(v, va, value, GUEST_ENTRY | SET_ENTRY | L3)
#define __guest_get_l3e(v, va, sl3e) \
  __rw_entry(v, va, gl3e, GUEST_ENTRY | GET_ENTRY | L3)

static inline void *  __guest_set_l2e(
    struct vcpu *v, u64 va, void *value, int size)
{
    switch(size) {
        case 4:
            // 32-bit guest
            {
                l2_pgentry_32_t *l2va;

                l2va = (l2_pgentry_32_t *)v->arch.guest_vtable;
                if (value)
                    l2va[l2_table_offset_32(va)] = *(l2_pgentry_32_t *)value;
                return &l2va[l2_table_offset_32(va)];
            }
        case 8:
            return __rw_entry(v, va, value, GUEST_ENTRY | SET_ENTRY | L2);
        default:
            BUG();
            return NULL;
    }
    return NULL;
}

#define __guest_set_l2e(v, va, value) \
  ( __typeof__(value) )__guest_set_l2e(v, (u64)va, value, sizeof(*value))

static inline void * __guest_get_l2e(
  struct vcpu *v, u64 va, void *gl2e, int size)
{
    switch(size) {
        case 4:
            // 32-bit guest
            {
                l2_pgentry_32_t *l2va;
                l2va = (l2_pgentry_32_t *)v->arch.guest_vtable;
                if (gl2e)
                    *(l2_pgentry_32_t *)gl2e = l2va[l2_table_offset_32(va)];
                return &l2va[l2_table_offset_32(va)];
            }
        case 8:
            return __rw_entry(v, va, gl2e, GUEST_ENTRY | GET_ENTRY | L2);
        default:
            BUG();
            return NULL;
    }
    return NULL;
}

#define __guest_get_l2e(v, va, gl2e) \
  (__typeof__ (gl2e))__guest_get_l2e(v, (u64)va, gl2e, sizeof(*gl2e))

static inline void *  __guest_set_l1e(
  struct vcpu *v, u64 va, void *value, int size)
{
    switch(size) {
        case 4:
            // 32-bit guest
            {
                l2_pgentry_32_t gl2e;
                l1_pgentry_32_t *l1va;
                unsigned long l1mfn;

                if (!__guest_get_l2e(v, va, &gl2e))
                    return NULL;
                if (unlikely(!(l2e_get_flags_32(gl2e) & _PAGE_PRESENT)))
                    return NULL;

                l1mfn = get_mfn_from_pfn(
                  l2e_get_pfn(gl2e));

                l1va = (l1_pgentry_32_t *)
                  phys_to_virt(l1mfn << L1_PAGETABLE_SHIFT);
                if (value)
                    l1va[l1_table_offset_32(va)] = *(l1_pgentry_32_t *)value;

                return &l1va[l1_table_offset_32(va)];
            }

        case 8:
            return __rw_entry(v, va, value, GUEST_ENTRY | SET_ENTRY | L1);
        default:
            BUG();
            return NULL;
    }
    return NULL;
}

#define __guest_set_l1e(v, va, value) \
  ( __typeof__(value) )__guest_set_l1e(v, (u64)va, value, sizeof(*value))

static inline void *  __guest_get_l1e(
  struct vcpu *v, u64 va, void *gl1e, int size)
{
    switch(size) {
        case 4:
            // 32-bit guest
            {
                l2_pgentry_32_t gl2e;
                l1_pgentry_32_t *l1va;
                unsigned long l1mfn;

                if (!(__guest_get_l2e(v, va, &gl2e)))
                    return NULL;


                if (unlikely(!(l2e_get_flags_32(gl2e) & _PAGE_PRESENT)))
                    return NULL;


                l1mfn = get_mfn_from_pfn(
                  l2e_get_pfn(gl2e));
                l1va = (l1_pgentry_32_t *) phys_to_virt(
                  l1mfn << L1_PAGETABLE_SHIFT);
                if (gl1e)
                    *(l1_pgentry_32_t *)gl1e = l1va[l1_table_offset_32(va)];

                return &l1va[l1_table_offset_32(va)];
            }
        case 8:
            // 64-bit guest
            return __rw_entry(v, va, gl1e, GUEST_ENTRY | GET_ENTRY | L1);
        default:
            BUG();
            return NULL;
    }
    return NULL;
}

#define __guest_get_l1e(v, va, gl1e) \
  ( __typeof__(gl1e) )__guest_get_l1e(v, (u64)va, gl1e, sizeof(*gl1e))

static inline void entry_general(
  struct domain *d,
  pgentry_64_t *gle_p,
  pgentry_64_t *sle_p,
  unsigned long smfn, u32 level)

{
    pgentry_64_t gle = *gle_p;
    pgentry_64_t sle;

    sle = entry_empty();
    if ( (entry_get_flags(gle) & _PAGE_PRESENT) && (smfn != 0) )
    {
        if ((entry_get_flags(gle) & _PAGE_PSE) && level == L2) {
            sle = entry_from_pfn(smfn, entry_get_flags(gle));
            entry_remove_flags(sle, _PAGE_PSE);

            if ( shadow_mode_log_dirty(d) ||
		 !(entry_get_flags(gle) & _PAGE_DIRTY) )
            {
                pgentry_64_t *l1_p;
                int i;

                l1_p =(pgentry_64_t *)map_domain_page(smfn);
                for (i = 0; i < L1_PAGETABLE_ENTRIES; i++)
                    entry_remove_flags(l1_p[i], _PAGE_RW);

                unmap_domain_page(l1_p);
            }
        } else {
            sle = entry_from_pfn(smfn,
				 (entry_get_flags(gle) | _PAGE_RW | _PAGE_ACCESSED) & ~_PAGE_AVAIL);
            entry_add_flags(gle, _PAGE_ACCESSED);
        }
        // XXX mafetter: Hmm...
        //     Shouldn't the dirty log be checked/updated here?
        //     Actually, it needs to be done in this function's callers.
        //
        *gle_p = gle;
    }

    if ( entry_get_value(sle) || entry_get_value(gle) )
        SH_VVLOG("%s: gpde=%lx, new spde=%lx", __func__,
          entry_get_value(gle), entry_get_value(sle));

    *sle_p = sle;
}

static inline void entry_propagate_from_guest(
  struct domain *d, pgentry_64_t *gle_p, pgentry_64_t *sle_p, u32 level)
{
    pgentry_64_t gle = *gle_p;
    unsigned long smfn = 0;

    if ( entry_get_flags(gle) & _PAGE_PRESENT ) {
        if ((entry_get_flags(gle) & _PAGE_PSE) && level == L2) {
            smfn =  __shadow_status(d, entry_get_value(gle) >> PAGE_SHIFT, PGT_fl1_shadow);
        } else {
            smfn =  __shadow_status(d, entry_get_pfn(gle), 
              shadow_level_to_type((level -1 )));
        }
    }
    entry_general(d, gle_p, sle_p, smfn, level);

}

static int inline
validate_entry_change(
  struct domain *d,
  pgentry_64_t *new_gle_p,
  pgentry_64_t *shadow_le_p,
  u32 level)
{
    pgentry_64_t old_sle, new_sle;
    pgentry_64_t new_gle = *new_gle_p;

    old_sle = *shadow_le_p;
    entry_propagate_from_guest(d, &new_gle, &new_sle, level);

    ESH_LOG("old_sle: %lx, new_gle: %lx, new_sle: %lx\n",
      entry_get_value(old_sle), entry_get_value(new_gle),
      entry_get_value(new_sle));

    if ( ((entry_get_value(old_sle) | entry_get_value(new_sle)) & _PAGE_PRESENT) &&
      entry_has_changed(old_sle, new_sle, _PAGE_PRESENT) )
    {
        perfc_incrc(validate_entry_changes);

        if ( (entry_get_flags(new_sle) & _PAGE_PRESENT) &&
          !get_shadow_ref(entry_get_pfn(new_sle)) )
            BUG();
        if ( entry_get_flags(old_sle) & _PAGE_PRESENT )
            put_shadow_ref(entry_get_pfn(old_sle));
    }

    *shadow_le_p = new_sle;

    return 1;
}

/*
 * Check P, R/W, U/S bits in the guest page table.
 * If the fault belongs to guest return 1,
 * else return 0.
 */
static inline int guest_page_fault(struct vcpu *v,
  unsigned long va, unsigned int error_code, pgentry_64_t *gpl2e, pgentry_64_t *gpl1e)
{
    struct domain *d = v->domain;
    pgentry_64_t gle, *lva;
    unsigned long mfn;
    int i;

    __rw_entry(v, va, &gle, GUEST_ENTRY | GET_ENTRY | L4);
    if (unlikely(!(entry_get_flags(gle) & _PAGE_PRESENT)))
        return 1;

    if (error_code & ERROR_W) {
        if (unlikely(!(entry_get_flags(gle) & _PAGE_RW)))
            return 1;
    }
    if (error_code & ERROR_U) {
        if (unlikely(!(entry_get_flags(gle) & _PAGE_USER)))
            return 1;
    }
    for (i = L3; i >= L1; i--) {
	/*
	 * If it's not external mode, then mfn should be machine physical.
	 */
	mfn = __gpfn_to_mfn(d, (entry_get_value(gle) >> PAGE_SHIFT));

        lva = (pgentry_64_t *) phys_to_virt(
	    mfn << PAGE_SHIFT);
        gle = lva[table_offset_64(va, i)];

        if (unlikely(!(entry_get_flags(gle) & _PAGE_PRESENT)))
            return 1;

        if (error_code & ERROR_W) {
            if (unlikely(!(entry_get_flags(gle) & _PAGE_RW)))
                return 1;
        }
        if (error_code & ERROR_U) {
            if (unlikely(!(entry_get_flags(gle) & _PAGE_USER)))
                return 1;
        }

        if (i == L2) {
            if (gpl2e)
                *gpl2e = gle;

            if (likely(entry_get_flags(gle) & _PAGE_PSE))
                return 0;

        }

        if (i == L1)
            if (gpl1e)
                *gpl1e = gle;
    }
    return 0;
}

static inline unsigned long gva_to_gpa(unsigned long gva)
{
    struct vcpu *v = current;
    pgentry_64_t gl1e = {0};
    pgentry_64_t gl2e = {0};
    unsigned long gpa;

    if (guest_page_fault(v, gva, 0, &gl2e, &gl1e))
        return 0;
    if (entry_get_flags(gl2e) & _PAGE_PSE)
        gpa = entry_get_paddr(gl2e) + (gva & ((1 << L2_PAGETABLE_SHIFT) - 1));
    else
        gpa = entry_get_paddr(gl1e) + (gva & ~PAGE_MASK);

    return gpa;

}
#endif


