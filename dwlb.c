#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>
#include <mpd/client.h>

#include "utf8.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "dwl-ipc-unstable-v2-protocol.h"

#define DIE(fmt, ...)						\
	do {							\
		fprintf(stderr, fmt "\n", ##__VA_ARGS__);	\
		exit(1);					\
	} while (0)
#define EDIE(fmt, ...)						\
	DIE(fmt ": %s", ##__VA_ARGS__, strerror(errno));

#define MIN(a, b)				\
	((a) < (b) ? (a) : (b))
#define MAX(a, b)				\
	((a) > (b) ? (a) : (b))
#define LENGTH(x)				\
	(sizeof x / sizeof x[0])

#define ARRAY_INIT_CAP 16
#define ARRAY_EXPAND(arr, len, cap, inc)				\
	do {								\
		uint32_t new_len, new_cap;				\
		new_len = (len) + (inc);				\
		if (new_len > (cap)) {					\
			new_cap = new_len * 2;				\
			if (new_cap < ARRAY_INIT_CAP)			\
				new_cap = ARRAY_INIT_CAP;		\
			if (!((arr) = realloc((arr), sizeof(*(arr)) * new_cap))) \
				EDIE("realloc");			\
			(cap) = new_cap;				\
		}							\
		(len) = new_len;					\
	} while (0)
#define ARRAY_APPEND(arr, len, cap, ptr)		\
	do {						\
		ARRAY_EXPAND((arr), (len), (cap), 1);	\
		(ptr) = &(arr)[(len) - 1];		\
	} while (0)

#define PROGRAM "dwlb"
#define VERSION "0.2"
#define USAGE	"us"

#define TEXT_MAX 2048

enum { WheelUp, WheelDown };

struct Blockpos{
	uint32_t xl;
	uint32_t xr;
	uint32_t sel;
	uint32_t clickid;
	uint32_t scroll;
};

typedef struct Block Block;
struct Block{
	pixman_color_t fg;
	pixman_color_t bg;
	pixman_color_t acc;
	char text[TEXT_MAX];
	pixman_color_t selfg;
	pixman_color_t selbg;
	pixman_color_t selacc;
	float xperc;
	float yperc;
	enum{LEFT, CENTER, RIGHT} gravity;
	void (*clickfn)(Block*, struct Blockpos*);
	int (*updatefn)(Block*);
	int32_t state;
	int32_t arg;
	uint32_t maxw;
};

typedef struct {
	struct wl_output *wl_output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct zxdg_output_v1 *xdg_output;
	struct zdwl_ipc_output_v2 *dwl_wm_output;

	uint32_t registry_name;
	char *xdg_output_name;

	bool configured;
	uint32_t width, height;
	uint32_t textpadding;
	uint32_t stride, bufsize;

	uint32_t mtags, ctags, urg, sel;
	char *layout, *window_title;
	uint32_t layout_idx, last_layout_idx;

	bool hidden, bottom;
	bool redraw;

	struct wl_list link;
	struct Blockpos *bdat;
} Bar;

typedef struct {
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;
	uint32_t registry_name;

	Bar *bar;
	uint32_t pointer_x, pointer_y;
	uint32_t pointer_button;
	uint32_t scroll_dx;

	struct wl_list link;
} Seat;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;

static struct zdwl_ipc_manager_v2 *dwl_wm;
static struct wl_cursor_image *cursor_image;
static struct wl_surface *cursor_surface;

static struct wl_list bar_list, seat_list;

static char **tags;
static uint32_t tags_l, tags_c;
static char **layouts;
static uint32_t layouts_l, layouts_c;

static struct fcft_font *font;
static uint32_t height, textpadding, buffer_scale;

static bool run_display;
struct mpd_connection *conn;

int timer(Block*);
int wireless(Block*);
int backlight(Block*);
int songinfo(Block*);
int bartime(Block*);
int battery(Block*);
int uptime(Block*);
int file(Block*);
int volume(Block*);
void clickhide(Block*, struct Blockpos*);
void cycle(Block*, struct Blockpos*);
void scroll(Block*, struct Blockpos*);
#include "config.h"

static void
shell_command(char *command)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", command, NULL);
		exit(EXIT_SUCCESS);
	}
}

/* status commands:{{{*/

int
volume(Block *st){
	static char v[100];
	if(st->state != 0){
		st->state /= 5;
		if(st->state >= 0)
			sprintf(v, "pactl set-sink-volume $(pactl get-default-sink) +%d%%", st->state);
		else
			sprintf(v, "pactl set-sink-volume $(pactl get-default-sink) %d%%", st->state);
		shell_command(v);
		st->state=0;
		return 0;
	}
	int vol_prc=0;
	//FILE* f = popen("pactl get-sink-volume $(pactl get-default-sink)","r");
	//fscanf(f, "Volume: front-left: %d /", &vol_prc);
	//fscanf(f, "%d", &vol_prc);
	//pclose(f);
	st->yperc = (float)vol_prc/100.;
	return 0;
}

