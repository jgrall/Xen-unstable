/******************************************************************************
 * vga.c
 * 
 * VGA support routines.
 */

#include <xen/config.h>
#include <xen/compile.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/spinlock.h>
#include <xen/console.h>
#include <xen/vga.h>
#include <asm/io.h>
#include "font.h"

/* Some of the code below is taken from SVGAlib.  The original,
   unmodified copyright notice for that code is below. */
/* VGAlib version 1.2 - (c) 1993 Tommy Frandsen                    */
/*                                                                 */
/* This library is free software; you can redistribute it and/or   */
/* modify it without any restrictions. This library is distributed */
/* in the hope that it will be useful, but without any warranty.   */

/* Multi-chipset support Copyright 1993 Harm Hanemaayer */
/* partially copyrighted (C) 1993 by Hartmut Schirmer */

/* VGA data register ports */
#define VGA_CRT_DC  	0x3D5	/* CRT Controller Data Register - color emulation */
#define VGA_CRT_DM  	0x3B5	/* CRT Controller Data Register - mono emulation */
#define VGA_ATT_R   	0x3C1	/* Attribute Controller Data Read Register */
#define VGA_ATT_W   	0x3C0	/* Attribute Controller Data Write Register */
#define VGA_GFX_D   	0x3CF	/* Graphics Controller Data Register */
#define VGA_SEQ_D   	0x3C5	/* Sequencer Data Register */
#define VGA_MIS_R   	0x3CC	/* Misc Output Read Register */
#define VGA_MIS_W   	0x3C2	/* Misc Output Write Register */
#define VGA_FTC_R	0x3CA	/* Feature Control Read Register */
#define VGA_IS1_RC  	0x3DA	/* Input Status Register 1 - color emulation */
#define VGA_IS1_RM  	0x3BA	/* Input Status Register 1 - mono emulation */
#define VGA_PEL_D   	0x3C9	/* PEL Data Register */
#define VGA_PEL_MSK 	0x3C6	/* PEL mask register */

/* EGA-specific registers */
#define EGA_GFX_E0	0x3CC	/* Graphics enable processor 0 */
#define EGA_GFX_E1	0x3CA	/* Graphics enable processor 1 */

/* VGA index register ports */
#define VGA_CRT_IC  	0x3D4	/* CRT Controller Index - color emulation */
#define VGA_CRT_IM  	0x3B4	/* CRT Controller Index - mono emulation */
#define VGA_ATT_IW  	0x3C0	/* Attribute Controller Index & Data Write Register */
#define VGA_GFX_I   	0x3CE	/* Graphics Controller Index */
#define VGA_SEQ_I   	0x3C4	/* Sequencer Index */
#define VGA_PEL_IW  	0x3C8	/* PEL Write Index */
#define VGA_PEL_IR  	0x3C7	/* PEL Read Index */

/* standard VGA indexes max counts */
#define VGA_CRT_C   	0x19	/* Number of CRT Controller Registers */
#define VGA_ATT_C   	0x15	/* Number of Attribute Controller Registers */
#define VGA_GFX_C   	0x09	/* Number of Graphics Controller Registers */
#define VGA_SEQ_C   	0x05	/* Number of Sequencer Registers */
#define VGA_MIS_C   	0x01	/* Number of Misc Output Register */

/* VGA misc register bit masks */
#define VGA_MIS_COLOR		0x01
#define VGA_MIS_ENB_MEM_ACCESS	0x02
#define VGA_MIS_DCLK_28322_720	0x04
#define VGA_MIS_ENB_PLL_LOAD	(0x04 | 0x08)
#define VGA_MIS_SEL_HIGH_PAGE	0x20

