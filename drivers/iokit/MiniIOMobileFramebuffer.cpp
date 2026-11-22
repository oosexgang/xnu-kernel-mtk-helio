/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of the system image 20F66. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSString.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <machine/machine_routines.h>
#include <kern/cpu_number.h>

/*
 * The clock of the processor this runs on, in MHz: a chain of dependent adds, one cycle each
 * on the A55 and the A75 alike, against the generic timer, interrupts off. (The PMU's cycle
 * counter is no help, and CNTFRQ_EL0 reads 0 on this SoC, so the time is XNU's own
 * mach_absolute_time, which it calibrated from the device tree.) Nothing under XNU sets
 * the MT6768's frequencies, so the cores run at whatever LK left them at -- which this tells.
 */
static uint32_t
hakios_core_mhz(int *cpu)
{
	uint64_t v0, v1, ns, x = 0;
	const uint64_t rounds = 20000;          /* of 50 adds: a million cycles */
	boolean_t on = ml_set_interrupts_enabled(FALSE);

	*cpu = cpu_number();
	v0 = mach_absolute_time();
	for (uint64_t i = 0; i < rounds; i++) {
		__asm__ volatile (
		    ".rept 50\n\tadd %0, %0, #1\n\t.endr" : "+r"(x));
	}
	v1 = mach_absolute_time();
	ml_set_interrupts_enabled(on);
	absolutetime_to_nanoseconds(v1 - v0, &ns);
	return ns ? (uint32_t)(rounds * 50 * 1000 / ns) : 0;
}

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void hakios_keep(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (the system image 20F66) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP26_SIZE             0x588                   /* the 26 and 27 releases' request ... */
#define SWAP26_SHIFT            0x48                    /* ... with these fields that much further on */
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the reference phone
 * (see the loader), and the system knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the reference phone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/*
 * Boot-arg fbxr=1 (with fbscale=2): the screen of the reference phone's larger sibling (the XR) instead, 414 x 896 points at two pixels a
 * point. The system has a status bar for that one; for the reference phone's 375 points at two it has none and puts
 * up a red bar that says so ("Please see rdar://45025538"). It is also more room: 414 points across
 * instead of 375, so less of everything is too large on a big panel.
 */
static TUNABLE(uint32_t, gFBXR, "fbxr", 0);
#define kFBXRPointsWide         414
#define kFBXRPointsHigh         896
#define kFBXRSubtype            1792
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);
/*
 * Boot-arg fbfit=1: keep the reference phone's proportions on a taller panel, black bands above and below.
 * fbfit=2: keep them by filling the panel's height instead, and cutting the sides: no bands, and
 * 18 pixels lost at each edge of the first phone's 720.
 */
static TUNABLE(uint32_t, gFBFit, "fbfit", 0);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/log/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had the system killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gShot, "fbshot", 0);      /* a small picture of the frame in the log, every minute from this many seconds of frames on */
static TUNABLE(uint32_t, gShotWidth, "fbshot_w", 0);      /* the picture's width (60 when not given) */
static TUNABLE(uint32_t, gShotBand, "fbshot_band", 0);    /* 1, 2 or 3: only that third of the height (0: all of it; 4: each third in turn) */
static TUNABLE(uint32_t, gShotEvery, "fbshot_every", 60);  /* seconds between pictures (a multiple of 5) */
static TUNABLE(uint32_t, gShotPanel, "fbshot_panel", 0);
/* fbshot_keep=SECONDS: one picture, 60 pixels wide, at the first frame after that much uptime, into the host's kept
 * lines as well as the log -- the log keeps only its last few hundred kilobytes, and runs 93 and 94 lost theirs */
static TUNABLE(uint32_t, gShotKeep, "fbshot_keep", 0);
/* fbshot_cx/cy/cw/ch: the fbshot_keep picture of that rectangle of the panel only (panel pixels), 60 wide */
static TUNABLE(uint32_t, gShotCX, "fbshot_cx", 0);
static TUNABLE(uint32_t, gShotCY, "fbshot_cy", 0);
static TUNABLE(uint32_t, gShotCW, "fbshot_cw", 0);
static TUNABLE(uint32_t, gShotCH, "fbshot_ch", 0);
/* boot-arg fbtext=1: the Metal driver's last lines (mtk_mali.c, hakios_overlay_copy) drawn over the top of every frame */
static TUNABLE(uint32_t, gFBText, "fbtext", 0);
extern "C" unsigned hakios_overlay_copy(char out[][96], unsigned max);
extern "C" void hakios_host_keep_add(const char *format, ...) __printflike(1, 2);
extern "C" unsigned char iso_font[256 * 16];    /* ... of a picture put on the panel from inside the kernel (MiniIOMobileFramebufferShow) */
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_flicker=1: every frame a picture 40 pixels wide is kept, and a part of the screen that
 * is one way in a frame, another in the next and the first way again in the one after is counted as
 * flicker (in the five-second lines); once a minute at most such a frame, the odd one and the one
 * before it, is put in the log as fbshot pictures, for a run nobody watches.
 */
