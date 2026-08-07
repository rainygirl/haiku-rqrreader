/*
 * QR decoding: bit matrix -> bytes.
 *
 * Everything here is self-contained -- there is no QR library packaged for
 * Haiku, so the format info, the mask patterns, the block layout, the
 * Reed-Solomon correction and the segment decoding are all implemented
 * directly from the specification.
 *
 * Scope: versions 1 to 10 (21x21 up to 57x57), error correction levels
 * L/M/Q/H, and the numeric, alphanumeric and byte segment modes. Kanji mode,
 * ECI, structured append and Micro QR are not handled -- they are rare in
 * practice and each one is a separate sub-format.
 *
 * Distributed under the terms of the MIT License.
 */

#include "QRDecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// #pragma mark - GF(256)


// The field QR uses: primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d),
// generator 2. Log/antilog tables make multiplication a table lookup, which
// matters on a CPU without a fast multiplier to spare.
static uint8 sExp[512];
static uint8 sLog[256];
static bool sTablesReady = false;


static void
gf_init()
{
	if (sTablesReady)
		return;

	int32 x = 1;
	for (int32 i = 0; i < 255; i++) {
		sExp[i] = (uint8)x;
		sLog[x] = (uint8)i;
		x <<= 1;
		if ((x & 0x100) != 0)
			x ^= 0x11d;
	}
	for (int32 i = 255; i < 512; i++)
		sExp[i] = sExp[i - 255];

	sTablesReady = true;
}


static inline uint8
gf_mul(uint8 a, uint8 b)
{
	if (a == 0 || b == 0)
		return 0;
	return sExp[sLog[a] + sLog[b]];
}


static inline uint8
gf_div(uint8 a, uint8 b)
{
	if (a == 0)
		return 0;
	return sExp[sLog[a] + 255 - sLog[b]];
}


// #pragma mark - Reed-Solomon


