/*
 * R QR Reader -- reads QR codes with the webcam.
 *
 * Frames come from BMediaRecorder rather than a hand-written BBufferConsumer:
 * the recorder's Connect() already knows how to reach the video input, and
 * the fix this project made to BMediaRecorder::Start() (it started the time
 * source instead of the node, so no buffers ever arrived) applies to video
 * exactly as it did to audio.
 *
 * The decode runs on its own thread. The camera here manages about three
 * frames a second and a scan of a 320x240 frame is not free on a 1.33 GHz
 * in-order CPU, so doing it in the capture callback would stall the media
 * node; instead the callback keeps the newest frame and the worker takes
 * whatever is there when it is ready.
 *
 * Distributed under the terms of the MIT License.
 */

#include "QRDecode.h"

#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <Clipboard.h>
#include <LocaleRoster.h>
#include <Locker.h>
#include <MediaDefs.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <File.h>
#include <String.h>
#include <StringView.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char* const kAppSignature = "application/x-vnd.RQRReader";
// WebPositive's actual signature. The BeOS-era "application/x-vnd.Be-WEBP"
// is not it, and launching a signature nothing answers to fails silently --
// the code was read, the dialog appeared, and nothing opened.
static const char* const kWebSignature
	= "application/x-vnd.Haiku-WebPositive";

static const uint32 kMsgTick		= 'tick';
static const uint32 kMsgFound		= 'fnd ';
static const uint32 kMsgCopy		= 'copy';
static const uint32 kMsgOpenUrl		= 'ourl';


// #pragma mark - strings


enum string_id {
	kStrTitle = 0, kStrScanning, kStrNoCamera, kStrFound, kStrCopy,
	kStrOpenUrl, kStrClear, kStrUrlQuestion, kStrOpen, kStrCancel, kStrCopied,
	kStrOk, kStrCannotOpen,
	kStringCount
};

static const char* const kStringsEn[kStringCount] = {
	"R QR Reader", "Hold a QR code up to the camera", "No camera found",
	"Found:", "Copy", "Open in WebPositive", "Scan again",
	"This looks like a web address. Open it in WebPositive?",
	"Open", "Not now", "Copied to the clipboard.", "OK",
	"Could not open the browser: "
};

static const char* const kStringsKo[kStringCount] = {
	"R QR Reader", "QR 코드를 카메라에 비춰 주세요", "카메라를 찾을 수 없습니다",
	"인식됨:", "복사", "WebPositive로 열기", "다시 스캔",
	"웹 주소로 보입니다. WebPositive로 열까요?",
	"열기", "지금 안 함", "클립보드에 복사했습니다.", "확인",
	"브라우저를 열 수 없습니다: "
};

static const char* const* sStrings = kStringsEn;

static inline const char*
T(string_id id)
{
	return sStrings[id];
}


static bool
looks_like_url(const BString& text)
{
	return text.IFindFirst("http://") == 0 || text.IFindFirst("https://") == 0;
}


// #pragma mark - PreviewView


class PreviewView : public BView {
public:
							PreviewView(BRect frame);
	virtual					~PreviewView();

	virtual	void			Draw(BRect updateRect);

			void			SetFrameData(const uint8* data, int32 width,
								int32 height, int32 bytesPerRow);

private:
			BBitmap*		fBitmap;
			BLocker			fLock;
};


PreviewView::PreviewView(BRect frame)
	:
	BView(frame, "preview", B_FOLLOW_ALL, B_WILL_DRAW),
	fBitmap(NULL),
	fLock("preview")
{
	SetViewColor(20, 20, 24);
}


PreviewView::~PreviewView()
{
	delete fBitmap;
}