/* VGA CRT controller register indices */
#define VGA_CRTC_H_TOTAL	0
#define VGA_CRTC_H_DISP		1
#define VGA_CRTC_H_BLANK_START	2
#define VGA_CRTC_H_BLANK_END	3
#define VGA_CRTC_H_SYNC_START	4
#define VGA_CRTC_H_SYNC_END	5
#define VGA_CRTC_V_TOTAL	6
#define VGA_CRTC_OVERFLOW	7
#define VGA_CRTC_PRESET_ROW	8
#define VGA_CRTC_MAX_SCAN	9
#define VGA_CRTC_CURSOR_START	0x0A
#define VGA_CRTC_CURSOR_END	0x0B
#define VGA_CRTC_START_HI	0x0C
#define VGA_CRTC_START_LO	0x0D
#define VGA_CRTC_CURSOR_HI	0x0E
#define VGA_CRTC_CURSOR_LO	0x0F
#define VGA_CRTC_V_SYNC_START	0x10
#define VGA_CRTC_V_SYNC_END	0x11
#define VGA_CRTC_V_DISP_END	0x12
#define VGA_CRTC_OFFSET		0x13
#define VGA_CRTC_UNDERLINE	0x14
#define VGA_CRTC_V_BLANK_START	0x15
#define VGA_CRTC_V_BLANK_END	0x16
#define VGA_CRTC_MODE		0x17
#define VGA_CRTC_LINE_COMPARE	0x18
#define VGA_CRTC_REGS		VGA_CRT_C

/* VGA CRT controller bit masks */
#define VGA_CR11_LOCK_CR0_CR7	0x80 /* lock writes to CR0 - CR7 */
#define VGA_CR17_H_V_SIGNALS_ENABLED 0x80

/* VGA attribute controller register indices */
#define VGA_ATC_PALETTE0	0x00
#define VGA_ATC_PALETTE1	0x01
#define VGA_ATC_PALETTE2	0x02
#define VGA_ATC_PALETTE3	0x03
#define VGA_ATC_PALETTE4	0x04
#define VGA_ATC_PALETTE5	0x05
#define VGA_ATC_PALETTE6	0x06
#define VGA_ATC_PALETTE7	0x07
#define VGA_ATC_PALETTE8	0x08
#define VGA_ATC_PALETTE9	0x09
#define VGA_ATC_PALETTEA	0x0A
#define VGA_ATC_PALETTEB	0x0B
#define VGA_ATC_PALETTEC	0x0C
#define VGA_ATC_PALETTED	0x0D
#define VGA_ATC_PALETTEE	0x0E
#define VGA_ATC_PALETTEF	0x0F
#define VGA_ATC_MODE		0x10
#define VGA_ATC_OVERSCAN	0x11
#define VGA_ATC_PLANE_ENABLE	0x12
#define VGA_ATC_PEL		0x13
#define VGA_ATC_COLOR_PAGE	0x14

#define VGA_AR_ENABLE_DISPLAY	0x20

/* VGA sequencer register indices */
#define VGA_SEQ_RESET		0x00
#define VGA_SEQ_CLOCK_MODE	0x01
#define VGA_SEQ_PLANE_WRITE	0x02
#define VGA_SEQ_CHARACTER_MAP	0x03
#define VGA_SEQ_MEMORY_MODE	0x04

/* VGA sequencer register bit masks */
#define VGA_SR01_CHAR_CLK_8DOTS	0x01 /* bit 0: character clocks 8 dots wide are generated */
#define VGA_SR01_SCREEN_OFF	0x20 /* bit 5: Screen is off */
#define VGA_SR02_ALL_PLANES	0x0F /* bits 3-0: enable access to all planes */
#define VGA_SR04_EXT_MEM	0x02 /* bit 1: allows complete mem access to 256K */
#define VGA_SR04_SEQ_MODE	0x04 /* bit 2: directs system to use a sequential addressing mode */
#define VGA_SR04_CHN_4M		0x08 /* bit 3: selects modulo 4 addressing for CPU access to display memory */

/* VGA graphics controller register indices */
#define VGA_GFX_SR_VALUE	0x00
#define VGA_GFX_SR_ENABLE	0x01
#define VGA_GFX_COMPARE_VALUE	0x02
#define VGA_GFX_DATA_ROTATE	0x03
#define VGA_GFX_PLANE_READ	0x04
#define VGA_GFX_MODE		0x05
#define VGA_GFX_MISC		0x06
#define VGA_GFX_COMPARE_MASK	0x07
#define VGA_GFX_BIT_MASK	0x08

/* VGA graphics controller bit masks */
#define VGA_GR06_GRAPHICS_MODE	0x01

/* macro for composing an 8-bit VGA register index and value
 * into a single 16-bit quantity */
#define VGA_OUT16VAL(v, r)       (((v) << 8) | (r))

#define vgabase 0         /* use in/out port-access macros  */
#define VGA_OUTW_WRITE    /* can use outw instead of 2xoutb */

/*
 * generic VGA port read/write
 */
 
static inline uint8_t vga_io_r(uint16_t port)
{
    return inb(port);
}