// Syndromes, Berlekamp-Massey for the error locator, Chien search for the
// positions and Forney for the magnitudes. Returns false when the codeword
// carries more errors than the parity can place.
static bool
rs_correct(uint8* data, int32 length, int32 eccCount)
{
	gf_init();

	// Syndromes: the received polynomial evaluated at a^0 .. a^(ecc-1). QR's
	// generator starts at a^0, which is what fixes the Forney formula below.
	uint8 syndromes[64];
	bool hasError = false;
	for (int32 i = 0; i < eccCount; i++) {
		uint8 value = 0;
		for (int32 j = 0; j < length; j++)
			value = gf_mul(value, sExp[i]) ^ data[j];
		syndromes[i] = value;
		if (value != 0)
			hasError = true;
	}

	if (!hasError)
		return true;

	// Berlekamp-Massey for the error locator.
	uint8 lambda[66];
	uint8 previous[66];
	uint8 saved[66];
	memset(lambda, 0, sizeof(lambda));
	memset(previous, 0, sizeof(previous));
	lambda[0] = 1;
	previous[0] = 1;

	int32 degree = 0;
	int32 shift = 1;
	uint8 lastDelta = 1;

	for (int32 n = 0; n < eccCount; n++) {
		uint8 delta = syndromes[n];
		for (int32 i = 1; i <= degree; i++)
			delta ^= gf_mul(lambda[i], syndromes[n - i]);

		if (delta == 0) {
			shift++;
			continue;
		}

		memcpy(saved, lambda, sizeof(lambda));
		uint8 scale = gf_div(delta, lastDelta);
		for (int32 i = 0; i + shift <= eccCount; i++)
			lambda[i + shift] ^= gf_mul(scale, previous[i]);

		if (2 * degree <= n) {
			degree = n + 1 - degree;
			memcpy(previous, saved, sizeof(saved));
			lastDelta = delta;
			shift = 1;
		} else
			shift++;
	}

	if (degree <= 0 || degree * 2 > eccCount)
		return false;

	// Chien search. Codeword position i holds the term with X = a^(n-1-i),
	// so it is in error when the locator vanishes at X inverse.
	int32 positions[32];
	int32 found = 0;
	for (int32 i = 0; i < length; i++) {
		uint8 xInverse = sExp[(255 - ((length - 1 - i) % 255)) % 255];
		uint8 value = 0;
		uint8 power = 1;
		for (int32 j = 0; j <= degree; j++) {
			value ^= gf_mul(lambda[j], power);
			power = gf_mul(power, xInverse);
		}
		if (value == 0) {
			if (found >= 32)
				return false;
			positions[found++] = i;
		}
	}

	if (found != degree)
		return false;

	// Error evaluator: omega = syndromes * lambda, truncated to ecc terms.
	uint8 omega[64];
	memset(omega, 0, sizeof(omega));
	for (int32 i = 0; i < eccCount; i++) {
		uint8 value = 0;
		for (int32 j = 0; j <= i && j <= degree; j++)
			value ^= gf_mul(syndromes[i - j], lambda[j]);
		omega[i] = value;
	}

	for (int32 e = 0; e < found; e++) {
		int32 position = positions[e];
		int32 power = (length - 1 - position) % 255;
		uint8 x = sExp[power];
		uint8 xInverse = sExp[(255 - power) % 255];

		uint8 numerator = 0;
		uint8 term = 1;
		for (int32 i = 0; i < eccCount; i++) {
			numerator ^= gf_mul(omega[i], term);
			term = gf_mul(term, xInverse);
		}
		// With the generator rooted at a^0 the magnitude carries an extra
		// factor of X. Leaving it out corrects the wrong amount at every
		// position, which the final syndrome check then rejects -- so the
		// symptom is not a wrong answer but a refusal to decode at all.
		numerator = gf_mul(numerator, x);

		// Formal derivative of the locator: only the odd-degree terms
		// survive in characteristic 2.
		uint8 denominator = 0;
		term = 1;
		for (int32 i = 1; i <= degree; i += 2) {
			uint8 value = lambda[i];
			uint8 scale = 1;
			for (int32 k = 0; k < i - 1; k++)
				scale = gf_mul(scale, xInverse);
			denominator ^= gf_mul(value, scale);
		}
		(void)term;

		if (denominator == 0)
			return false;

		data[position] ^= gf_div(numerator, denominator);
	}

	// A wrong correction is worse than a refusal, and the syndromes are cheap
	// to recompute.
	for (int32 i = 0; i < eccCount; i++) {
		uint8 value = 0;
		for (int32 j = 0; j < length; j++)
			value = gf_mul(value, sExp[i]) ^ data[j];
		if (value != 0)
			return false;
	}

	return true;
}


// #pragma mark - tables


// Per version and error correction level: error correction codewords per
// block, then two groups of (block count, data codewords per block).
struct block_layout {
	uint8	eccPerBlock;
	uint8	group1Blocks;
	uint8	group1Data;
	uint8	group2Blocks;
	uint8	group2Data;
};

// Index: [version - 1][level], level order L, M, Q, H.
static const block_layout kBlocks[10][4] = {
	{ {  7, 1, 19, 0,  0 }, { 10, 1, 16, 0,  0 }, { 13, 1, 13, 0,  0 }, { 17, 1,  9, 0,  0 } },
	{ { 10, 1, 34, 0,  0 }, { 16, 1, 28, 0,  0 }, { 22, 1, 22, 0,  0 }, { 28, 1, 16, 0,  0 } },
	{ { 15, 1, 55, 0,  0 }, { 26, 1, 44, 0,  0 }, { 18, 2, 17, 0,  0 }, { 22, 2, 13, 0,  0 } },
	{ { 20, 1, 80, 0,  0 }, { 18, 2, 32, 0,  0 }, { 26, 2, 24, 0,  0 }, { 16, 4,  9, 0,  0 } },
	{ { 26, 1, 108, 0, 0 }, { 24, 2, 43, 0,  0 }, { 18, 2, 15, 2, 16 }, { 22, 2, 11, 2, 12 } },
	{ { 18, 2, 68, 0,  0 }, { 16, 4, 27, 0,  0 }, { 24, 4, 19, 0,  0 }, { 28, 4, 15, 0,  0 } },
	{ { 20, 2, 78, 0,  0 }, { 18, 4, 31, 0,  0 }, { 18, 2, 14, 4, 15 }, { 26, 4, 13, 1, 14 } },
	{ { 24, 2, 97, 0,  0 }, { 22, 2, 38, 2, 39 }, { 22, 4, 18, 2, 19 }, { 26, 4, 14, 2, 15 } },
	{ { 30, 2, 116, 0, 0 }, { 22, 3, 36, 2, 37 }, { 20, 4, 16, 4, 17 }, { 24, 4, 12, 4, 13 } },
	{ { 18, 2, 68, 2, 69 }, { 26, 4, 43, 1, 44 }, { 24, 6, 19, 2, 20 }, { 28, 6, 15, 2, 16 } }
};

