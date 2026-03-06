/*
 * UTD (Universal Engine) - bootlog.c
 * Intelligent Boot Diagnostics & Logcat Overlay for GSIs/ROMs
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/drm.h>
#include <linux/drm_mode.h>
#include <linux/input.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

/* ═══════════════════════════════════════════════════════════════
   Configuration
   ═══════════════════════════════════════════════════════════════ */
#define FONT_W              8
#define FONT_H              16
#define STATUS_H_PX         20
#define BOOTDONE_POLL_MS    500

/* Colours (ARGB8888) */
#define COL_BG              0xFF0B0B0B
#define COL_KERN_FG         0xFF00FFFF
#define COL_LOGD_FG         0xFFFFD700
#define COL_STATUS_BG       0xFF12122A
#define COL_WHITE           0xFFFFFFFF
#define COL_TRUNC           0xFFFF4444
#define COL_DIVIDER         0xFF333355
#define DRAIN_SZ            8192

static uint32_t g_scale = 1;
static uint32_t fw = 8;
static uint32_t fh = 16;
static uint32_t status_h = 20;

/* Global state */
static int g_view_mode = 0;    /* 0=Kern, 1=Split, 2=Logcat */
static int g_scroll_pause = 0; /* Vol Up pauses kernel scroll */
static int g_cache_fd = -1;    /* For /cache/boot_debug.log */

