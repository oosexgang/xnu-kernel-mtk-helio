#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../userland/tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /hakios. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the system's launchd
#                                   after init's checks; rc.system runs a few the system programs and halts
#                      JOBS="..."   labels of the stock the system launchd jobs to load (default: IOS_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=1      ... and wifid, the system's Wi-Fi daemon (see below at IOS_JOBS)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      EXTRA="..."  more programs for /hakios/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      RC_JOBS=<file> what the job com.hakios.demo runs (a name in jobs/, or a path): rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet)
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (userland/tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (15 MB) and lib/libhakios-gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      METAL=1      with the Metal driver (src/hakiosmetal.m over src/metal) as the bundle
#                                   lib/HakiosMetal.bundle, bin/metal_props and bin/metal_drv_test. Wants GL=1 (on
#                                   the Mali the driver compiles through libOSMesa, lib/libhakiosair.dylib) and
#                                   RC=rc.launchd.metal with the kernel's boot-arg metal=1, which put the plug-in's
#                                   name on IOAcceleratorES. The window server stays on OpenGL ES (src/inject.c,
#                                   window_server_keeps_opengl_es); applications get the device.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, bsd/dev/hakios_adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of the system image 20F66 chosen by ../darwin/system/select-system.py at
#                    its real paths, the dyld shared cache, and src/stage0.c as /sbin/launchd (the
#                    real one is /sbin/launchd.system). 2.5 GB; upload once per loader session with
#                    `xl ramdisk userland/system.apfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#   flashboot.apfs   FLASHBOOT=1: the RAM part of a boot whose root is the system's volume on the phone's
#                    flash (see below); with CRYPTEXES=flash the boot volume alone, the cryptexes being
#                    on the flash
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../kernel/deps/setup.sh);
#                    the images are then made of them and of the system's files alone (python3)
#   CHECK=1          check every image (every container) as it is made: apfsck has to find it clean and
#                    the kernel's apfs_lib has to read every volume back as the directory it was made of
#                    (../userland/tools/apfs/check-image.sh; the system image takes a few minutes more)
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../kernel/env.sh
# Three things GNU coreutils has and macOS has not: a tree of hard links (cp -al; Sonoma's cp fails on every
# file of one, "Bad file descriptor"), sha256sum, and stat -c
linktree() { # linktree DIR NEW: NEW (which is not there yet) becomes DIR's tree, every file a hard link
	if [ "$(uname -s)" = Darwin ]; then
		mkdir -p "$2" && (dst=$(cd "$2" && pwd) && cd "$1" && pax -rwl -p e . "$dst")
	else
		cp -al "$1" "$2"
	fi
}
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
SYSFILES=../darwin/system
CACHE=$SYSFILES/20F66__iPhone10,3_6
# darwin/system/rootfs and darwin/system/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $SYSFILES/$d ] || continue
	echo "build.sh: darwin/system/$d is a symlink to $(readlink $SYSFILES/$d)" >&2
	echo "build.sh: it has to be a directory of this copy's own: rm it and cp -al the original over," >&2
	echo "build.sh: as tools/new-workspace.sh does. Building against a link damages the shared tree." >&2
	exit 1
done
# The stock the system jobs given to launchd, roughly in the order they were brought up.
IOS_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard com.apple.chronod com.apple.sleepd com.apple.mediaremoted
"}
# (com.apple.mediaremoted: without it SpringBoard's MediaRemote spins in its XPC handler -- two of the four
# cores on an idle home screen once mediaserverd runs, and the widgets with it; run 328-329, 400% -> 14%)
# WIFID=1: The system's own Wi-Fi daemon as well, for Settings to have a Wi-Fi to show: the kernel answers it in
# bsd/dev/hakios_apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
# With it what a network needs on this port (the first phone's radio, runs 450-453): configd (DHCP and the routes),
# mDNSResponder (names), and for HTTPS trustd and securityd (without them every certificate is "invalid"),
# nsurlsessiond and CFNetwork's agent.
[ -n "$WIFID" ] && IOS_JOBS="$IOS_JOBS com.apple.wifid com.apple.configd com.apple.mDNSResponder com.apple.mDNSResponderHelper
	com.apple.trustd com.apple.securityd com.apple.nsurlsessiond com.apple.cfnetwork.cfnetworkagent"
