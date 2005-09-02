/****************************************************************
 * secpol_tool.c
 *
 * Copyright (C) 2005 IBM Corporation
 *
 * Authors:
 * Reiner Sailer <sailer@watson.ibm.com>
 * Stefan Berger <stefanb@watson.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * sHype policy management tool. This code runs in a domain and
 *     manages the Xen security policy by interacting with the
 *     Xen access control module via a /proc/xen/privcmd proc-ioctl,
 *     which is translated into a acm_op hypercall into Xen.
 *
 * indent -i4 -kr -nut
 */


#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <netinet/in.h>
#include "secpol_compat.h"
#include <xen/acm.h>
#include <xen/acm_ops.h>
#include <xen/linux/privcmd.h>

#define PERROR(_m, _a...) \
fprintf(stderr, "ERROR: " _m " (%d = %s)\n" , ## _a ,	\
                errno, strerror(errno))

void usage(char *progname)
{
    printf("Use: %s \n"
           "\t getpolicy\n"
           "\t dumpstats\n"
           "\t loadpolicy <binary policy file>\n"
           "\t getssid -d <domainid> [-f]\n"
		   "\t getssid -s <ssidref> [-f]\n", progname);
    exit(-1);
}

static inline int do_policycmd(int xc_handle, unsigned int cmd,
                               unsigned long data)
{
    return ioctl(xc_handle, cmd, data);
}

static inline int do_xen_hypercall(int xc_handle,
                                   privcmd_hypercall_t * hypercall)
{
    return do_policycmd(xc_handle,
                        IOCTL_PRIVCMD_HYPERCALL,
                        (unsigned long) hypercall);
}

static inline int do_acm_op(int xc_handle, acm_op_t * op)
{
    int ret = -1;
    privcmd_hypercall_t hypercall;

    op->interface_version = ACM_INTERFACE_VERSION;

    hypercall.op = __HYPERVISOR_acm_op;
    hypercall.arg[0] = (unsigned long) op;

    if (mlock(op, sizeof(*op)) != 0)
    {
        PERROR("Could not lock memory for Xen policy hypercall");
        goto out1;
    }

    if ((ret = do_xen_hypercall(xc_handle, &hypercall)) < 0)
    {
        if (errno == EACCES)
            fprintf(stderr, "ACM operation failed -- need to"
                    " rebuild the user-space tool set?\n");
        goto out2;
    }

  out2:(void) munlock(op, sizeof(*op));
  out1:return ret;
}

/*************************** DUMPS *******************************/