/* Embedded 8x16 font (truncated for brevity, same as your file) */
static const uint8_t g_font[96][16] = {
{0},{0,0,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18},{0,0x66,0x66,0x66,0x24},
{0,0,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C},{0,0x18,0x7E,0xDB,0xD8,0x78,0x1E,0x1B,0xDB,0x7E,0x18},
{0,0,0xC6,0xC6,0x0C,0x18,0x30,0x60,0xC6,0xC6},{0,0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0x76},
{0,0x18,0x18,0x18,0x30},{0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C},
{0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30},{0,0,0,0x66,0x3C,0xFF,0x3C,0x66},
{0,0,0,0x18,0x18,0x7E,0x18,0x18},{0,0,0,0,0,0,0,0,0x18,0x18,0x30},
{0,0,0,0,0,0x7E},{0,0,0,0,0,0,0,0,0,0x18},{0,0,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0},
{0,0,0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C},{0,0,0x18,0x38,0x18,0x18,0x18,0x18,0x7E},
{0,0,0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E},{0,0,0x7E,0x0C,0x18,0x0E,0x06,0x66,0x3C},
{0,0,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C},{0,0,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C},
{0,0,0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C},{0,0,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30},
{0,0,0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C},{0,0,0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38},
{0,0,0,0x18,0,0,0,0x18},{0,0,0,0x18,0,0,0,0x18,0x18,0x30},
{0,0,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C},{0,0,0,0,0x7E,0,0x7E},
{0,0,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30},{0,0,0x3C,0x66,0x06,0x0C,0x18,0,0x18},
{0,0,0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3E},{0,0,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66},
{0,0,0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C},{0,0,0x3C,0x66,0x60,0x60,0x60,0x66,0x3C},
{0,0,0x78,0x6C,0x66,0x66,0x66,0x6C,0x78},{0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E},
{0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60},{0,0,0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C},
{0,0,0x66,0x66,0x66,0x7E,0x66,0x66,0x66},{0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x3C},
{0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38},{0,0,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66},
{0,0,0x60,0x60,0x60,0x60,0x60,0x60,0x7E},{0,0,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63},
{0,0,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66},{0,0,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C},
{0,0,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60},{0,0,0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06},
{0,0,0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66},{0,0,0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C},
{0,0,0x7E,0x18,0x18,0x18,0x18,0x18,0x18},{0,0,0x66,0x66,0x66,0x66,0x66,0x66,0x3C},
{0,0,0x66,0x66,0x66,0x66,0x3C,0x18,0x18},{0,0,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63},
{0,0,0x66,0x66,0x3C,0x18,0x3C,0x66,0x66},{0,0,0x66,0x66,0x3C,0x18,0x18,0x18,0x18},
{0,0,0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E},{0,0x1E,0x18,0x18,0x18,0x18,0x18,0x18,0x1E},
{0,0,0xC0,0x60,0x30,0x18,0x0C,0x06,0x03},{0,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x78},
{0,0x18,0x3C,0x66},{0,0,0,0,0,0,0,0,0,0xFF},{0,0x30,0x18},
{0,0,0,0,0x3C,0x06,0x3E,0x66,0x3E},{0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C},
{0,0,0,0,0x3C,0x60,0x60,0x60,0x3C},{0,0,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E},
{0,0,0,0,0x3C,0x66,0x7E,0x60,0x3C},{0,0,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30},
{0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x3C},{0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66},
{0,0,0x18,0,0x38,0x18,0x18,0x18,0x3C},{0,0,0x06,0,0x06,0x06,0x06,0x06,0x66,0x3C},
{0,0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66},{0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x3C},
{0,0,0,0,0x66,0x7F,0x7F,0x6B,0x63},{0,0,0,0,0x7C,0x66,0x66,0x66,0x66},
{0,0,0,0,0x3C,0x66,0x66,0x66,0x3C},{0,0,0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},
{0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x06},{0,0,0,0,0x6C,0x76,0x60,0x60,0x60},
{0,0,0,0,0x3C,0x60,0x3C,0x06,0x7C},{0,0,0x30,0x30,0x7C,0x30,0x30,0x30,0x1C},
{0,0,0,0,0x66,0x66,0x66,0x66,0x3E},{0,0,0,0,0x66,0x66,0x66,0x3C,0x18},
{0,0,0,0,0x63,0x6B,0x7F,0x3E,0x36},{0,0,0,0,0x66,0x3C,0x18,0x3C,0x66},
{0,0,0,0,0x66,0x66,0x3E,0x06,0x66,0x3C},{0,0,0,0,0x7E,0x0C,0x18,0x30,0x7E},
{0,0x0E,0x18,0x18,0x70,0x18,0x18,0x18,0x0E},{0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
{0,0x70,0x18,0x18,0x0E,0x18,0x18,0x18,0x70},{0,0x76,0xDC}
};

/* ═══════════════════════════════════════════════════════════════
   Display
   ═══════════════════════════════════════════════════════════════ */
typedef enum { DISP_NONE, DISP_FBDEV, DISP_DRM } disp_backend_t;

typedef struct {
    disp_backend_t backend;
    uint32_t  w, h, bpp, stride;
    uint8_t  *back;
    int       fb_fd;
    uint8_t  *fb_map;
    uint32_t  r_off, r_len, g_off, g_len, b_off;
    int       drm_fd;
    uint32_t  drm_fb_id, drm_crtc_id, drm_conn_id, drm_handle;
    uint8_t  *drm_map;
    int       has_dirtyfb;
} disp_t;

static disp_t g_d;

static inline uint32_t dpk(const disp_t *d, uint32_t a) {
    uint8_t r=(a>>16)&0xFF, g=(a>>8)&0xFF, b=a&0xFF;
    if (d->backend==DISP_DRM) return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
    return ((r>>(8-d->r_len))<<d->r_off)|((g>>(8-d->g_len))<<d->g_off)|(b<<d->b_off);
}

static void disp_fill(disp_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    uint32_t p=dpk(d,c);
    for (uint32_t r=y; r<y+h&&r<d->h; r++) {
        uint8_t *l=d->back+r*d->stride+x*d->bpp;
        for (uint32_t cc=0; cc<w&&x+cc<d->w; cc++)
            memcpy(l+cc*d->bpp, &p, d->bpp);
    }
}

static void disp_flush(disp_t *d) {
    if (!d->back) return;
    if (d->backend==DISP_FBDEV)
        memcpy(d->fb_map, d->back, (size_t)d->h*d->stride);
    else if (d->backend==DISP_DRM && d->drm_map) {
        memcpy(d->drm_map, d->back, (size_t)d->h*d->stride);
        if (d->has_dirtyfb) {
            struct drm_mode_fb_dirty_cmd dc={.fb_id=d->drm_fb_id};
            ioctl(d->drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dc);
        } else {
            struct drm_mode_crtc_page_flip pf={0};
            pf.crtc_id=d->drm_crtc_id; pf.fb_id=d->drm_fb_id;
            pf.flags=DRM_MODE_PAGE_FLIP_ASYNC;
            ioctl(d->drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
        }
    }
}

static void disp_glyph(disp_t *d, uint32_t px, uint32_t py, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (ch<0x20||ch>0x7F) ch='?';
    const uint8_t *rows=g_font[ch-0x20];
    uint32_t fp=dpk(d,fg), bp=dpk(d,bg);
    for (int r=0; r<FONT_H; r++) {
        uint8_t bits=rows[r];
        for (int c=0; c<FONT_W; c++) {
            uint32_t col=(bits&(0x80>>c))?fp:bp;
            /* Pixel Doubler for hi-res screens */
            for (int sy=0; sy<g_scale; sy++) {
                for (int sx=0; sx<g_scale; sx++) {
                    uint32_t dr = py + r*g_scale + sy;
                    uint32_t dc = px + c*g_scale + sx;
                    if (dr < d->h && dc < d->w) {
                        uint8_t *dst = d->back + dr*d->stride + dc*d->bpp;
                        memcpy(dst, &col, d->bpp);
                    }
                }
            }
        }
    }
}

static int disp_init_fbdev(disp_t *d) {
    int fd=open("/dev/graphics/fb0",O_RDWR);
    if (fd<0) fd=open("/dev/fb0",O_RDWR);
    if (fd<0) return -1;
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (ioctl(fd,FBIOGET_VSCREENINFO,&vi)||ioctl(fd,FBIOGET_FSCREENINFO,&fi)) {close(fd);return -1;}
    d->backend=DISP_FBDEV; d->fb_fd=fd; d->w=vi.xres; d->h=vi.yres;
    d->bpp=vi.bits_per_pixel/8; d->stride=fi.line_length;
    d->r_off=vi.red.offset; d->r_len=vi.red.length;
    d->g_off=vi.green.offset; d->g_len=vi.green.length; d->b_off=vi.blue.offset;
    d->fb_map=mmap(NULL,fi.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (d->fb_map==MAP_FAILED){close(fd);return -1;}
    d->back=malloc((size_t)d->h*d->stride);
    return d->back?0:-1;
}

static int disp_init_drm(disp_t *d) {
    int fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);
    if (fd<0) return -1;
    struct drm_mode_card_res res={0};
    ioctl(fd,DRM_IOCTL_MODE_GETRESOURCES,&res);
    if (!res.count_connectors||!res.count_crtcs){close(fd);return -1;}
    uint32_t *ci=calloc(res.count_connectors,4),*cr=calloc(res.count_crtcs,4);
    res.connector_id_ptr=(uint64_t)(uintptr_t)ci; res.crtc_id_ptr=(uint64_t)(uintptr_t)cr;
    ioctl(fd,DRM_IOCTL_MODE_GETRESOURCES,&res);
    uint32_t conn=0,crtc=0; struct drm_mode_modeinfo mode={0};
    for (uint32_t i=0;i<res.count_connectors&&!conn;i++){
        struct drm_mode_get_connector c={.connector_id=ci[i]};
        ioctl(fd,DRM_IOCTL_MODE_GETCONNECTOR,&c);
        if (c.connection!=1||!c.count_modes) continue;
        struct drm_mode_modeinfo *modes=calloc(c.count_modes,sizeof(*modes));
        uint32_t *ei=calloc(c.count_encoders,4);
        c.modes_ptr=(uint64_t)(uintptr_t)modes; c.encoders_ptr=(uint64_t)(uintptr_t)ei;
        ioctl(fd,DRM_IOCTL_MODE_GETCONNECTOR,&c);
        mode=modes[0]; conn=ci[i];
        struct drm_mode_get_encoder e={.encoder_id=c.encoder_id};
        ioctl(fd,DRM_IOCTL_MODE_GETENCODER,&e); crtc=e.crtc_id;
        free(modes);free(ei);
    }
    free(ci);free(cr);
    if (!conn||!crtc){close(fd);return -1;}
    struct drm_mode_create_dumb cd={.width=mode.hdisplay,.height=mode.vdisplay,.bpp=32};
    if (ioctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&cd)<0){close(fd);return -1;}
    struct drm_mode_fb_cmd fc={.width=cd.width,.height=cd.height,.pitch=cd.pitch,.bpp=32,.depth=24,.handle=cd.handle};
    if (ioctl(fd,DRM_IOCTL_MODE_ADDFB,&fc)<0){close(fd);return -1;}
    struct drm_mode_map_dumb md={.handle=cd.handle};
    if (ioctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&md)<0){close(fd);return -1;}
    size_t sz=(size_t)cd.height*cd.pitch;
    void *m=mmap(NULL,sz,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)md.offset);
    if (m==MAP_FAILED){close(fd);return -1;}
    struct drm_mode_crtc setcrtc={0};
    setcrtc.crtc_id=crtc; setcrtc.fb_id=fc.fb_id;
    setcrtc.set_connectors_ptr=(uint64_t)(uintptr_t)&conn;
    setcrtc.count_connectors=1; setcrtc.mode=mode; setcrtc.mode_valid=1;
    ioctl(fd,DRM_IOCTL_MODE_SETCRTC,&setcrtc);
    d->backend=DISP_DRM; d->drm_fd=fd; d->drm_fb_id=fc.fb_id; d->drm_crtc_id=crtc;
    d->drm_map=m; d->w=cd.width; d->h=cd.height; d->bpp=4; d->stride=cd.pitch;
    d->back=calloc(1,sz);
    struct drm_mode_fb_dirty_cmd dc={.fb_id=fc.fb_id};
    d->has_dirtyfb=(ioctl(fd,DRM_IOCTL_MODE_DIRTYFB,&dc)==0);
    return d->back?0:-1;
}