int
timer(Block* st){
	static int mode = 0;
	int rettime;
	static struct timespec base[3];
	static struct timespec tm[3];
	static const uint32_t cl_type[] = {CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_REALTIME};

	if((st->state & 1) != 0){
		strcpy(st->text, "󱦟");
		return 0;
	}
	if((st->state & 2) != 0)
		mode++, st->state ^= 2;

	clock_gettime(cl_type[mode%3], tm+mode%3);

	if((st->state & 4) != 0 && tm[mode%3].tv_sec == base[mode%3].tv_sec && tm[mode%3].tv_nsec - base[mode%3].tv_nsec < 7e8)
		memset(base + mode%3, 0, sizeof(struct timespec)), st->state ^= 4;
	if((st->state & 4) != 0)
		base[mode%3] = tm[mode%3], st->state ^= 4;
	rettime = tm[mode%3].tv_sec - base[mode%3].tv_sec;

	if(rettime<60*60)
		sprintf(st->text, "%02d:%02d", (int)(rettime/60),(int)rettime%60);
	else
		sprintf(st->text, "%02d:%02d", (int)(rettime/3600),(int)(rettime/60)%60);
	st->yperc = (float)(mode%3 + 1)/3.;
	return 0;
}

int
uptime(Block* st){
	static enum{ICON, UPTIME, TIMER} mode = UPTIME;
	float value;
	static FILE *f = 0;
	static int zerot = 0;
	if((st->state & 1) != 0)
		mode = (mode == ICON ? TIMER : ICON), st->state ^= 1;
	if((st->state & 2) != 0)
		mode = (mode == ICON ? ICON : (mode == TIMER ? UPTIME : TIMER)), st->state ^= 2;
	if((st->state & 4) != 0)
		zerot = time(0), mode = TIMER, st->state ^= 4;
	switch(mode){
case ICON:
		strcpy(st->text, "󱦟");
		return 0;
case UPTIME:	f=fopen("/proc/uptime","r");
		fscanf(f, "%f", &value);
		fclose(f);
		break;
case TIMER:
		value = time(0) - zerot;
		break;
	}
	if(value<60*60)
		sprintf(st->text, "%02d:%02d", (int)(value/60),(int)value%60);
	else
		sprintf(st->text, "%02d:%02d", (int)(value/3600),(int)(value/60)%60);
	return 0;
}

int
bartime(Block* st){
	static time_t t=0;
	time_t tt = time(0);
	if(tt == t) return 0;
	t = tt;
	static struct tm now;
	localtime_r(&t, &now);
	st->xperc = (float)now.tm_sec/60.0f;
	st->yperc = 1;
	sprintf(st->text, "%02d:%02d", now.tm_hour, now.tm_min);
	return 1;
}

int
wireless(Block* st){
	st->xperc = 1.;
	st->yperc = 1.;
	FILE* f = fopen("/proc/net/wireless", "r");
	fseek(f, 162+13, SEEK_SET);
	float level = 0;
	fscanf(f, "%f%f", &level, &level);
	fclose(f);
	if((st->state & 1)!= 0){
		if(level == 0)
			strcpy(st->text,"󰤮");
		else if(level >-40)
			strcpy(st->text,"󰤨");
		else if(level >-50)
			strcpy(st->text,"󰤥");
		else if(level >-60)
			strcpy(st->text,"󰤢");
		else if(level >-70)
			strcpy(st->text,"󰤟");
		else
			strcpy(st->text,"󰤯");
	}
	else{
		sprintf(st->text, "%.0fdB", level);
	}
	return 0;
}

void
scroll(Block* st, struct Blockpos* dt){
	if(dt->clickid == BTN_LEFT)
		st->state ^= 2;
	if(dt->clickid == BTN_RIGHT)
		st->state ^= 1;
	if(dt->clickid == BTN_MIDDLE)
		st->state ^= 4;
	if(dt->scroll != 0){
		st->state ^= 8;
		st->arg = dt->scroll;
		dt->scroll = 0;
	}
	dt->clickid = 0;
}

void
clickhide(Block* st, struct Blockpos* dt){
	if(dt->clickid == BTN_LEFT)
		st->state ^= 2;
	if(dt->clickid == BTN_RIGHT)
		st->state ^= 1;
	if(dt->clickid == BTN_MIDDLE)
		st->state ^= 4;
}

