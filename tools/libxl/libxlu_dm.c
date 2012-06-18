#include "libxl_osdeps.h" /* must come before any other headers */
#include <stdlib.h>
#include "libxlu_internal.h"
#include "libxlu_cfg_i.h"

static void split_string_into_string_list(const char *str,
                                          const char *delim,
                                          libxl_string_list *psl)
{
    char *s, *saveptr;
    const char *p;
    libxl_string_list sl;

    int i = 0, nr = 0;

    s = strdup(str);
    if (s == NULL) {
        fprintf(stderr, "xlu_dm: unable to allocate memory\n");
        exit(-1);
    }

    /* Count number of entries */
    p = strtok_r(s, delim, &saveptr);
    do {
        nr++;
    } while ((p = strtok_r(NULL, delim, &saveptr)));

    free(s);

    s = strdup(str);

    sl = malloc((nr+1) * sizeof (char *));
    if (sl == NULL) {
        fprintf(stderr, "xlu_dm: unable to allocate memory\n");
        exit(-1);
    }

    p = strtok_r(s, delim, &saveptr);
    do {
        assert(i < nr);
        // Skip blank
        while (*p == ' ')
            p++;
        sl[i] = strdup(p);
        i++;
    } while ((p = strtok_r(NULL, delim, &saveptr)));
    sl[i] = NULL;

    *psl = sl;

    free(s);
}

int xlu_dm_parse(XLU_Config *cfg, const char *spec,
                 libxl_dm *dm)
{
    char *buf = strdup(spec);
    char *p, *p2;
    int rc = 0;

    p = strtok(buf, ",");
    if (!p)
        goto skip_dm;
    do {
        while (*p == ' ')
            p++;
        if ((p2 = strchr(p, '=')) == NULL) {
            if (!strcmp(p, "ui"))
                dm->capabilities |= LIBXL_DM_CAP_UI;
            else if (!strcmp(p, "ide"))
                dm->capabilities |= LIBXL_DM_CAP_IDE;
            else if (!strcmp(p, "serial"))
                dm->capabilities |= LIBXL_DM_CAP_SERIAL;
            else if (!strcmp(p, "audio"))
                dm->capabilities |= LIBXL_DM_CAP_AUDIO;
        } else {
            *p2 = '\0';
            if (!strcmp(p, "name"))
                dm->name = strdup(p2 + 1);
            else if (!strcmp(p, "path"))
                dm->path = strdup(p2 + 1);
            else if (!strcmp(p, "vifs"))
                split_string_into_string_list(p2 + 1, ";", &dm->vifs);
       }
    } while ((p = strtok(NULL, ",")) != NULL);

    if (!dm->name && dm->path)
    {
        fprintf(stderr, "xl: Unable to parse device_deamon\n");
        exit(-ERROR_FAIL);
    }
skip_dm:
    free(buf);

    return rc;
}
