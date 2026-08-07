/*
 * Finding a QR code in a camera frame.
 *
 * The steps are the classic ones: threshold the image, look for the 1:1:3:1:1
 * run pattern that only the three finder squares produce, work out where the
 * fourth corner must be, and sample the grid through a perspective transform.
 *
 * The camera on this machine delivers 320x240 at around 3 frames a second, so
 * the whole pass has to stay cheap: the threshold is computed per block over
 * a downsampled grid rather than per pixel, and the run scan looks at every
 * other row.
 *
 * Distributed under the terms of the MIT License.
 */

#include "QRDecode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


bool qr_verbose = false;
int32 qr_last_centres = 0;

static const int32 kBlockSize = 16;
static const int32 kMaxCentres = 32;


struct finder {
	float	x;
	float	y;
	float	moduleSize;
	int32	count;
	// Whether the 1:1:3:1:1 run also held along the diagonal.
	bool	diagonal;
};


// #pragma mark - binarization


// A single threshold fails as soon as one side of the frame is brighter than
// the other, which is the normal case for something held up to a webcam. The
// image is divided into blocks and each block gets the average of its own
// neighbourhood; flat blocks fall back to their neighbours so that a block of
// pure white paper is not split down the middle by noise.
static void
binarize(const uint8* frame, int32 width, int32 height, int32 bytesPerRow,
	uint8* binary, uint8* gray)
{
	int32 blocksX = (width + kBlockSize - 1) / kBlockSize;
	int32 blocksY = (height + kBlockSize - 1) / kBlockSize;

	for (int32 y = 0; y < height; y++) {
		const uint8* row = frame + y * bytesPerRow;
		for (int32 x = 0; x < width; x++) {
			// B_RGB32 is BGRA in memory. The green channel alone tracks
			// luminance closely enough here and costs two operations less
			// per pixel than a weighted sum.
			gray[y * width + x] = row[x * 4 + 1];
		}
	}

	int32* averages = (int32*)malloc(blocksX * blocksY * sizeof(int32));
	if (averages == NULL)
		return;

	for (int32 by = 0; by < blocksY; by++) {
		for (int32 bx = 0; bx < blocksX; bx++) {
			int32 sum = 0;
			int32 count = 0;
			int32 minimum = 255;
			int32 maximum = 0;

			for (int32 y = by * kBlockSize;
					y < (by + 1) * kBlockSize && y < height; y++) {
				for (int32 x = bx * kBlockSize;
						x < (bx + 1) * kBlockSize && x < width; x++) {
					int32 value = gray[y * width + x];
					sum += value;
					count++;
					if (value < minimum)
						minimum = value;
					if (value > maximum)
						maximum = value;
				}
			}

			int32 average = count > 0 ? sum / count : 128;

			// Too little contrast to contain an edge: this block is all paper
			// or all ink, so bias it away from its own average.
			if (maximum - minimum <= 24)
				average = minimum > 128 ? minimum - 1 : maximum + 1;

			averages[by * blocksX + bx] = average;
		}
	}

	for (int32 by = 0; by < blocksY; by++) {
		for (int32 bx = 0; bx < blocksX; bx++) {
			// Smooth over the 3x3 neighbourhood so a block boundary does not
			// become a visible step in the thresholded image.
			int32 sum = 0;
			int32 count = 0;
			for (int32 dy = -1; dy <= 1; dy++) {
				for (int32 dx = -1; dx <= 1; dx++) {
					int32 nx = bx + dx;
					int32 ny = by + dy;
					if (nx < 0 || ny < 0 || nx >= blocksX || ny >= blocksY)
						continue;
					sum += averages[ny * blocksX + nx];
					count++;
				}
			}
			int32 threshold = count > 0 ? sum / count : 128;

			for (int32 y = by * kBlockSize;
					y < (by + 1) * kBlockSize && y < height; y++) {
				for (int32 x = bx * kBlockSize;
						x < (bx + 1) * kBlockSize && x < width; x++) {
					binary[y * width + x]
						= gray[y * width + x] < threshold ? 1 : 0;
				}
			}
		}
	}

	free(averages);
}


// One sample. Mode 0 reads a binary map, where any non-zero byte is dark;
// mode 1 reads the grey image and compares against a threshold.
static inline bool
sample_at(const uint8* source, int32 width, int32 height, float x, float y,
	int32 mode, int32 limit)
{
	int32 ix = (int32)(x + 0.5f);
	int32 iy = (int32)(y + 0.5f);
	if (ix < 0 || iy < 0 || ix >= width || iy >= height)
		return false;

	if (mode == 0)
		return source[iy * width + ix] != 0;

	// Bilinear, for the grey path only. The grid lands between pixels far
	// more often than on one, and at three pixels per module rounding to the
	// nearest pixel moves the sample a third of a module -- enough to read a
	// neighbour instead. The binary path cannot be interpolated meaningfully,
	// so it keeps the nearest sample.
	float fx = x < 0 ? 0 : x;
	float fy = y < 0 ? 0 : y;
	int32 x0 = (int32)fx;
	int32 y0 = (int32)fy;
	int32 x1 = x0 + 1 < width ? x0 + 1 : x0;
	int32 y1 = y0 + 1 < height ? y0 + 1 : y0;
	float dx = fx - x0;
	float dy = fy - y0;

	float top = source[y0 * width + x0] * (1 - dx)
		+ source[y0 * width + x1] * dx;
	float bottom = source[y1 * width + x0] * (1 - dx)
		+ source[y1 * width + x1] * dx;

	return (top * (1 - dy) + bottom * dy) < limit;
}