void
cycle(Block* st, struct Blockpos* dt){
	if(dt->clickid == BTN_RIGHT)
		st->state += 1;
}

int
backlight(Block* st){
	FILE* f;
	f = fopen("/sys/class/backlight/intel_backlight/brightness","r");
	uint32_t now = 0;
	fscanf(f, "%d", &now);
	fclose(f);
	if(st->arg != 0){
		f = fopen("/sys/class/backlight/intel_backlight/brightness","w");
		if(f != 0){
			fprintf(f, "%d", now+st->arg);
			fclose(f);
		}
		st->arg=0;
	}
	f = fopen("/sys/class/backlight/intel_backlight/max_brightness","r");
	int max = 0;
	fscanf(f, "%d", &max);
	fclose(f);
	sprintf(st->text, "󱉵"); //󱉵󱈈
	st->xperc = 1;
	st->yperc = (float)now/(float)max;
	return 0;
}

int
songinfo(Block* st){
	mpd_run_clearerror(conn);
	if((st->state & 8) != 0){
		st->state ^= 8;
		mpd_run_change_volume(conn, st->arg/5);
		return 0;
	}
	if((st->state & 2) != 0){
		st->state ^= 2;
		mpd_run_toggle_pause(conn);
	}
	if((st->state & 4) != 0){
		st->state ^= 4;
		shell_command("foot ncmpcpp");
	}
	if((st->state & 1) != 0){
		st->xperc = 1;
		st->yperc = 1;
		strcpy(st->text, "󰽲");//  
		return 0;
	}
	struct mpd_status* stat = mpd_run_status(conn);
	/* get song name (and length) */
	struct mpd_song* now_playing = mpd_run_current_song(conn);
	if(now_playing == 0 || stat==0){
		strcpy(st->text, "no song active");
		st->xperc = 1;
		st->yperc = 1;
		return 0;
	}
	const char* name = mpd_song_get_tag(now_playing, MPD_TAG_TITLE, 0);
	//mpd_response_finish(conn);
	const char* artist = mpd_song_get_tag(now_playing, MPD_TAG_ALBUM_ARTIST, 0);
	if(artist == NULL)
		artist = mpd_song_get_tag(now_playing, MPD_TAG_ARTIST, 0);

	unsigned a = mpd_song_get_duration(now_playing);
	unsigned b = mpd_status_get_elapsed_time(stat);
	sprintf(st->text, "%s - %s", artist, name);
	st->xperc = (float)b/(float)a;
	st->yperc = (float)mpd_status_get_volume(stat)/100.0f;

	mpd_song_free(now_playing);
	mpd_status_free(stat);
	return 0;
}

int
battery(Block* st){
	FILE* f;
	f = fopen("/sys/class/power_supply/BAT1/power_now","r");
	int pow = 0;
	fscanf(f, "%d", &pow);
	fclose(f);

	float full = 0;
	float now = 0;
	f = fopen("/sys/class/power_supply/BAT1/energy_full","r");
	fscanf(f, "%f", &full);
	fclose(f);
	f = fopen("/sys/class/power_supply/BAT1/energy_now","r");
	fscanf(f, "%f", &now);
	fclose(f);

	if(st->state % 3 == 0)
		sprintf(st->text, "%.2fW", (float)pow/1000000.0f);
	if(st->state % 3 == 1)
		sprintf(st->text, "");//󰁹
	if(st->state % 3 == 2)
		sprintf(st->text, "%.2f%%", 100*now/full);
	st->xperc = 1.0f;
	st->yperc = now/full;
	return 0;
}

int
file(Block* st){
	static FILE* f;
	if(f == 0)
		f = fopen("/home/pyiin/bar","r");
	if((st->state & 1) != 0){
		char* tmp = malloc(TEXT_MAX);
		size_t n = TEXT_MAX;
		ssize_t b_read = getline(&tmp, &n, f);
		if(b_read <= 0){
			freopen("/home/pyiin/bar","r",f);
			strcpy(st->text, "󱀺");
		}
		else{
			strncpy(st->text, tmp, b_read-1);
			st->text[b_read-1] = 0;
		}
		free(tmp);
		st->state ^= 1;
	}
	if(st->text[0] == 0){
		strcpy(st->text, "󱀺");
	}
	return 0;
}
/*}}}*/

/* OK; BUFFER:{{{*/
static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};
/*}}}*/