// Centre coordinates of the alignment patterns, per version.
static const uint8 kAlignment[10][3] = {
	{ 0,  0,  0 },
	{ 6, 18,  0 },
	{ 6, 22,  0 },
	{ 6, 26,  0 },
	{ 6, 30,  0 },
	{ 6, 34,  0 },
	{ 6, 22, 38 },
	{ 6, 24, 42 },
	{ 6, 26, 46 },
	{ 6, 28, 50 }
};


// #pragma mark - matrix helpers


static inline bool
module(const uint8* matrix, int32 size, int32 x, int32 y)
{
	if (x < 0 || y < 0 || x >= size || y >= size)
		return false;
	return matrix[y * size + x] != 0;
}


// True where the module carries format, version, finder, timing or alignment
// information rather than data.
static bool
is_function_module(int32 size, int32 version, int32 x, int32 y)
{
	// Finder patterns plus their separators and the format areas.
	if (x < 9 && y < 9)
		return true;
	if (x >= size - 8 && y < 9)
		return true;
	if (x < 9 && y >= size - 8)
		return true;

	// Timing patterns.
	if (x == 6 || y == 6)
		return true;

	// Version information, present from version 7 on.
	if (version >= 7) {
		if (x < 6 && y >= size - 11 && y < size - 8)
			return true;
		if (y < 6 && x >= size - 11 && x < size - 8)
			return true;
	}

	// Alignment patterns, skipping the three that would collide with the
	// finder patterns.
	const uint8* centres = kAlignment[version - 1];
	for (int32 i = 0; i < 3 && centres[i] != 0; i++) {
		for (int32 j = 0; j < 3 && centres[j] != 0; j++) {
			int32 cx = centres[j];
			int32 cy = centres[i];
			if ((cx == 6 && cy == 6)
				|| (cx == 6 && cy == size - 7)
				|| (cx == size - 7 && cy == 6)) {
				continue;
			}
			if (x >= cx - 2 && x <= cx + 2 && y >= cy - 2 && y <= cy + 2)
				return true;
		}
	}

	return false;
}


static bool
mask_bit(int32 mask, int32 x, int32 y)
{
	switch (mask) {
		case 0: return ((y + x) % 2) == 0;
		case 1: return (y % 2) == 0;
		case 2: return (x % 3) == 0;
		case 3: return ((y + x) % 3) == 0;
		case 4: return (((y / 2) + (x / 3)) % 2) == 0;
		case 5: return ((y * x) % 2 + (y * x) % 3) == 0;
		case 6: return (((y * x) % 2 + (y * x) % 3) % 2) == 0;
		case 7: return (((y + x) % 2 + (y * x) % 3) % 2) == 0;
	}
	return false;
}