static TUNABLE(uint32_t, gFlicker, "fb_flicker", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * the reference phone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Three since 26 October: a swap comes in as soon as its command buffer is committed, before the GPU
 * has begun it, and waits here some 8 ms for the GPU and then for its beat -- 20 ms a frame. With two
 * places CoreAnimation's next swap found them both taken and waited (46 of 125 swaps, run 149), and the
 * frames came every other refresh; with three, one frame's GPU work overlaps the one before's beat.
 */
#define SWAP_QUEUE              3                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 3);
/*
 * Boot-arg fb_inuse=0: a swap's surfaces are not counted in use while they wait for the panel. By
 * default they are, as on a reference phone: CoreAnimation took a surface still waiting here for the next
 * frame and drew it over, and the panel showed a frame from before or after its turn -- part of the
 * screen flickering in every page swipe (fb_flicker=1, 24 October).
 */
static TUNABLE(uint32_t, gSwapInUse, "fb_inuse", 1);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/log/latency.py; the touch's own lines: bsd/dev/hakios_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	/* boot-arg fb_flip=1: two of the boot framebuffer's pictures, drawn in turn, the panel's
	 * scanout (OVL0 layer 0) pointed at the finished one -- see flipStart() */
	bool            fFlip;
	unsigned        fFront;                 /* which of the two the panel shows */
	uint8_t *       fFlipVA[2];             /* each, where the frame drawn into it starts */
	uint32_t        fFlipPA[2];             /* each, the address OVL0 is given */
	volatile uint32_t * fOvl;
	void flipStart(void);
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fFillWidth, fCropX;         /* fbfit=2: the picture's width on the panel, and how much of it is cut off at each side */
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void shot(MiniIOSurface * s);
	void keepShot(void);
	void frameShot(uint32_t when);
	bool grab(uint8_t * out, uint32_t max, uint32_t * width, uint32_t * height, uint32_t * stride);
	uint32_t        fShotStats;
	bool            fShotKept;      /* the fbshot_keep picture taken */
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	/*
	 * the 26 release takes the modes the way a real phone's display driver lays them out (an M1's AppleCLCD2,
	 * read with ioreg, is the model here): besides the attributes, each element carries its binary
	 * description (ElementData), and the timing says it is the preferred one. With only the attributes
	 * the 16.5 release was content; the 26 release found no preferred mode ("preferred mode (0 x 0)", current mode
	 * 16383 x 16383) and never drew. The porches are made up, the pixel clock follows from them.
	 */
	const uint32_t hz = refreshHz();
	const uint32_t hSync = 32, hBack = 42, hFront = 8, vSync = 8, vBack = 42, vFront = 32;
	const uint32_t hTotal = fWidth + hSync + hBack + hFront, vTotal = fHeight + vSync + vBack + vFront;
	const uint64_t pixelClock = (uint64_t) hTotal * vTotal * hz;
	const uint32_t lineRate = (uint32_t) (((pixelClock << 16) / hTotal) / 1000);     /* kHz, 16.16 */
	const uint32_t frameRate = hz << 16;                                            /* Hz, 16.16 */
	uint32_t timingData[20] = {
		0, hTotal, fWidth, hSync, hBack, hFront, lineRate, 1, 0,
		vTotal, fHeight, vSync, vBack, vFront, frameRate, 1, 0,
		(uint32_t) pixelClock, 13, 0,
	};
	uint8_t colorData[32] = { 8 };
	OSDictionary * timing = OSDictionary::withCapacity(24);
	OSDictionary * horizontal = OSDictionary::withCapacity(9);
	OSDictionary * vertical = OSDictionary::withCapacity(9);
	OSDictionary * color = OSDictionary::withCapacity(12);
	OSDictionary * refresh = OSDictionary::withCapacity(6);
	OSDictionary * attributes = OSDictionary::withCapacity(1);
	OSDictionary * product = OSDictionary::withCapacity(1);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	OSArray * none = OSArray::withCapacity(1);
	OSData * data;
	uint64_t frameTicks;

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || refresh == NULL ||
	    attributes == NULL || product == NULL || timings == NULL || colors == NULL || none == NULL) {
		return;
	}
	colorData[12] = 16;                                     /* colorimetry */
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "ElementType", 1);
	setNumber(color, "StandardType", 0);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 16);
	setNumber(color, "EOTF", 0);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "SupportsDSC", 0);
	setNumber(color, "Score", 240999);
	color->setObject("IsVirtual", kOSBooleanFalse);
	if ((data = OSData::withBytes(colorData, sizeof(colorData))) != NULL) {
		color->setObject("ElementData", data);
		data->release();
	}
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", hTotal);
	setNumber(horizontal, "SyncWidth", hSync);
	setNumber(horizontal, "BackPorch", hBack);
	setNumber(horizontal, "FrontPorch", hFront);
	setNumber(horizontal, "SyncPolarity", 1);
	setNumber(horizontal, "PixelRepetition", 0);
	setNumber(horizontal, "SyncRate", lineRate);
	setNumber(horizontal, "PreciseSyncRate", lineRate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", vTotal);
	setNumber(vertical, "SyncWidth", vSync);
	setNumber(vertical, "BackPorch", vBack);
	setNumber(vertical, "FrontPorch", vFront);
	setNumber(vertical, "SyncPolarity", 1);
	setNumber(vertical, "PixelRepetition", 0);
	setNumber(vertical, "SyncRate", frameRate);
	setNumber(vertical, "PreciseSyncRate", frameRate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "ElementType", 0);
	setNumber(timing, "StandardType", 0);
	setNumber(timing, "TimingType", 3);
	setNumber(timing, "ScanInformation", 0);
	setNumber(timing, "Score", 16346);
	setNumber(timing, "AspectRatio", 13);
	setNumber(timing, "PreciseAspectRatio", (uint64_t) fWidth * 65536 / fHeight);
	setNumber(timing, "ValidPixelEncodings", 1);
	setNumber(timing, "MinimumVariableRefreshRate", 0);
	setNumber(timing, "MaximumVariableRefreshRate", 0);
	timing->setObject("IsPreferred", kOSBooleanTrue);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("IsInterlaced", kOSBooleanFalse);
	timing->setObject("IsSplit", kOSBooleanFalse);
	timing->setObject("IsPromoted", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timing->setObject("DiscreteMediaRefreshRates", none);
	timing->setObject("UnsafeColorElementIDs", none);
	timing->setObject("DSCRequiredColorElementIDs", none);
	if ((data = OSData::withBytes(timingData, sizeof(timingData))) != NULL) {
		timing->setObject("ElementData", data);
		data->release();
	}
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	setProperty("DPTimingModeId", kFBTimingID, 32);
	setProperty("DisplayWidth", fWidth, 32);
	setProperty("DisplayHeight", fHeight, 32);
	setProperty("PixelClock", pixelClock, 64);
	attributes->setObject("ProductAttributes", product);
	setProperty("DisplayAttributes", attributes);
	nanoseconds_to_absolutetime(NSEC_PER_SEC / hz, &frameTicks);
	setNumber(refresh, "displayMinRefreshInterval", (1ULL << 32) / hz);
	setNumber(refresh, "displayMaxRefreshInterval", (1ULL << 32) / hz);
	setNumber(refresh, "displayMinRefreshIntervalMachTime", frameTicks);
	setNumber(refresh, "displayMaxRefreshIntervalMachTime", frameTicks);
	setNumber(refresh, "displayRefreshStep", 0);
	setNumber(refresh, "displayRefreshStepMachTime", 0);
	setProperty("IOMFBDisplayRefresh", refresh);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(none);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
	OSSafeReleaseNULL(refresh);
	OSSafeReleaseNULL(attributes);
	OSSafeReleaseNULL(product);
}

/*
 * Boot-arg fb_flip=1: no frame copied into the picture the panel is scanning out. The panel is the
 * framebuffer LK left, which the display engine reads continuously (OVL0 layer 0, video mode); a
 * frame copied into it while it is read shows on the panel as part this frame, part the last -- in
 * bands, the copy being split between fb_threads processors -- the strips of the slow-motion video
 * of 27 October. LK's framebuffer holds more than one picture (the board file's [display]: three and a half
 * on the first phone): the second one is drawn while the first is shown, and then OVL0 is pointed at
 * it. Only when OVL0 reads where the console's framebuffer is and the second picture fits.
 */
extern "C" {
#include "../../bsd/dev/hakios_board.h"
}
#define FLIP_OVL0               (hakios_board()->blocks.disp_ovl0)
#define FLIP_OVL_L0_ADDR        0xf40
#define FLIP_LK_FB_BASE         (hakios_board()->display.fb_base)      /* the board file's [display] */
#define FLIP_LK_FB_SIZE         (hakios_board()->display.fb_size)
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
static TUNABLE(uint32_t, gFlipOn, "fb_flip", 0);

void
IOMobileFramebuffer::flipStart(void)
{
	uint64_t picture = (uint64_t) fRowBytes * vinfo.v_height, offset = (uint64_t) (fBase - (uint8_t *) vinfo.v_baseaddr);
	uint32_t scanned;
	vm_offset_t second;

	if (!gFlipOn || !hakios_board_wants(HAKIOS_HAS_DISPLAY_OVL_FLIP, "fb_flip=1")) {
		return;
	}
	fOvl = (volatile uint32_t *) ml_io_map(FLIP_OVL0, PAGE_SIZE);
	scanned = fOvl != NULL ? fOvl[FLIP_OVL_L0_ADDR / 4] : 0;
	picture = (picture + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);
	if (scanned != (uint32_t) vinfo.v_physaddr || scanned < FLIP_LK_FB_BASE ||
	    scanned + 2 * picture > FLIP_LK_FB_BASE + FLIP_LK_FB_SIZE) {
		IOLog("MiniIOMobileFramebuffer: fb_flip: OVL0 reads %#x, the console's framebuffer is at %#llx: "
		    "not flipping\n", scanned, (unsigned long long) vinfo.v_physaddr);
		return;
	}
	second = ml_io_map_wcomb(scanned + picture, (vm_size_t) picture);
	if (second == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_flip: the second picture could not be mapped\n");
		return;
	}
	memcpy((void *) second, (const void *) vinfo.v_baseaddr, (size_t) picture);
	fFlipVA[0] = fBase;
	fFlipVA[1] = (uint8_t *) second + offset;
	fFlipPA[0] = scanned;
	fFlipPA[1] = (uint32_t) (scanned + picture);
	fFront = 0;
	fFlip = true;
	hakios_keep("MiniIOMobileFramebuffer: fb_flip: two pictures, at %#x and %#x", fFlipPA[0], fFlipPA[1]);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	/*
	 * A panel taller for its width tha reference phone's screen gets the picture at the reference phone's
	 * proportions, centred, with black above and below -- scaled to the panel's whole height as
	 * well it came out stretched (the first phone's 720 x 1640 is 2.28 tall to the reference phone's 2.17).
	 * Its touch screen driver maps fingers into the same band. Only with boot-arg fbfit=1: the
	 * other phone's touch driver knows nothing of the bands.
	 */
	fFillWidth = fPanelWidth;
	fCropX = 0;
	if (gFBFit == 2 && fWidth != 0 && fHeight != 0 && (uint64_t) fPanelHeight * fWidth > (uint64_t) fHeight * fPanelWidth + fWidth) {
		fFillWidth = (uint32_t) ((uint64_t) fPanelHeight * fWidth / fHeight);
		fCropX = (fFillWidth - fPanelWidth) / 2;
		IOLog("MiniIOMobileFramebuffer: the picture fills the panel's height, %u wide with %u cut at each side\n",
		    fFillWidth, fCropX);
	} else if (gFBFit == 1 && fPanelWidth != 0 && fWidth != 0 && (uint64_t) fPanelHeight * fWidth > (uint64_t) fHeight * fPanelWidth + fWidth) {
		uint32_t fit = (uint32_t) ((uint64_t) fPanelWidth * fHeight / fWidth);

		fBase += (size_t) ((fPanelHeight - fit) / 2) * fRowBytes;
		IOLog("MiniIOMobileFramebuffer: the picture keeps its proportions: %u of the panel's %u rows, from row %u\n",
		    fit, fPanelHeight, (fPanelHeight - fit) / 2);
		fPanelHeight = fit;
	}
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = (gFBXR ? kFBXRPointsWide : kFBPointsWide) * scale;
		fHeight = (gFBXR ? kFBXRPointsHigh : kFBPointsHigh) * scale;
		if (gFBXR) {
			IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
			uint32_t subtype = kFBXRSubtype;

			if (product != NULL) {
				product->setProperty("artwork-device-subtype", &subtype, sizeof(subtype));
				product->release();
			}
		}
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	/*
	 * fbfit=3: the picture one pixel to a pixel of the panel, nothing scaled -- with fbscale=2 the
	 * the reference phone's 750 x 1624 on the first phone's 720 x 1640 is 15 columns cut at each side and 8 rows
	 * of black above and below. A frame onto the panel is then a copy of its rows: scaling it
	 * (bilinear) kept three of the four processors busy for 7 ms of every frame, the window
	 * server and SpringBoard waiting for them (24 October). The touch screen driver
	 * (bsd/dev/mtk_tcm.c) places fingers the same way.
	 */
	if (gFBFit == 3 && fWidth != 0 && fHeight != 0 && fHeight <= fPanelHeight) {
		fFillWidth = fWidth;
		fCropX = fWidth > fPanelWidth ? (fWidth - fPanelWidth) / 2 : 0;
		fBase += (size_t) ((fPanelHeight - fHeight) / 2) * fRowBytes;
		fPanelHeight = fHeight;
		IOLog("MiniIOMobileFramebuffer: the picture one to one: %u of its %u columns, the panel's rows from %u\n",
		    fWidth - 2 * fCropX, fWidth, (vinfo.v_height - fHeight) / 2);
	}
	flipStart();
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + cx0, pixels + (size_t) (sy + y - py0) * stride + (size_t) (sx + cx0 - px0) * 4,
			    (size_t) (cx1 - cx0) * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fFillWidth / fWidth) - (int32_t) fCropX;
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fFillWidth / fWidth) - (int32_t) fCropX;
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && cx1 > cx0;     /* (cut at the sides: fbfit=3) */
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	/*
	 * And the whole run's so far, once a minute as a kept line: the five-second lines of a run
	 * somebody used by hand are gone from the log by its end (run 143, 26 October). Counted only
	 * from reports of 50 frames or more, which is something moving rather than the clock.
	 */
	{
		static uint32_t run[5], reports;

		if (total >= 50) {
			for (uint32_t i = 0; i < 5; i++) {
				run[i] += within[i];
			}
		}
		if (++reports % 12 == 0) {
			uint32_t all = run[0] + run[1] + run[2] + run[3] + run[4];

			hakios_keep("MiniIOMobileFramebuffer: busy frames so far: %u, %u%% within 18 ms; %u under 26, %u under 35, %u longer",
			    all, all ? (run[0] + run[1]) * 100 / all : 0, run[2], run[3], run[4]);
		}
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/*
 * Boot-arg fbshot=SECONDS: the frame, 60 pixels wide, red green blue, in base64 on lines that begin
 * "fbshot N:" (tools/log/fbshot.py makes a picture of them): what the screen shows, for a run nobody
 * watches. Every minute from SECONDS of frames on, so that the end of the log has one.
 */
static bool shot_keep;          /* (set around one shot_pixels: its lines go to the host's kept lines too) */

static void
shot_pixels(const uint8_t * base, uint32_t full_width, uint32_t full_height, uint32_t row_bytes, uint32_t across,
    uint32_t band, uint32_t bands)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static uint32_t shots;
	uint32_t step = full_width / across, width, height;
	uint8_t chunk[150];
	char line[204];
	unsigned used = 0;

	if (step == 0) {
		return;
	}
	width = full_width / step;
	height = full_height / step;
	/* one band of `bands` across the height: a wider picture than the log's end can keep whole, a third at a time */
	uint32_t first = bands > 1 ? height * band / bands : 0, last = bands > 1 ? height * (band + 1) / bands : height;
	shots++;
	IOLog("fbshot %u: begin %u %u (rows %u to %u of %u)\n", shots, width, last - first, first, last, height);
	if (shot_keep) {
		hakios_host_keep_add("fbshot %u: begin %u %u (rows %u to %u of %u)", shots, width, last - first, first, last, height);
	}
	for (uint32_t y = first; y < last; y++) {
		const uint32_t * row = (const uint32_t *) (base + (size_t) (y * step + step / 2) * row_bytes);

		for (uint32_t x = 0; x < width; x++) {
			uint32_t pixel = row[x * step + step / 2];      /* B G R A in memory */

			chunk[used++] = (pixel >> 16) & 0xff;
			chunk[used++] = (pixel >> 8) & 0xff;
			chunk[used++] = pixel & 0xff;
			/* (a kept line is cut at about 200 characters: 96 bytes, 128 of them, for those) */
			if (used == (shot_keep ? 96u : sizeof(chunk)) || (y == last - 1 && x == width - 1)) {
				unsigned n = 0;

				for (unsigned i = 0; i < used; i += 3) {
					uint32_t v = (uint32_t) chunk[i] << 16 | (i + 1 < used ? (uint32_t) chunk[i + 1] << 8 : 0) |
					    (i + 2 < used ? chunk[i + 2] : 0);

					line[n++] = digits[v >> 18 & 63];
					line[n++] = digits[v >> 12 & 63];
					line[n++] = digits[v >> 6 & 63];
					line[n++] = digits[v & 63];
				}
				line[n] = 0;
				IOLog("fbshot %u: %s\n", shots, line);
				if (shot_keep) {
					hakios_host_keep_add("fbshot %u: %s", shots, line);
				}
				used = 0;
			}
		}
	}
	IOLog("fbshot %u: end\n", shots);
	if (shot_keep) {
		hakios_host_keep_add("fbshot %u: end", shots);
	}
}

