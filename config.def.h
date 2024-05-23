#define HEX_COLOR(hex)				\
	{ .red   = ((hex >> 24) & 0xff) * 257,	\
	  .green = ((hex >> 16) & 0xff) * 257,	\
	  .blue  = ((hex >> 8) & 0xff) * 257,	\
	  .alpha = (hex & 0xff) * 257 }

// initially hide all bars
static bool hidden = false;
// initially draw all bars at the bottom
static bool bottom = false;
// hide vacant tags
static bool hide_vacant = false;
// vertical pixel padding above and below text
static uint32_t vertical_padding = 1;
// scale
static uint32_t buffer_scale = 1;
// font
static char *fontstr = "UbuntuMono nerd font propo:size=16";
// tag names
static char *tags_names[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", " "};

// set 16-bit colors for bar
// use either pixman_color_t struct or HEX_COLOR macro for 8-bit colors
static pixman_color_t active_fg_color = HEX_COLOR(0xffffffff);
static pixman_color_t active_bg_color = HEX_COLOR(0x5F1C58ff);
static pixman_color_t occupied_fg_color = HEX_COLOR(0xeeeeeeff);
static pixman_color_t occupied_bg_color = HEX_COLOR(0x2B1325ff);
static pixman_color_t inactive_fg_color = HEX_COLOR(0x555555ff);
static pixman_color_t inactive_bg_color = HEX_COLOR(0x2B1325ff);
static pixman_color_t urgent_fg_color = HEX_COLOR(0x222222ff);
static pixman_color_t urgent_bg_color = HEX_COLOR(0xeeeeeeff);

static int nblocks = 7;
Block blocks[] = {
	{.updatefn = bartime, .clickfn = 0, .gravity=CENTER, .fg    = HEX_COLOR(0xffffffff), .bg    = HEX_COLOR(0x2B1325ff), .acc    = HEX_COLOR(0x4B2142ff),\
	                                                     .selfg = HEX_COLOR(0x222222ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)
	},
	{.updatefn = songinfo, .clickfn = clickhide, .gravity=RIGHT, .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0xD062C5ff), .acc = HEX_COLOR(0x8D2A83ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
	{.updatefn = backlight, .clickfn = scroll, .gravity=RIGHT,   .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0x2B1325ff), .acc = HEX_COLOR(0x6E2166ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
	{.updatefn = battery, .clickfn = cycle, .gravity=RIGHT , .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0x2B1325ff), .acc = HEX_COLOR(0x5F1C58ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
	{.updatefn = uptime, .clickfn = clickhide, .gravity=RIGHT,   .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0x2B1325ff), .acc = HEX_COLOR(0x4B2142ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
	{.updatefn = wireless, .clickfn = clickhide, .gravity=RIGHT,   .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0x2B1325ff), .acc = HEX_COLOR(0x3B1A34ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
	{.updatefn = file, .clickfn = clickhide, .gravity=RIGHT,   .fg = HEX_COLOR(0xffffffff), .bg = HEX_COLOR(0x2B1325ff), .acc = HEX_COLOR(0x3B1A34ff),\
	                                                     .selfg = HEX_COLOR(0x1D0C18ff), .selbg = HEX_COLOR(0xffffffff), .selacc = HEX_COLOR(0xD062C5ff)},
};
