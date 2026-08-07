# R QR Reader — development notes

## Why the decoder is written from scratch

There is no QR library packaged for Haiku, so everything is implemented
directly from the specification: format information and its BCH code, the
mask patterns, the block layout, Reed-Solomon correction over GF(256)
(Berlekamp-Massey, Chien search, Forney), and segment decoding.

| File | Role |
|---|---|
| `QRDetect.cpp` | Binarization, finder-pattern search, perspective grid, sampling |
| `QRDecode.cpp` | Format info, de-interleaving, Reed-Solomon, segments |
| `RQRReader.cpp` | Camera capture, preview, UI |

## The webcam returns a mirrored image

This was the hard one, and it does not look like what it is.

A mirrored QR still has its three finder squares in three corners, so
detection succeeds completely — module positions came out accurate to within
a pixel. What flips is which finder is "to the right" of the corner, and that
swap transposes the sampled grid. Finder patterns are unchanged by a
transpose, so the sampled matrix looks perfectly correct right up to the
point where the data will not decode.

`qr_decode()` therefore tries the transposed matrix whenever the first
attempt fails. zbar and OpenCV both do the same thing.

## Traps in the detector, all of them found the hard way

- **Alignment pattern centre is a *dark* module.** Reading across the middle
  of the 5×5 gives dark-light-dark-light-dark, so the run on the centre is
  the dark one in a light-dark-light window. Searching for dark-light-dark
  and taking the middle of the light run centres on the ring instead — one
  whole module out.
- **The alignment search box lies inside the data area** and is full of
  light-dark-light runs that are not the pattern. Take the candidate nearest
  the predicted position, not the first in raster order.
- **Extrapolating from the alignment centre to the grid corner is 3.0
  modules, not 3.5.** Finder centres sit at module 3.5; the last alignment
  pattern is centred on `dimension - 6.5`.
- **A run spanning `L..L+n-1` has its middle at `L + (n-1)/2`.** The obvious
  expression gives `L + n/2` — half a pixel out, in the same direction for
  all three finders, which bends the perspective grid.
- **Module size measured from finder runs is inflated** wherever dark modules
  bleed (photographing a screen does it reliably). Recompute it from the
  dimension being tried before using it for the alignment search or the
  sampling offsets.
- **The vertical cross-check needs slack in x.** A scan line crosses the
  square at whatever height it happens to be at, so the horizontal centre can
  be a module out; a vertical scan launched from near the edge of the centre
  block reads a short run and the candidate is discarded. Retrying across
  ±1.5 modules is what stopped a plainly visible finder from going missing.
- **The diagonal check is a preference, not a requirement.** Requiring
  1:1:3:1:1 along the diagonal cuts false candidates dramatically — a scan
  went from 1.2 s to 0.37 s — but it is also the first check to fail on a
  small blurred square. Three *spurious* squares can pass it while the real
  third finder does not. Use it to order candidates, not to filter them.

## Format information bit order

MSB first, along row 8 and then up column 8. Verified against a reference
encoder rather than reasoned about: all 32 (level, mask) combinations were
generated and read back both ways — this order recovers all 32, the reverse
recovers none.

Worth stating because reading it backwards does not fail loudly. The reversed
value still lands within correcting distance of another valid format word, so
it returns a plausible wrong error-correction level, and the wrong block
layout hands Reed-Solomon bytes that were never a codeword.

## Verify your ground truth before trusting it

OpenCV's `straight_qrcode` output was used as a "perfect matrix" to test the
decoder in isolation. It is transposed relative to the standard layout, which
led to a correct piece of code being "fixed" and a regression. Cross-check
any reference against an authoritative encoder (segno) before drawing
conclusions from it.

The test set in `screenshots/` and the generated ones are all verified with
`zbarimg` first — an image the reference decoder cannot read is not a
meaningful test.

## Capture

Frames come from `BMediaRecorder` rather than a hand-written
`BBufferConsumer`. Decoding runs on its own thread: a scan of a 320×240 frame
takes around 370 ms on a 1.33 GHz Atom, and doing that in the capture
callback would stall the media node. The callback keeps the newest frame and
the worker takes whatever is there when it is ready — and it does *not*
overwrite a frame the scanner has not looked at yet, because at three frames
a second the one being dropped may be the only sharp one in the run.

`BMediaRoster::GetVideoInput()` returns `B_NAME_NOT_FOUND` until something
assigns a default video node, and nothing does that automatically — a camera
that is plugged in and already producing frames still looks absent. The app
looks for the node itself and sets the default once it finds one.

## Browser launch

WebPositive's signature is `application/x-vnd.Haiku-WebPositive`. The
BeOS-era `application/x-vnd.Be-WEBP` is not it, and launching a signature
nothing answers to fails silently. Ask for the scheme handler
(`application/x-vnd.Be.URL.https`) first so the user's chosen browser wins,
and fall back to WebPositive by its real signature.
