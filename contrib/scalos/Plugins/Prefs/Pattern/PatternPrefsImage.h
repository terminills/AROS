static const ULONG PatternPrefs_colors[48] =
{
	0xdddddddd,0x66666666,0x33333333,
	0x99999999,0x99999999,0x99999999,
	0x11111111,0x55555555,0x33333333,
	0x22222222,0x44444444,0x44444444,
	0x55555555,0x55555555,0x55555555,
	0x33333333,0x44444444,0x99999999,
	0x33333333,0x99999999,0x44444444,
	0x11111111,0xcccccccc,0x22222222,
	0x44444444,0xaaaaaaaa,0x77777777,
	0x22222222,0x88888888,0x88888888,
	0xaaaaaaaa,0xaaaaaaaa,0xaaaaaaaa,
	0x66666666,0x99999999,0xcccccccc,
	0x33333333,0x33333333,0x44444444,
	0xcccccccc,0xaaaaaaaa,0x44444444,
	0xaaaaaaaa,0xaaaaaaaa,0xaaaaaaaa,
	0xeeeeeeee,0xeeeeeeee,0xeeeeeeee,
};

#define PATTERNPREFS_WIDTH        22
#define PATTERNPREFS_HEIGHT       19
#define PATTERNPREFS_DEPTH         4
#define PATTERNPREFS_COMPRESSION   0
#define PATTERNPREFS_MASKING       2

#ifdef USE_PATTERNPREFS_HEADER
static const struct BitMapHeader PatternPrefs_header =
{ 22,19,0,0,4,2,0,0,0,44,44,320,256 };
#endif

static const UBYTE PatternPrefs_body[304] = {
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x05,0x2c,0x80,0x00,0x05,0x76,0x80,0x00,0x0a,0xf7,0xc0,0x00,0x07,
0xf6,0x80,0x00,0x0f,0xbc,0x80,0x00,0x01,0x3d,0x40,0x00,0x1e,0xff,0xe0,0x00,
0x01,0x7d,0xc0,0x00,0x1f,0xc3,0x00,0x00,0x04,0x1f,0x00,0x00,0x39,0x7f,0xe0,
0x00,0x06,0xbf,0xc0,0x00,0x3f,0xe9,0x00,0x00,0x11,0x0f,0x00,0x00,0x2e,0xff,
0xc0,0x00,0x11,0x1f,0x80,0x00,0x1f,0xe0,0x00,0x00,0x17,0xc6,0x00,0x00,0x28,
0x3f,0xe0,0x00,0x37,0xdf,0xc0,0x00,0x0f,0xc8,0x60,0x00,0x0b,0x00,0x00,0x00,
0x34,0xff,0xf0,0x00,0x1b,0x3f,0xe0,0x00,0x07,0x84,0xf0,0x00,0x02,0xb0,0x00,
0x00,0x1d,0x4f,0xf8,0x00,0x0a,0xcf,0xf0,0x00,0x03,0x03,0xf8,0x00,0x00,0x68,
0x00,0x00,0x0d,0x97,0xf8,0x00,0x06,0x97,0xf8,0x00,0x00,0x01,0xf8,0x00,0x00,
0xbe,0x00,0x00,0x07,0x43,0xf8,0x00,0x03,0x41,0xf8,0x00,0x00,0x00,0xf0,0x00,
0x01,0xb3,0x00,0x00,0x02,0x4c,0xf0,0x00,0x02,0x4c,0xf0,0x00,0x00,0x00,0x60,
0x00,0x03,0x26,0x00,0x00,0x00,0xd9,0xe0,0x00,0x00,0xd8,0x60,0x00,0x00,0x00,
0x00,0x00,0x01,0x68,0x00,0x00,0x07,0x97,0x00,0x00,0x02,0x94,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0xd0,0x00,0x00,0x03,0x2e,0x00,0x00,0x01,0x28,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x01,0xfc,0x00,0x00,0x00,0xb8,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf8,0x00,0x00,0x00,0x70,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00, };