// The 15-bit format field is protected by a BCH(15,5) code; the nearest
// codeword by Hamming distance wins, which recovers a badly sampled field.
static bool
decode_format(uint32 bits, int32* level, int32* mask, int32* distance)
{
	// These are the encoded sequences as they appear in the symbol -- the
	// 0x5412 mask is already folded in. Unmasking the bits before comparing
	// would apply the mask twice.
	static const uint32 kFormats[32] = {
		0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9, 0x40CE, 0x4F97, 0x4AA0,
		0x77C4, 0x72F3, 0x7DAA, 0x789D, 0x662F, 0x6318, 0x6C41, 0x6976,
		0x1689, 0x13BE, 0x1CE7, 0x19D0, 0x0762, 0x0255, 0x0D0C, 0x083B,
		0x355F, 0x3068, 0x3F31, 0x3A06, 0x24B4, 0x2183, 0x2EDA, 0x2BED
	};

	int32 best = -1;
	int32 bestDistance = 32;

	for (int32 i = 0; i < 32; i++) {
		uint32 difference = bits ^ kFormats[i];
		int32 distance = 0;
		while (difference != 0) {
			distance += difference & 1;
			difference >>= 1;
		}
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}

	if (best < 0 || bestDistance > 3)
		return false;

	if (distance != NULL)
		*distance = bestDistance;

	// The top two bits are the level, in the order M, L, H, Q.
	static const int32 kLevelOrder[4] = { 1, 0, 3, 2 };
	*level = kLevelOrder[(best >> 3) & 3];
	*mask = best & 7;
	return true;
}


// #pragma mark - segment decoding


class BitReader {
public:
	BitReader(const uint8* data, int32 length)
		:
		fData(data),
		fLength(length),
		fPosition(0)
	{
	}

	bool Read(int32 count, uint32* value)
	{
		if (fPosition + count > fLength * 8)
			return false;

		uint32 result = 0;
		for (int32 i = 0; i < count; i++) {
			int32 bit = fPosition + i;
			uint8 byte = fData[bit / 8];
			result = (result << 1) | ((byte >> (7 - (bit % 8))) & 1);
		}
		fPosition += count;
		*value = result;
		return true;
	}

	int32 Remaining() const { return fLength * 8 - fPosition; }

private:
	const uint8*	fData;
	int32			fLength;
	int32			fPosition;
};


static const char* const kAlphanumeric
	= "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";


static bool
decode_segments(const uint8* data, int32 length, int32 version, BString* out)
{
	BitReader reader(data, length);

	// Character count field widths change at version 10 and 27; only the
	// first boundary matters for the versions handled here.
	int32 numericBits = version < 10 ? 10 : 12;
	int32 alphaBits = version < 10 ? 9 : 11;
	int32 byteBits = version < 10 ? 8 : 16;

	while (reader.Remaining() >= 4) {
		uint32 mode = 0;
		if (!reader.Read(4, &mode))
			break;

		if (mode == 0)
			break;

		if (mode == 1) {
			uint32 count = 0;
			if (!reader.Read(numericBits, &count))
				return false;

			while (count >= 3) {
				uint32 group = 0;
				if (!reader.Read(10, &group))
					return false;
				char digits[4];
				snprintf(digits, sizeof(digits), "%03d", (int)(group % 1000));
				out->Append(digits, 3);
				count -= 3;
			}
			if (count == 2) {
				uint32 group = 0;
				if (!reader.Read(7, &group))
					return false;
				char digits[3];
				snprintf(digits, sizeof(digits), "%02d", (int)(group % 100));
				out->Append(digits, 2);
			} else if (count == 1) {
				uint32 group = 0;
				if (!reader.Read(4, &group))
					return false;
				char digit = (char)('0' + (group % 10));
				out->Append(&digit, 1);
			}
		} else if (mode == 2) {
			uint32 count = 0;
			if (!reader.Read(alphaBits, &count))
				return false;

			while (count >= 2) {
				uint32 pair = 0;
				if (!reader.Read(11, &pair))
					return false;
				if (pair >= 45 * 45)
					return false;
				char letters[2];
				letters[0] = kAlphanumeric[pair / 45];
				letters[1] = kAlphanumeric[pair % 45];
				out->Append(letters, 2);
				count -= 2;
			}
			if (count == 1) {
				uint32 single = 0;
				if (!reader.Read(6, &single))
					return false;
				if (single >= 45)
					return false;
				out->Append(&kAlphanumeric[single], 1);
			}
		} else if (mode == 4) {
			uint32 count = 0;
			if (!reader.Read(byteBits, &count))
				return false;

			for (uint32 i = 0; i < count; i++) {
				uint32 byte = 0;
				if (!reader.Read(8, &byte))
					return false;
				char c = (char)byte;
				out->Append(&c, 1);
			}
		} else {
			// Kanji, ECI, structured append and the FNC1 variants are not
			// handled; stopping is better than emitting nonsense.
			break;
		}
	}

	return out->Length() > 0;
}