static inline int32
gray_at(const uint8* gray, int32 width, int32 height, float x, float y)
{
	int32 ix = (int32)(x + 0.5f);
	int32 iy = (int32)(y + 0.5f);
	if (ix < 0)
		ix = 0;
	if (iy < 0)
		iy = 0;
	if (ix >= width)
		ix = width - 1;
	if (iy >= height)
		iy = height - 1;
	return gray[iy * width + ix];
}


// #pragma mark - finder patterns


static inline bool
dark(const uint8* binary, int32 width, int32 height, int32 x, int32 y)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
		return false;
	return binary[y * width + x] != 0;
}


// The finder square is five bands in the ratio 1:1:3:1:1. Allowing each band
// half a module of slack is what makes this survive a hand-held camera.
static bool
ratio_ok(const int32* runs, float* moduleSize)
{
	int32 total = 0;
	for (int32 i = 0; i < 5; i++) {
		if (runs[i] == 0)
			return false;
		total += runs[i];
	}

	if (total < 7)
		return false;

	float module = total / 7.0f;

	// Two thirds of a module rather than a half. A blurred edge moves the
	// boundary between two bands by up to half a module in either direction,
	// and at three pixels per module -- which is what a code held up to this
	// camera actually measures -- half a module is a single pixel of slack.
	float slack = module * 0.67f;

	if (fabs(module - runs[0]) >= slack)
		return false;
	if (fabs(module - runs[1]) >= slack)
		return false;
	if (fabs(3 * module - runs[2]) >= 3 * slack)
		return false;
	if (fabs(module - runs[3]) >= slack)
		return false;
	if (fabs(module - runs[4]) >= slack)
		return false;

	*moduleSize = module;
	return true;
}


// Confirms a horizontal candidate by looking for the same ratio vertically
// through its centre. Without this, any striped texture produces candidates.
static bool
check_vertical(const uint8* binary, int32 width, int32 height, int32 centreX,
	int32 centreY, float expected, float* centre)
{
	int32 runs[5];
	memset(runs, 0, sizeof(runs));

	int32 y = centreY;
	while (y >= 0 && dark(binary, width, height, centreX, y)) {
		runs[2]++;
		y--;
	}
	while (y >= 0 && !dark(binary, width, height, centreX, y)
		&& runs[1] < expected * 2) {
		runs[1]++;
		y--;
	}
	while (y >= 0 && dark(binary, width, height, centreX, y)
		&& runs[0] < expected * 2) {
		runs[0]++;
		y--;
	}

	y = centreY + 1;
	while (y < height && dark(binary, width, height, centreX, y)) {
		runs[2]++;
		y++;
	}
	while (y < height && !dark(binary, width, height, centreX, y)
		&& runs[3] < expected * 2) {
		runs[3]++;
		y++;
	}
	while (y < height && dark(binary, width, height, centreX, y)
		&& runs[4] < expected * 2) {
		runs[4]++;
		y++;
	}

	float module = 0;
	if (!ratio_ok(runs, &module))
		return false;

	// Half a pixel back. After the outward scan the index sits one past the
	// last dark pixel, so the expression below lands on the pixel *after* the
	// middle of the centre run, not on its middle: for a run spanning
	// L..L+n-1 the middle is L + (n-1)/2, and this gives L + n/2.
	//
	// Half a pixel at three pixels per module is a sixth of a module, which
	// on its own is nothing. But it biases all three finder centres the same
	// way, and the perspective grid is anchored on them, so the error does
	// not stay put -- it grows across the code. The sampled grid came out
	// near-perfect for the first rows and 12% wrong by the bottom.
	*centre = y - runs[4] - runs[3] - runs[2] / 2.0f - 0.5f;
	return true;
}


// A third pass through the candidate, along the diagonal. The horizontal and
// vertical checks together still accept plenty of things that are not finder
// squares -- a striped shirt, a keyboard, a window blind -- and each false
// candidate multiplies into triples that then cost a full sampling and decode
// pass each. On this CPU a scan spent most of its time that way. A real
// finder square is 1:1:3:1:1 along its diagonal too; almost nothing else is.
static bool
check_diagonal(const uint8* binary, int32 width, int32 height, int32 centreX,
	int32 centreY, float expected)
{
	int32 runs[5];
	memset(runs, 0, sizeof(runs));

	int32 limit = (int32)(expected * 4) + 4;

	int32 x = centreX;
	int32 y = centreY;
	while (x >= 0 && y >= 0 && dark(binary, width, height, x, y)
		&& runs[2] < limit) {
		runs[2]++;
		x--;
		y--;
	}
	while (x >= 0 && y >= 0 && !dark(binary, width, height, x, y)
		&& runs[1] < limit) {
		runs[1]++;
		x--;
		y--;
	}
	while (x >= 0 && y >= 0 && dark(binary, width, height, x, y)
		&& runs[0] < limit) {
		runs[0]++;
		x--;
		y--;
	}

	x = centreX + 1;
	y = centreY + 1;
	while (x < width && y < height && dark(binary, width, height, x, y)
		&& runs[2] < limit * 2) {
		runs[2]++;
		x++;
		y++;
	}
	while (x < width && y < height && !dark(binary, width, height, x, y)
		&& runs[3] < limit) {
		runs[3]++;
		x++;
		y++;
	}
	while (x < width && y < height && dark(binary, width, height, x, y)
		&& runs[4] < limit) {
		runs[4]++;
		x++;
		y++;
	}

	float module = 0;
	return ratio_ok(runs, &module);
}