static inline void vga_io_w(uint16_t port, uint8_t val)
{
    outb(val, port);
}

static inline void vga_io_w_fast(uint16_t port, uint8_t reg, uint8_t val)
{
    outw(VGA_OUT16VAL(val, reg), port);
}

static inline uint8_t vga_mm_r(void __iomem *regbase, uint16_t port)
{
    return readb((char *)regbase + port);
}

static inline void vga_mm_w(void __iomem *regbase, uint16_t port, uint8_t val)
{
    writeb(val, (char *)regbase + port);
}

static inline void vga_mm_w_fast(void __iomem *regbase, uint16_t port, uint8_t reg, uint8_t val)
{
    writew(VGA_OUT16VAL(val, reg), (char *)regbase + port);
}

static inline uint8_t vga_r(void __iomem *regbase, uint16_t port)
{
    if (regbase)
        return vga_mm_r(regbase, port);
    else
        return vga_io_r(port);
}

static inline void vga_w(void __iomem *regbase, uint16_t port, uint8_t val)
{
    if (regbase)
        vga_mm_w(regbase, port, val);
    else
        vga_io_w(port, val);
}


static inline void vga_w_fast(void __iomem *regbase, uint16_t port, uint8_t reg, uint8_t val)
{
    if (regbase)
        vga_mm_w_fast(regbase, port, reg, val);
    else
        vga_io_w_fast(port, reg, val);
}


/*
 * VGA CRTC register read/write
 */
 
static inline uint8_t vga_rcrt(void __iomem *regbase, uint8_t reg)
{
    vga_w(regbase, VGA_CRT_IC, reg);
    return vga_r(regbase, VGA_CRT_DC);
}

static inline void vga_wcrt(void __iomem *regbase, uint8_t reg, uint8_t val)
{
#ifdef VGA_OUTW_WRITE
    vga_w_fast(regbase, VGA_CRT_IC, reg, val);
#else
    vga_w(regbase, VGA_CRT_IC, reg);
    vga_w(regbase, VGA_CRT_DC, val);
#endif /* VGA_OUTW_WRITE */
}

/*
 * VGA sequencer register read/write
 */
 
static inline uint8_t vga_rseq(void __iomem *regbase, uint8_t reg)
{
    vga_w(regbase, VGA_SEQ_I, reg);
    return vga_r(regbase, VGA_SEQ_D);
}

static inline void vga_wseq(void __iomem *regbase, uint8_t reg, uint8_t val)
{
#ifdef VGA_OUTW_WRITE
    vga_w_fast(regbase, VGA_SEQ_I, reg, val);
#else
    vga_w(regbase, VGA_SEQ_I, reg);
    vga_w(regbase, VGA_SEQ_D, val);
#endif /* VGA_OUTW_WRITE */
}

/*
 * VGA graphics controller register read/write
 */
 
static inline uint8_t vga_rgfx(void __iomem *regbase, uint8_t reg)
{
    vga_w(regbase, VGA_GFX_I, reg);
    return vga_r(regbase, VGA_GFX_D);
}

static inline void vga_wgfx(void __iomem *regbase, uint8_t reg, uint8_t val)
{
#ifdef VGA_OUTW_WRITE
    vga_w_fast(regbase, VGA_GFX_I, reg, val);
#else
    vga_w(regbase, VGA_GFX_I, reg);
    vga_w(regbase, VGA_GFX_D, val);
#endif /* VGA_OUTW_WRITE */
}

/*
 * VGA attribute controller register read/write
 */
 
static inline uint8_t vga_rattr(void __iomem *regbase, uint8_t reg)
{
    vga_w(regbase, VGA_ATT_IW, reg);
    return vga_r(regbase, VGA_ATT_R);
}

static inline void vga_wattr(void __iomem *regbase, uint8_t reg, uint8_t val)
{
    vga_w(regbase, VGA_ATT_IW, reg);
    vga_w(regbase, VGA_ATT_W, val);
}

static int detect_video(void *video_base)
{
    volatile u16 *p = (volatile u16 *)video_base;
    u16 saved1 = p[0], saved2 = p[1];
    int video_found = 1;

    p[0] = 0xAA55;
    p[1] = 0x55AA;
    if ( (p[0] != 0xAA55) || (p[1] != 0x55AA) )
        video_found = 0;

    p[0] = 0x55AA;
    p[1] = 0xAA55;
    if ( (p[0] != 0x55AA) || (p[1] != 0xAA55) )
        video_found = 0;

    p[0] = saved1;
    p[1] = saved2;

    return video_found;
}