/* ═══════════════════════════════════════════════════════════════
   Pane
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t  x,y,w,h,cols,rows,cx,cy;
    uint32_t  fg,bg;
    uint8_t  *cbuf;
    uint32_t *ccol;
    int       alive;
    int       dirty;
} pane_t;

static pane_t g_kp, g_lp;

static void pane_init(pane_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fg, uint32_t bg) {
    if (p->cbuf) { free(p->cbuf); p->cbuf=NULL; }
    if (p->ccol) { free(p->ccol); p->ccol=NULL; }
    *p=(pane_t){0};
    if (h < fh) return; // Hidden pane
    p->x=x; p->y=y; p->w=w; p->h=h;
    p->cols=w/fw; p->rows=h/fh;
    p->fg=fg; p->bg=bg; p->alive=1;
    p->cbuf=calloc(p->cols*p->rows,1);
    p->ccol=calloc(p->cols*p->rows,4);
    if (p->cbuf) memset(p->cbuf,' ',p->cols*p->rows);
    if (p->ccol) for (uint32_t i=0;i<p->cols*p->rows;i++) p->ccol[i]=fg;
}

static void pane_redraw(pane_t *p) {
    if (!p->alive||!p->cbuf||p->rows==0) return;
    for (uint32_t r=0;r<p->rows;r++)
        for (uint32_t c=0;c<p->cols;c++) {
            uint32_t i=r*p->cols+c;
            disp_glyph(&g_d, p->x+c*fw, p->y+r*fh, p->cbuf[i], p->ccol?p->ccol[i]:p->fg, p->bg);
        }
    p->dirty=1;
}

static void pane_scroll(pane_t *p) {
    if (!p->alive||!p->cbuf||p->rows==0) return;
    memmove(p->cbuf, p->cbuf+p->cols, (p->rows-1)*p->cols);
    memset(p->cbuf+(p->rows-1)*p->cols,' ',p->cols);
    memmove(p->ccol, p->ccol+p->cols, (p->rows-1)*p->cols*4);
    for (uint32_t c=0;c<p->cols;c++) p->ccol[(p->rows-1)*p->cols+c]=p->fg;
    pane_redraw(p);
}

static void pane_putchar(pane_t *p, uint8_t ch, uint32_t fg) {
    if (!p->alive||!p->cbuf||!g_d.back||p->rows==0) return;
    /* Phase 1: Scroll Pause Feature */
    if (p == &g_kp && g_scroll_pause) return; 

    if (ch=='\n'||p->cx>=p->cols) {
        p->cx=0;
        if (p->cy+1>=p->rows) pane_scroll(p); else p->cy++;
        if (ch=='\n') return;
    }
    if (ch=='\t') {
        uint32_t s=4-(p->cx%4);
        for (uint32_t i=0;i<s;i++) pane_putchar(p,' ',fg);
        return;
    }
    if (ch<0x20) ch='.';
    uint32_t i=p->cy*p->cols+p->cx;
    p->cbuf[i]=ch; p->ccol[i]=fg;
    disp_glyph(&g_d, p->x+p->cx*fw, p->y+p->cy*fh, ch, fg, p->bg);
    p->cx++; p->dirty=1;
}