void
PreviewView::SetFrameData(const uint8* data, int32 width, int32 height,
	int32 bytesPerRow)
{
	BAutolock lock(fLock);

	if (fBitmap == NULL || fBitmap->Bounds().IntegerWidth() != width - 1
		|| fBitmap->Bounds().IntegerHeight() != height - 1) {
		delete fBitmap;
		fBitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
	}

	if (fBitmap == NULL || fBitmap->InitCheck() != B_OK)
		return;

	uint8* destination = (uint8*)fBitmap->Bits();
	int32 destinationRow = fBitmap->BytesPerRow();
	for (int32 y = 0; y < height; y++) {
		memcpy(destination + y * destinationRow, data + y * bytesPerRow,
			width * 4);
	}
}


void
PreviewView::Draw(BRect updateRect)
{
	BAutolock lock(fLock);

	BRect bounds = Bounds();
	if (fBitmap == NULL || fBitmap->InitCheck() != B_OK) {
		SetHighColor(20, 20, 24);
		FillRect(bounds);
		return;
	}

	// Letterboxed rather than stretched: a squashed code is harder to read
	// for the person aiming the camera, even though the decoder works from
	// the original frame either way.
	BRect source = fBitmap->Bounds();
	float scale = bounds.Width() / (source.Width() + 1);
	float verticalScale = bounds.Height() / (source.Height() + 1);
	if (verticalScale < scale)
		scale = verticalScale;

	float width = (source.Width() + 1) * scale;
	float height = (source.Height() + 1) * scale;
	BRect target(0, 0, width - 1, height - 1);
	target.OffsetBy((bounds.Width() - width) / 2,
		(bounds.Height() - height) / 2);

	SetHighColor(20, 20, 24);
	FillRect(bounds);
	DrawBitmap(fBitmap, source, target);
}


// #pragma mark - ReaderWindow


class ReaderWindow : public BWindow {
public:
							ReaderWindow();
	virtual					~ReaderWindow();

	virtual	void			MessageReceived(BMessage* message);
	virtual	bool			QuitRequested();

			void			FrameArrived(const void* data, size_t size,
								const media_format& format);

private:
	static	int32			_ScanEntry(void* data);
			void			_ScanLoop();
			status_t		_StartCamera();
			void			_StopCamera();
			void			_Report(const BString& text);

			PreviewView*	fPreview;
			BStringView*	fStatus;
			BTextView*		fResult;
			BButton*		fCopyButton;

			BMediaRecorder*	fRecorder;
			media_node		fInput;
			bool			fHaveInput;

			BLocker			fFrameLock;
			int32			dropped;
			uint8*			fFrame;
			int32			fFrameWidth;
			int32			fFrameHeight;
			int32			fFrameRow;
			bool			fFrameReady;

			thread_id		fScanThread;
	volatile bool			fQuitScan;

			BString			fText;
};


static void
frame_hook(void* cookie, bigtime_t, void* data, size_t size,
	const media_format& format)
{
	((ReaderWindow*)cookie)->FrameArrived(data, size, format);
}


ReaderWindow::ReaderWindow()
	:
	BWindow(BRect(120, 120, 620, 560), T(kStrTitle), B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE),
	fPreview(NULL),
	fStatus(NULL),
	fResult(NULL),
	fRecorder(NULL),
	fHaveInput(false),
	fFrameLock("frame"),
	dropped(0),
	fFrame(NULL),
	fFrameWidth(0),
	fFrameHeight(0),
	fFrameRow(0),
	fFrameReady(false),
	fScanThread(-1),
	fQuitScan(false)
{
	BRect bounds = Bounds();

	fPreview = new PreviewView(BRect(0, 0, bounds.right, 300));
	AddChild(fPreview);

	fStatus = new BStringView(BRect(10, 308, bounds.right - 10, 326),
		"status", T(kStrScanning), B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP);
	AddChild(fStatus);

	BRect resultFrame(10, 332, bounds.right - 10, 392);
	fResult = new BTextView(resultFrame, "result",
		BRect(0, 0, resultFrame.Width() - 8, resultFrame.Height()),
		B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW);
	fResult->MakeEditable(false);
	fResult->SetStylable(false);
	fResult->SetWordWrap(true);
	AddChild(fResult);

	fCopyButton = new BButton(BRect(10, 400, 110, 424), "copy", T(kStrCopy),
		new BMessage(kMsgCopy), B_FOLLOW_LEFT | B_FOLLOW_BOTTOM);
	fCopyButton->SetEnabled(false);
	AddChild(fCopyButton);

	if (_StartCamera() != B_OK)
		fStatus->SetText(T(kStrNoCamera));

	fScanThread = spawn_thread(_ScanEntry, "qr scan", B_LOW_PRIORITY, this);
	if (fScanThread >= 0)
		resume_thread(fScanThread);
}