/* DRAW ROUTINES:{{{*/
/* Shared memory support function adapted from [wayland-book] */
static int
allocate_shm_file(size_t size)
{
	int fd = memfd_create("surface", MFD_CLOEXEC);
	if (fd == -1)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret == -1 && errno == EINTR);
	if (ret == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

#define TEXTW(text, padding)				\
	zdraw_text(text, 0, 0, NULL, NULL, UINT32_MAX, 0, padding)

// draw text, return end of drawn text.
static uint32_t
zdraw_text(char *text,
	  uint32_t x,
	  uint32_t y,
	  pixman_image_t *img,
	  pixman_color_t *color,
	  uint32_t max_x,
	  uint32_t buf_height,
	  uint32_t padding)
{
	if (!text || !*text || !max_x)
		return x;
	bool draw_img = img && color;
	pixman_image_t *fill;
	if (draw_img)
		fill = pixman_image_create_solid_fill(color);

	uint32_t ix = x, nx;
	x += padding;
	uint32_t codepoint, state = UTF8_ACCEPT, last_cp = 0;
	for (char *p = text; *p; p++) {
		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p)) continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph) continue;

		/* Adjust x position based on kerning with previous glyph */
		long kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &kern, NULL);
		if ((nx = x + kern + glyph->advance.x) + padding > max_x)
			break;
		last_cp = codepoint;
		x += kern;

		if (draw_img) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fill, img, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fill, glyph->pix, img, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			}
		}
		/* increment pen position */
		x = nx;
	}
	if (draw_img)
		pixman_image_unref(fill);
	if (!last_cp)
		return ix;

	nx = x + padding;
	return nx;
}

// old color{{{
static uint32_t
draw_text(char *text,
	  uint32_t x,
	  uint32_t y,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t *fg_color,
	  pixman_color_t *bg_color,
	  uint32_t max_x,
	  uint32_t buf_height,
	  uint32_t padding)
{
	if (!text || !*text || !max_x)
		return x;

	uint32_t ix = x, nx;

	if ((nx = x + padding) + padding >= max_x)
		return x;
	x = nx;

	bool draw_fg = foreground && fg_color;
	bool draw_bg = background && bg_color;

	pixman_image_t *fg_fill;
	if (draw_fg)
		fg_fill = pixman_image_create_solid_fill(fg_color);

	uint32_t codepoint, state = UTF8_ACCEPT, last_cp = 0;
	for (char *p = text; *p; p++) {
		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		/* Adjust x position based on kerning with previous glyph */
		long kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &kern, NULL);
		if ((nx = x + kern + glyph->advance.x) + padding > max_x)
			break;
		last_cp = codepoint;
		x += kern;

		if (draw_fg) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fg_fill, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fg_fill, glyph->pix, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			}
		}
		/* increment pen position */
		x = nx;
	}

	if (draw_fg)
		pixman_image_unref(fg_fill);
	if (!last_cp)
		return ix;

	nx = x + padding;
	if (draw_bg) {
		/* Fill padding background */
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = ix, .x2 = nx,
						.y1 = 0, .y2 = buf_height
					});
	}

	return nx;
}
//}}}

#define TEXT_WIDTH(text, maxwidth, padding)				\
	draw_text(text, 0, 0, NULL, NULL, NULL, NULL, maxwidth, 0, padding)

