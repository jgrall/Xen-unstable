/******************************************************************************
 * libxc_misc.c
 * 
 * Miscellaneous control interface functions.
 */

#include "libxc_private.h"

int xc_interface_open(void)
{
    int fd = open("/proc/xeno/privcmd", O_RDWR);
    if ( fd == -1 )
        PERROR("Could not obtain handle on privileged command interface");
    return fd;
}

int xc_interface_close(int xc_handle)
{
    return close(xc_handle);
}


#define CONSOLE_RING_CLEAR	1

int xc_readconsolering(int xc_handle,
                       char *str, 
                       unsigned int max_chars, 
                       int clear)
{
    int ret;
    dom0_op_t op;

    op.cmd = DOM0_READCONSOLE;
    op.u.readconsole.str = (unsigned long)str;
    op.u.readconsole.count = max_chars;
    op.u.readconsole.cmd = clear ? CONSOLE_RING_CLEAR : 0;

    if ( (ret = do_dom0_op(xc_handle, &op)) > 0 )
        str[ret] = '\0';

    return ret;
}    