void acm_dump_chinesewall_buffer(void *buf, int buflen)
{

    struct acm_chwall_policy_buffer *cwbuf =
        (struct acm_chwall_policy_buffer *) buf;
    domaintype_t *ssids, *conflicts, *running_types, *conflict_aggregate;
    int i, j;


    if (htonl(cwbuf->policy_code) != ACM_CHINESE_WALL_POLICY)
    {
        printf("CHINESE WALL POLICY CODE not found ERROR!!\n");
        return;
    }
    printf("\n\nChinese Wall policy:\n");
    printf("====================\n");
    printf("Policy version= %x.\n", ntohl(cwbuf->policy_version));
    printf("Max Types     = %x.\n", ntohl(cwbuf->chwall_max_types));
    printf("Max Ssidrefs  = %x.\n", ntohl(cwbuf->chwall_max_ssidrefs));
    printf("Max ConfSets  = %x.\n", ntohl(cwbuf->chwall_max_conflictsets));
    printf("Ssidrefs Off  = %x.\n", ntohl(cwbuf->chwall_ssid_offset));
    printf("Conflicts Off = %x.\n",
           ntohl(cwbuf->chwall_conflict_sets_offset));
    printf("Runing T. Off = %x.\n",
           ntohl(cwbuf->chwall_running_types_offset));
    printf("C. Agg. Off   = %x.\n",
           ntohl(cwbuf->chwall_conflict_aggregate_offset));
    printf("\nSSID To CHWALL-Type matrix:\n");

    ssids = (domaintype_t *) (buf + ntohl(cwbuf->chwall_ssid_offset));
    for (i = 0; i < ntohl(cwbuf->chwall_max_ssidrefs); i++)
    {
        printf("\n   ssidref%2x:  ", i);
        for (j = 0; j < ntohl(cwbuf->chwall_max_types); j++)
            printf("%02x ",
                   ntohs(ssids[i * ntohl(cwbuf->chwall_max_types) + j]));
    }
    printf("\n\nConfict Sets:\n");
    conflicts =
        (domaintype_t *) (buf + ntohl(cwbuf->chwall_conflict_sets_offset));
    for (i = 0; i < ntohl(cwbuf->chwall_max_conflictsets); i++)
    {
        printf("\n   c-set%2x:    ", i);
        for (j = 0; j < ntohl(cwbuf->chwall_max_types); j++)
            printf("%02x ",
                   ntohs(conflicts
                         [i * ntohl(cwbuf->chwall_max_types) + j]));
    }
    printf("\n");

    printf("\nRunning\nTypes:         ");
    if (ntohl(cwbuf->chwall_running_types_offset))
    {
        running_types =
            (domaintype_t *) (buf +
                              ntohl(cwbuf->chwall_running_types_offset));
        for (i = 0; i < ntohl(cwbuf->chwall_max_types); i++)
        {
            printf("%02x ", ntohs(running_types[i]));
        }
        printf("\n");
    } else {
        printf("Not Reported!\n");
    }
    printf("\nConflict\nAggregate Set: ");
    if (ntohl(cwbuf->chwall_conflict_aggregate_offset))
    {
        conflict_aggregate =
            (domaintype_t *) (buf +
                              ntohl(cwbuf->chwall_conflict_aggregate_offset));
        for (i = 0; i < ntohl(cwbuf->chwall_max_types); i++)
        {
            printf("%02x ", ntohs(conflict_aggregate[i]));
        }
        printf("\n\n");
    } else {
        printf("Not Reported!\n");
    }
}

void acm_dump_ste_buffer(void *buf, int buflen)
{

    struct acm_ste_policy_buffer *stebuf =
        (struct acm_ste_policy_buffer *) buf;
    domaintype_t *ssids;
    int i, j;


    if (ntohl(stebuf->policy_code) != ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY) {
        printf("SIMPLE TYPE ENFORCEMENT POLICY CODE not found ERROR!!\n");
        return;
    }
    printf("\nSimple Type Enforcement policy:\n");
    printf("===============================\n");
    printf("Policy version= %x.\n", ntohl(stebuf->policy_version));
    printf("Max Types     = %x.\n", ntohl(stebuf->ste_max_types));
    printf("Max Ssidrefs  = %x.\n", ntohl(stebuf->ste_max_ssidrefs));
    printf("Ssidrefs Off  = %x.\n", ntohl(stebuf->ste_ssid_offset));
    printf("\nSSID To STE-Type matrix:\n");

    ssids = (domaintype_t *) (buf + ntohl(stebuf->ste_ssid_offset));
    for (i = 0; i < ntohl(stebuf->ste_max_ssidrefs); i++)
    {
        printf("\n   ssidref%2x: ", i);
        for (j = 0; j < ntohl(stebuf->ste_max_types); j++)
            printf("%02x ", ntohs(ssids[i * ntohl(stebuf->ste_max_types) + j]));
    }
    printf("\n\n");
}