static int
draw_frame(Bar *bar)
{
	/* Allocate buffer to be attached to the surface */
        int fd = allocate_shm_file(bar->bufsize);
	if (fd == -1)
		return -1;

	uint32_t *data = mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bar->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Pixman image corresponding to main buffer */
	pixman_image_t *final = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, data, bar->width * 4);

	/* Text background and foreground layers */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);

	/* Draw on images */
	uint32_t x = 0;
	uint32_t y = (bar->height + font->ascent - font->descent) / 2;
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
				&inactive_bg_color, 1, &(pixman_box32_t){
				.x1 = 0, .x2 = bar->width,
				.y1 = 0, .y2 = bar->height
				});
	for (uint32_t i = 0; i < tags_l; i++) {
		const bool active = bar->mtags & 1 << i;
		const bool occupied = bar->ctags & 1 << i;
		const bool urgent = bar->urg & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;

		pixman_color_t *fg_color = urgent ? &urgent_fg_color : (active ? &active_fg_color : (occupied ? &occupied_fg_color : &inactive_fg_color));
		pixman_color_t *bg_color = urgent ? &urgent_bg_color : (active ? &active_bg_color : (occupied ? &occupied_bg_color : &inactive_bg_color));

		if (!hide_vacant && occupied) {
			pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
						fg_color, 1, &(pixman_box32_t){
							.x1 = x + boxs, .x2 = x + boxs + boxw,
							.y1 = boxs, .y2 = boxs + boxw
						});
			if ((!bar->sel || !active) && boxw >= 3) {
				/* Make box hollow */
				pixman_image_fill_boxes(PIXMAN_OP_SRC, foreground,
							&(pixman_color_t){ 0 },
							1, &(pixman_box32_t){
								.x1 = x + boxs + 1, .x2 = x + boxs + boxw - 1,
								.y1 = boxs + 1, .y2 = boxs + boxw - 1
							});
			}
		}

		x = draw_text(tags[i], x, y, foreground, background, fg_color, bg_color,
			      bar->width, bar->height, bar->textpadding);
	}

	x = draw_text(bar->layout, x, y, foreground, background,
		      &inactive_fg_color, &inactive_bg_color, bar->width,
		      bar->height, bar->textpadding);
	// from x free space
	uint32_t nx;
	uint32_t lx = x;
	uint32_t cx = bar->width/2;
	uint32_t rx = bar->width;
	for(int i=0; i<nblocks; i++){
		switch(blocks[i].gravity){
			case RIGHT:
				x = rx = (rx - TEXT_WIDTH(blocks[i].text, blocks[i].maxw == 0 ? bar->width : blocks[i].maxw, bar->textpadding));
				break;
			case LEFT:
				x = lx;
				break;
			case CENTER:
				x =(cx - TEXT_WIDTH(blocks[i].text,bar->width, bar->textpadding)/2);
				break;
		}
		blocks[i].xperc = MAX(0., MIN(1., blocks[i].xperc));
		blocks[i].yperc = MAX(0., MIN(1., blocks[i].yperc));
		nx = draw_text(blocks[i].text, x, y, foreground, 0,  bar->bdat[i].sel ? &blocks[i].selfg : &blocks[i].fg, 0, blocks[i].maxw == 0 ? bar->width : blocks[i].maxw + x, bar->height, bar->textpadding);
		pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
					bar->bdat[i].sel ? &blocks[i].selbg : &blocks[i].bg,
					1, &(pixman_box32_t){
						.x1 = x, .x2 = nx,
						.y1 = 0, .y2 = bar->height
					});
		pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
					bar->bdat[i].sel ? &blocks[i].selacc : &blocks[i].acc,
					1, &(pixman_box32_t){
						.x1 = x, .x2 = x + (nx - x)*blocks[i].xperc,
						.y1 = bar->height * (1 - blocks[i].yperc), .y2 = bar->height
					});
		bar->bdat[i].xl = x;
		bar->bdat[i].xr = nx;
	}
	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(final);

	munmap(data, bar->bufsize);

	wl_surface_set_buffer_scale(bar->wl_surface, buffer_scale);
	wl_surface_attach(bar->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
	wl_surface_commit(bar->wl_surface);

	return 0;
}

/*}}}*/

/* OK; LAYER SURFACE:{{{*/

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			uint32_t serial, uint32_t w, uint32_t h)
{
	w = w * buffer_scale;
	h = h * buffer_scale;

	zwlr_layer_surface_v1_ack_configure(surface, serial);

	Bar *bar = (Bar *)data;

	if (bar->configured && w == bar->width && h == bar->height)
		return;

	bar->width = w;
	bar->height = h;
	bar->stride = bar->width * 4;
	bar->bufsize = bar->stride * bar->height;
	bar->configured = true;

	draw_frame(bar);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};
/*}}}*/

/* OK; XDG_OUTPUT:{{{ */
static void
output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
	Bar *bar = (Bar *)data;

	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!(bar->xdg_output_name = strdup(name)))
		EDIE("strdup");
}

static void
output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
			int32_t x, int32_t y)
{
}

static void
output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
		    int32_t width, int32_t height)
{
}

static void
output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
output_description(void *data, struct zxdg_output_v1 *xdg_output,
		   const char *description)
{
}

static const struct zxdg_output_v1_listener output_listener = {
	.name = output_name,
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = output_done,
	.description = output_description
};
/*}}}*/