// The vertical check, retried across a couple of pixels either side.
//
// The horizontal run gives a centre that can be a pixel or two out -- the
// scan line crosses the square at whatever height it happens to be at, and
// on a code seen in perspective that is not the square's middle. A vertical
// scan launched from a column near the edge of the centre block reads a short
// run and the candidate is thrown away, which is how a perfectly visible
// finder went missing: its horizontal run was found on five consecutive rows
// and rejected on every one.
static bool
check_vertical_near(const uint8* binary, int32 width, int32 height,
	float centreX, int32 y, float module, float* centreY)
{
	// A module and a half either side. The point is not to correct a rounding
	// error but to step off a column where the thin light ring has been
	// blurred into the dark one -- at three pixels per module that ring is
	// three pixels wide, and it survives in some columns and not others.
	int32 reach = (int32)(module * 1.5f);
	if (reach < 2)
		reach = 2;
	if (reach > 6)
		reach = 6;

	for (int32 offset = 0; offset <= reach; offset++) {
		if (check_vertical(binary, width, height,
				(int32)(centreX + 0.5f) + offset, y, module, centreY)) {
			return true;
		}
		if (offset > 0 && check_vertical(binary, width, height,
				(int32)(centreX + 0.5f) - offset, y, module, centreY)) {
			return true;
		}
	}

	return false;
}


static void
add_centre(finder* centres, int32* count, float x, float y, float moduleSize,
	bool diagonal)
{
	for (int32 i = 0; i < *count; i++) {
		// Same square seen on a neighbouring scan line: average it in rather
		// than adding a duplicate, which also sharpens the estimate.
		if (fabs(centres[i].x - x) < centres[i].moduleSize
			&& fabs(centres[i].y - y) < centres[i].moduleSize) {
			float n = centres[i].count;
			centres[i].x = (centres[i].x * n + x) / (n + 1);
			centres[i].y = (centres[i].y * n + y) / (n + 1);
			centres[i].moduleSize
				= (centres[i].moduleSize * n + moduleSize) / (n + 1);
			centres[i].count++;
			if (diagonal)
				centres[i].diagonal = true;
			return;
		}
	}

	if (*count >= kMaxCentres)
		return;

	centres[*count].x = x;
	centres[*count].y = y;
	centres[*count].moduleSize = moduleSize;
	centres[*count].count = 1;
	centres[*count].diagonal = diagonal;
	(*count)++;
}


// Measures the 1:1:3:1:1 run across a row through (x, y) and returns the
// centre of it. Same idea as check_vertical, one axis over.
static bool
measure_horizontal(const uint8* binary, int32 width, int32 height, int32 x,
	int32 y, float expected, float* centre, float* module)
{
	int32 runs[5];
	memset(runs, 0, sizeof(runs));
	int32 limit = (int32)(expected * 4) + 4;

	int32 i = x;
	while (i >= 0 && dark(binary, width, height, i, y) && runs[2] < limit * 2) {
		runs[2]++;
		i--;
	}
	while (i >= 0 && !dark(binary, width, height, i, y) && runs[1] < limit) {
		runs[1]++;
		i--;
	}
	while (i >= 0 && dark(binary, width, height, i, y) && runs[0] < limit) {
		runs[0]++;
		i--;
	}

	i = x + 1;
	while (i < width && dark(binary, width, height, i, y)
		&& runs[2] < limit * 2) {
		runs[2]++;
		i++;
	}
	while (i < width && !dark(binary, width, height, i, y) && runs[3] < limit) {
		runs[3]++;
		i++;
	}
	while (i < width && dark(binary, width, height, i, y) && runs[4] < limit) {
		runs[4]++;
		i++;
	}

	if (!ratio_ok(runs, module))
		return false;

	*centre = i - runs[4] - runs[3] - runs[2] / 2.0f - 0.5f;
	return true;
}