#define FLICKER_W       40
#define FLICKER_H       200
static uint32_t flickerPics[3][FLICKER_W * FLICKER_H];  /* (the presenter's thread only) */
static uint32_t flickerNext, flickerKept, flickerFrames, flickerPixels;
static uint64_t flickerShot;

static inline uint32_t
pixel_apart(uint32_t a, uint32_t b)
{
	uint32_t d = 0;

	for (unsigned shift = 0; shift < 24; shift += 8) {
		int32_t x = (int32_t) (a >> shift & 0xff) - (int32_t) (b >> shift & 0xff);

		d += x < 0 ? -x : x;
	}
	return d;
}

static void
flicker(MiniIOSurface * s)
{
	uint32_t step = s->info.width / FLICKER_W, height, odd = 0, left = ~0u, top = ~0u, right = 0, bottom = 0;

	if (step == 0) {
		return;
	}
	height = s->info.height / step;
	height = height < FLICKER_H ? height : FLICKER_H;
	uint32_t * now = flickerPics[flickerNext % 3], * was = flickerPics[(flickerNext + 2) % 3],
	    * before = flickerPics[(flickerNext + 1) % 3];

	for (uint32_t y = 0; y < height; y++) {
		const uint32_t * row = (const uint32_t *) (s->pixels() + (size_t) (y * step + step / 2) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < FLICKER_W; x++) {
			now[y * FLICKER_W + x] = row[x * step + step / 2];
		}
	}
	flickerNext++;
	if (flickerKept < 3) {
		flickerKept++;
		return;
	}
	/* `was` the odd one out: unlike the frame before it and the one after, and those two alike */
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < FLICKER_W; x++) {
			uint32_t i = y * FLICKER_W + x;

			if (pixel_apart(was[i], before[i]) > 90 && pixel_apart(was[i], now[i]) > 90 &&
			    pixel_apart(before[i], now[i]) < 30) {
				odd++;
				left = x < left ? x : left;
				right = x > right ? x : right;
				top = y < top ? y : top;
				bottom = y > bottom ? y : bottom;
			}
		}
	}
	if (odd < 4) {
		return;
	}
	flickerFrames++;
	flickerPixels += odd;
	/* a pair of pictures a minute at most: the log keeps only its last 256 KB */
	uint64_t minute;

	nanoseconds_to_absolutetime(60 * NSEC_PER_SEC, &minute);
	if (flickerShot == 0 || mach_absolute_time() - flickerShot > minute) {
		flickerShot = mach_absolute_time();
		IOLog("MiniIOMobileFramebuffer: flicker: %u pixels of %u in a frame unlike the one before and after, within %u,%u to "
		    "%u,%u; the frame before, then the odd one:\n", odd, FLICKER_W * height, left, top, right, bottom);
		shot_pixels((const uint8_t *) before, FLICKER_W, height, FLICKER_W * 4, FLICKER_W, 0, 1);
		shot_pixels((const uint8_t *) was, FLICKER_W, height, FLICKER_W * 4, FLICKER_W, 0, 1);
	}
}

