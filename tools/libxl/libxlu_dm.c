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

static int xlu_dm_check_pci(const char *pci)
{
    unsigned long bus;
    unsigned long function;
    unsigned long device;
    char *buf;

    while (*pci == ' ')
        pci++;

    bus = strtol(pci, &buf, 16);
    if (pci == buf || *buf != ':')
        return 1;

    pci = buf + 1;
    device = strtol(pci, &buf, 16);
    if (pci == buf || *buf != '.')
        return 1;

    pci = buf + 1;
    function = strtol(pci, &buf, 16);
    if (pci == buf)
        return 1;

    pci = buf;

    while (*pci == ' ')
        pci++;

    if (*pci != '\0')
        return 1;

    buf[0] = '\0';

    if (bus > 0xff || device > 0x1f || function > 0x7
        || (bus == 0xff && device == 0x1f && function == 0x7))
        return 1;

    return 0;
}

static int xlu_dm_check_range(const char *range)
{
    unsigned long begin;
    unsigned long end;
    char *buf;

    begin = strtol(range, &buf, 0);
    if (buf == range)
        return 0;

    if (*buf == '-')
    {
        range = buf + 1;
        end = strtol(range, &buf, 0);
        if (buf == range)
            return 1;
    }
    else
        end = begin;

    range = buf;

    while (*range == ' ')
        range++;

    if (*range != '\0')
        return 1;

    buf[0] = '\0';

    if (begin > end)
        return 1;

    return 0;
}

int xlu_dm_parse(XLU_Config *cfg, const char *spec,
                     libxl_dm *dm)
{
    char *buf = strdup(spec);
    char *p, *p2;
    int i = 0;
    int rc = 0;

    p = strtok (buf, ",");
    if (!p)
        goto skip_dm;
    do {
        while (*p == ' ')
            p++;
        if ((p2 = strchr (p, '=')) == NULL)
            break;
        *p2 = '\0';
        if (!strcmp (p, "name"))
            dm->name = strdup (p2 + 1);
        else if (!strcmp (p, "path"))
            dm->path = strdup (p2 + 1);
        else if (!strcmp (p, "pci"))
        {
            split_string_into_string_list(p2 + 1, ";", &dm->pcis);
            for (i = 0; dm->pcis[i]; i++)
            {
                if (xlu_dm_check_pci(dm->pcis[i]))
                {
                    fprintf(stderr, "xlu_dm: invalid pci \"%s\"\n",
                            dm->pcis[i]);
                    rc = 1;
                }
            }
        }
        else if (!strcmp (p, "mmio"))
        {
            split_string_into_string_list(p2 + 1, ";", &dm->mmios);
            for (i = 0; dm->mmios[i]; i++)
            {
                if (xlu_dm_check_range(dm->mmios[i]))
                {
                    fprintf(stderr, "xlu_dm: invalid mmio range \"%s\"\n",
                            dm->mmios[i]);
                    rc = 1;
                }
            }
        }
        else if (!strcmp (p, "pio"))
        {
            split_string_into_string_list(p2 + 1, ";", &dm->pios);
            for (i = 0; dm->pios[i]; i++)
            {
                if (xlu_dm_check_range(dm->pios[i]))
                {
                    fprintf(stderr, "xlu_dm: invalid pio range \"%s\"\n",
                            dm->pios[i]);
                    rc = 1;
                }
            }
        }
    } while ((p = strtok (NULL, ",")) != NULL);

    if (!dm->name && dm->path)
    {
        fprintf (stderr, "xl: Unable to parse device_deamon\n");
        exit (-ERROR_FAIL);
    }
skip_dm:
    free(buf);

    return rc;
}
