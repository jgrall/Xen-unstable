/*
 * console.c
 *
 * read domain console output buffer ring in Xen 
 *
 */

#include <xeno/console.h>
#include <asm-i386/uaccess.h>

console_ring_t console_ring = {
    .len = 0
};

long read_console_ring(unsigned long str, unsigned int count, unsigned cmd)
{
    unsigned int len;
    
    len = (console_ring.len < count) ? console_ring.len : count;
    
    if ( copy_to_user((char *)str, console_ring.buf, len) )
        return -EFAULT;

    if ( cmd & CONSOLE_RING_CLEAR ) {
	console_ring.len = 0;
    }
    
    return len;
}