ReaderWindow::~ReaderWindow()
{
	fQuitScan = true;
	if (fScanThread >= 0) {
		status_t dummy;
		wait_for_thread(fScanThread, &dummy);
		fScanThread = -1;
	}

	_StopCamera();
	free(fFrame);
}


status_t
ReaderWindow::_StartCamera()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL)
		return B_ERROR;

	// GetVideoInput() only answers once something has assigned a default
	// video node, and nothing does that automatically -- a camera that is
	// plugged in, enumerated and already producing frames still leaves it
	// returning B_NAME_NOT_FOUND. Fall back to looking for the node itself.
	status_t status = roster->GetVideoInput(&fInput);
	if (status != B_OK) {
		live_node_info nodes[32];
		int32 count = 32;
		status = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL,
			B_BUFFER_PRODUCER | B_PHYSICAL_INPUT);
		if (status != B_OK || count <= 0)
			return status != B_OK ? status : B_NAME_NOT_FOUND;

		bool picked = false;
		for (int32 i = 0; i < count; i++) {
			// A sound card is a physical input too; only take one that can
			// actually hand out video.
			media_output outputs[8];
			int32 outputCount = 8;
			if (roster->GetFreeOutputsFor(nodes[i].node, outputs, 8,
					&outputCount, B_MEDIA_RAW_VIDEO) != B_OK
				|| outputCount <= 0) {
				continue;
			}
			fInput = nodes[i].node;
			picked = true;
			break;
		}

		if (!picked)
			return B_NAME_NOT_FOUND;

		// Set it, so every other media application finds the camera too.
		roster->SetVideoInput(fInput);
	}
	fHaveInput = true;

	fRecorder = new BMediaRecorder("R QR Reader", B_MEDIA_RAW_VIDEO);
	if (fRecorder->InitCheck() != B_OK) {
		status = fRecorder->InitCheck();
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	fRecorder->SetHooks(frame_hook, NULL, this);

	// Whatever the camera offers, negotiated as a wildcard.
	//
	// Asking for 640x480 and falling back looked like a free improvement and
	// was not: the failed attempt still claimed the producer's output, so the
	// retry got "Bad source", the preview stayed black, and the camera was
	// left occupied until the media server was restarted.
	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;

	status = fRecorder->Connect(fInput, NULL, &format);
	if (status != B_OK) {
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	media_format got = fRecorder->AcceptedFormat();
	printf("camera: %" B_PRIu32 "x%" B_PRIu32 "\n",
		got.u.raw_video.display.line_width,
		got.u.raw_video.display.line_count);

	status = fRecorder->Start();
	if (status != B_OK) {
		fRecorder->Disconnect();
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	return B_OK;
}


void
ReaderWindow::_StopCamera()
{
	if (fRecorder == NULL)
		return;

	fRecorder->Stop();
	fRecorder->Disconnect();
	delete fRecorder;
	fRecorder = NULL;
}


void
ReaderWindow::FrameArrived(const void* data, size_t size,
	const media_format& format)
{
	int32 width = format.u.raw_video.display.line_width;
	int32 height = format.u.raw_video.display.line_count;
	int32 row = format.u.raw_video.display.bytes_per_row;
	if (row == 0)
		row = width * 4;

	if (width <= 0 || height <= 0 || (size_t)(row * height) > size)
		return;

	{
		BAutolock lock(fFrameLock);

		if (fFrame == NULL || fFrameWidth != width || fFrameHeight != height) {
			free(fFrame);
			fFrame = (uint8*)malloc(row * height);
			fFrameWidth = width;
			fFrameHeight = height;
			fFrameRow = row;
		}

		if (fFrame != NULL) {
			// Do not clobber a frame the scanner has not looked at yet: with
			// three frames a second, the one being dropped may well be the
			// only sharp one in the whole run.
			if (fFrameReady) {
				dropped++;
				return;
			}
			memcpy(fFrame, data, row * height);
			// The scanner takes the newest frame whenever it comes free; an
			// older one it did not get to is simply overwritten, which is
			// what keeps the media node from ever waiting on the decoder.
			fFrameReady = true;
		}
	}

	if (fPreview != NULL && LockWithTimeout(20000) == B_OK) {
		fPreview->SetFrameData((const uint8*)data, width, height, row);
		fPreview->Invalidate();
		Unlock();
	}
}


int32
ReaderWindow::_ScanEntry(void* data)
{
	((ReaderWindow*)data)->_ScanLoop();
	return 0;
}


void
ReaderWindow::_ScanLoop()
{
	uint8* copy = NULL;
	int32 capacity = 0;
	int32 bestCentres = 0;

	while (!fQuitScan) {
		int32 width = 0;
		int32 height = 0;
		int32 row = 0;
		bool have = false;

		{
			BAutolock lock(fFrameLock);
			if (fFrameReady && fFrame != NULL) {
				int32 needed = fFrameRow * fFrameHeight;
				if (capacity < needed) {
					free(copy);
					copy = (uint8*)malloc(needed);
					capacity = copy != NULL ? needed : 0;
				}
				if (copy != NULL) {
					memcpy(copy, fFrame, needed);
					width = fFrameWidth;
					height = fFrameHeight;
					row = fFrameRow;
					have = true;
				}
				fFrameReady = false;
			}
		}

		if (have) {

			bigtime_t began = system_time();
			BString text;
			bool found = qr_scan(copy, width, height, row, &text);

			// Debug only: keep the frame that got furthest, so a failure can
			// be looked at instead of guessed about.
			if (qr_verbose) {
				printf("frame %dx%d: %d centres, %lld ms%s\n", (int)width,
					(int)height, (int)qr_last_centres,
					(long long)((system_time() - began) / 1000),
					found ? " DECODED" : "");
				if (found || qr_last_centres >= bestCentres) {
					bestCentres = qr_last_centres;
					BBitmap shot(BRect(0, 0, width - 1, height - 1), B_RGB32);
					if (shot.InitCheck() == B_OK) {
						uint8* bits = (uint8*)shot.Bits();
						for (int32 line = 0; line < height; line++) {
							memcpy(bits + line * shot.BytesPerRow(),
								copy + line * row, width * 4);
						}
						BBitmapStream stream(&shot);
						BFile file("/boot/home/qrshot.png", B_WRITE_ONLY
							| B_CREATE_FILE | B_ERASE_FILE);
						BTranslatorRoster* roster
							= BTranslatorRoster::Default();
						if (roster != NULL && file.InitCheck() == B_OK) {
							roster->Translate(&stream, NULL, NULL, &file,
								B_PNG_FORMAT);
						}
						BBitmap* detach = NULL;
						stream.DetachBitmap(&detach);
					}
				}
			}

			if (found) {
				BMessage found(kMsgFound);
				found.AddString("text", text);
				BMessenger(this).SendMessage(&found);
			}
		}

		// Only idle when there was nothing to do. Sleeping after a scan as
		// well threw away frames: the camera produces about three a second,
		// and a fixed pause after each one meant the next good frame arrived
		// while this thread was asleep and was overwritten before it looked.
		// At three frames a second there is nothing to be gained by skipping
		// any of them.
		if (!have)
			snooze(40000);
	}

	free(copy);
}


void
ReaderWindow::_Report(const BString& text)
{
	// Scanning never stops, so the same code keeps arriving for as long as it
	// is held up to the camera. Only a *different* result is worth
	// interrupting for; without this the dialog would reopen several times a
	// second on the same code.
	if (text == fText)
		return;

	fText = text;
	fResult->SetText(text.String());
	// Just the contents. A label in front of it only pushes the text along
	// and the panel is already obviously where the result goes.
	fStatus->SetText(text.String());

	fCopyButton->SetEnabled(true);

	// Both kinds of content get a dialog. A code that turns out to hold a
	// message rather than a link is just as much a result, and leaving it to
	// appear silently in the panel means it can be missed entirely -- the
	// camera is what the user is looking at, not the window.
	if (looks_like_url(text)) {
		BString question(T(kStrUrlQuestion));
		question << "\n\n" << text;

		BAlert* alert = new BAlert(T(kStrTitle), question.String(),
			T(kStrCancel), T(kStrOpen), NULL, B_WIDTH_AS_USUAL,
			B_INFO_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() == 1)
			PostMessage(kMsgOpenUrl);
	} else {
		BString message(T(kStrFound));
		message << "\n\n" << text;

		BAlert* alert = new BAlert(T(kStrTitle), message.String(),
			T(kStrOk), T(kStrCopy), NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() == 1)
			PostMessage(kMsgCopy);
	}
}


void
ReaderWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFound:
		{
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK && text != NULL)
				_Report(text);
			break;
		}

		case kMsgCopy:
		{
			if (fText.Length() == 0)
				break;
			if (be_clipboard->Lock()) {
				be_clipboard->Clear();
				BMessage* clip = be_clipboard->Data();
				if (clip != NULL) {
					clip->AddData("text/plain", B_MIME_TYPE, fText.String(),
						fText.Length());
					be_clipboard->Commit();
				}
				be_clipboard->Unlock();
				fStatus->SetText(T(kStrCopied));
			}
			break;
		}

		case kMsgOpenUrl:
		{
			if (!looks_like_url(fText))
				break;

			const char* argv = fText.String();

			// Ask for whatever handles the scheme first, so the user's chosen
			// browser wins; Haiku registers these as ordinary MIME types.
			BString scheme("application/x-vnd.Be.URL.");
			scheme << (fText.IFindFirst("https://") == 0 ? "https" : "http");

			status_t status = be_roster->Launch(scheme.String(), 1,
				(char**)&argv);

			if (status != B_OK && status != B_ALREADY_RUNNING)
				status = be_roster->Launch(kWebSignature, 1, (char**)&argv);

			if (status != B_OK && status != B_ALREADY_RUNNING) {
				BString message(T(kStrCannotOpen));
				message << strerror(status);
				BAlert* alert = new BAlert(T(kStrTitle), message.String(),
					T(kStrOk));
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go(NULL);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
ReaderWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


// #pragma mark -


static void
choose_language()
{
	BMessage preferred;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&preferred) != B_OK)
		return;

	const char* language = NULL;
	for (int32 i = 0;
			preferred.FindString("language", i, &language) == B_OK; i++) {
		if (language != NULL && strncmp(language, "ko", 2) == 0) {
			sStrings = kStringsKo;
			return;
		}
	}
}


int
main(void)
{
	BApplication app(kAppSignature);

	// Set RQR_DEBUG in the environment to have the pipeline report what it
	// finds in each frame; without real frames to look at, tuning the
	// detector is guesswork.
	if (getenv("RQR_DEBUG") != NULL) {
		qr_verbose = true;
		// Redirected to a file, stdout is block buffered and a run that never
		// exits never flushes -- the log comes out empty.
		setvbuf(stdout, NULL, _IOLBF, 0);
	}

	choose_language();

	ReaderWindow* window = new ReaderWindow();
	window->Show();
	app.Run();
	return 0;
}