// #pragma mark - entry point


// One attempt with a known error correction level and mask.
static bool
decode_with(const uint8* matrix, int32 size, int32 version, int32 level,
	int32 mask, BString* result)
{
	const block_layout& layout = kBlocks[version - 1][level];
	int32 totalBlocks = layout.group1Blocks + layout.group2Blocks;
	int32 totalData = layout.group1Blocks * layout.group1Data
		+ layout.group2Blocks * layout.group2Data;
	int32 totalCodewords = totalData + totalBlocks * layout.eccPerBlock;

	uint8* codewords = (uint8*)calloc(totalCodewords, 1);
	if (codewords == NULL)
		return false;

	// The standard zigzag: two columns at a time, right to left, alternating
	// upwards and downwards, skipping the vertical timing column.
	int32 bitIndex = 0;
	int32 direction = -1;
	int32 y = size - 1;

	for (int32 x = size - 1; x > 0; x -= 2) {
		if (x == 6)
			x--;

		while (true) {
			for (int32 column = 0; column < 2; column++) {
				int32 cx = x - column;
				if (is_function_module(size, version, cx, y))
					continue;

				bool bit = module(matrix, size, cx, y);
				if (mask_bit(mask, cx, y))
					bit = !bit;

				if (bit && bitIndex / 8 < totalCodewords)
					codewords[bitIndex / 8] |= 0x80 >> (bitIndex % 8);
				bitIndex++;
			}

			y += direction;
			if (y < 0 || y >= size) {
				y -= direction;
				direction = -direction;
				break;
			}
		}
	}

	// De-interleave: codewords are stored taking one from each block in turn.
	uint8* blocks[32];
	int32 blockData[32];

	for (int32 i = 0; i < totalBlocks; i++) {
		blockData[i] = i < layout.group1Blocks
			? layout.group1Data : layout.group2Data;
		blocks[i] = (uint8*)calloc(blockData[i] + layout.eccPerBlock, 1);
		if (blocks[i] == NULL) {
			for (int32 j = 0; j < i; j++)
				free(blocks[j]);
			free(codewords);
			return false;
		}
	}

	int32 source = 0;
	int32 maxData = layout.group1Data > layout.group2Data
		? layout.group1Data : layout.group2Data;

	for (int32 i = 0; i < maxData; i++) {
		for (int32 b = 0; b < totalBlocks; b++) {
			if (i < blockData[b] && source < totalCodewords)
				blocks[b][i] = codewords[source++];
		}
	}
	for (int32 i = 0; i < layout.eccPerBlock; i++) {
		for (int32 b = 0; b < totalBlocks; b++) {
			if (source < totalCodewords)
				blocks[b][blockData[b] + i] = codewords[source++];
		}
	}

	free(codewords);

	uint8* data = (uint8*)calloc(totalData, 1);
	if (data == NULL) {
		for (int32 i = 0; i < totalBlocks; i++)
			free(blocks[i]);
		return false;
	}

	bool ok = true;
	int32 offset = 0;
	for (int32 b = 0; b < totalBlocks; b++) {
		if (!rs_correct(blocks[b], blockData[b] + layout.eccPerBlock,
				layout.eccPerBlock)) {
			ok = false;
			break;
		}
		memcpy(data + offset, blocks[b], blockData[b]);
		offset += blockData[b];
	}

	for (int32 i = 0; i < totalBlocks; i++)
		free(blocks[i]);

	if (ok) {
		result->SetTo("");
		ok = decode_segments(data, totalData, version, result);
	}

	free(data);
	return ok;
}