/* POINTER_EVENTS: {{{*/
static void
pointer_enter(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface,
	      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	// get correct bar
	seat->bar = NULL;
	Bar *bar;
	wl_list_for_each(bar, &bar_list, link) {
		if (bar->wl_surface == surface) {
			seat->bar = bar;
			break;
		}
	}

	if (!cursor_image) {
		const char *size_str = getenv("XCURSOR_SIZE");
		int size = size_str ? atoi(size_str) : 0;
		if (size == 0)
			size = 24;
		struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * buffer_scale, shm);
		cursor_image = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr")->images[0];
		cursor_surface = wl_compositor_create_surface(compositor);
		wl_surface_set_buffer_scale(cursor_surface, buffer_scale);
		wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0, 0);
		wl_surface_commit(cursor_surface);
	}
	wl_pointer_set_cursor(pointer, serial, cursor_surface,
			      cursor_image->hotspot_x,
			      cursor_image->hotspot_y);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface)
{
	Seat *seat = (Seat *)data;
	for(int i=0; i<nblocks; i++){
		uint32_t old = seat->bar->bdat[i].sel = 1;
		seat->bar->bdat[i].sel = 0;
		if(old != seat->bar->bdat[i].sel)
			seat->bar->redraw = 1;
	}
	seat->pointer_x = -1;
	seat->pointer_y = -1;
	seat->bar = NULL;
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
	       uint32_t time, uint32_t button, uint32_t state)
{
	Seat *seat = (Seat *)data;

	seat->pointer_button = state == WL_POINTER_BUTTON_STATE_PRESSED ? button : 0;
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	seat->pointer_x = wl_fixed_to_int(surface_x);
	seat->pointer_y = wl_fixed_to_int(surface_y);
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	Seat *seat = (Seat *)data;

	if(!seat->bar) return;
	for(int i=0; i<nblocks; i++){
		uint32_t old = seat->bar->bdat[i].sel = 1;
		if(seat->pointer_x >= seat->bar->bdat[i].xl && seat->pointer_x < seat->bar->bdat[i].xr){
			seat->bar->bdat[i].sel = 1;
			if(seat->pointer_button != 0){
				seat->bar->bdat[i].clickid = seat->pointer_button;
				if(blocks[i].clickfn != 0)
					blocks[i].clickfn(blocks+i, seat->bar->bdat+i);
				seat->bar->redraw = 1;
			}
			if(seat->scroll_dx != 0){
				seat->bar->bdat[i].scroll = seat->scroll_dx;
				blocks[i].clickfn(blocks+i, seat->bar->bdat+i);
				seat->scroll_dx = 0;
				seat->bar->redraw = 1;
			}
		}
		else{
			seat->bar->bdat[i].sel = 0;
		}
		if(old != seat->bar->bdat[i].sel)
			seat->bar->redraw = 1;
	}

	if (!seat->pointer_button)
		return;

	uint32_t x = 0, i = 0;
	do {
		if (hide_vacant) {
			const bool active = seat->bar->mtags & 1 << i;
			const bool occupied = seat->bar->ctags & 1 << i;
			const bool urgent = seat->bar->urg & 1 << i;
			if (!active && !occupied && !urgent)
				continue;
		}
		x += TEXT_WIDTH(tags[i], seat->bar->width - x, seat->bar->textpadding) / buffer_scale;
	} while (seat->pointer_x >= x && ++i < tags_l);

	if (i < tags_l) {
		/* Clicked on tags */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, 1 << i, 1);
		else if (seat->pointer_button == BTN_MIDDLE)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, ~0, 1);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, seat->bar->mtags ^ (1 << i), 0);
	} else if (seat->pointer_x < (x += TEXT_WIDTH(seat->bar->layout, seat->bar->width - x, seat->bar->textpadding))) {
		/* Clicked on layout */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, seat->bar->last_layout_idx);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, 2);
	}
	seat->pointer_button = 0;
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
	     uint32_t time, uint32_t axis, wl_fixed_t value)
{
	Seat *seat = (Seat *)data;
	if(!seat->bar)
		return;
	seat->scroll_dx = wl_fixed_to_int(value);
	//printf("%d\n", seat->scroll_dx);
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
	//uint32_t btn = discrete < 0 ? WheelUp : WheelDown;
	Seat *seat = (Seat *)data;

	if (!seat->bar)
		return;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer,
		    uint32_t axis_source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis)
{
}

static void
pointer_axis_value120(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.axis = pointer_axis,
	.axis_discrete = pointer_axis_discrete,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_value120 = pointer_axis_value120,
	.button = pointer_button,
	.enter = pointer_enter,
	.frame = pointer_frame,
	.leave = pointer_leave,
	.motion = pointer_motion,
};
/*}}}*/

/* OK; SEAT:{{{*/
static void
seat_capabilities(void *data, struct wl_seat *wl_seat,
		  uint32_t capabilities)
{
	Seat *seat = (Seat *)data;

	uint32_t has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && !seat->wl_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	} else if (!has_pointer && seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};
/*}}}*/

/* MOSTLY OK; add num clients. DWL COMMUNICATION:{{{*/
static void
show_bar(Bar *bar)
{
	bar->wl_surface = wl_compositor_create_surface(compositor);
	if (!bar->wl_surface)
		DIE("Could not create wl_surface");

	bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar->wl_surface, bar->wl_output,
								   ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, PROGRAM);
	if (!bar->layer_surface)
		DIE("Could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

	zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, bar->height / buffer_scale);
	zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
					 (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->height / buffer_scale);
	wl_surface_commit(bar->wl_surface);

	bar->hidden = false;
}

