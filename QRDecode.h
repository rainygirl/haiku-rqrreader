/*
 * QR scanning and decoding.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef QR_DECODE_H
#define QR_DECODE_H


#include <String.h>
#include <SupportDefs.h>


// Set to have the pipeline report where it gives up. Off in the application;
// the test harness turns it on.
extern bool qr_verbose;

// How many finder squares the last qr_scan() call kept. Lets a caller tell a
// frame with no code in it from one it nearly read.
extern int32 qr_last_centres;

// Decodes a sampled module matrix: one byte per module, non-zero means dark,
// laid out row by row with the top-left finder pattern at (0,0).
bool qr_decode(const uint8* matrix, int32 size, BString* result);

// The whole pipeline: takes a B_RGB32 frame, finds a code in it, and decodes
// it. Returns false when there is nothing to read, which is the normal case
// for most frames.
bool qr_scan(const uint8* frame, int32 width, int32 height, int32 bytesPerRow,
	BString* result);


#endif	// QR_DECODE_H
