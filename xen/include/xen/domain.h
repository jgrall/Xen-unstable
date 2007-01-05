
#ifndef __XEN_DOMAIN_H__
#define __XEN_DOMAIN_H__

typedef union {
    struct vcpu_guest_context *nat;
    struct compat_vcpu_guest_context *cmp;
} vcpu_guest_context_u __attribute__((__transparent_union__));

struct vcpu *alloc_vcpu(
    struct domain *d, unsigned int vcpu_id, unsigned int cpu_id);
int boot_vcpu(
    struct domain *d, int vcpuid, vcpu_guest_context_u ctxt);
struct vcpu *alloc_idle_vcpu(unsigned int cpu_id);

struct domain *alloc_domain(domid_t domid);
void free_domain(struct domain *d);

struct xen_domctl_getdomaininfo;
void getdomaininfo(
    struct domain *d, struct xen_domctl_getdomaininfo *info);
struct compat_domctl_getdomaininfo;
void compat_getdomaininfo(
    struct domain *d, struct compat_domctl_getdomaininfo *info);

/*
 * Arch-specifics.
 */

/* Allocate/free a VCPU structure. */
struct vcpu *alloc_vcpu_struct(void);
void free_vcpu_struct(struct vcpu *v);

/*
 * Initialise/destroy arch-specific details of a VCPU.
 *  - vcpu_initialise() is called after the basic generic fields of the
 *    VCPU structure are initialised. Many operations can be applied to the
 *    VCPU at this point (e.g., vcpu_pause()).
 *  - vcpu_destroy() is called only if vcpu_initialise() previously succeeded.
 */
int  vcpu_initialise(struct vcpu *v);
void vcpu_destroy(struct vcpu *v);

int arch_domain_create(struct domain *d);

void arch_domain_destroy(struct domain *d);

int arch_set_info_guest(struct vcpu *, vcpu_guest_context_u);
void arch_get_info_guest(struct vcpu *, vcpu_guest_context_u);

void domain_relinquish_resources(struct domain *d);

void dump_pageframe_info(struct domain *d);

void arch_dump_vcpu_info(struct vcpu *v);

void arch_dump_domain_info(struct domain *d);

#endif /* __XEN_DOMAIN_H__ */
