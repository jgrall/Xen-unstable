#include "libxl_osdeps.h" /* must come before any other headers */
#include <stdlib.h>
#include "libxlu_internal.h"
#include "libxlu_cfg_i.h"

static int xlu_dm_assign_vif(const char  *vif,
                             libxl_dmid dmid,
                             libxl_domain_config *d_config)
{
    int i = 0;
    libxl_device_nic *nic = NULL;

    fprintf(stderr, "dmid = %u vif = %s\n", dmid, vif);

    for (i = 0; i < d_config->num_nics; i++) {
        nic = &d_config->nics[i];

        if (nic->ifname && strcmp(nic->ifname, vif))
            continue;

        if (nic->dmid != LIBXL_DMID_INVALID) {
            fprintf(stderr,
                    "xl: nic %s already assigned to a device model\n",
                    nic->ifname);
            return 1;
        }

        nic->dmid = dmid;

        return 0;
    }

    fprintf(stderr, "xl: can't find nic %s\n", vif);

    return 1;
}

static int xlu_dm_assign_vifs(char *vifs,
                              libxl_dmid dmid,
                              libxl_domain_config *d_config)
{
    char *saveptr;
    const char *p;

    fprintf(stderr, "xlu_dm_assign_vifs = %s\n", vifs);

    p = strtok_r(vifs, ";", &saveptr);
    if (!p)
        return 0;

    do {
        while (*p == ' ')
            p++;

        if (xlu_dm_assign_vif(p, dmid, d_config)) {
            return 1;
        }
    } while ((p = strtok_r(NULL, ";", &saveptr)));

    return 0;
}

int xlu_dm_parse(XLU_Config *cfg, const char *spec,
                 libxl_dm *dm, libxl_domain_config *d_config)
{
    char *buf = strdup(spec);
    char *p, *p2;
    int rc = 0;
    libxl_dm_cap cap;

    p = strtok(buf, ",");
    if (!p)
        goto skip_dm;
    do {
        while (*p == ' ')
            p++;
        if ((p2 = strchr(p, '=')) == NULL) {
            if (libxl_dm_cap_from_string(p, &cap)) {
                fprintf(stderr,
                        "xl: Unknow capability '%s' for a device model\n",
                        p);
                exit(-ERROR_FAIL);

            }
            dm->capabilities |= cap;
        } else {
            *p2 = '\0';
            if (!strcmp(p, "name"))
                dm->name = strdup(p2 + 1);
            else if (!strcmp(p, "path"))
                dm->path = strdup(p2 + 1);
            else if (!strcmp(p, "vifs")) {
                if (xlu_dm_assign_vifs(p2 + 1, dm->dmid, d_config)) {
                    fprintf(stderr,
                            "xl: Unable to assigned nics to the device model\n");
                    exit(-ERROR_FAIL);
                }
            }
       }
    } while ((p = strtok(NULL, ",")) != NULL);

skip_dm:
    free(buf);

    return rc;
}