static void
hide_bar(Bar *bar)
{
	zwlr_layer_surface_v1_destroy(bar->layer_surface);
	wl_surface_destroy(bar->wl_surface);

	bar->configured = false;
	bar->hidden = true;
}

static void
dwl_wm_tags(void *data, struct zdwl_ipc_manager_v2 *dwl_wm,
	uint32_t amount)
{
	if (!tags && !(tags = malloc(amount * sizeof(char *))))
		EDIE("malloc");
	uint32_t i = tags_l;
	ARRAY_EXPAND(tags, tags_l, tags_c, MAX(0, (int)amount - (int)tags_l));
	for (; i < amount; i++)
		if (!(tags[i] = strdup(tags_names[MIN(i, LENGTH(tags_names)-1)])))
			EDIE("strdup");
}

static void
dwl_wm_layout(void *data, struct zdwl_ipc_manager_v2 *dwl_wm,
	const char *name)
{
	char **ptr;
	ARRAY_APPEND(layouts, layouts_l, layouts_c, ptr);
	if (!(*ptr = strdup(name)))
		EDIE("strdup");
}

static const struct zdwl_ipc_manager_v2_listener dwl_wm_listener = {
	.tags = dwl_wm_tags,
	.layout = dwl_wm_layout
};

static void
dwl_wm_output_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output)
{
	Bar *bar = (Bar *)data;

	if (bar->hidden)
		show_bar(bar);
	else
		hide_bar(bar);
}

static void
dwl_wm_output_active(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t active)
{
	Bar *bar = (Bar *)data;

	if (active != bar->sel)
		bar->sel = active;
}

static void
dwl_wm_output_tag(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t tag, uint32_t state, uint32_t clients, uint32_t focused)
{
	Bar *bar = (Bar *)data;

	if (state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE)
		bar->mtags |= 1 << tag;
	else
		bar->mtags &= ~(1 << tag);
	if (clients > 0)
		bar->ctags |= 1 << tag;
	else
		bar->ctags &= ~(1 << tag);
	if (state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT)
		bar->urg |= 1 << tag;
	else
		bar->urg &= ~(1 << tag);
}

static void
dwl_wm_output_layout(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t layout)
{
	Bar *bar = (Bar *)data;

	bar->last_layout_idx = bar->layout_idx;
	bar->layout_idx = layout;
}

static void
dwl_wm_output_title(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *title)
{
	Bar *bar = (Bar *)data;

	if (bar->window_title)
		free(bar->window_title);
	if (!(bar->window_title = strdup(title)))
		EDIE("strdup");
}

static void
dwl_wm_output_appid(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *appid)
{
}

static void
dwl_wm_output_layout_symbol(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *layout)
{
	Bar *bar = (Bar *)data;

	if (layouts[bar->layout_idx])
		free(layouts[bar->layout_idx]);
	if (!(layouts[bar->layout_idx] = strdup(layout)))
		EDIE("strdup");
	bar->layout = layouts[bar->layout_idx];
}

static void
dwl_wm_output_frame(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output)
{
	Bar *bar = (Bar *)data;
	bar->redraw = true;
}

static void
dwl_wm_output_fullscreen(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t is_fullscreen)
{
}

static void
dwl_wm_output_floating(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t is_floating)
{
}

static const struct zdwl_ipc_output_v2_listener dwl_wm_output_listener = {
	.toggle_visibility = dwl_wm_output_toggle_visibility,
	.active = dwl_wm_output_active,
	.tag = dwl_wm_output_tag,
	.layout = dwl_wm_output_layout,
	.title = dwl_wm_output_title,
	.appid = dwl_wm_output_appid,
	.layout_symbol = dwl_wm_output_layout_symbol,
	.frame = dwl_wm_output_frame,
	.fullscreen = dwl_wm_output_fullscreen,
	.floating = dwl_wm_output_floating
};
/*}}}*/