// Walks the estimate back onto the middle of the square.
//
// The centre that comes out of the scan is the midpoint of whatever run the
// scan line happened to cross, and a line that crosses the square off-centre
// gives a midpoint that is off-centre too -- by more than a module on a
// photographed screen, where the dark modules bleed outwards. That is enough
// to skew the perspective grid past the point where the code can be read,
// while still looking like a clean detection. Re-measuring through the
// current estimate and taking the new midpoint converges in two or three
// passes.
// Centre of mass of the finder's solid middle square.
//
// A run-length midpoint is only as good as the line it was measured on: a
// scan that crosses the square above or below its middle measures a shorter
// chord and reports a midpoint that is off along the other axis too. Three
// finder centres each off by half a module is enough to bend the perspective
// grid past what the error correction can absorb. The middle square is 3x3
// modules of solid dark with light on every side, so its centre of mass does
// not depend on where the scan happened to cross it.
static bool
centroid_refine(const uint8* binary, int32 width, int32 height, finder* f)
{
	int32 cx = (int32)(f->x + 0.5f);
	int32 cy = (int32)(f->y + 0.5f);
	if (!dark(binary, width, height, cx, cy))
		return false;

	// Reach: the middle square is three modules across, so two modules from
	// the centre is inside it and four is past the light ring around it.
	int32 reach = (int32)(f->moduleSize * 2.5f) + 2;

	int32 left = cx;
	while (left > cx - reach && dark(binary, width, height, left - 1, cy))
		left--;
	int32 right = cx;
	while (right < cx + reach && dark(binary, width, height, right + 1, cy))
		right++;
	int32 top = cy;
	while (top > cy - reach && dark(binary, width, height, cx, top - 1))
		top--;
	int32 bottom = cy;
	while (bottom < cy + reach && dark(binary, width, height, cx, bottom + 1))
		bottom++;

	// A blob that filled the whole reach is not the middle square -- the scan
	// has wandered into the surrounding ring or into ordinary dark data.
	if (right - left + 1 >= reach * 2 || bottom - top + 1 >= reach * 2)
		return false;

	double sumX = 0;
	double sumY = 0;
	int32 count = 0;
	for (int32 y = top; y <= bottom; y++) {
		for (int32 x = left; x <= right; x++) {
			if (!dark(binary, width, height, x, y))
				continue;
			sumX += x;
			sumY += y;
			count++;
		}
	}

	if (count < 4)
		return false;

	f->x = (float)(sumX / count);
	f->y = (float)(sumY / count);
	return true;
}


static void
refine_centre(const uint8* binary, int32 width, int32 height, finder* f)
{
	for (int32 pass = 0; pass < 3; pass++) {
		float x = f->x;
		float y = f->y;
		float moduleH = f->moduleSize;
		float moduleV = f->moduleSize;

		float newX = x;
		float newY = y;

		if (!measure_horizontal(binary, width, height, (int32)(x + 0.5f),
				(int32)(y + 0.5f), f->moduleSize, &newX, &moduleH)) {
			return;
		}
		if (!check_vertical(binary, width, height, (int32)(newX + 0.5f),
				(int32)(y + 0.5f), f->moduleSize, &newY)) {
			return;
		}

		float shift = fabs(newX - x) + fabs(newY - y);

		f->x = newX;
		f->y = newY;
		f->moduleSize = (moduleH + moduleV) / 2;

		// Settled: another pass would only move it by rounding.
		if (shift < 0.2f)
			break;
	}

	centroid_refine(binary, width, height, f);
}


static int32
find_centres(const uint8* binary, int32 width, int32 height, finder* centres)
{
	int32 count = 0;

	// Every row, not every other one. Skipping rows halves the cost but also
	// halves the chance of catching a small or slightly rotated finder, and
	// the camera only produces a few frames a second anyway.
	for (int32 y = 0; y < height; y++) {
		int32 runs[5];
		memset(runs, 0, sizeof(runs));
		int32 index = 0;

		for (int32 x = 0; x < width; x++) {
			bool isDark = dark(binary, width, height, x, y);

			if (isDark == ((index & 1) == 0)) {
				runs[index]++;
				continue;
			}

			if (index < 4) {
				index++;
				runs[index] = 1;
				continue;
			}

			float module = 0;
			if (ratio_ok(runs, &module)) {
				float centreX = x - runs[4] - runs[3] - runs[2] / 2.0f - 0.5f;
				float centreY = 0;
				bool vertical = check_vertical_near(binary, width, height,
					centreX, y, module, &centreY);
				if (vertical) {
					add_centre(centres, &count, centreX, centreY, module,
						check_diagonal(binary, width, height, (int32)centreX,
							(int32)centreY, module));
				}
			}

			// Shift the window: the last two runs become the first two of a
			// possible next pattern, so overlapping squares are still seen.
			runs[0] = runs[2];
			runs[1] = runs[3];
			runs[2] = runs[4];
			runs[3] = 1;
			runs[4] = 0;
			index = 3;
		}

		float module = 0;
		if (index == 4 && ratio_ok(runs, &module)) {
			float centreX = width - runs[4] - runs[3] - runs[2] / 2.0f - 0.5f;
			float centreY = 0;
			if (check_vertical_near(binary, width, height, centreX, y, module,
					&centreY)) {
				add_centre(centres, &count, centreX, centreY, module,
					check_diagonal(binary, width, height, (int32)centreX,
						(int32)centreY, module));
			}
		}
	}

	return count;
}