void acm_dump_policy_buffer(void *buf, int buflen)
{
    struct acm_policy_buffer *pol = (struct acm_policy_buffer *) buf;

    printf("\nPolicy dump:\n");
    printf("============\n");
    printf("PolicyVer = %x.\n", ntohl(pol->policy_version));
    printf("Magic     = %x.\n", ntohl(pol->magic));
    printf("Len       = %x.\n", ntohl(pol->len));
    printf("Primary   = %s (c=%x, off=%x).\n",
           ACM_POLICY_NAME(ntohl(pol->primary_policy_code)),
           ntohl(pol->primary_policy_code),
           ntohl(pol->primary_buffer_offset));
    printf("Secondary = %s (c=%x, off=%x).\n",
           ACM_POLICY_NAME(ntohl(pol->secondary_policy_code)),
           ntohl(pol->secondary_policy_code),
           ntohl(pol->secondary_buffer_offset));
    switch (ntohl(pol->primary_policy_code))
    {
    case ACM_CHINESE_WALL_POLICY:
        acm_dump_chinesewall_buffer(buf +
                                    ntohl(pol->primary_buffer_offset),
                                    ntohl(pol->len) -
                                    ntohl(pol->primary_buffer_offset));
        break;

    case ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY:
        acm_dump_ste_buffer(buf + ntohl(pol->primary_buffer_offset),
                            ntohl(pol->len) -
                            ntohl(pol->primary_buffer_offset));
        break;

    case ACM_NULL_POLICY:
        printf("Primary policy is NULL Policy (n/a).\n");
        break;

    default:
        printf("UNKNOWN POLICY!\n");
    }

    switch (ntohl(pol->secondary_policy_code))
    {
    case ACM_CHINESE_WALL_POLICY:
        acm_dump_chinesewall_buffer(buf +
                                    ntohl(pol->secondary_buffer_offset),
                                    ntohl(pol->len) -
                                    ntohl(pol->secondary_buffer_offset));
        break;

    case ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY:
        acm_dump_ste_buffer(buf + ntohl(pol->secondary_buffer_offset),
                            ntohl(pol->len) -
                            ntohl(pol->secondary_buffer_offset));
        break;

    case ACM_NULL_POLICY:
        printf("Secondary policy is NULL Policy (n/a).\n");
        break;

    default:
        printf("UNKNOWN POLICY!\n");
    }
}

/******************************* get policy ******************************/

#define PULL_CACHE_SIZE		8192
u8 pull_buffer[PULL_CACHE_SIZE];
int acm_domain_getpolicy(int xc_handle)
{
    acm_op_t op;
    int ret;

    memset(pull_buffer, 0x00, sizeof(pull_buffer));
    op.cmd = ACM_GETPOLICY;
    op.interface_version = ACM_INTERFACE_VERSION;
    op.u.getpolicy.pullcache = (void *) pull_buffer;
    op.u.getpolicy.pullcache_size = sizeof(pull_buffer);
    ret = do_acm_op(xc_handle, &op);
    /* dump policy  */
    acm_dump_policy_buffer(pull_buffer, sizeof(pull_buffer));
    return ret;
}

/************************ load binary policy ******************************/

int acm_domain_loadpolicy(int xc_handle, const char *filename)
{
    struct stat mystat;
    int ret, fd;
    off_t len;
    u8 *buffer;

    if ((ret = stat(filename, &mystat)))
    {
        printf("File %s not found.\n", filename);
        goto out;
    }

    len = mystat.st_size;
    if ((buffer = malloc(len)) == NULL)
    {
        ret = -ENOMEM;
        goto out;
    }
    if ((fd = open(filename, O_RDONLY)) <= 0)
    {
        ret = -ENOENT;
        printf("File %s not found.\n", filename);
        goto free_out;
    }
    if (len == read(fd, buffer, len))
    {
        acm_op_t op;
        /* dump it and then push it down into xen/acm */
        acm_dump_policy_buffer(buffer, len);
        op.cmd = ACM_SETPOLICY;
        op.interface_version = ACM_INTERFACE_VERSION;
        op.u.setpolicy.pushcache = (void *) buffer;
        op.u.setpolicy.pushcache_size = len;
        ret = do_acm_op(xc_handle, &op);

        if (ret)
            printf
                ("ERROR setting policy. Try 'xm dmesg' to see details.\n");
        else
            printf("Successfully changed policy.\n");

    } else {
        ret = -1;
    }
    close(fd);
  free_out:
    free(buffer);
  out:
    return ret;
}