/*
 * What the GPU drew into a surface, as memory holds it, for the processor to read: its lines of the surface are
 * cleaned and invalidated first. Shown in place (fb_flip=1) a frame is never copied, so nothing else does it, and a read
 * through the kernel's cached mapping gave lines the processor had read frames before -- fbshot's pictures were of an
 * old frame (run 70's had none of the magenta on the panel, run 77's was black). The surface's lines are clean (the
 * processor does not write a frame, and its zeroes were written back when it was made), so nothing old is written back.
 */
static void
surface_from_memory(MiniIOSurface * s)
{
	const uint8_t * from = s->pixels();
	size_t bytes = (size_t) s->info.bytesPerRow * s->info.height;

	for (uintptr_t line = (uintptr_t) from & ~(uintptr_t) 63; line < (uintptr_t) from + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
IOMobileFramebuffer::shot(MiniIOSurface * s)
{
	static uint32_t taken;

	surface_from_memory(s);
	shot_pixels(s->pixels(), s->info.width, s->info.height, s->info.bytesPerRow, gShotWidth ? gShotWidth : 60,
	    gShotBand == 4 ? taken++ % 3 : gShotBand ? gShotBand - 1 : 0, gShotBand ? 3 : 1);
}

/*
 * The picture on the panel, whole, for bsd/dev/hakios_fbgrab.c (sysctl kern.hakios.fbgrab): what is in the front buffer now, B G R A in
 * memory, a row of fRowBytes. fbshot's pictures go through the log and are 60 to 130 pixels wide; this one goes through a program that sends
 * it over the network (src/fbgrab.m).
 */
static IOMobileFramebuffer * gLiveFB;

bool
IOMobileFramebuffer::grab(uint8_t * out, uint32_t max, uint32_t * width, uint32_t * height, uint32_t * stride)
{
	const uint8_t * from = fFlip ? fFlipVA[fFront] : fBase;
	uint32_t w = fPanelWidth, h = fPanelHeight;

	if (from == NULL || w == 0 || h == 0 || (uint64_t) w * h * 4 > max) {
		return false;
	}
	IOLockLock(fLock);
	for (uint32_t y = 0; y < h; y++) {
		memcpy(out + (size_t) y * w * 4, from + (size_t) y * fRowBytes, (size_t) w * 4);
	}
	IOLockUnlock(fLock);
	*width = w;
	*height = h;
	*stride = w * 4;
	return true;
}

/*
 * Boot-arg fbshot_keep=SECONDS: the picture on the panel at that much uptime, 60 pixels wide, into the host's kept lines
 * as well as the log. On a timer and not at a frame: a home screen with nothing moving presents no frames at all, and
 * run 95's went still at 198 s, before its picture was due.
 */
void
IOMobileFramebuffer::keepShot(void)
{
	const uint8_t * from = fFlip ? fFlipVA[fFront] : fBase;

	if (from == NULL || fPanelWidth == 0 || fPanelHeight == 0) {
		hakios_host_keep_add("fbshot_keep: no picture on the panel to take");
		return;
	}
	uint32_t w = fPanelWidth, h = fPanelHeight;

	if (gShotCW != 0 && gShotCH != 0 && gShotCX + gShotCW <= fPanelWidth && gShotCY + gShotCH <= fPanelHeight) {
		from += (size_t) gShotCY * fRowBytes + (size_t) gShotCX * 4;
		w = gShotCW;
		h = gShotCH;
		hakios_host_keep_add("fbshot_keep: the rectangle %u,%u %ux%u of the panel's %ux%u", gShotCX, gShotCY, w, h,
		    fPanelWidth, fPanelHeight);
	}
	IOLockLock(fLock);
	shot_keep = true;
	/* fbshot_w=N: N pixels wide instead of 60 (a crop's detail: what an icon's name says, 16 November) */
	shot_pixels(from, w, h, fRowBytes, gShotWidth >= 16 && gShotWidth <= 240 ? gShotWidth : 60, 0, 1);
	shot_keep = false;
	IOLockUnlock(fLock);
}

static void
keepShotFired(thread_call_param_t fb, thread_call_param_t unused)
{
	(void) unused;
	((IOMobileFramebuffer *) fb)->keepShot();
}

/*
 * Boot-arg fbframes=SECONDS[,SECONDS...] (up to six, uptime): the picture on the panel at each of those times, every
 * fbframes_step-th pixel across and down (4 by default: 187 x 406 of the 750 x 1624 panel), as RGB565, into
 * hakios_host_frames (bsd/dev/mtk_mali.c), which mtkloader lays over ramoops' dump zone for the host
 * (tools/log/fbframes.py). A picture that does not fit what is left is not taken, and says so.
 */
extern "C" char hakios_host_frames[];
extern "C" uint32_t hakios_host_frames_len;
extern "C" const uint32_t hakios_host_frames_size;

static uint32_t gFrameTimes[6];
static unsigned gFrameCount = (unsigned) -1;
static uint32_t gFrameStep;

static void
frames_parse(void)
{
	char args[256] = "";
	const char *at;

	gFrameCount = 0;
	gFrameStep = 4;
	if (!PE_parse_boot_argn("fbframes", args, sizeof(args))) {
		return;
	}
	/* one number alone comes back as a binary integer (XNU's boot-args take "300" for a number, "300,320" for text) */
	if ((args[0] < '0' || args[0] > '9') && args[0] != 0) {
		uint32_t one;

		memcpy(&one, args, sizeof(one));
		if (one != 0) {
			gFrameTimes[gFrameCount++] = one;
		}
		args[0] = 0;
	}
	at = args;
	while (*at != 0 && gFrameCount < 6) {
		uint32_t v = 0;

		while (*at >= '0' && *at <= '9') {
			v = v * 10 + (uint32_t) (*at++ - '0');
		}
		if (v != 0) {
			gFrameTimes[gFrameCount++] = v;
		}
		if (*at != ',') {
			break;
		}
		at++;
	}
	uint32_t step = 0;

	if (PE_parse_boot_argn("fbframes_step", &step, sizeof(step)) && step >= 1 && step <= 16) {
		gFrameStep = step;
	}
}

void
IOMobileFramebuffer::frameShot(uint32_t when)
{
	const uint8_t * from = fFlip ? fFlipVA[fFront] : fBase;
	struct {
		char mark[4];
		uint16_t width, height;
		uint32_t uptime, bytes;
	} head = { { 'H', 'K', 'F', 'P' }, 0, 0, when, 0 };

	if (from == NULL || fPanelWidth == 0 || fPanelHeight == 0) {
		hakios_host_keep_add("fbframes: no picture on the panel at %u s", when);
		return;
	}
	head.width = (uint16_t) (fPanelWidth / gFrameStep);
	head.height = (uint16_t) (fPanelHeight / gFrameStep);
	head.bytes = (uint32_t) head.width * head.height * 2;
	IOLockLock(fLock);
	if (hakios_host_frames_len + sizeof(head) + head.bytes > hakios_host_frames_size) {
		IOLockUnlock(fLock);
		hakios_host_keep_add("fbframes: the picture at %u s (%ux%u) does not fit what is left (%u of %u bytes)", when,
		    head.width, head.height, hakios_host_frames_size - hakios_host_frames_len, hakios_host_frames_size);
		return;
	}
	uint8_t * out = (uint8_t *) hakios_host_frames + hakios_host_frames_len;

	memcpy(out, &head, sizeof(head));
	out += sizeof(head);
	for (uint32_t y = 0; y < head.height; y++) {
		const uint32_t * row = (const uint32_t *) (from + (size_t) (y * gFrameStep + gFrameStep / 2) * fRowBytes);

		for (uint32_t x = 0; x < head.width; x++) {
			uint32_t pixel = row[x * gFrameStep + gFrameStep / 2];  /* B G R A in memory */
			uint32_t r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
			uint16_t v = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));

			*out++ = (uint8_t) (v & 0xff);
			*out++ = (uint8_t) (v >> 8);
		}
	}
	hakios_host_frames_len += (uint32_t) sizeof(head) + head.bytes;
	IOLockUnlock(fLock);
	hakios_host_keep_add("fbframes: a picture at %u s, %ux%u RGB565, %u bytes kept of %u", when, head.width,
	    head.height, hakios_host_frames_len, hakios_host_frames_size);
}