/* This is actually code from vgaHWRestore in an old version of XFree86 :-) */
static void *setup_vga(void)
{
    /* The following VGA state was saved from a chip in text mode 3. */
    static unsigned char regs[] = {
        /* Sequencer registers */
        0x03, 0x00, 0x03, 0x00, 0x02,
        /* CRTC registers */
        0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f, 0x20,
        0x0e, 0x00, 0x00, 0x01, 0xe0, 0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96,
        0xb9, 0xa3, 0xff,
        /* Graphic registers */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x00, 0xff,
        /* Attribute registers */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3a,
        0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x0c, 0x00, 0x0f, 0x08, 0x00
    };

    char *video;
    int i, j;

    if ( memory_is_conventional_ram(0xB8000) )
        goto no_vga;

    inb(VGA_IS1_RC);
    outb(0x00, VGA_ATT_IW);
    
    for ( i = j = 0; i < 5;  i++ )
        vga_wseq(vgabase, i, regs[j++]);
    
    /* Ensure CRTC registers 0-7 are unlocked by clearing bit 7 of CRTC[17]. */
    vga_wcrt(vgabase, 17, regs[5+17] & 0x7F);
    
    for ( i = 0; i < 25; i++ ) 
        vga_wcrt(vgabase, i, regs[j++]);
    
    for ( i = 0; i < 9;  i++ )
        vga_wgfx(vgabase, i, regs[j++]);
    
    inb(VGA_IS1_RC);
    for ( i = 0; i < 21; i++ )
        vga_wattr(vgabase, i, regs[j++]);
    
    inb(VGA_IS1_RC);
    outb(0x20, VGA_ATT_IW);

    video = ioremap(0xB8000, 0x8000);

    if ( !detect_video(video) )
    {
        iounmap(video);
        goto no_vga;
    }

    return video;

 no_vga:
    printk("No VGA adaptor detected!\n");
    return NULL;
}

static int vga_set_scanlines(unsigned scanlines)
{
    unsigned vtot, ovr, vss, vbs;
    uint8_t vse, vbe, misc = 0;

    switch (scanlines) {
    case 43*8:
        vtot = 0x1bf;
        vss = 0x183;
        vse = 0x05;
        vbs = 0x163;
        vbe = 0xba;
        break;
    case 25*16:
    case 28*14:
        vtot = 0x1bf;
        vss = 0x19c;
        vse = 0x0e;
        vbs = 0x196;
        vbe = 0xb9;
        break;
    case 30*16:
    case 34*14:
        vtot = 0x20b;
        vss = 0x1ea;
        vse = 0x0c;
        vbs = 0x1e7;
        vbe = 0x04;
        /* Preserve clock select bits and color bit, set correct sync polarity. */
        misc = (inb(VGA_MIS_R) & 0x0d) | 0xe2;
        break;
    default:
        return -ENOSYS;
    }

    ovr = vga_rcrt(vgabase, VGA_CRTC_OVERFLOW);
    if(vga_rcrt(vgabase, VGA_CRTC_V_DISP_END) + ((ovr & 0x02) << 7) + ((ovr & 0x40) << 3) == scanlines - 1)
        return 0;

    ovr = (ovr & 0x10)
        | ((vtot >> 8) & 0x01)
        | ((vtot >> 4) & 0x20)
        | (((scanlines - 1) >> 7) & 0x02)
        | (((scanlines - 1) >> 3) & 0x40)
        | ((vss >> 6) & 0x04)
        | ((vss >> 2) & 0x80)
        | ((vbs >> 5) & 0x08);
    vse |= vga_rcrt(vgabase, VGA_CRTC_V_SYNC_END) & 0x70;

    vga_wcrt(vgabase, VGA_CRTC_V_SYNC_END, vse & 0x7f); /* Vertical sync end (also unlocks CR0-7) */
    vga_wcrt(vgabase, VGA_CRTC_V_TOTAL, (uint8_t)vtot); /* Vertical total */
    vga_wcrt(vgabase, VGA_CRTC_OVERFLOW, (uint8_t)ovr); /* Overflow */
    vga_wcrt(vgabase, VGA_CRTC_V_SYNC_START, (uint8_t)vss); /* Vertical sync start */
    vga_wcrt(vgabase, VGA_CRTC_V_SYNC_END, vse | 0x80); /* Vertical sync end (also locks CR0-7) */
    vga_wcrt(vgabase, VGA_CRTC_V_DISP_END, (uint8_t)(scanlines - 1)); /* Vertical display end */
    vga_wcrt(vgabase, VGA_CRTC_V_BLANK_START, (uint8_t)vbs); /* Vertical blank start */
    vga_wcrt(vgabase, VGA_CRTC_V_BLANK_END, vbe); /* Vertical blank end */

    if (misc)
        outb(misc, VGA_MIS_W); /* Misc output register */

    return 0;
}