static void pane_puts(pane_t *p, const char *s, uint32_t fg) {
    while (*s) pane_putchar(p,(uint8_t)*s++,fg);
}

static void __attribute__((format(printf,3,4))) pane_printf(pane_t *p, uint32_t fg, const char *fmt, ...) {
    char buf[1024]; va_list ap;
    va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    pane_puts(p,buf,fg);
}

/* ═══════════════════════════════════════════════════════════════
   Diagnostics & Status Bar
   ═══════════════════════════════════════════════════════════════ */
static void draw_statusbar(void) {
    disp_fill(&g_d, 0, 0, g_d.w, status_h, COL_STATUS_BG);

    /* Phase 2: SELinux Check */
    int en = -1;
    FILE *sf = fopen("/sys/fs/selinux/enforce", "r");
    if(sf) { fscanf(sf, "%d", &en); fclose(sf); }
    const char *se_str = (en==1) ? "ENF" : (en==0) ? "PERM" : "UNK";

    /* Phase 2: Battery Check */
    int batt = -1;
    FILE *bf = fopen("/sys/class/power_supply/battery/capacity", "r");
    if(bf) { fscanf(bf, "%d", &batt); fclose(bf); }

    /* Phase 2: Mount Map Monitor */
    int sys=0, ven=0, dat=0, odm=0, apx=0;
    FILE *mf = fopen("/proc/mounts", "r");
    if(mf) {
        char mbuf[256];
        while(fgets(mbuf, sizeof(mbuf), mf)) {
            if(strstr(mbuf, " /system ") || strstr(mbuf, " / ")) sys=1;
            if(strstr(mbuf, " /vendor ")) ven=1;
            if(strstr(mbuf, " /data ")) dat=1;
            if(strstr(mbuf, " /odm ")) odm=1;
            if(strstr(mbuf, " /apex ")) apx=1;
        }
        fclose(mf);
    }
    
    char mt_str[128];
    if(sys && ven && dat && odm && apx) {
        strcpy(mt_str, "\x01" "Sys Ven Dat Odm Apx"); // \x01 triggers green
    } else {
        strcpy(mt_str, "\x02" "Fail: ");             // \x02 triggers red
        if(!sys) strcat(mt_str, "SUPER ");
        if(!ven) strcat(mt_str, "V ");
        if(!dat) strcat(mt_str, "D ");
        if(!odm) strcat(mt_str, "O ");
        if(!apx) strcat(mt_str, "A ");
    }

    char buf[512];
    snprintf(buf, sizeof(buf), " UTD v1.0 | SELinux: %s | ADB: USB+5555 | MT: %s\x03 | BATT: %d%% | V: %s %s",
        se_str, mt_str, batt, 
        g_view_mode==0?"KERN":(g_view_mode==1?"SPLIT":"LOGCAT"),
        g_scroll_pause?"\x02[PAUSED]\x03":"");

    uint32_t tx = 4;
    uint32_t fg = COL_WHITE;
    for(const char *c = buf; *c && tx + fw <= g_d.w; c++) {
        if(*c == '\x01') { fg = 0xFF39FF14; continue; } /* Green */
        if(*c == '\x02') { fg = COL_TRUNC;  continue; } /* Red */
        if(*c == '\x03') { fg = COL_WHITE;  continue; } /* Reset */
        disp_glyph(&g_d, tx, 2*g_scale, (uint8_t)*c, fg, COL_STATUS_BG);
        tx += fw;
    }
}