// #pragma mark - perspective


struct transform {
	float a11, a12, a13;
	float a21, a22, a23;
	float a31, a32, a33;
};


static void
square_to_quad(float x0, float y0, float x1, float y1, float x2, float y2,
	float x3, float y3, transform* out)
{
	float dx3 = x0 - x1 + x2 - x3;
	float dy3 = y0 - y1 + y2 - y3;

	if (fabs(dx3) < 1e-6 && fabs(dy3) < 1e-6) {
		out->a11 = x1 - x0; out->a12 = y1 - y0; out->a13 = 0;
		out->a21 = x2 - x1; out->a22 = y2 - y1; out->a23 = 0;
		out->a31 = x0;      out->a32 = y0;      out->a33 = 1;
		return;
	}

	float dx1 = x1 - x2;
	float dy1 = y1 - y2;
	float dx2 = x3 - x2;
	float dy2 = y3 - y2;
	float denominator = dx1 * dy2 - dx2 * dy1;
	if (fabs(denominator) < 1e-6)
		denominator = 1e-6;

	float a13 = (dx3 * dy2 - dx2 * dy3) / denominator;
	float a23 = (dx1 * dy3 - dx3 * dy1) / denominator;

	out->a11 = x1 - x0 + a13 * x1;
	out->a12 = y1 - y0 + a13 * y1;
	out->a13 = a13;
	out->a21 = x3 - x0 + a23 * x3;
	out->a22 = y3 - y0 + a23 * y3;
	out->a23 = a23;
	out->a31 = x0;
	out->a32 = y0;
	out->a33 = 1;
}


static void
map_point(const transform& t, float x, float y, float* outX, float* outY)
{
	float denominator = t.a13 * x + t.a23 * y + t.a33;
	if (fabs(denominator) < 1e-6)
		denominator = 1e-6;
	*outX = (t.a11 * x + t.a21 * y + t.a31) / denominator;
	*outY = (t.a12 * x + t.a22 * y + t.a32) / denominator;
}


// #pragma mark - assembly


static float
distance(const finder& a, const finder& b)
{
	float dx = a.x - b.x;
	float dy = a.y - b.y;
	return sqrt(dx * dx + dy * dy);
}


// Of the three squares, the one opposite the longest side is the corner: the
// other two sit along the top and left edges of the code.
static void
order_centres(finder* centres, finder* corner, finder* right, finder* bottom)
{
	float d01 = distance(centres[0], centres[1]);
	float d12 = distance(centres[1], centres[2]);
	float d02 = distance(centres[0], centres[2]);

	finder a, b, c;
	if (d12 >= d01 && d12 >= d02) {
		a = centres[0]; b = centres[1]; c = centres[2];
	} else if (d02 >= d01 && d02 >= d12) {
		a = centres[1]; b = centres[0]; c = centres[2];
	} else {
		a = centres[2]; b = centres[0]; c = centres[1];
	}

	// Cross product decides which of the two remaining squares is to the
	// right of the corner and which is below it.
	float cross = (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
	*corner = a;
	if (cross < 0) {
		*right = b;
		*bottom = c;
	} else {
		*right = c;
		*bottom = b;
	}
}


// The bottom-right corner has no finder square. From version 2 on there is an
// alignment pattern near where it should be; finding it is what keeps a
// tilted code readable.
//
// Two things about this that were wrong before and are easy to get wrong:
//
// The pattern's centre module is *dark*. Reading across the middle of the 5x5
// gives dark-light-dark-light-dark, so the run that sits on the centre is the
// dark one in a light-dark-light window -- searching for dark-light-dark and
// taking the middle of the light run centres on the ring instead, a whole
// module out.
//
// And the search box lies inside the data area, so it is full of light-dark-
// light runs that are not the alignment pattern. Taking the first in raster
// order picked one of those more often than not; the candidate nearest the
// predicted position is the one to keep.
static bool
find_alignment(const uint8* binary, int32 width, int32 height, float guessX,
	float guessY, float moduleSize, float* outX, float* outY)
{
	int32 radius = (int32)(moduleSize * 4);
	if (radius < 4)
		radius = 4;

	int32 left = (int32)(guessX - radius);
	int32 top = (int32)(guessY - radius);
	int32 right = (int32)(guessX + radius);
	int32 bottom = (int32)(guessY + radius);

	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right >= width) right = width - 1;
	if (bottom >= height) bottom = height - 1;

	float bestX = 0;
	float bestY = 0;
	float bestDistance = 0;
	bool found = false;

	for (int32 y = top; y <= bottom; y++) {
		int32 runs[3];
		memset(runs, 0, sizeof(runs));
		// Index 0 counts light, 1 dark, 2 light.
		int32 index = 0;

		for (int32 x = left; x <= right; x++) {
			bool isDark = dark(binary, width, height, x, y);

			if (isDark == ((index & 1) != 0)) {
				runs[index]++;
				continue;
			}

			if (index < 2) {
				index++;
				runs[index] = 1;
				continue;
			}

			float module = (runs[0] + runs[1] + runs[2]) / 3.0f;
			if (runs[1] > 0 && module > moduleSize * 0.5f
				&& module < moduleSize * 2.0f
				&& fabs(runs[0] - module) < module
				&& fabs(runs[1] - module) < module
				&& fabs(runs[2] - module) < module) {
				// Centre of the dark middle run.
				float centreX = x - runs[2] - runs[1] / 2.0f - 0.5f;

				// The same run vertically: one dark row is ordinary data, a
				// dark module bounded above and below is the centre.
				int32 cx = (int32)(centreX + 0.5f);
				int32 limit = (int32)(moduleSize * 2) + 2;
				int32 up = 0;
				while (up < limit && dark(binary, width, height, cx, y - up - 1))
					up++;
				int32 down = 0;
				while (down < limit
					&& dark(binary, width, height, cx, y + down + 1)) {
					down++;
				}

				float vertical = (float)(up + down + 1);
				if (vertical > moduleSize * 0.5f
					&& vertical < moduleSize * 2.0f) {
					float centreY = y + (down - up) / 2.0f;
					float dx = centreX - guessX;
					float dy = centreY - guessY;
					float distance = dx * dx + dy * dy;
					if (!found || distance < bestDistance) {
						bestDistance = distance;
						bestX = centreX;
						bestY = centreY;
						found = true;
					}
				}
			}

			runs[0] = runs[2];
			runs[1] = 1;
			runs[2] = 0;
			index = 1;
		}
	}

	if (!found)
		return false;

	*outX = bestX;
	*outY = bestY;
	return true;
}