#define FONT_COUNT_MAX 256
#define FONT_HEIGHT_MAX 32
#define CHAR_MAP_SIZE (FONT_COUNT_MAX * FONT_HEIGHT_MAX)

/*
 * We use font slot 0 because ATI cards do not honour changes to the
 * character map select register. The fontslot parameter can be used to
 * choose a non-default slot if the video card supports it and you wish to
 * preserve the BIOS-initialised font data.
 */
static unsigned font_slot = 0;
integer_param("fontslot", font_slot);

static int vga_load_font(const struct font_desc *font, unsigned rows)
{
    unsigned fontheight = font ? font->height : 16;
    uint8_t fsr = vga_rcrt(vgabase, VGA_CRTC_MAX_SCAN); /* Font size register */
    int ret;

    if (font_slot > 3 || (!font_slot && !font))
        return -ENOSYS;

    if (font
        && (font->count > FONT_COUNT_MAX
            || fontheight > FONT_HEIGHT_MAX
            || font->width != 8))
        return -EINVAL;

    ret = vga_set_scanlines(rows * fontheight);
    if (ret < 0)
        return ret;

    if ((fsr & 0x1f) == fontheight - 1)
        return 0;

    /* First, the Sequencer */
    vga_wseq(vgabase, VGA_SEQ_RESET, 0x1);
    /* CPU writes only to map 2 */
    vga_wseq(vgabase, VGA_SEQ_PLANE_WRITE, 0x04);
    /* Sequential addressing */
    vga_wseq(vgabase, VGA_SEQ_MEMORY_MODE, 0x07);
    /* Clear synchronous reset */
    vga_wseq(vgabase, VGA_SEQ_RESET, 0x03);

    /* Now, the graphics controller, select map 2 */
    vga_wgfx(vgabase, VGA_GFX_PLANE_READ, 0x02);
    /* disable odd-even addressing */
    vga_wgfx(vgabase, VGA_GFX_MODE, 0x00);
    /* map start at A000:0000 */
    vga_wgfx(vgabase, VGA_GFX_MISC, 0x00);

    if ( font )
    {
        unsigned i, j;
        const uint8_t *data = font->data;
        uint8_t *map;

        map = ioremap(0xA0000 + font_slot*2*CHAR_MAP_SIZE, CHAR_MAP_SIZE);

        for ( i = j = 0; i < CHAR_MAP_SIZE; )
        {
            writeb(j < font->count * fontheight ? data[j++] : 0, map + i++);
            if ( !(j % fontheight) )
                while ( i & (FONT_HEIGHT_MAX - 1) )
                    writeb(0, map + i++);
        }

        iounmap(map);
    }

    /* First, the sequencer, Synchronous reset */
    vga_wseq(vgabase, VGA_SEQ_RESET, 0x01);
    /* CPU writes to maps 0 and 1 */
    vga_wseq(vgabase, VGA_SEQ_PLANE_WRITE, 0x03);
    /* odd-even addressing */
    vga_wseq(vgabase, VGA_SEQ_MEMORY_MODE, 0x03);
    /* Character Map Select: The default font is kept in slot 0. */
    vga_wseq(vgabase, VGA_SEQ_CHARACTER_MAP,
             font ? font_slot | (font_slot << 2) : 0x00);
    /* clear synchronous reset */
    vga_wseq(vgabase, VGA_SEQ_RESET, 0x03);

    /* Now, the graphics controller, select map 0 for CPU */
    vga_wgfx(vgabase, VGA_GFX_PLANE_READ, 0x00);
    /* enable even-odd addressing */
    vga_wgfx(vgabase, VGA_GFX_MODE, 0x10);
    /* map starts at b800:0 */
    vga_wgfx(vgabase, VGA_GFX_MISC, 0x0e);

    /* Font size register */
    fsr = (fsr & 0xe0) + (fontheight - 1);
    vga_wcrt(vgabase, VGA_CRTC_MAX_SCAN, fsr);

    /* Cursor shape registers */
    fsr  = vga_rcrt(vgabase, VGA_CRTC_CURSOR_END) & 0xe0;
    fsr |= fontheight - fontheight / 6;
    vga_wcrt(vgabase, VGA_CRTC_CURSOR_END, fsr);
    fsr  = vga_rcrt(vgabase, VGA_CRTC_CURSOR_START) & 0xe0;
    fsr |= (fsr & 0x1f) - 1;
    vga_wcrt(vgabase, VGA_CRTC_CURSOR_START, fsr);

    return 0;
}