/* OK; after removal of customtext fix. SETUP + WL_REGISTRY:{{{*/
static void
setup_bar(Bar *bar)
{
	bar->height = height * buffer_scale;
	bar->textpadding = textpadding;
	bar->bottom = bottom;
	bar->hidden = hidden;
	bar->bdat = calloc(nblocks, sizeof(struct Blockpos));
	memset(bar->bdat, 0, nblocks*sizeof(struct Blockpos));

	bar->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
	if (!bar->xdg_output)
		DIE("Could not create xdg_output");
	zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);

	bar->dwl_wm_output = zdwl_ipc_manager_v2_get_output(dwl_wm, bar->wl_output);
	if (!bar->dwl_wm_output)
		DIE("Could not create dwl_wm_output");
	zdwl_ipc_output_v2_add_listener(bar->dwl_wm_output, &dwl_wm_output_listener, bar);

	if (!bar->hidden)
		show_bar(bar);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
	} else if (!strcmp(interface, zdwl_ipc_manager_v2_interface.name)) {
		dwl_wm = wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface, 2);
		zdwl_ipc_manager_v2_add_listener(dwl_wm, &dwl_wm_listener, NULL);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		Bar *bar = calloc(1, sizeof(Bar));
		if (!bar)
			EDIE("calloc");
		bar->registry_name = name;
		bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		if (run_display)
			setup_bar(bar);
		wl_list_insert(&bar_list, &bar->link);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		Seat *seat = calloc(1, sizeof(Seat));
		if (!seat)
			EDIE("calloc");
		seat->registry_name = name;
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
		wl_list_insert(&seat_list, &seat->link);
	}
}

static void
teardown_bar(Bar *bar)
{
	if (bar->bdat)
		free(bar->bdat);
	if (bar->window_title)
		free(bar->window_title);
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!bar->hidden) {
		zwlr_layer_surface_v1_destroy(bar->layer_surface);
		wl_surface_destroy(bar->wl_surface);
	}
	zdwl_ipc_output_v2_destroy(bar->dwl_wm_output);
	zxdg_output_v1_destroy(bar->xdg_output);
	wl_output_destroy(bar->wl_output);
	free(bar);
}

static void
teardown_seat(Seat *seat)
{
	if (seat->wl_pointer)
		wl_pointer_destroy(seat->wl_pointer);
	wl_seat_destroy(seat->wl_seat);
	free(seat);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	Bar *bar;
	Seat *seat;

	wl_list_for_each(bar, &bar_list, link) {
		if (bar->registry_name == name) {
			wl_list_remove(&bar->link);
			teardown_bar(bar);
			return;
		}
	}
	wl_list_for_each(seat, &seat_list, link) {
		if (seat->registry_name == name) {
			wl_list_remove(&seat->link);
			teardown_seat(seat);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove
};
/*}}}*/

static void
event_loop(void)
{
	Bar *bar;
	int wl_fd = wl_display_get_fd(display);
	struct pollfd wl_p = {wl_fd, POLLIN, POLLIN};
	while (run_display) {
		wl_display_flush(display);
		int npol = poll(&wl_p, 1, 1000);
		if(npol > 0){
			if (wl_display_dispatch(display) == -1)
				break;
		}
		int status_redraw = 0;
		for(int i=0; i<nblocks; i++){
			status_redraw |= blocks[i].updatefn(blocks+i);
		}
		wl_list_for_each(bar, &bar_list, link) {
			if (bar->redraw || status_redraw) {
				if (!bar->hidden)
					draw_frame(bar);
				bar->redraw = false;
			}
		}
	}
}

void
sig_handler(int sig)
{
	if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM)
		run_display = false;
}

int
main(int argc, char **argv)
{
	conn = mpd_connection_new("localhost", 0, 0);
	/* Set up display and protocols */
	display = wl_display_connect(NULL);
	if (!display)
		DIE("Failed to create display");

	wl_list_init(&bar_list);
	wl_list_init(&seat_list);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell || !output_manager || !dwl_wm)
		DIE("Compositor does not support all needed protocols");

	/* dpi setup */
	unsigned int dpi = 96 * buffer_scale;
	char buf[10];
	snprintf(buf, sizeof buf, "dpi=%u", dpi);

	/* Load selected font */
	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);

	if (!(font = fcft_from_name(1, (const char *[]) {fontstr}, buf)))
		DIE("Could not load font");
	textpadding = font->height / 2;
	height = font->height/ buffer_scale + vertical_padding * 2;

	/* Setup bars */
	Bar *bar, *bar2;
	Seat *seat, *seat2;
	wl_list_for_each(bar, &bar_list, link)
		setup_bar(bar);
	wl_display_roundtrip(display);

	/* Set up signals */
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGCHLD, SIG_IGN);

	/* Run */
	run_display = true;
	event_loop();

	/* Clean everything up */
	if (tags) {
		for (uint32_t i = 0; i < tags_l; i++)
			free(tags[i]);
		free(tags);
	}
	if (layouts) {
		for (uint32_t i = 0; i < layouts_l; i++)
			free(layouts[i]);
		free(layouts);
	}

	wl_list_for_each_safe(bar, bar2, &bar_list, link)
		teardown_bar(bar);
	wl_list_for_each_safe(seat, seat2, &seat_list, link)
		teardown_seat(seat);

	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	zdwl_ipc_manager_v2_destroy(dwl_wm);

	fcft_destroy(font);
	fcft_fini();

	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