// One orientation. qr_decode() tries the mirror image too.
static bool
decode_oriented(const uint8* matrix, int32 size, BString* result)
{
	if (size < 21 || size > 57 || ((size - 17) % 4) != 0)
		return false;

	int32 version = (size - 17) / 4;
	if (version < 1 || version > 10)
		return false;

	// Format information, first copy: along row 8 and then up column 8, most
	// significant bit first.
	//
	// Verified against a reference encoder rather than reasoned about: all 32
	// (level, mask) combinations were generated and read back both ways, and
	// this order recovers all 32 while the reverse recovers none. Worth
	// stating because reading it the other way round does not fail loudly --
	// the reversed value still lands within correcting distance of some other
	// valid format word, so it returns a plausible wrong answer.
	uint32 format = 0;
	for (int32 i = 0; i <= 5; i++)
		format = (format << 1) | (module(matrix, size, i, 8) ? 1 : 0);
	format = (format << 1) | (module(matrix, size, 7, 8) ? 1 : 0);
	format = (format << 1) | (module(matrix, size, 8, 8) ? 1 : 0);
	format = (format << 1) | (module(matrix, size, 8, 7) ? 1 : 0);
	for (int32 i = 5; i >= 0; i--)
		format = (format << 1) | (module(matrix, size, 8, i) ? 1 : 0);

	int32 level = 0;
	int32 mask = 0;
	int32 distance = 15;
	if (decode_format(format, &level, &mask, &distance)) {
		if (qr_verbose) {
			printf("    format says version %d level %d mask %d\n",
				(int)version, (int)level, (int)mask);
		}
		if (decode_with(matrix, size, version, level, mask, result))
			return true;

		// An exact read means the fifteen format modules and their BCH parity
		// all agree. Guessing the same fields 32 ways after that is not going
		// to find anything the sampling did not already lose, and on this CPU
		// the search costs more than the rest of the scan put together --
		// time that is better spent on the next camera frame.
		if (distance == 0)
			return false;
	}

	// The format field is fifteen modules out of several hundred and sits at
	// the edge of the symbol, so it is the first thing a poor sample gets
	// wrong. Every combination is only 32 attempts, and the error correction
	// check at the end of each one makes a false acceptance implausible.
	for (int32 tryLevel = 0; tryLevel < 4; tryLevel++) {
		for (int32 tryMask = 0; tryMask < 8; tryMask++) {
			if (decode_with(matrix, size, version, tryLevel, tryMask,
					result)) {
				if (qr_verbose) {
					printf("    recovered with level %d mask %d\n",
						(int)tryLevel, (int)tryMask);
				}
				return true;
			}
		}
	}

	return false;
}


bool
qr_decode(const uint8* matrix, int32 size, BString* result)
{
	if (decode_oriented(matrix, size, result))
		return true;

	// Try the code mirrored.
	//
	// This laptop's webcam hands back a mirrored image, the way a front
	// camera usually does, and a mirrored QR is not a broken one -- the three
	// finder squares are all still there and land in three corners, so the
	// detector is perfectly happy. What flips is which of them is "to the
	// right" of the corner, and that swap transposes the sampled grid. The
	// finder patterns survive a transpose unchanged, so the matrix looks
	// correct right up to the point where the data refuses to decode.
	//
	// Transposing is the whole correction: reflecting a QR about its main
	// diagonal is exactly what a mirror does to it, once the corner has been
	// re-identified.
	uint8* flipped = (uint8*)malloc(size * size);
	if (flipped == NULL)
		return false;

	for (int32 y = 0; y < size; y++) {
		for (int32 x = 0; x < size; x++)
			flipped[x * size + y] = matrix[y * size + x];
	}

	bool ok = decode_oriented(flipped, size, result);
	if (ok && qr_verbose)
		printf("    decoded mirrored\n");

	free(flipped);
	return ok;
}
