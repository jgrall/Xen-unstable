/*
 * parse xen-specific informations out of elf kernel binaries.
 */

#include "libelf-private.h"

/* ------------------------------------------------------------------------ */
/* xen features                                                             */

const char *elf_xen_feature_names[] = {
    [XENFEAT_writable_page_tables] = "writable_page_tables",
    [XENFEAT_writable_descriptor_tables] = "writable_descriptor_tables",
    [XENFEAT_auto_translated_physmap] = "auto_translated_physmap",
    [XENFEAT_supervisor_mode_kernel] = "supervisor_mode_kernel",
    [XENFEAT_pae_pgdir_above_4gb] = "pae_pgdir_above_4gb"
};
const int elf_xen_features =
    sizeof(elf_xen_feature_names) / sizeof(elf_xen_feature_names[0]);

int elf_xen_parse_features(const char *features,
			   uint32_t *supported,
			   uint32_t *required)
{
    char feature[64];
    int pos, len, i;

    if (NULL == features)
	return 0;
    for (pos = 0; features[pos] != '\0'; pos += len)
    {
	memset(feature, 0, sizeof(feature));
	for (len = 0;; len++)
	{
	    if (len >= sizeof(feature)-1)
		break;
	    if (features[pos + len] == '\0')
		break;
	    if (features[pos + len] == '|')
	    {
		len++;
		break;
	    }
	    feature[len] = features[pos + len];
	}

	for (i = 0; i < elf_xen_features; i++)
	{
	    if (!elf_xen_feature_names[i])
		continue;
	    if (NULL != required && feature[0] == '!')
	    {
		/* required */
		if (0 == strcmp(feature + 1, elf_xen_feature_names[i]))
		{
		    elf_xen_feature_set(i, supported);
		    elf_xen_feature_set(i, required);
		    break;
		}
	    }
	    else
	    {
		/* supported */
		if (0 == strcmp(feature, elf_xen_feature_names[i]))
		{
		    elf_xen_feature_set(i, supported);
		    break;
		}
	    }
	}
	if (i == elf_xen_features)
	    return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* xen elf notes                                                            */

int elf_xen_parse_note(struct elf_binary *elf,
		       struct elf_dom_parms *parms,
		       const elf_note *note)
{
/* *INDENT-OFF* */
    static const struct {
	char *name;
	int str;
    } note_desc[] = {
	[XEN_ELFNOTE_ENTRY] = { "ENTRY", 0},
	[XEN_ELFNOTE_HYPERCALL_PAGE] = { "HYPERCALL_PAGE", 0},
	[XEN_ELFNOTE_VIRT_BASE] = { "VIRT_BASE", 0},
	[XEN_ELFNOTE_PADDR_OFFSET] = { "PADDR_OFFSET", 0},
	[XEN_ELFNOTE_HV_START_LOW] = { "HV_START_LOW", 0},
	[XEN_ELFNOTE_XEN_VERSION] = { "XEN_VERSION", 1},
	[XEN_ELFNOTE_GUEST_OS] = { "GUEST_OS", 1},
	[XEN_ELFNOTE_GUEST_VERSION] = { "GUEST_VERSION", 1},
	[XEN_ELFNOTE_LOADER] = { "LOADER", 1},
	[XEN_ELFNOTE_PAE_MODE] = { "PAE_MODE", 1},
	[XEN_ELFNOTE_FEATURES] = { "FEATURES", 1},
	[XEN_ELFNOTE_BSD_SYMTAB] = { "BSD_SYMTAB", 1},
    };
/* *INDENT-ON* */

    const char *str = NULL;
    uint64_t val = 0;
    int type = elf_uval(elf, note, type);

    if ((type >= sizeof(note_desc) / sizeof(note_desc[0])) ||
	(NULL == note_desc[type].name))
    {
	elf_err(elf, "%s: unknown xen elf note (0x%x)\n",
		__FUNCTION__, type);
	return -1;
    }

    if (note_desc[type].str)
    {
	str = elf_note_desc(elf, note);
	elf_msg(elf, "%s: %s = \"%s\"\n", __FUNCTION__,
		note_desc[type].name, str);
    }
    else
    {
	val = elf_note_numeric(elf, note);
	elf_msg(elf, "%s: %s = 0x%" PRIx64 "\n", __FUNCTION__,
		note_desc[type].name, val);
    }

    switch (type)
    {
    case XEN_ELFNOTE_LOADER:
	strlcpy(parms->loader, str, sizeof(parms->loader));
	break;
    case XEN_ELFNOTE_GUEST_OS:
	strlcpy(parms->guest_os, str, sizeof(parms->guest_os));
	break;
    case XEN_ELFNOTE_GUEST_VERSION:
	strlcpy(parms->guest_ver, str, sizeof(parms->guest_ver));
	break;
    case XEN_ELFNOTE_XEN_VERSION:
	strlcpy(parms->xen_ver, str, sizeof(parms->xen_ver));
	break;
    case XEN_ELFNOTE_PAE_MODE:
	if (0 == strcmp(str, "yes"))
	    parms->pae = 2 /* extended_cr3 */;
	if (strstr(str, "bimodal"))
	    parms->pae = 3 /* bimodal */;
	break;
    case XEN_ELFNOTE_BSD_SYMTAB:
	if (0 == strcmp(str, "yes"))
	    parms->bsd_symtab = 1;
	break;

    case XEN_ELFNOTE_VIRT_BASE:
	parms->virt_base = val;
	break;
    case XEN_ELFNOTE_ENTRY:
	parms->virt_entry = val;
	break;
    case XEN_ELFNOTE_PADDR_OFFSET:
	parms->elf_paddr_offset = val;
	break;
    case XEN_ELFNOTE_HYPERCALL_PAGE:
	parms->virt_hypercall = val;
	break;
    case XEN_ELFNOTE_HV_START_LOW:
	parms->virt_hv_start_low = val;
	break;

    case XEN_ELFNOTE_FEATURES:
	if (0 != elf_xen_parse_features(str, parms->f_supported,
					parms->f_required))
	    return -1;
	break;

    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* __xen_guest section                                                      */

int elf_xen_parse_guest_info(struct elf_binary *elf,
			     struct elf_dom_parms *parms)
{
    const char *h;
    char name[32], value[128];
    int len;

    h = parms->guest_info;
    while (*h)
    {
	memset(name, 0, sizeof(name));
	memset(value, 0, sizeof(value));
	for (len = 0;; len++, h++) {
	    if (len >= sizeof(name)-1)
		break;
	    if (*h == '\0')
		break;
	    if (*h == ',')
	    {
		h++;
		break;
	    }
	    if (*h == '=')
	    {
		h++;
		for (len = 0;; len++, h++) {
		    if (len >= sizeof(value)-1)
			break;
		    if (*h == '\0')
			break;
		    if (*h == ',')
		    {
			h++;
			break;
		    }
		    value[len] = *h;
		}
		break;
	    }
	    name[len] = *h;
	}
	elf_msg(elf, "%s: %s=\"%s\"\n", __FUNCTION__, name, value);

	/* strings */
	if (0 == strcmp(name, "LOADER"))
	    strlcpy(parms->loader, value, sizeof(parms->loader));
	if (0 == strcmp(name, "GUEST_OS"))
	    strlcpy(parms->guest_os, value, sizeof(parms->guest_os));
	if (0 == strcmp(name, "GUEST_VER"))
	    strlcpy(parms->guest_ver, value, sizeof(parms->guest_ver));
	if (0 == strcmp(name, "XEN_VER"))
	    strlcpy(parms->xen_ver, value, sizeof(parms->xen_ver));
	if (0 == strcmp(name, "PAE"))
	{
	    if (0 == strcmp(value, "yes[extended-cr3]"))
		parms->pae = 2 /* extended_cr3 */;
	    else if (0 == strncmp(value, "yes", 3))
		parms->pae = 1 /* yes */;
	}
	if (0 == strcmp(name, "BSD_SYMTAB"))
	    parms->bsd_symtab = 1;

	/* longs */
	if (0 == strcmp(name, "VIRT_BASE"))
	    parms->virt_base = strtoull(value, NULL, 0);
	if (0 == strcmp(name, "VIRT_ENTRY"))
	    parms->virt_entry = strtoull(value, NULL, 0);
	if (0 == strcmp(name, "ELF_PADDR_OFFSET"))
	    parms->elf_paddr_offset = strtoull(value, NULL, 0);
	if (0 == strcmp(name, "HYPERCALL_PAGE"))
	    parms->virt_hypercall = (strtoull(value, NULL, 0) << 12) +
		parms->virt_base;

	/* other */
	if (0 == strcmp(name, "FEATURES"))
	    if (0 != elf_xen_parse_features(value, parms->f_supported,
					    parms->f_required))
		return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* sanity checks                                                            */

static int elf_xen_note_check(struct elf_binary *elf,
			      struct elf_dom_parms *parms)
{
    if (NULL == parms->elf_note_start && NULL == parms->guest_info) {
	int machine = elf_uval(elf, elf->ehdr, e_machine);
	if (EM_386 == machine || EM_X86_64 == machine) {
	    elf_err(elf, "%s: ERROR: Not a Xen-ELF image: "
		    "No ELF notes or '__xen_guest' section found.\n",
		    __FUNCTION__);
	    return -1;
	}
	return 0;
    }

    /* Check the contents of the Xen notes or guest string. */
    if ( ( 0 == strlen(parms->loader) || strncmp(parms->loader, "generic", 7) ) &&
	 ( 0 == strlen(parms->guest_os) || strncmp(parms->guest_os, "linux", 5) ) )
    {
	elf_err(elf, "%s: ERROR: Will only load images built for the generic "
		"loader or Linux images", __FUNCTION__);
	return -1;
    }

    if ( 0 == strlen(parms->xen_ver) || strncmp(parms->xen_ver, "xen-3.0", 7) )
    {
	elf_err(elf, "%s: ERROR: Xen will only load images built for Xen v3.0\n",
		__FUNCTION__);
	return -1;
    }
    return 0;
}

static int elf_xen_addr_calc_check(struct elf_binary *elf,
				   struct elf_dom_parms *parms)
{
    if (UNSET_ADDR != parms->elf_paddr_offset &&
	UNSET_ADDR == parms->virt_base )
    {
	elf_err(elf, "%s: ERROR: ELF_PADDR_OFFSET set, VIRT_BASE unset\n",
		__FUNCTION__);
        return -1;
    }

    /* Initial guess for virt_base is 0 if it is not explicitly defined. */
    if (UNSET_ADDR == parms->virt_base)
    {
	parms->virt_base = 0;
	elf_msg(elf, "%s: VIRT_BASE unset, using 0x%" PRIx64 "\n",
		__FUNCTION__, parms->virt_base);
    }

    /*
     * If we are using the legacy __xen_guest section then elf_pa_off
     * defaults to v_start in order to maintain compatibility with
     * older hypervisors which set padd in the ELF header to
     * virt_base.
     *
     * If we are using the modern ELF notes interface then the default
     * is 0.
     */
    if (UNSET_ADDR == parms->elf_paddr_offset)
    {
	if (parms->elf_note_start)
	    parms->elf_paddr_offset = 0;
	else
	    parms->elf_paddr_offset = parms->virt_base;
	elf_msg(elf, "%s: ELF_PADDR_OFFSET unset, using 0x%" PRIx64 "\n",
		__FUNCTION__, parms->elf_paddr_offset);
    }

    parms->virt_offset = parms->virt_base - parms->elf_paddr_offset;
    parms->virt_kstart = elf->pstart + parms->virt_offset;
    parms->virt_kend   = elf->pend   + parms->virt_offset;

    if (UNSET_ADDR == parms->virt_entry)
	parms->virt_entry = elf_uval(elf, elf->ehdr, e_entry);

    elf_msg(elf, "%s: addresses:\n", __FUNCTION__);
    elf_msg(elf, "    virt_base        = 0x%" PRIx64 "\n", parms->virt_base);
    elf_msg(elf, "    elf_paddr_offset = 0x%" PRIx64 "\n", parms->elf_paddr_offset);
    elf_msg(elf, "    virt_offset      = 0x%" PRIx64 "\n", parms->virt_offset);
    elf_msg(elf, "    virt_kstart      = 0x%" PRIx64 "\n", parms->virt_kstart);
    elf_msg(elf, "    virt_kend        = 0x%" PRIx64 "\n", parms->virt_kend);
    elf_msg(elf, "    virt_entry       = 0x%" PRIx64 "\n", parms->virt_entry);

    if ( (parms->virt_kstart > parms->virt_kend) ||
         (parms->virt_entry < parms->virt_kstart) ||
         (parms->virt_entry > parms->virt_kend) ||
         (parms->virt_base > parms->virt_kstart) )
    {
        elf_err(elf, "%s: ERROR: ELF start or entries are out of bounds.\n",
		__FUNCTION__);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* glue it all together ...                                                 */

int elf_xen_parse(struct elf_binary *elf,
		  struct elf_dom_parms *parms)
{
    const elf_note *note;
    const elf_shdr *shdr;
    int xen_elfnotes = 0;
    int i, count;

    memset(parms, 0, sizeof(*parms));
    parms->virt_base = UNSET_ADDR;
    parms->virt_entry = UNSET_ADDR;
    parms->virt_hypercall = UNSET_ADDR;
    parms->virt_hv_start_low = UNSET_ADDR;
    parms->elf_paddr_offset = UNSET_ADDR;

    /* find and parse elf notes */
    count = elf_shdr_count(elf);
    for (i = 0; i < count; i++)
    {
	shdr = elf_shdr_by_index(elf, i);
	if (0 == strcmp(elf_section_name(elf, shdr), "__xen_guest"))
	    parms->guest_info = elf_section_start(elf, shdr);
	if (elf_uval(elf, shdr, sh_type) != SHT_NOTE)
	    continue;
	parms->elf_note_start = elf_section_start(elf, shdr);
	parms->elf_note_end   = elf_section_end(elf, shdr);
	for (note = parms->elf_note_start;
	     (void *)note < parms->elf_note_end;
	     note = elf_note_next(elf, note))
	{
	    if (0 != strcmp(elf_note_name(elf, note), "Xen"))
		continue;
	    if (0 != elf_xen_parse_note(elf, parms, note))
		return -1;
	    xen_elfnotes++;
	}
    }

    if (!xen_elfnotes && parms->guest_info)
    {
	parms->elf_note_start = NULL;
	parms->elf_note_end   = NULL;
	elf_msg(elf, "%s: __xen_guest: \"%s\"\n", __FUNCTION__,
		parms->guest_info);
	elf_xen_parse_guest_info(elf, parms);
    }

    if (0 != elf_xen_note_check(elf, parms))
	return -1;
    if (0 != elf_xen_addr_calc_check(elf, parms))
	return -1;
    return 0;
}