static void
frameShotFired(thread_call_param_t fb, thread_call_param_t when)
{
	((IOMobileFramebuffer *) fb)->frameShot((uint32_t) (uintptr_t) when);
}

extern "C" bool
HakiosFramebufferGrab(uint8_t * out, uint32_t max, uint32_t * width, uint32_t * height, uint32_t * stride)
{
	return gLiveFB != NULL && gLiveFB->grab(out, max, width, height, stride);
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	gLiveFB = this;
	if (gFrameCount == (unsigned) -1) {
		frames_parse();
		for (unsigned i = 0; i < gFrameCount; i++) {
			thread_call_t fcall = thread_call_allocate_with_options(frameShotFired, this, THREAD_CALL_PRIORITY_KERNEL,
			    THREAD_CALL_OPTIONS_ONCE);
			uint64_t fwhen;

			nanoseconds_to_absolutetime((uint64_t) gFrameTimes[i] * NSEC_PER_SEC, &fwhen);
			if (fcall != NULL) {
				thread_call_enter1_delayed(fcall, (thread_call_param_t) (uintptr_t) gFrameTimes[i], fwhen);
			}
		}
	}
	if (gShotKeep != 0 && !fShotKept) {
		thread_call_t call = thread_call_allocate(keepShotFired, this);
		uint64_t when;

		fShotKept = true;
		nanoseconds_to_absolutetime((uint64_t) gShotKeep * NSEC_PER_SEC, &when);
		if (call != NULL) {
			thread_call_enter_delayed(call, when);
		}
	}

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	/* fbswaplog=S: every 20 s from S, how many swaps came in, were shown and were given up */
	{
		static int from = -2;
		static unsigned came, told;
		static uint64_t next_ns;
		uint64_t now_ns;

		if (from == -2 && !PE_parse_boot_argn("fbswaplog", &from, sizeof(from))) {
			from = -1;
		}
		came++;
		absolutetime_to_nanoseconds(now, &now_ns);
		if (from >= 0 && told < 20 && now_ns >= (uint64_t)from * NSEC_PER_SEC && now_ns >= next_ns) {
			told++;
			next_ns = now_ns + 20 * NSEC_PER_SEC;
			hakios_keep("fbswaplog: at %llu ms %u swaps came, %u shown, %u given up, %u waiting", now_ns / NSEC_PER_MSEC,
			    came, fSwapsShown, fStatLost, fSwapsWaiting);
		}
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
		if (gSwapInUse) {
			MiniIOSurfaceUse(fSwapSurfaces[slot][layer], 1);
		}
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				if (gSwapInUse) {
					MiniIOSurfaceUse(surfaces[layer], -1);
				}
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	if (fFlip) {
		fBase = fFlipVA[fFront ^ 1];    /* (the picture not on the panel) */
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				/*
				 * Plain aligned stores, not bzero: the panel's memory is mapped as device memory, and bzero's
				 * DC ZVA and unaligned stores fault there ("Unaligned kernel data abort" in
				 * IOMobileFramebuffer::show, the first time the 27 release's SpringBoard dropped the bottom layer;
				 * the Qualcomm port met it first).
				 */
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					volatile uint64_t *row = (volatile uint64_t *) (fBase + (size_t) y * fRowBytes);

					for (uint32_t x = 0; x < fPanelWidth / 2; x++) {
						row[x] = 0;
					}
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		/* every 400th swap's layers, whole into the host's lines: which of them make the picture once the home screen
		 * is up (the 27 release's showed its wallpaper's stripes and none of what the window server draws) */
		if (fSwapsShown % 400 == 200) {
			hakios_host_keep_add("swap %u layer %u: surface %u %ux%u '%c%c%c%c' from %d,%d %dx%d to %d,%d %dx%d, set 0x%x",
			    swapWord(swap, SWAP_ID), layer, sid, s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	/* a swap that shows more than its bottom layer, the first twenty, into the host's kept lines (15 November: the
	 * 27 release's icons are drawn into the shown surfaces once and wiped by their next frame -- are they meant to be
	 * a layer of their own?) */
	{
		static unsigned said;

		if ((layers & ~1u) != 0 && said < 20) {
			said++;
			hakios_host_keep_add("swap %u layers 0x%x: surfaces %u %u %u %u",
			    swapWord(swap, SWAP_ID), layers, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
			    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)));
		}
	}
	if (gFBText) {
		/*
		 * The driver's lines, white on black, 8 x 16 (the console's own font, iso_font: its lowest bit the
		 * leftmost pixel), over the top rows of the panel's picture. Word stores only: the panel's memory is
		 * device memory (see the clearing above).
		 */
		char lines[8][96];
		unsigned count = hakios_overlay_copy(lines, 8);
		uint32_t columns = fPanelWidth / 8;

		for (unsigned l = 0; l < count; l++) {
			for (uint32_t r = 0; r < 16 && (l * 16 + r) < fPanelHeight; r++) {
				volatile uint32_t *row = (volatile uint32_t *) (fBase + (size_t) (l * 16 + r) * fRowBytes);
				bool ended = false;

				for (uint32_t c = 0; c < columns; c++) {
					unsigned char ch = ended ? ' ' : (unsigned char) lines[l][c];
					unsigned char bits;

					if (ch == 0) {
						ended = true;
						ch = ' ';
					}
					bits = ch < 128 ? iso_font[ch * 16 + r] : 0;
					for (uint32_t b = 0; b < 8; b++) {
						row[c * 8 + b] = (bits >> b) & 1 ? 0xffffffffu : 0xff000000u;
					}
				}
			}
		}
	}
	if (fFlip) {
		fFront ^= 1;
		fOvl[FLIP_OVL_L0_ADDR / 4] = fFlipPA[fFront];
		__builtin_arm_dsb(0xf);
	}
	drawn = mach_absolute_time();
	/*
	 * Its surfaces are on the panel now, and only now may the swap after it be handed over. Until
	 * 27 October this was done before the copy: the window server, which draws in turn into just
	 * two surfaces, began its next frame in the one being copied, and the panel showed part of one
	 * frame and part of the next -- 768 frames begun in a surface the display still held in run
	 * 228 (hakiosmetal.m counts them), the "breakcore" of every video of the Metal port.
	 */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	if (recorded != NULL) {
		record(recorded);
	}
	if (recorded != NULL && gFlicker) {
		flicker(recorded);
	}
	fSwapsShown++;
	/* boot-arg fbswaplog=S: from S seconds of uptime, the surface each swap put on the panel, a line for each change
	 * of it (60 at most) -- which of the window server's surfaces the panel shows (16 November) */
	{
		static int from = -2;
		static unsigned said, last_surface;
		static uint64_t repeats;

		if (from == -2 && !PE_parse_boot_argn("fbswaplog", &from, sizeof(from))) {
			from = -1;
		}
		if (from >= 0 && said < 60) {
			uint64_t now_ns;

			absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
			unsigned surface = swapWord(swap, SWAP_SURFACE(0));

			if (now_ns >= (uint64_t)from * NSEC_PER_SEC && surface != last_surface) {
				said++;
				hakios_keep("fbswaplog: swap #%u at %llu ms: surface %u (after %llu swaps of %u)", fSwapsShown,
				    now_ns / NSEC_PER_MSEC, surface, repeats, last_surface);
				last_surface = surface;
				repeats = 0;
			} else {
				repeats++;
			}
		}
	}
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		static unsigned kept_layers;

		if (kept_layers++ < 40) {
			hakios_keep("fbswaplog: swap #%u: layers 0x%x now (0x%x before): %u %u %u %u", fSwapsShown, layers,
			    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
			    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)));
		}
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		/* the first few reports only: the measurement holds interrupts off for a millisecond */
		static unsigned measured;

		if (measured < 8) {
			int core;
			uint32_t mhz = hakios_core_mhz(&core);

			measured++;
			hakios_keep("MiniIOMobileFramebuffer: core %d runs at %u MHz", core, mhz);
		}
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		if (recorded != NULL) {
			/* what this frame looks like, for a run nobody watches: its average colour, and how much of it is not black */
			uint64_t sum[3] = { 0, 0, 0 };
			uint32_t lit = 0, samples = 0;

			surface_from_memory(recorded);
			for (uint32_t y = recorded->info.height / 64; y < recorded->info.height; y += recorded->info.height / 32) {
				const uint32_t * row = (const uint32_t *) (recorded->pixels() + (size_t) y * recorded->info.bytesPerRow);

				for (uint32_t x = recorded->info.width / 32; x < recorded->info.width; x += recorded->info.width / 16) {
					uint32_t pixel = row[x];        /* B G R A in memory */

					sum[0] += (pixel >> 16) & 0xff;
					sum[1] += (pixel >> 8) & 0xff;
					sum[2] += pixel & 0xff;
					lit += ((pixel >> 16) & 0xff) + ((pixel >> 8) & 0xff) + (pixel & 0xff) > 48;
					samples++;
				}
			}
			if (samples != 0) {
				IOLog("MiniIOMobileFramebuffer: this frame on average red %llu green %llu blue %llu; %u%% of it not black\n",
				    sum[0] / samples, sum[1] / samples, sum[2] / samples, lit * 100 / samples);
			}
			uint32_t every = gShotEvery >= 5 ? gShotEvery / 5 : 1;

			if (gShot != 0 && ++fShotStats >= gShot / 5 && (fShotStats - gShot / 5) % every == 0) {
				shot(recorded);
			}
		}
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (gFlicker) {
			/* (and the run's total, which the last of these lines carries past the log's lost middle) */
			static uint32_t flickeredAll, framesAll;

			flickeredAll += flickerFrames;
			framesAll += fStatFrames;
			IOLog("MiniIOMobileFramebuffer: %u frames flickered (unlike the frames before and after), %u pixels of the "
			    "small picture each; %u of %u frames so far\n", flickerFrames, flickerFrames ? flickerPixels / flickerFrames : 0,
			    flickeredAll, framesAll);
			flickerFrames = flickerPixels = 0;
		}
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		if (args->structureInputSize >= SWAP26_SIZE) {
			/*
			 * The 26 and 27 releases' request (0x588 bytes and more: 0x6e0 from 24B119) is 16.5's with 0x48
			 * bytes more ahead of the swap's own fields -- the ID, the layers' surfaces, source rectangles,
			 * flags, destination rectangles, the masks of the layers set and cleared all sit 0x48 further on
			 * (the Qualcomm port's reading of kern_SwapBegin/SwapSetLayer). Seen from there it is the old one.
			 */
			uint8_t old[SWAP_SIZE];

			memcpy(old, (const uint8_t *) args->structureInput + SWAP26_SHIFT, SWAP_SIZE);
			fOwner->present(old);
			return kIOReturnSuccess;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("hakios,framebuffer", sizeof("hakios,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and userland/docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (userland/docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (userland/docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.system, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
	/*
	 * Boot-arg fbshot_panel=1: what such a picture put on the panel, 90 pixels wide, as
	 * fbshot's lines (tools/log/fbshot.py): on the 60th picture and every 400th after, so that a
	 * program repainting for minutes leaves one at the end of the log.
	 */
	static uint32_t shown;
	if (gShotPanel && (++shown == 60 || (shown > 60 && (shown - 60) % 400 == 0))) {
		shot_pixels((const uint8_t *) vinfo.v_baseaddr, columns, rows, (uint32_t) vinfo.v_rowbytes, 90, 0, 1);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