/* Dynamic Layout Logic */
static void layout_update(int mode) {
    if(mode < 0 || mode > 2) return;
    g_view_mode = mode; // 0=Kern, 1=Split, 2=Logcat

    float k_frac = (mode == 0) ? 1.0f : ((mode == 1) ? 0.60f : 0.0f);
    uint32_t usable = g_d.h - status_h;
    uint32_t new_kh = ((uint32_t)(usable * k_frac) / fh) * fh;
    uint32_t lh = usable - new_kh;
    if(mode == 0) lh = 0; else if(mode == 2) new_kh = 0; else lh -= 1;

    /* Save history */
    uint32_t okc=g_kp.cols, okr=g_kp.rows; uint8_t *okb=g_kp.cbuf; uint32_t *okcl=g_kp.ccol;
    uint32_t olc=g_lp.cols, olr=g_lp.rows; uint8_t *olb=g_lp.cbuf; uint32_t *olcl=g_lp.ccol;
    g_kp.cbuf=NULL; g_kp.ccol=NULL; g_lp.cbuf=NULL; g_lp.ccol=NULL;

    disp_fill(&g_d, 0, status_h, g_d.w, usable, COL_BG);
    if(mode == 1) disp_fill(&g_d, 0, status_h+new_kh, g_d.w, 1, COL_DIVIDER);

    pane_init(&g_kp, 0, status_h, g_d.w, new_kh, COL_KERN_FG, COL_BG);
    pane_init(&g_lp, 0, status_h+new_kh+(mode==1?1:0), g_d.w, lh, COL_LOGD_FG, COL_BG);

    /* Restore kernel buffer */
    if(okb && g_kp.rows > 0 && okr > 0) {
        uint32_t c_r = (okr<g_kp.rows)?okr:g_kp.rows;
        uint32_t c_c = (okc<g_kp.cols)?okc:g_kp.cols;
        uint32_t s_s = okr - c_r;
        for(uint32_t r=0; r<c_r; r++) for(uint32_t c=0; c<c_c; c++) {
            g_kp.cbuf[r*g_kp.cols+c] = okb[(s_s+r)*okc+c];
            g_kp.ccol[r*g_kp.cols+c] = okcl[(s_s+r)*okc+c];
        }
        g_kp.cy = c_r - 1; g_kp.cx = 0;
    }
    /* Restore logcat buffer */
    if(olb && g_lp.rows > 0 && olr > 0) {
        uint32_t c_r = (olr<g_lp.rows)?olr:g_lp.rows;
        uint32_t c_c = (olc<g_lp.cols)?olc:g_lp.cols;
        uint32_t s_s = olr - c_r;
        for(uint32_t r=0; r<c_r; r++) for(uint32_t c=0; c<c_c; c++) {
            g_lp.cbuf[r*g_lp.cols+c] = olb[(s_s+r)*olc+c];
            g_lp.ccol[r*g_lp.cols+c] = olcl[(s_s+r)*olc+c];
        }
        g_lp.cy = c_r - 1; g_lp.cx = 0;
    }
    free(okb); free(okcl); free(olb); free(olcl);

    pane_redraw(&g_kp); pane_redraw(&g_lp);
    draw_statusbar();
    disp_flush(&g_d);
}

