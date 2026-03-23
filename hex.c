#include <stdint.h>
#include <string.h>

static uint32_t
rgba32(float *rgba)
{
	uint32_t r[4] = { 0 };
	for (int i = 0; i < 4; i++) {
		r[i] = rgba[i] * 255;
	}
	return ((r[0] & 0xff) << 24) | ((r[1] & 0xff) << 16) |
		((r[2] & 0xff) << 8) | (r[3] & 0xff);
}

static int
hex_to_dec(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return 0;
}

static void
parse_hexstr(const char *hex, float *rgba)
{
	// TODO: this defaults to 00000000, so not great
	if (hex[0] != '#') {
		return;
	}

	size_t len = strlen(hex);
	if (len == 4) {
		/* #fff is shorthand for #f0f0f0, per theme spec */
		rgba[0] = (hex_to_dec(hex[1]) * 16) / 255.0;
		rgba[1] = (hex_to_dec(hex[2]) * 16) / 255.0;
		rgba[2] = (hex_to_dec(hex[3]) * 16) / 255.0;
	} else if (len >= 7) {
		rgba[0] = (hex_to_dec(hex[1]) * 16 + hex_to_dec(hex[2])) / 255.0;
		rgba[1] = (hex_to_dec(hex[3]) * 16 + hex_to_dec(hex[4])) / 255.0;
		rgba[2] = (hex_to_dec(hex[5]) * 16 + hex_to_dec(hex[6])) / 255.0;
	} else {
		return;
	}

	rgba[3] = 1.0;

	if (len == 9) {
		/* Inline alpha encoding like #aabbccff */
		rgba[3] = (hex_to_dec(hex[7]) * 16 + hex_to_dec(hex[8])) / 255.0;
	}
}

uint32_t
parse_hex(const char *hex)
{
	float color[4] = { 0 };
	parse_hexstr(hex, color);
	return rgba32(color);
}