/*
 * HIGH-LEVEL INITIALISATION AND TEXT OUTPUT.
 */

static int vgacon_enabled = 0;
static int vgacon_keep    = 0;
static int vgacon_lines   = 25;
static const struct font_desc *font;

static int xpos, ypos;
static unsigned char *video;

/* vga: comma-separated options. */
static char opt_vga[30] = "";
string_param("vga", opt_vga);

/* VGA text-mode definitions. */
#define COLUMNS     80
#define LINES       vgacon_lines
#define ATTRIBUTE   7
#define VIDEO_SIZE  (COLUMNS * LINES * 2)

void vga_init(void)
{
    char *p;

    for ( p = opt_vga; p != NULL; p = strchr(p, ',') )
    {
        if ( *p == ',' )
            p++;
        if ( strncmp(p, "keep", 4) == 0 )
            vgacon_keep = 1;
        else if ( strncmp(p, "text-80x", 8) == 0 )
            vgacon_lines = simple_strtoul(p + 8, NULL, 10);
    }

    video = setup_vga();
    if ( !video )
        return;

    switch ( vgacon_lines )
    {
    case 25:
    case 30:
        font = &font_vga_8x16;
        break;
    case 28:
    case 34:
        font = &font_vga_8x14;
        break;
    case 43:
    case 50:
    case 60:
        font = &font_vga_8x8;
        break;
    default:
        vgacon_lines = 25;
        break;
    }

    if ( (font != NULL) && (vga_load_font(font, vgacon_lines) < 0) )
    {
        vgacon_lines = 25;
        font = NULL;
    }
    
    /* Clear the screen. */
    memset(video, 0, VIDEO_SIZE);
    xpos = ypos = 0;

    /* Disable cursor. */
    vga_wcrt(vgabase, VGA_CRTC_CURSOR_START, 0x20);

    vgacon_enabled = 1;
}

void vga_endboot(void)
{
    if ( !vgacon_enabled )
        return;

    if ( !vgacon_keep )
        vgacon_enabled = 0;
        
    printk("Xen is %s VGA console.\n",
           vgacon_keep ? "keeping" : "relinquishing");
}


static void put_newline(void)
{
    xpos = 0;
    ypos++;

    if ( ypos >= LINES )
    {
        ypos = LINES-1;
        memmove((char*)video, 
                (char*)video + 2*COLUMNS, (LINES-1)*2*COLUMNS);
        memset((char*)video + (LINES-1)*2*COLUMNS, 0, 2*COLUMNS);
    }
}

void vga_putchar(int c)
{
    if ( !vgacon_enabled )
        return;

    if ( c == '\n' )
    {
        put_newline();
    }
    else
    {
        if ( xpos >= COLUMNS )
            put_newline();
        video[(xpos + ypos * COLUMNS) * 2]     = c & 0xFF;
        video[(xpos + ypos * COLUMNS) * 2 + 1] = ATTRIBUTE;
        ++xpos;
    }
}

int fill_console_start_info(struct dom0_vga_console_info *ci)
{
    memset(ci, 0, sizeof(*ci));

    if ( !vgacon_enabled )
        return 0;

    ci->video_type = XEN_VGATYPE_TEXT_MODE_3;
    ci->u.text_mode_3.rows     = LINES;
    ci->u.text_mode_3.columns  = COLUMNS;
    ci->u.text_mode_3.cursor_x = 0;
    ci->u.text_mode_3.cursor_y = LINES - 1;
    ci->u.text_mode_3.font_height = font ? font->height : 16;

    return 1;
}