/* ═══════════════════════════════════════════════════════════════
   Log Drain
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    char buf[DRAIN_SZ]; int len;
    pane_t *pane; uint32_t fg;
    int strip_kmsg, first_seen;
} drain_t;

static drain_t g_kd, g_ld;

static void drain_flush_line(drain_t *d, char *line) {
    char *msg = line;
    if (d->strip_kmsg) {
        char *semi = strchr(msg, ';');
        if (semi && (semi-msg)<48) msg=semi+1;
    }
    while (*msg==' '||*msg=='\r') msg++;
    if (!*msg) return;

    /* Phase 4: Snapshot to /cache */
    if (g_cache_fd >= 0) {
        write(g_cache_fd, msg, strlen(msg));
        write(g_cache_fd, "\n", 1);
    }

    uint32_t fg = d->fg;
    /* Phase 2: Color-Coded Parsing (Errors/Fatal in Red) */
    if (d == &g_ld && (strstr(msg, " E/") || strstr(msg, " F/"))) {
        fg = COL_TRUNC;
    }

    pane_puts(d->pane, msg, fg);
    pane_putchar(d->pane, '\n', fg);
}

static void drain_feed(drain_t *d, int fd) {
    ssize_t n = read(fd, d->buf+d->len, (DRAIN_SZ-1)-d->len);
    if (n <= 0) return;

    if (!d->first_seen) {
        d->first_seen = 1;
        if (d == &g_ld && g_view_mode == 0) layout_update(1); /* Auto-split */
    }

    d->len += (int)n; d->buf[d->len] = '\0';
    char *s = d->buf, *nl;
    while ((nl = memchr(s, '\n', d->len-(int)(s-d->buf)))) {
        *nl = '\0'; drain_flush_line(d, s); s = nl+1;
    }
    int rem = d->len-(int)(s-d->buf);
    if (rem > DRAIN_SZ-512) {
        d->buf[rem]='\0'; drain_flush_line(d, s);
        pane_puts(d->pane, "[trunc]\n", COL_TRUNC);
        rem=0; s=d->buf;
    }
    if (rem>0) memmove(d->buf, s, rem);
    d->len = rem;
}

