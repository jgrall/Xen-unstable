/******************************************************************************
 * features.c
 *
 * Xen feature flags.
 *
 * Copyright (c) 2006, Ian Campbell
 */
#include <linux/types.h>
#include <linux/cache.h>
#include <asm/hypervisor.h>
#include <asm-xen/features.h>

/* When we rebase to a more recent version of Linux we can use __read_mostly here. */
unsigned long xen_features[XENFEAT_NR_SUBMAPS] __cacheline_aligned;

void setup_xen_features(void)
{
     uint32_t *flags = (uint32_t *)&xen_features[0];
     xen_feature_info_t fi;
     int i;

     for (i=0; i<XENFEAT_NR_SUBMAPS; i++) {
	  fi.submap_idx = i;
	  if (HYPERVISOR_xen_version(XENVER_get_features, &fi) < 0)
	       break;
	  flags[i] = fi.submap;
     }
}