bool
qr_scan(const uint8* frame, int32 width, int32 height, int32 bytesPerRow,
	BString* result)
{
	if (frame == NULL || width < 32 || height < 32)
		return false;

	uint8* binary = (uint8*)malloc(width * height);
	uint8* gray = (uint8*)malloc(width * height);
	if (binary == NULL || gray == NULL) {
		free(binary);
		free(gray);
		return false;
	}

	for (int32 y = 0; y < height; y++) {
		const uint8* row = frame + y * bytesPerRow;
		for (int32 x = 0; x < width; x++) {
			// B_RGB32 is BGRA in memory. The green channel alone tracks
			// luminance closely enough here and costs two operations less
			// per pixel than a weighted sum.
			gray[y * width + x] = row[x * 4 + 1];
		}
	}

	binarize(frame, width, height, bytesPerRow, binary, gray);

	finder centres[kMaxCentres];
	int32 count = find_centres(binary, width, height, centres);

	// Squares seen on only one scan line are noise more often than not.
	// Keep every plausible candidate and order them by confidence rather
	// than filtering down to the confident ones.
	//
	// Stopping at the first three that pass the strictest test looked like a
	// sensible way to cut work, and it was wrong: three *spurious* squares
	// can pass the diagonal check while the real third finder -- small,
	// blurred, seen at an angle -- does not, and the code is then unreadable
	// even though every part of it was detected. Ordering costs nothing and
	// the triple filters below reject the bad combinations cheaply.
	int32 kept = 0;
	for (int32 i = 0; i < count; i++) {
		if (centres[i].diagonal && centres[i].count >= 3)
			centres[kept++] = centres[i];
	}
	for (int32 i = 0; i < count; i++) {
		if (centres[i].diagonal && centres[i].count < 3)
			centres[kept++] = centres[i];
	}
	for (int32 i = 0; i < count; i++) {
		if (!centres[i].diagonal && centres[i].count >= 2)
			centres[kept++] = centres[i];
	}
	for (int32 i = 0; i < count; i++) {
		if (!centres[i].diagonal && centres[i].count < 2)
			centres[kept++] = centres[i];
	}

	for (int32 i = 0; i < kept; i++)
		refine_centre(binary, width, height, &centres[i]);

	qr_last_centres = kept;

	if (qr_verbose)
		printf("  centres: %d raw, %d kept\n", (int)count, (int)kept);

	if (kept < 3) {
		free(binary);
		free(gray);
		return false;
	}

	// With more than three candidates, take the three whose module sizes
	// agree best -- they are the ones belonging to the same code.
	bool decoded = false;

	for (int32 a = 0; a < kept && !decoded; a++) {
		for (int32 b = a + 1; b < kept && !decoded; b++) {
			for (int32 c = b + 1; c < kept && !decoded; c++) {
				finder chosen[3];
				chosen[0] = centres[a];
				chosen[1] = centres[b];
				chosen[2] = centres[c];

				float moduleSize = (chosen[0].moduleSize
					+ chosen[1].moduleSize + chosen[2].moduleSize) / 3.0f;
				if (moduleSize < 1.5f)
					continue;

				// The three squares are measured on different scan lines and
				// a rotated code crosses them at different angles, so their
				// module estimates legitimately disagree by a fair margin.
				bool consistent = true;
				for (int32 i = 0; i < 3; i++) {
					if (fabs(chosen[i].moduleSize - moduleSize)
							> moduleSize * 0.8f) {
						consistent = false;
					}
				}
				if (!consistent)
					continue;

				finder corner, right, bottom;
				order_centres(chosen, &corner, &right, &bottom);

				float across = distance(corner, right);
				float down = distance(corner, bottom);
				// A version 1 code is 21 modules across, so the finder
				// centres are 14 apart; allow for an under-estimated module
				// size rather than rejecting the smallest codes outright.
				if (across < moduleSize * 8 || down < moduleSize * 8) {
					continue;
				}
				// The two sides of a square seen in perspective differ; only
				// a gross mismatch means these three squares are not one code.
				if (fabs(across - down) > (across + down) * 0.35f) {
					continue;
				}

				// Distance between two finder centres spans dimension - 7
				// modules, so the size is that plus the 3.5 modules hidden
				// inside each of the two squares.
				float span = (across + down) / 2.0f;
				int32 estimate = (int32)(span / moduleSize + 0.5f) + 7;

				// Snap to the nearest 4n+1, which every valid size is.
				estimate -= (estimate - 1) % 4;

				if (qr_verbose) {
					printf("  triple %d/%d/%d: module %.2f across %.1f "
						"down %.1f -> dimension ~%d\n", (int)a, (int)b,
						(int)c, moduleSize, across, down, (int)estimate);
					printf("    corner (%.1f,%.1f) right (%.1f,%.1f) "
						"bottom (%.1f,%.1f)\n", corner.x, corner.y,
						right.x, right.y, bottom.x, bottom.y);
				}

				// The module size comes from a handful of pixels, so the
				// size derived from it can easily be one version out -- a
				// small code measured at 3.1 px per module instead of 3.4
				// lands a whole version away. Trying the neighbours costs
				// one sampling pass each and there is no other way to tell
				// which was right: only a version that decodes is correct.
				static const int32 kOffsets[5] = { 0, 4, -4, 8, -8 };

				for (int32 o = 0; o < 5 && !decoded; o++) {
					int32 dimension = estimate + kOffsets[o];
					if (dimension < 21 || dimension > 57)
						continue;

					// Where the fourth corner would be if the code were flat.
					float guessX = right.x + bottom.x - corner.x;
					float guessY = right.y + bottom.y - corner.y;

					float bottomRightX = guessX;
					float bottomRightY = guessY;
					float debugToX = 0, debugToY = 0;
					float debugAlignX = -1, debugAlignY = -1;

					// One module step along each axis of the code, in image
					// pixels. The two axes have to be kept apart: using the
					// horizontal vector for the vertical offset as well sends
					// the alignment search to the wrong place, and a wrong
					// fourth corner skews the whole sampling grid.
					float ax = (right.x - corner.x) / (dimension - 7);
					float ay = (right.y - corner.y) / (dimension - 7);
					float bx = (bottom.x - corner.x) / (dimension - 7);
					float by = (bottom.y - corner.y) / (dimension - 7);

					// Module size implied by *this* dimension, rather than the
					// one measured from the finder run lengths.
					//
					// Those runs are inflated whenever the dark modules bleed
					// into the light ones -- photographing a screen does it
					// reliably -- and a 25% over-estimate was enough to make
					// the size come out 25 for a 29-module code. The size
					// itself is recovered by trying the neighbours, but every
					// step that still used the measured module size (the
					// alignment search and the sampling offsets) then worked
					// to a scale that disagreed with the grid, so the right
					// dimension decoded no better than the wrong ones.
					float gridModule = (across + down) / 2.0f / (dimension - 7);
					if (gridModule < 1.0f)
						gridModule = 1.0f;

					if (dimension > 21 && getenv("QR_NOALIGN") == NULL) {
						// Three modules, not three and a half.
						//
						// The transform's fourth corner is the point the other
						// three are measured to: the finder centres sit at
						// module 3.5, so it sits at dimension - 3.5. The last
						// alignment pattern is centred on module index
						// dimension - 7, which is dimension - 6.5 in
						// continuous module coordinates. The gap between them
						// is exactly 3.
						//
						// Half a module does not sound like much and is not,
						// in the middle of the code -- but this is the corner
						// the perspective transform is anchored on, so the
						// error fans out across the whole grid. It read as a
						// frame too poor to decode right up until a reference
						// decoder read the same frame without trouble.
						float toX = guessX - 3.0f * (ax + bx);
						float toY = guessY - 3.0f * (ay + by);
						float alignX = 0;
						float alignY = 0;
						debugToX = toX;
						debugToY = toY;

						if (find_alignment(binary, width, height, toX, toY,
								gridModule, &alignX, &alignY)) {
							debugAlignX = alignX;
							debugAlignY = alignY;
							// Only trust it if it turned up roughly where it
							// should be; a false match is worse than none.
							float offX = alignX - toX;
							float offY = alignY - toY;
							if (offX * offX + offY * offY
									< gridModule * gridModule * 9) {
								bottomRightX = alignX + 3.0f * (ax + bx);
								bottomRightY = alignY + 3.0f * (ay + by);
							}
						}
					}

					if (qr_verbose) {
						printf("      dim %d: module %.2f, predicted align "
							"(%.1f,%.1f) found (%.1f,%.1f)\n",
							(int)dimension, gridModule, debugToX, debugToY,
							debugAlignX, debugAlignY);
					}

					// The transform maps the unit square onto the code, with
					// the finder centres 3.5 modules in from each edge.
					float edge = dimension - 3.5f;
					transform toImage;
					square_to_quad(corner.x, corner.y, right.x, right.y,
						bottomRightX, bottomRightY, bottom.x, bottom.y,
						&toImage);

					uint8* matrix = (uint8*)calloc(dimension * dimension, 1);
					if (matrix == NULL)
						continue;

					// Two ways of deciding whether a module is dark, tried in
					// turn. The frame-wide binarization is right most of the
					// time; the local threshold rescues codes whose modules
					// are smaller than the 16-pixel blocks that pass works
					// in. Neither is reliably better than the other, and a
					// failed attempt costs one sampling pass, so both are
					// tried rather than guessing.
					for (int32 mode = 0; mode < 2 && !decoded; mode++) {

					// Threshold from the code's own black and white levels
					// rather than from the frame-wide binarization.
					//
					// The global pass thresholds 16x16 pixel blocks, which at
					// three or four pixels per module covers five modules at
					// once -- fine for finding the finder squares, too coarse
					// to decide individual modules once they are blurred into
					// each other. The three finder centres are known to be
					// dark and their surrounding rings light, so the code
					// carries its own reference levels.
					float darkSum = 0;
					float lightSum = 0;
					int32 darkCount = 0;
					int32 lightCount = 0;

					for (int32 i = 0; i < 3; i++) {
						float fx = chosen[i].x;
						float fy = chosen[i].y;
						darkSum += gray_at(gray, width, height, fx, fy);
						darkCount++;
						// Two modules out is the light ring inside the square.
						float ring = gridModule * 2;
						lightSum += gray_at(gray, width, height, fx + ring, fy);
						lightSum += gray_at(gray, width, height, fx - ring, fy);
						lightSum += gray_at(gray, width, height, fx, fy + ring);
						lightSum += gray_at(gray, width, height, fx, fy - ring);
						lightCount += 4;
					}

					int32 threshold = 128;
					if (darkCount > 0 && lightCount > 0) {
						float darkLevel = darkSum / darkCount;
						float lightLevel = lightSum / lightCount;
						if (lightLevel > darkLevel + 12)
							threshold = (int32)((darkLevel + lightLevel) / 2);
					}

					// Vote over a small cross rather than reading the centre
					// pixel alone. A blurred module has a clean middle and
					// muddy edges, and a single sample that lands on an edge
					// is simply wrong -- with five it has to be outvoted.
					float sampleStep = gridModule * 0.22f;
					if (sampleStep < 0.4f)
						sampleStep = 0.4f;

					for (int32 y = 0; y < dimension; y++) {
						for (int32 x = 0; x < dimension; x++) {
							float u = (x + 0.5f - 3.5f) / (edge - 3.5f);
							float v = (y + 0.5f - 3.5f) / (edge - 3.5f);
							float px = 0;
							float py = 0;
							map_point(toImage, u, v, &px, &py);

							// Mode 0 reads the frame-wide binarization, mode 1
							// the grey image against a threshold taken from
							// this module's own neighbourhood.
							//
							// One threshold for the whole code is not enough
							// on a photographed screen: the glare gradient
							// across the surface moves the black and white
							// levels by more than the gap between them at the
							// dim end. Halfway between the darkest and
							// lightest pixel within a module and a half tracks
							// the gradient, and a window that size always
							// contains both a dark and a light module.
							int32 limit = 1;
							if (mode != 0) {
								float span = gridModule * 1.5f;
								int32 lo = 255;
								int32 hi = 0;
								for (int32 sy = -1; sy <= 1; sy++) {
									for (int32 sx = -1; sx <= 1; sx++) {
										int32 v = gray_at(gray, width, height,
											px + sx * span, py + sy * span);
										if (v < lo)
											lo = v;
										if (v > hi)
											hi = v;
									}
								}
								// Too flat to hold an edge: fall back to the
								// code-wide level rather than inventing one.
								limit = hi - lo > 20 ? (lo + hi) / 2 : threshold;
							}
							const uint8* source = mode == 0 ? binary : gray;
							int32 votes = 0;

							if (sample_at(source, width, height, px, py, mode,
									limit)) {
								votes += 2;
							}
							if (sample_at(source, width, height,
									px - sampleStep, py, mode, limit)) {
								votes++;
							}
							if (sample_at(source, width, height,
									px + sampleStep, py, mode, limit)) {
								votes++;
							}
							if (sample_at(source, width, height, px,
									py - sampleStep, mode, limit)) {
								votes++;
							}
							if (sample_at(source, width, height, px,
									py + sampleStep, mode, limit)) {
								votes++;
							}

							matrix[y * dimension + x] = votes >= 3 ? 1 : 0;
						}
					}

					decoded = qr_decode(matrix, dimension, result);
					if (qr_verbose && decoded) {
						printf("    decoded at dimension %d (mode %d)\n",
							(int)dimension, (int)mode);
					}
					}

					free(matrix);
				}
			}
		}
	}

	free(binary);
	free(gray);
	return decoded;
}