# ANGELS="label ...": launch angels among them (System/Library/LaunchAngels, which init hides from launchd): the
# wallpaper as the system draws it wants com.apple.PosterBoard (the posters) and com.apple.PaperBoard, which SpringBoard
# asks for and which do not run otherwise -- the black behind the icons with sb_wallpaper=0 (run 246)
[ -n "$ANGELS" ] && IOS_JOBS="$IOS_JOBS $ANGELS"
# MORE_JOBS="label ...": other the system jobs added to the list for a run (the wallpaper's: com.apple.mediaserverd,
# which decodes HEIC -- the posters' pictures -- for every process)
[ -n "$MORE_JOBS" ] && IOS_JOBS="$IOS_JOBS $MORE_JOBS"
# DROP_JOBS="label ...": jobs left out for a run (com.apple.chronod: no widget extensions, run 326)
for j in $DROP_JOBS; do IOS_JOBS=$(echo " $IOS_JOBS " | tr '\n\t' '  ' | sed "s/ $j / /g"); done
# (and TRIM="label:service,...;..." keeps only those Mach services of a job: see mkjobs.py)
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libhakios.dylib, src/inject_sb.c is libhakios-sb.dylib, src/inject_media.c is
# libhakios-media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the system's volume's /bin when that is the root (src/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libhakios-tweaks is the tweak loader of the package manager's prefix, src/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
INJECT=${INJECT-"/hakios/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system com.apple.WebKit.GPU
/hakios/lib/libhakios-sb.dylib SpringBoard
/hakios/lib/libhakios-media.dylib mediaserverd
/hakios/lib/libhakios-app.dylib Sileo
/hakios/lib/libhakios-tweaks.dylib Preferences"}
# APPLEID=1: the Apple ID / App Store libraries of ours as well (ported from origin/AppleID). Opt-in,
# so every other image is untouched -- like aks_keys, these change daemons (accountsd, akd, ...) that
# must not be touched where no sign-in happens. libhakios.mad.dylib tells mobileactivationd the
# device is development-fused (activation short-circuits to Activated); libhakios.kc.dylib rewrites
# the one passcode-only keychain class akd/accountsd cannot store here to its passcode-free kin.
if [ -n "$APPLEID" ]; then
	INJECT="$INJECT
/hakios/lib/libhakios.mad.dylib mobileactivationd
/hakios/lib/libhakios.kc.dylib akd accountsd itunesstored appstored"
fi
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.apfs depends on stage0
		cmp -s "$f" "obj/$(basename "$f")" || cp "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0 var_store; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
# The data volume into the flash and back (src/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the system's job list below, which several working
# copies are editing at once.
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp launchd/com.hakios.varstore.plist launchd/com.hakios.nullcomm.plist root/LaunchDaemons/
echo "Darwin on image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
if [ -d $SYSFILES/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the system's SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		$DYN -o obj/rwtouch src/rwtouch.c
		$DYN -o obj/cputop src/cputop.c
		$DYN -o obj/nullxpc src/nullxpc.c
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test src/camera_test.c src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework CoreBluetooth -sectcreate __TEXT __info_plist src/bt_test.plist \
			-o obj/bt_test src/bt_test.m &&
			{ ${LDID:-ldid} -Ssrc/bt_test.entitlements obj/bt_test 2>/dev/null ||
			  echo "build.sh: no ldid: bt_test is unsigned, and TCC may not let it use Bluetooth"; }
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test src/record_test.c &&
			{ ${LDID:-ldid} -Ssrc/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what the system here believes about Metal, and what CoreImage does without it (src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test src/metal_test.m
		if [ -n "$METAL" ]; then
			# the bundle Metal's discovery opens (step 3: <MetalPluginName>.bundle under
			# /System/Library/Extensions, -[NSBundle classNamed:] then wants the class out of it),
			# the switch that names it on IOAcceleratorES (src/metal_props.c), and the driver's test
			# the driver's core is C++ (src/metal: the compiler library's calls, typed by
			# userland/tools/mesa/air/air_metal.h itself, buffers, what the driver says), the plug-in over
			# it Metal's classes in Objective-C (src/metal/rwm.h is the line between them)
			mkdir -p obj/metal
			for f in air buffer log texture blit draw command recipes import perf plib shape argenc run worker surfaces render compute cmdbuf pipeline drawable capture; do
				${DYN/xnu-clang/xnu-clang++} -Os -std=c++20 -fno-exceptions -fno-rtti -I../userland/tools/mesa/air \
					-c src/metal/$f.cpp -o obj/metal/$f.o
			done
			$DYN -Os -bundle -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc \
				-lc++ -I../userland/tools/mesa/air -Isrc -o obj/HakiosMetal src/hakiosmetal.m \
				obj/metal/air.o obj/metal/buffer.o obj/metal/log.o obj/metal/texture.o obj/metal/blit.o obj/metal/draw.o obj/metal/command.o obj/metal/recipes.o obj/metal/import.o obj/metal/perf.o obj/metal/plib.o obj/metal/shape.o obj/metal/argenc.o obj/metal/run.o obj/metal/worker.o obj/metal/surfaces.o obj/metal/render.o obj/metal/compute.o obj/metal/cmdbuf.o obj/metal/pipeline.o obj/metal/drawable.o
			$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props src/metal_props.c
			$DYN -o obj/psgrep src/psgrep.c
			$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
				-o obj/metal_drv_test src/metal_drv_test.m
		fi
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		$DYN -fobjc-arc -framework Foundation -framework CoreGraphics -framework ImageIO -o obj/fbgrab src/fbgrab.m
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock the system programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /hakios/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.dylib src/inject.c
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios-sb.dylib src/inject_sb.c
		# ... the tweak loader of the package manager's prefix (src/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libhakios-tweaks.dylib src/inject_tweaks.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-app.dylib -lobjc \
			-o obj/libhakios-app.dylib src/inject_app.c
		$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios-media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libhakios-media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c
		# ... the Apple ID / App Store libraries (APPLEID=1; ported from origin/AppleID). inside
		# mobileactivationd, the dev-fused MobileGestalt answers (src/inject_mad.c); inside
		# akd/accountsd, the one unstorable keychain class rewritten (src/inject_kc.c, the inline
		# hook via /var/jb's CydiaSubstrate since AuthKit's SecItem* calls bind inside the dyld cache)
		if [ -n "$APPLEID" ]; then
			$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios.mad.dylib -framework CoreFoundation \
				-framework Security -lMobileGestalt -o obj/libhakios.mad.dylib src/inject_mad.c
			$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios.kc.dylib -framework CoreFoundation \
				-framework Security -o obj/libhakios.kc.dylib src/inject_kc.c
		fi
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/HakiosAudio src/audio_plugin.c
		# a program's output into the log (rc.launchd.aks and the job lists run programs under it): every image has it
		$DYN -o obj/tolog src/tolog.c
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../kernel/deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../userland/tools/mesa/*.patch ../userland/tools/mesa/hakios-drm/* | sha256)" ] ||
				../userland/tools/mk-mesa.sh
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /hakios/lib/libhakios-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libhakios-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (docs/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 userland/tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
		fi
	fi
	cp obj/hello_dyn obj/svc_test obj/surf_test obj/rwtouch obj/cputop obj/nullxpc obj/mg_test root/bin/
	[ -f obj/tolog ] && cp obj/tolog root/bin/
	[ -n "$BT" ] && [ -f obj/bt_test ] && cp obj/bt_test root/bin/     # (src/bt_test.m: CoreBluetooth's scan, for Bluetooth runs)
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/fbgrab ] && cp obj/fbgrab root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	for f in $EXTRA; do cp "$f" root/bin/; done
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libhakios.dylib obj/libhakios-sb.dylib root/lib/
	# the wallpaper of this system release as one picture, for SpringBoard where PosterBoard cannot draw it (src/inject_sb.c,
	# put_wallpaper): drawn on this Mac from the .ca packages of the extracted system, with the Mac's own
	# QuartzCore (../userland/tools/render-ca-wallpaper.m), and not kept in the repository, being Apple's. Half the
	# screen's size and JPEG quality 0.4: about 15 KB, what boot_a has room for (SpringBoard scales it up)
	WALLPAPER="$SYSFILES/rootfs/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper"
	if [ "$(uname)" = Darwin ] && [ -d "$WALLPAPER" ] && [ ! -f root/share/wallpaper.jpg ]; then
		clang -fobjc-arc -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
			-o obj/render-ca-wallpaper ../userland/tools/render-ca-wallpaper.m &&
			mkdir -p root/share && obj/render-ca-wallpaper "$WALLPAPER" root/share/wallpaper.jpg 375 812 0.4 | tail -1
	fi
	[ -f obj/libhakios-media.dylib ] && cp obj/libhakios-media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libhakios-app.dylib ] && cp obj/libhakios-app.dylib root/lib/
	[ -f obj/libhakios-tweaks.dylib ] && cp obj/libhakios-tweaks.dylib root/lib/
	[ -f obj/libhakios.mad.dylib ] && cp obj/libhakios.mad.dylib root/lib/     # Apple ID (APPLEID=1)
	[ -f obj/libhakios.kc.dylib ] && cp obj/libhakios.kc.dylib root/lib/
	if [ -f obj/HakiosAudio ]; then
		mkdir -p root/lib/HakiosAudio.driver
		cp obj/HakiosAudio root/lib/HakiosAudio.driver/
		cp src/audio_plugin.plist root/lib/HakiosAudio.driver/Info.plist
	fi
	rm -rf root/lib/HakiosMetal.bundle root/lib/libhakiosair.dylib
	if [ -n "$METAL" ]; then
		mkdir -p root/lib/HakiosMetal.bundle
		cp obj/HakiosMetal root/lib/HakiosMetal.bundle/
		cp src/metal_plugin.plist root/lib/HakiosMetal.bundle/Info.plist
		cp obj/metal_props obj/metal_drv_test obj/psgrep root/bin/
		# on the Mali the driver's compiler is inside libOSMesa (userland/tools/mesa/panfrost-osmesa-air.patch)
		ln -s libOSMesa.8.dylib root/lib/libhakiosair.dylib
		# QC_METALLIB=FILE: QuartzCore's shaders in AIR for CoreAnimation's Metal renderer (boot-arg
		# camtl=1, src/inject.c give_quartzcore_its_shaders): the AIR slice of a macOS QuartzCore's
		# default.metallib, which is Apple's and not kept in the repository
		rm -f root/share/QuartzCore-default.metallib
		[ -z "$QC_METALLIB" ] || { mkdir -p root/share; cp "$QC_METALLIB" root/share/QuartzCore-default.metallib; }
		# CoreAnimation's pipelines for backboardd to build at start (src/hakiosmetal.m, rw_warm_start;
		# userland/tools/rwm-recipes.py makes the file out of a run's log)
		rm -f root/share/rwm-recipes.txt
		[ ! -f share/rwm-recipes.txt ] || cp share/rwm-recipes.txt root/share/rwm-recipes.txt
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libhakios-gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /hakios/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/hakios/lib/*) if [ -f "root/lib/${library#/hakios/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	cp $SYSFILES/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(readlink /etc/localtime 2>/dev/null | sed 's|.*/zoneinfo/||' || true)
	[ -n "$zone" ] || zone=$(timedatectl show -p Timezone --value 2>/dev/null || true)
	zone=${zone:-UTC}
	python3 -c "
import sys, re
path = 'root/etc/var.skel'
with open(path, 'r') as f:
	c = f.read()
c = re.sub(r'^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$', r'l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo', c, flags=re.MULTILINE)
c = re.sub(r'^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime$', r'l\t/var/db/timezone/zoneinfo/' + sys.argv[1] + r'\t/private/var/db/timezone/localtime', c, flags=re.MULTILINE)
with open(path, 'w') as f:
	f.write(c)
" "$zone"
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	# mobile's own temporary directory, $HOME/tmp, which the system's template does not have: an application of
	# mobile's without a data container of its own keeps its temporary files there -- Sileo, registered from
	# the prefix with the root on the flash, downloads every package and index there, and without it each
	# download ended in "No such file or directory"
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	# A key of ours for root, when there is one: ssh/authorized_keys, never in the repository (like
	# fw-*). dropbear from the package manager's prefix reads it and lets us in over the phone's
	# Wi-Fi, which saves rebuilding an image for every question one wants to ask a running the system. It
	# refuses a key whose directory anyone but its owner may write, hence the 700.
	if [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		# the key that lets us in, and the host's own key beside it: made here and not on the
		# phone, because ssh-keygen asks who uid 0 is and gets "No user exists for uid 0" --
		# it asks through libiosexec, which reads a database the bootstrap does not ship (see
		# init.c, where it is now built at every boot); the key is made here all the same,
		# since nothing should depend on the order of those two
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi
	cp ${RC:-rc.launchd} root/etc/rc
	# a job list by its name in jobs/ (rc.jobs.unlock), or by a path of its own
	rc_jobs=${RC_JOBS:-rc.jobs}
	[ -f "$rc_jobs" ] || rc_jobs=jobs/$rc_jobs
	cp "$rc_jobs" root/etc/rc.jobs
	# QUIET=1: launchd's own standard error goes nowhere (src/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	# the jobs the system's launchd will see (/System/Library/LaunchDaemons is a symlink to this directory)
	cp launchd/com.hakios.demo.plist launchd/com.hakios.wifi.plist root/LaunchDaemons/
	[ -n "$IOS_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $SYSFILES/rootfs root/LaunchDaemons $IOS_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, bsd/dev/hakios_adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (bsd/dev/hakios_wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
fi
# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and so not in
# the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1; the first phone's radio,
# connsys_step=17, without the Qualcomm phone's firmware beside it)
rm -f root/fw/network
# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then mkdir -p root/fw; install -m 0600 ../fw-wifi/network root/fw/network; fi
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (bsd/dev/hakios_haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# ... and the cameras' register tables (bsd/dev/hakios_camera.c, cam=2 and up), made by tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# hakios_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.hakios_*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.hakios_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (bsd/dev/hakios_adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
# Firmware that is not there is otherwise silent at build time and speaks only much later, deep in a
# device log -- "adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin" cost a phone
# slot on 19 October 2026. It is the vendor's and never in the repository, so a copy made with bare
# git worktree add has none of it, and neither has one whose tools/new-workspace.sh predates the fw-*
# loop there. Say what went in and what was not there to go in; what each one is for is above.
fwhave= fwnone=
for d in gpu wifi haptic camera audio; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"
# an image carrying fw/network carries the name and passphrase of your network, and the images are
# what somebody would be handed to run this on a second phone
if [ -f root/fw/network ]; then
	echo "build.sh: and fw/network -- the name and passphrase of your Wi-Fi. Do not pass this image on;"
	echo "build.sh: SHARE=1 builds the same image without it."
fi
# When the system's volume on the phone's flash is to become the root (boot-arg root_apfs=ufs_darwin,
# with flashboot.apfs resident; src/init.c, flash_root): an empty directory to hide the system's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of the system's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $SYSFILES/rootfs/System/Library/xpc/$f ] && cp $SYSFILES/rootfs/System/Library/xpc/$f root/xpc/
done
[ -d root/lib/HakiosAudio.driver ] && ln -s /hakios/lib/HakiosAudio.driver root/hal/HakiosAudio.driver
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (bsd/dev/hakios_image.c)
python3 ../userland/tools/apfs/mkapfs.py -z --plain fw -o root.apfs hakios=root | tail -1
[ -z "$CHECK" ] || ../userland/tools/apfs/check-image.sh root.apfs hakios=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the system's volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /hakios and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where the system grafts the cryptexes (src/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside the system, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the system's volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/dyld_shared_cache_arm64 ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/hakios flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused
	if [ "$CRYPTEXES" != flash ]; then
	if [ -d $SYSFILES/cryptex ]; then linktree $SYSFILES/cryptex flashboot.os; else mkdir -p flashboot.os; fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the system's volume's /fw, which src/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when userland/tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the system's volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on a reference phone as well; init grafts it onto /var/jb (src/init.c, mount_writable)
	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (src/init.c)
		# and what Sileo runs from /bin, for init to put over the system's volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	D=flashboot.os/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	fi
	python3 ../userland/tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../userland/tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; fetch.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $SYSFILES/cryptex-app.dmg ] && APP=$SYSFILES/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../userland/tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1
		[ -z "$CHECK" ] || ../userland/tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(wc -c < flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $SYSFILES/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ $SYSFILES/system.manifest -nt system.apfs ] ||
	  [ $SYSFILES/cryptex -nt system.apfs ] || [ $SYSFILES/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what darwin/system/select-system.py has chosen since darwin/system/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: The system found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $SYSFILES/.done-rootfs ] && [ $SYSFILES/select-system.py -nt $SYSFILES/.done-rootfs ]; then
		echo "build.sh: darwin/system/select-system.py has changed since darwin/system/rootfs was extracted: run KEEP=1 darwin/system/fetch.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	linktree $SYSFILES/rootfs sysroot                      # hard links: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that the system has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /hakios/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/hakios sysroot/sbin
	# where the kernel mounts the system's root container off the phone's flash (boot-arg apfs_volume=):
	# The system has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/iOS
	# the applications the system installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.system
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/hakios_adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/hakios_speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the first phone's CONNSYS firmware (Wi-Fi, Bluetooth), the phone's own /vendor/firmware taken off it through root
	# into ../kernel/deps/build/connsys-fw (never in the repository): /fw/connsys, where bsd/dev/mtk_connsys.c reads
	# it once the root is mounted (connsys_step=5)
	if [ -d ../kernel/deps/build/connsys-fw/firmware ]; then
		mkdir -p sysroot/fw/connsys
		cp ../kernel/deps/build/connsys-fw/firmware/* sysroot/fw/connsys/
	fi
	# the package manager's prefix, when userland/tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and the system's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		# the tweak loader (src/inject_tweaks.c) in the system image as well: a test build's boot partition has no room
		# for it (runkit drops it from root/lib), and SpringBoard opens this copy when that one is not there
		# (src/inject_sb.c, load_tweaks)
		if [ -f obj/libhakios-tweaks.dylib ]; then
			mkdir -p sysroot/usr/lib/hakios
			cp obj/libhakios-tweaks.dylib sysroot/usr/lib/hakios/
		fi
		# and the Metal driver's test (src/metal_drv_test.m) as /usr/local/bin/metal_drv_test: the boot partition
		# is cut to fit 32 MB without it (runkit/mk.sh), and it is what runs the compute and argument-buffer roads
		# the home screen never takes
		if [ -f obj/metal_drv_test ]; then
			mkdir -p sysroot/usr/local/bin
			cp obj/metal_drv_test sysroot/usr/local/bin/
		fi
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	# libOSMesa (11 MB, the Mali's compiler and GL inside it) in the system image as well, /usr/lib/hakios/libOSMesa.8.dylib: the boot
	# partition (32 MB) is full, and with OSMESA_EXT=1 runkit/mk.sh makes /hakios/lib/libOSMesa.8.dylib a link to this copy. It does no
	# harm to an image that keeps its own.
	if [ -f obj/libOSMesa.8.dylib ]; then
		mkdir -p sysroot/usr/lib/hakios
		cp obj/libOSMesa.8.dylib sysroot/usr/lib/hakios/
	fi
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /hakios/lib/HakiosAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $SYSFILES/cryptex ]; then linktree $SYSFILES/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $SYSFILES/cryptex-app ]; then linktree $SYSFILES/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside the system
	python3 ../userland/tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=$SYSFILES/system.manifest | tail -1
	[ -z "$CHECK" ] || ../userland/tools/apfs/check-image.sh system.apfs System=sysroot,manifest=$SYSFILES/system.manifest
	rm -rf sysroot
fi