/* ═══════════════════════════════════════════════════════════════
   Epoll Events & Background Tasks
   ═══════════════════════════════════════════════════════════════ */
#define EV_KMSG     1
#define EV_LOGCAT   2
#define EV_FLUSH    3
#define EV_BOOTDONE 4
#define EV_INPUT_BASE 0x100000000ULL

static int epoll_add(int efd, int fd, uint32_t ev, uint64_t tag) {
    struct epoll_event e={.events=ev}; e.data.u64=tag;
    return epoll_ctl(efd,EPOLL_CTL_ADD,fd,&e);
}

int main(void) {
    if (disp_init_fbdev(&g_d) < 0) disp_init_drm(&g_d);
    if (g_d.backend == DISP_NONE) return 0;

    /* Phase 1: Adaptive Font Scaling */
    g_scale = (g_d.h >= 1400) ? ((g_d.h >= 2800) ? 3 : 2) : 1;
    fw = FONT_W * g_scale; fh = FONT_H * g_scale; status_h = STATUS_H_PX * g_scale;

    /* Phase 4: Snapshot file */
    g_cache_fd = open("/cache/boot_debug.log", O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC, 0644);

    /* Phase 4: Remote TCP-ADB Override */
    if(fork() == 0) {
        char *argv[] = {"/system/bin/setprop", "service.adb.tcp.port", "5555", NULL};
        execv(argv[0], argv); _exit(0);
    }

    memset(&g_kd,0,sizeof(g_kd)); g_kd.pane=&g_kp; g_kd.fg=COL_KERN_FG; g_kd.strip_kmsg=1;
    memset(&g_ld,0,sizeof(g_ld)); g_ld.pane=&g_lp; g_ld.fg=COL_LOGD_FG; g_ld.strip_kmsg=0;

    layout_update(0);

    /* Phase 2: Apex Integrity Warning */
    struct stat st;
    if(stat("/linkerconfig/ld.config.txt", &st) != 0 || st.st_size == 0) {
        pane_puts(&g_kp, "\n\x02[FATAL] /linkerconfig/ld.config.txt is missing/empty!\n", COL_TRUNC);
        pane_puts(&g_kp, "        Apex mounts failed. adbd/surfaceflinger will crash.\n\n", COL_TRUNC);
        if(g_cache_fd >= 0) write(g_cache_fd, "FATAL: Apex ld.config.txt missing\n", 34);
    }

    int kpipe[2], lpipe[2]; pipe2(kpipe,O_CLOEXEC); pipe2(lpipe,O_CLOEXEC);
    if(fork()==0){close(kpipe[0]); dup2(kpipe[1],1); execlp("cat","cat","/proc/kmsg",NULL); _exit(1);}
    if(fork()==0){close(lpipe[0]); dup2(lpipe[1],1); dup2(lpipe[1],2); execlp("/system/bin/logcat","logcat","-v","brief",NULL); _exit(1);}
    fcntl(kpipe[0],F_SETFL,O_NONBLOCK); fcntl(lpipe[0],F_SETFL,O_NONBLOCK);

    int efd=epoll_create1(EPOLL_CLOEXEC);
    epoll_add(efd,kpipe[0],EPOLLIN,EV_KMSG); epoll_add(efd,lpipe[0],EPOLLIN,EV_LOGCAT);

    /* Phase 1: Hardware Input Listener */
    DIR *dir = opendir("/dev/input");
    if(dir) {
        struct dirent *de;
        while((de = readdir(dir))) {
            if(strncmp(de->d_name, "event", 5) == 0) {
                char path[64]; snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
                int ifd = open(path, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
                if(ifd >= 0) epoll_add(efd, ifd, EPOLLIN, EV_INPUT_BASE | (uint32_t)ifd);
            }
        }
        closedir(dir);
    }

    int flush_tfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    struct itimerspec its={{0,33000000L},{0,33000000L}}; timerfd_settime(flush_tfd,0,&its,NULL);
    epoll_add(efd,flush_tfd,EPOLLIN,EV_FLUSH);

    struct epoll_event evs[8];
    for (;;) {
        int n=epoll_wait(efd,evs,8,-1);
        for (int i=0;i<n;i++) {
            uint64_t t=evs[i].data.u64;
            if (t==EV_KMSG) drain_feed(&g_kd, kpipe[0]);
            else if (t==EV_LOGCAT) drain_feed(&g_ld, lpipe[0]);
            else if (t==EV_FLUSH) {
                uint64_t x; read(flush_tfd,&x,8);
                draw_statusbar(); /* Periodically check Mounts/Batt */
                if (g_kp.dirty||g_lp.dirty){ disp_flush(&g_d); g_kp.dirty=g_lp.dirty=0; }
            }
            else if ((t >> 32) == (EV_INPUT_BASE >> 32)) {
                int ifd = t & 0xFFFFFFFF;
                struct input_event ie;
                while(read(ifd, &ie, sizeof(ie)) == sizeof(ie)) {
                    if(ie.type == EV_KEY && ie.value == 1) {
                        if(ie.code == KEY_VOLUMEUP) g_scroll_pause = !g_scroll_pause;
                        else if(ie.code == KEY_VOLUMEDOWN) layout_update(1); // Force Split
                        else if(ie.code == KEY_POWER) layout_update((g_view_mode + 1) % 3);
                    }
                }
            }
        }
    }
    return 0;
}