/************************ dump hook statistics ******************************/
void dump_ste_stats(struct acm_ste_stats_buffer *ste_stats)
{
    printf("STE-Policy Security Hook Statistics:\n");
    printf("ste: event_channel eval_count      = %d\n",
           ntohl(ste_stats->ec_eval_count));
    printf("ste: event_channel denied_count    = %d\n",
           ntohl(ste_stats->ec_denied_count));
    printf("ste: event_channel cache_hit_count = %d\n",
           ntohl(ste_stats->ec_cachehit_count));
    printf("ste:\n");
    printf("ste: grant_table   eval_count      = %d\n",
           ntohl(ste_stats->gt_eval_count));
    printf("ste: grant_table   denied_count    = %d\n",
           ntohl(ste_stats->gt_denied_count));
    printf("ste: grant_table   cache_hit_count = %d\n",
           ntohl(ste_stats->gt_cachehit_count));
}

#define PULL_STATS_SIZE		8192
int acm_domain_dumpstats(int xc_handle)
{
    u8 stats_buffer[PULL_STATS_SIZE];
    acm_op_t op;
    int ret;
    struct acm_stats_buffer *stats;

    memset(stats_buffer, 0x00, sizeof(stats_buffer));
    op.cmd = ACM_DUMPSTATS;
    op.interface_version = ACM_INTERFACE_VERSION;
    op.u.dumpstats.pullcache = (void *) stats_buffer;
    op.u.dumpstats.pullcache_size = sizeof(stats_buffer);
    ret = do_acm_op(xc_handle, &op);

    if (ret < 0)
    {
        printf("ERROR dumping policy stats. Try 'xm dmesg' to see details.\n");
        return ret;
    }
    stats = (struct acm_stats_buffer *) stats_buffer;

    printf("\nPolicy dump:\n");
    printf("============\n");
    printf("Magic     = %x.\n", ntohl(stats->magic));
    printf("Len       = %x.\n", ntohl(stats->len));

    switch (ntohl(stats->primary_policy_code))
    {
    case ACM_NULL_POLICY:
        printf("NULL Policy: No statistics apply.\n");
        break;

    case ACM_CHINESE_WALL_POLICY:
        printf("Chinese Wall Policy: No statistics apply.\n");
        break;

    case ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY:
        dump_ste_stats((struct acm_ste_stats_buffer *) (stats_buffer +
                                                        ntohl(stats->
                                                              primary_stats_offset)));
        break;

    default:
        printf("UNKNOWN PRIMARY POLICY ERROR!\n");
    }

    switch (ntohl(stats->secondary_policy_code))
    {
    case ACM_NULL_POLICY:
        printf("NULL Policy: No statistics apply.\n");
        break;

    case ACM_CHINESE_WALL_POLICY:
        printf("Chinese Wall Policy: No statistics apply.\n");
        break;

    case ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY:
        dump_ste_stats((struct acm_ste_stats_buffer *) (stats_buffer +
                                                        ntohl(stats->
                                                              secondary_stats_offset)));
        break;

    default:
        printf("UNKNOWN SECONDARY POLICY ERROR!\n");
    }
    return ret;
}
/************************ get ssidref & types ******************************/
/*
 * the ssid (types) can be looked up either by domain id or by ssidref
 */
int acm_domain_getssid(int xc_handle, int argc, char * const argv[])
{
    /* this includes header and a set of types */
    #define MAX_SSIDBUFFER  2000
    int ret, i;
    acm_op_t op;
    struct acm_ssid_buffer *hdr;
    unsigned char *buf;
	int nice_print = 1;

    op.cmd = ACM_GETSSID;
    op.interface_version = ACM_INTERFACE_VERSION;
	op.u.getssid.get_ssid_by = UNSET;
	/* arguments
	   -d ... domain id to look up
	   -s ... ssidref number to look up
	   -f ... formatted print (scripts depend on this format)
	*/
	while (1)
    {
		int c = getopt(argc, argv, "d:s:f");
		if (c == -1)
			break;
		if (c == 'd')
        {
			if (op.u.getssid.get_ssid_by != UNSET)
				usage(argv[0]);
			op.u.getssid.get_ssid_by = DOMAINID;
			op.u.getssid.id.domainid = strtoul(optarg, NULL, 0);
		}
		else if (c== 's')
        {
			if (op.u.getssid.get_ssid_by != UNSET)
				usage(argv[0]);
			op.u.getssid.get_ssid_by = SSIDREF;
			op.u.getssid.id.ssidref = strtoul(optarg, NULL, 0);
		}
		else if (c== 'f')
		{
			nice_print = 0;
		}
		else
			usage(argv[0]);
	}
	if (op.u.getssid.get_ssid_by == UNSET)
		usage(argv[0]);

	buf = malloc(MAX_SSIDBUFFER);
    if (!buf)
        return -ENOMEM;

    /* dump it and then push it down into xen/acm */
    op.u.getssid.ssidbuf = buf;   /* out */
    op.u.getssid.ssidbuf_size = MAX_SSIDBUFFER;
    ret = do_acm_op(xc_handle, &op);

    if (ret)
    {
        printf("ERROR getting ssidref. Try 'xm dmesg' to see details.\n");
        goto out;
    }
    hdr = (struct acm_ssid_buffer *)buf;
    if (hdr->len > MAX_SSIDBUFFER)
    {
        printf("ERROR: Buffer length inconsistent (ret=%d, hdr->len=%d)!\n",
               ret, hdr->len);
            return -EIO;
    }
	if (nice_print)
    {
		printf("SSID: ssidref = 0x%08x \n", hdr->ssidref);
		printf("      P: %s, max_types = %d\n",
			   ACM_POLICY_NAME(hdr->primary_policy_code), hdr->primary_max_types);
		printf("	  Types: ");
		for (i=0; i< hdr->primary_max_types; i++)
			if (buf[hdr->primary_types_offset + i])
				printf("%02x ", i);
			else
				printf("-- ");
		printf("\n");

		printf("      S: %s, max_types = %d\n",
			   ACM_POLICY_NAME(hdr->secondary_policy_code), hdr->secondary_max_types);
		printf("	  Types: ");
		for (i=0; i< hdr->secondary_max_types; i++)
			if (buf[hdr->secondary_types_offset + i])
				printf("%02x ", i);
			else
				printf("-- ");
		printf("\n");
	}
	else
    {
		/* formatted print for use with scripts (.sh)
		 *  update scripts when updating here (usually
		 *  used in combination with -d to determine a
		 *  running domain's label
		 */
		printf("SSID: ssidref = 0x%08x \n", hdr->ssidref);
	}

    /* return ste ssidref */
    if (hdr->primary_policy_code == ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY)
        ret = (hdr->ssidref) & 0xffff;
    else if (hdr->secondary_policy_code == ACM_SIMPLE_TYPE_ENFORCEMENT_POLICY)
        ret = (hdr->ssidref) >> 16;
 out:
    return ret;
}

/***************************** main **************************************/

int main(int argc, char **argv)
{

    int acm_cmd_fd, ret = 0;

    if (argc < 2)
        usage(argv[0]);

    if ((acm_cmd_fd = open("/proc/xen/privcmd", O_RDONLY)) <= 0)
    {
        printf("ERROR: Could not open xen privcmd device!\n");
        exit(-1);
    }

    if (!strcmp(argv[1], "getpolicy")) {
        if (argc != 2)
            usage(argv[0]);
        ret = acm_domain_getpolicy(acm_cmd_fd);
    } else if (!strcmp(argv[1], "loadpolicy")) {
        if (argc != 3)
            usage(argv[0]);
        ret = acm_domain_loadpolicy(acm_cmd_fd, argv[2]);
    } else if (!strcmp(argv[1], "dumpstats")) {
        if (argc != 2)
            usage(argv[0]);
        ret = acm_domain_dumpstats(acm_cmd_fd);
    } else if (!strcmp(argv[1], "getssid")) {
        ret = acm_domain_getssid(acm_cmd_fd, argc, argv);
    } else
        usage(argv[0]);

    close(acm_cmd_fd);
    return ret;
}
