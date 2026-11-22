#!/bin/bash
# The small image (/hakios) for a boot of the 26 or 27 release towards SpringBoard: darwin-renoir's root with our
# arm64e libraries; and the launchd jobs, made from the 26 release's own plists (darwin-renoir's mkjobs.py: the
# named jobs as they are, every other job with Mach services on demand only) into ios26/jobs26, for
# the system image (LAUNCHDAEMONS=.../jobs26 mkimage26.py): the 26 release's launchd takes a job for a system
# service only when its plist is on the system volume.
# Writes $REL/root26-sys.apfs and $REL/jobs26 (REL=.../ios27 for the 27 release, with OBJ=.../obj27).
# JOBS="label ..." replaces the named jobs; OSLOG as for mkjobs.py.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository, darwin-renoir and ios26/ios27
DR=${DR:-$DK/darwin-renoir}
REL=${REL:-$DK/ios26}            # the extracted IPSW (ios26: 23G90, ios27: 24B119); the outputs go there
OBJ=${OBJ:-$RN/userland/obj26}   # our arm64e libraries (inj26.sh), built for that IPSW's addresses
T=$REL/root26-sys-tree
JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}

# mkjobs.py looks for the cryptexes beside the root filesystem, as ios165/ has them
S=$REL/jobsrc
rm -rf $S; mkdir -p $S
ln -s $REL/rootfs $S/rootfs
ln -s $REL/cryptex/os $S/cryptex
ln -s $REL/cryptex/app $S/cryptex-app

J=$REL/jobs26
rm -rf $T $J; cp -a $DR/userland/root $T
mkdir -p $J; cp $T/LaunchDaemons/com.hakios.demo.plist $J/
rm -f $T/LaunchDaemons/*.plist
cp $OBJ/*.dylib $T/lib/
cp $OBJ/gl_test $T/bin/gl_test
# METAL27=DIR (the MTK port): the Metal driver for the Mali (metal27.sh) and Mesa's panfrost with its
# AIR compiler, QuartzCore's own AIR shaders, and etc/rc naming the plug-in on IOAcceleratorES (boot-arg metal=1)
if [ -n "${METAL27:-}" ]; then
	mkdir -p $T/lib/HakiosMetal.bundle $T/share
	cp $METAL27/HakiosMetal $T/lib/HakiosMetal.bundle/
	cp "$(dirname "$RN")/userland/src/metal_plugin.plist" $T/lib/HakiosMetal.bundle/Info.plist
	cp $METAL27/metal_props $METAL27/psgrep $T/bin/
	cp $METAL27/libOSMesa.8.dylib $T/lib/
	ln -sf libOSMesa.8.dylib $T/lib/libhakiosair.dylib
	cp $METAL27/QuartzCore-default.metallib $T/share/
	# the 27 release's CoreAnimation pipelines, built ahead by each process's warm-up thread (userland/tools/rwm-recipes.py)
	[ -f "$(dirname "$RN")/userland/share/rwm-recipes27.txt" ] && cp "$(dirname "$RN")/userland/share/rwm-recipes27.txt" $T/share/rwm-recipes.txt
	# ... and the generic pipelines' variants, built ahead the same way (userland/tools/rwm-variants.py)
	[ -f "$(dirname "$RN")/userland/share/rwm-variants27.txt" ] && cp "$(dirname "$RN")/userland/share/rwm-variants27.txt" $T/share/rwm-variants.txt
	# RWA_CACHE_BIN=FILE: the native library's stages compiled on the Mac (userland/tools/rwacache.sh), loaded instead of built
	[ -n "${RWA_CACHE_BIN:-}" ] && [ -f "$RWA_CACHE_BIN" ] && cp "$RWA_CACHE_BIN" $T/share/rwa-cache.bin
	{ echo "/hakios/bin/tolog /hakios/bin/metal_props on ../../../hakios/lib/HakiosMetal HakiosMetalDevice"; cat $T/etc/rc; } > $T/etc/rc.m
	mv $T/etc/rc.m $T/etc/rc
	# RWA_REPLAY_DIR=DIR: captured draws (capNNN.txt) and the two shader modules (vspc.bc, fcph.bc) drawn again on the
	# Mali before launchd by ca27_replay, when boot-arg rwa_replay=1 asks (it returns at once otherwise); the
	# captures stay out of the tree, they hold Apple's shaders
	if [ -n "${RWA_REPLAY_DIR:-}" ] && [ -f $METAL27/ca27_replay ]; then
		mkdir -p $T/share/replay
		cp $METAL27/ca27_replay $T/bin/
		cp "$RWA_REPLAY_DIR"/vspc.bc "$RWA_REPLAY_DIR"/fcph.bc $T/share/replay/
		# runs.txt in it, if there: one replay a line, "CAPTURE [NAME=VALUE...]" (the switches of the compiler library
		# for that one process); without it every capture once, as it is
		{ if [ -f "$RWA_REPLAY_DIR"/runs.txt ]; then
			# "CAPTURE VERTEX.bc FRAGMENT.bc [NAME=VALUE...]"
			while read -r c v f envs; do
				[ -z "$c" ] || [ "${c#\#}" != "$c" ] && continue
				cp "$RWA_REPLAY_DIR/$c" "$RWA_REPLAY_DIR/$v" "$RWA_REPLAY_DIR/$f" $T/share/replay/
				echo "/hakios/bin/tolog $envs /hakios/bin/ca27_replay /hakios/share/replay/$c /hakios/share/replay/$v /hakios/share/replay/$f"
			done < "$RWA_REPLAY_DIR"/runs.txt
		  else
			for c in "$RWA_REPLAY_DIR"/cap*.txt; do
				cp "$c" $T/share/replay/
				echo "/hakios/bin/tolog /hakios/bin/ca27_replay /hakios/share/replay/$(basename $c) /hakios/share/replay/vspc.bc /hakios/share/replay/fcph.bc"
			done
		  fi; cat $T/etc/rc; } > $T/etc/rc.m
		mv $T/etc/rc.m $T/etc/rc
	fi
fi
# MALI27=DIR (the MTK port): hakios-mali's probe (mali27.sh: the Mali's job descriptors written by our own library, no
# Panfrost), /hakios/bin/mali-probe; the job list runs it (RC_JOBS=userland/jobs/rc.jobs.maliprobe), or a shell by hand
if [ -n "${MALI27:-}" ]; then
	cp $MALI27/mali-probe $T/bin/mali-probe
	# Apple's kernels its apple-kernels stage compiles on the phone (mali27.sh copies them out of the IPSW)
	if [ -f $MALI27/PortraitFilters.metallib ]; then
		mkdir -p $T/share
		cp $MALI27/PortraitFilters.metallib $T/share/
	fi
	if [ -f $MALI27/ARKitCore.metallib ]; then
		mkdir -p $T/share
		cp $MALI27/ARKitCore.metallib $T/share/
	fi
	# the native Metal library: the plug-in loads it in place of libhakiosair.dylib with boot-arg mali_native=1
	[ -f $MALI27/libhakiosmali.dylib ] && cp $MALI27/libhakiosmali.dylib $T/lib/
fi
# the audio server's output device (inj26.sh): arm64e, in place of 16.5's arm64 one, which the audio server cannot load;
# the system image links /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver to it (mkimage26.py)
if [ -f $OBJ/HakiosAudio ]; then
	cp $OBJ/HakiosAudio $T/lib/HakiosAudio.driver/HakiosAudio
	cp "$(dirname "$RN")/userland/src/audio_plugin.plist" $T/lib/HakiosAudio.driver/Info.plist
fi
# the 26 release's launchd, on a normal boot, refuses to start a job whose program is not a platform binary
# ("unexpected exec of non-platform binary: /hakios/sbin/init"); the kernel's AMFI stand-in takes a
# signature marked ad hoc (CodeDirectory flag CS_ADHOC) for one. darwin-renoir's programs are either
# unsigned or signed by ldid -S, which leaves the flags 0: every Mach-O here is signed again with
# -Cadhoc, keeping the entitlements it has.
LDID=${LDID:-$DR/build-tools/theos/toolchain/linux/iphone/bin/ldid}
# battery_test with this repository's entitlements, not the ones the copy above was signed with: "battery_test launch
# ID" (SBSLaunchApplicationWithIdentifier) needs com.apple.springboard.launchapplications, and the 27 release's
# SpringBoard said 9, "security policy error", without it (run 342)
if [ -f $T/bin/battery_test ]; then "$LDID" -S"$RN/../userland/src/battery_test.entitlements" $T/bin/battery_test; fi
ENT=$(mktemp)
python3 - "$T" <<'PY' | while read -r f; do
import os, struct, sys
for d, _, names in os.walk(sys.argv[1]):
    for n in names:
        p = os.path.join(d, n)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as f:
            h = f.read(4)
        if len(h) == 4 and struct.unpack('<I', h)[0] == 0xfeedfacf:
            print(p)
PY
	"$LDID" -e "$f" > "$ENT" 2>/dev/null || : > "$ENT"
	if [ -s "$ENT" ]; then "$LDID" -Cadhoc -S"$ENT" "$f"; else "$LDID" -Cadhoc -S "$f"; fi
	echo "  signed ad hoc: ${f#$T/}"
done
rm -f "$ENT"
# The audio server (audiomxd, the 26 release's) with its output device above: volume, the slider in Control Center and the
# sound of a timer depend on it. bluetoothd stays enabled: without it audiomxd asks for com.apple.bluetooth.xpc
# thousands of times a second.
(cd $DR/userland && ALLOW="audio bluetooth ${ALLOW:-}" ./mkjobs.py --rest $S/rootfs $J $JOBS)
for job in "$J"/*audiomxd* "$J"/*bluetoothd*; do
	if [ -e "$job" ]; then echo "  job $(basename "$job")"; fi
done
# Wi-Fi's daemon, when ALLOW lets it in ("wifi WiFi"): its job is limited to a device tree with a node named wlan,
# marconi-wifi or centauri-alpha, and this phone's tree has none (the radio is bsd/dev/mtk_wifi.c and
# iokit/Kernel/HakiosWLAN.cpp, connsys_step=17): launchd left the job out (run 647, com.apple.wifi-xpc not there).
if [ -f "$J/com.apple.wifid.plist" ]; then
	python3 - "$J/com.apple.wifid.plist" <<'PY'
import plistlib, sys
job = plistlib.load(open(sys.argv[1], 'rb'))
job.pop('_LimitLoadToDeviceTree', None)
plistlib.dump(job, open(sys.argv[1], 'wb'))
PY
	echo "  job com.apple.wifid.plist (any device tree)"
fi
# No EDR here: backboardd's job names com.apple.libEDR.Notification, whose server (EDRServerStartService)
# CoreBrightness starts, and CoreBrightness has no display to run here. launchd kept the port and queued
# every request: CoreAnimation in backboardd asks EDR at each display state change while holding the lock
# its display thread needs, and waited out libEDR's timeout, about 40 s: one frame in 40 s on the 27 release.
# Without the name a lookup fails at once and libEDR answers that there is no EDR.
python3 - "$J/com.apple.backboardd.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    job = plistlib.load(f)
if job.get('MachServices', {}).pop('com.apple.libEDR.Notification', None) is not None:
    with open(sys.argv[1], 'wb') as f:
        plistlib.dump(job, f)
    print('  backboardd: no com.apple.libEDR.Notification')
PY
# Jobs that cost the phone more than they give it: ScreenTimeAgent (Screen Time) keeps a core busy all the
# time on the 27 release here (ustacks=top: ~90 % of a core, touched or not) while the window server draws in
# software. Without the job its Mach services are unknown and their clients fail at once.
# airplayd: the audio server starts it to look for AirPlay routes, and it dies at once (SIGSEGV), every 5 s.
# PerfPowerServices is refused by the kernel (its busy loop does not work here). Do not leave its
# Mach services advertised: PLClientLogger's synchronous permission request otherwise waits forever.
# NO_JOBS="LABEL ...": more of them left out, for a run (a test of what a job does)
for job in com.apple.ScreenTimeAgent com.apple.airplayd com.apple.PerfPowerServices ${NO_JOBS:-}; do
	rm -f "$J/$job.plist" && echo "  no job $job"
done
# The audio server asks for airplayd's com.apple.coremedia.endpointmanager.xpc without end (the job is gone, and the
# service is under a feature flag in its plist anyway: run 614, audiomxd a core). nullxpc stands in for it: there, and
# answering nothing.
python3 - "$J/com.hakios.nullairplay.plist" <<'PY'
import plistlib, sys
plistlib.dump({'Label': 'com.hakios.nullairplay', 'ProgramArguments': ['/hakios/bin/nullxpc',
               'com.apple.coremedia.endpointmanager.xpc'], 'MachServices': {'com.apple.coremedia.endpointmanager.xpc': True},
               'POSIXSpawnType': 'Adaptive'}, open(sys.argv[1], 'wb'))
PY
# LPROJ="BUNDLE ...": the system volume's bundles given an empty en.lproj, through init's overlays (init/init.c
# overlay_dirs). The system image keeps only files, so every .lproj directory that held nothing is gone from it, and a
# bundle whose CFBundleDevelopmentRegion is "English" (411 of them, SpringBoard.app among them) then shows its
# strings' keys in place of its strings: Settings > Bluetooth said PLACARD_SUBTITLE and POWER_OFF_WARNING (run 650), a
# context menu ADD_EVENT. (The lasting fix is mkimage26.py keeping those directories; this needs no new image.)
rm -f $T/etc/overlays
# LPROJ=auto: Settings' panes of that kind without code of their own, from the system's tree (a path the system image left
# out is passed over by init). A bundle with code does not survive the union: over SpringBoard.app SpringBoard died at
# once, an assertion in -[FBServiceFacilityServer _initWithDomain:] (run 655), and over BluetoothSettings.bundle the pane
# came up empty and never asked bluetoothd anything (runs 657-660). Those wait for an image made with mkimage26.py's
# empty_lproj.
if [ "${LPROJ:-}" = auto ]; then
	LPROJ=$(python3 - "$S/rootfs" <<'PY'
import os, plistlib, sys
root = sys.argv[1]
for base in ('System/Library/PreferenceBundles', 'System/Library/Settings'):
    d = os.path.join(root, base)
    for name in sorted(os.listdir(d)) if os.path.isdir(d) else ():
        bundle = os.path.join(d, name)
        try:
            info = plistlib.load(open(os.path.join(bundle, 'Info.plist'), 'rb'))
        except Exception:
            continue
        if ' ' not in name and info.get('CFBundleDevelopmentRegion') == 'English' and 'CFBundleExecutable' not in info and \
           any(f.endswith('.loctable') for f in os.listdir(bundle)) and os.path.isdir(os.path.join(bundle, 'en.lproj')) and \
           not os.listdir(os.path.join(bundle, 'en.lproj')):
            print('/' + base + '/' + name)
PY
)
fi
for b in ${LPROJ:-}; do
	mkdir -p "$T/overlay$b/en.lproj"
	echo "$b" >> $T/etc/overlays
done
[ -f $T/etc/overlays ] && echo "  overlays: $(wc -l < $T/etc/overlays | tr -d ' ') bundles given an empty en.lproj"
# JOBCACHE=1: the jobs also as launchd's job cache in the small image, etc/xpc/launchd.plist, which init binds over the
# system volume's /System/Library/xpc (init/init.c overlay_job_cache): a change to the jobs then needs no new system
# image. The cache is the system's own with its LaunchDaemons ours, as mkimage26.py's write_job_cache makes it.
if [ "${JOBCACHE:-}" = 1 ]; then
	mkdir -p $T/etc/xpc
	python3 - "$S/rootfs/System/Library/xpc/launchd.plist" "$J" "$T/etc/xpc/launchd.plist" <<'PY'
import os, plistlib, sys
cache = plistlib.load(open(sys.argv[1], 'rb'))
cache['LaunchDaemons'] = {'/System/Library/LaunchDaemons/' + n: plistlib.load(open(os.path.join(sys.argv[2], n), 'rb'))
                          for n in sorted(os.listdir(sys.argv[2])) if n.endswith('.plist')}
plistlib.dump(cache, open(sys.argv[3], 'wb'), fmt=plistlib.FMT_BINARY)
print(f'  etc/xpc/launchd.plist: the job cache, {len(cache["LaunchDaemons"])} jobs')
PY
fi
# The last migration: darwin-renoir's skeleton says 20F66 (16.5), so the 26 release took itself for just updated
# and ran DataMigrator, a progress bar on the screen in place of the lock screen. It is this build's own.
python3 - "$T/var/mobile/Library/Preferences/com.apple.migration.plist" "$REL/rootfs/System/Library/CoreServices/SystemVersion.plist" <<'PY'
import plistlib, sys
build = plistlib.load(open(sys.argv[2], 'rb'))['ProductBuildVersion']
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'LastSystemVersion': build, 'DMLastMigrationResults': {'success': True, 'buildVersion': build}}, f,
                  fmt=plistlib.FMT_BINARY)
PY
# REDUCE_TRANSPARENCY=1: Settings' Reduce Transparency already on (com.apple.Accessibility's
# EnhancedBackgroundContrastEnabled, the key libAccessibility reads): the glass materials drawn without their
# glass. On the Mali the home screen is one fragment job of 128 ms, and the glass shader (glass_background) is the
# one the compiler named heavy -- 64 registers, one thread a core (run 30); this is to measure what it costs.
if [ "${REDUCE_TRANSPARENCY:-0}" = 1 ]; then
	python3 - "$T/var/mobile/Library/Preferences/com.apple.Accessibility.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'EnhancedBackgroundContrastEnabled': True}, f, fmt=plistlib.FMT_BINARY)
PY
	echo "  Reduce Transparency on"
fi
# The persona manifest. usermanagerd reads /private/var/keybags/persona.kb as it starts and aborts without
# it ("Daemon failed to load persona manifest."), and every daemon that asks it for the user's personas
# waits (runningboardd, so backboardd and SpringBoard). On a reference phone the file is left by the restore; this
# /private/var is new every boot. An empty manifest of the version the 26 release writes (1: a dictionary with
# UsePersonaManifestVersion, UsePersonaGenerationID and UserPersonaDictionary) has usermanagerd take its
# own way for a user without personas: "creating new set of default personas", in the kernel as well.
mkdir -p $T/var/keybags
python3 - "$T/var/keybags/persona.kb" <<'PY'
import plistlib, sys
manifest = {'UsePersonaManifestVersion': 1, 'UsePersonaGenerationID': 1, 'UserPersonaDictionary': {}}
with open(sys.argv[1], 'wb') as f:
    plistlib.dump(manifest, f, fmt=plistlib.FMT_BINARY)
PY
printf 'd\t700\t0\t0\t/private/var/keybags\nf\t600\t0\t0\t/private/var/keybags/persona.kb\t/var/keybags/persona.kb\n' >> $T/etc/var.skel
# Feature flags (/Library/Preferences/FeatureFlags is /private/var/preferences/FeatureFlags; Domain/NAME.plist,
# {feature: {Enabled: bool}}). MobileTimerFlags/core_data, on the 26 release: off. With it, mobiletimerd keeps alarms
# and timers in a Core Data store that never finishes loading here, and answers every request "System isn't
# ready..." -- the Clock's start button and Control Center's timer do nothing. Off, it keeps them in its
# defaults, as before. On the 27 release: on. There the timers have no other store: with core_data off
# -[MTTimerStorage coreDataReady] is always false, every request fails with MTTimerStorage error 4 and
# nothing is kept; on, the store loads (seen with 24B119: a 10-second timer ran and fired).
mkdir -p $T/var/preferences/FeatureFlags/Domain
python3 - "$T/var/preferences/FeatureFlags/Domain" "$REL/rootfs/System/Library/CoreServices/SystemVersion.plist" <<'PY'
import plistlib, sys
try:
    with open(sys.argv[2], 'rb') as f:
        major = int(plistlib.load(f)['ProductVersion'].split('.')[0])
except (OSError, KeyError, ValueError):
    major = 26                      # (the version cannot be read: as it always was)
flags = {'MobileTimerFlags': {'core_data': {'Enabled': major >= 27}}}
for domain, features in flags.items():
    with open(f'{sys.argv[1]}/{domain}.plist', 'wb') as f:
        plistlib.dump(features, f)
PY
printf 'd\t755\t0\t0\t/private/var/preferences/FeatureFlags\nd\t755\t0\t0\t/private/var/preferences/FeatureFlags/Domain\n' >> $T/etc/var.skel
for f in $T/var/preferences/FeatureFlags/Domain/*.plist; do
	printf 'f\t644\t0\t0\t/private/var/preferences/FeatureFlags/Domain/%s\t/var/preferences/FeatureFlags/Domain/%s\n' \
		"${f##*/}" "${f##*/}" >> $T/etc/var.skel
done
# PosterBoard: a first wallpaper that draws without Metal, the collections poster (src: darwin/userland/inject_poster.c)
echo "/hakios/lib/libhakios-poster.dylib PosterBoard" >> $T/etc/inject.conf
# SpringBoard: display mode changes that complete, there being no display driver to say so (src:
# darwin/userland/inject_backlight.c)
echo "/hakios/lib/libhakios-backlight.dylib SpringBoard" >> $T/etc/inject.conf
# The brightness slider (Control Center in SpringBoard, Settings) and Auto-Brightness, to the kernel's
# hw.backlight; backboardd for CoreBrightness's own side (src: darwin/userland/inject_brightness.c). It does
# nothing where the kernel has no backlight (boot-arg backlight=0).
echo "/hakios/lib/libhakios-brightness.dylib SpringBoard backboardd Preferences" >> $T/etc/inject.conf
# Spotlight gets libhakios.dylib too, for one thing: its search ranked without the L3 model, which is a downloaded asset
# and in no image here (src/inject.c spotlight_fixes; run 628: Spotlight died of it with the first results).
# BlueTool: its service com.apple.BlueTool answered by libhakios (inject.c bluetool_serves: no addresses of 16.5's in
# it), as on 16.5: the boot script it would run picks a Broadcom firmware this phone has not got. Not bluetoothd:
# inject.c's hooks there are at 16.5's addresses.
sed -i '' -e 's|^\(/hakios/lib/libhakios\.dylib .*\)$|\1 Spotlight BlueTool|' $T/etc/inject.conf
# PREFS_LOG=1: and Settings, for its log lines about Bluetooth and pairing kept for the host (inject.c
# preferences_log_watched; libhakios does nothing else there)
[ "${PREFS_LOG:-}" = 1 ] && sed -i '' -e 's|^\(/hakios/lib/libhakios\.dylib .*\)$|\1 Preferences|' $T/etc/inject.conf
# The kernel matches executable names, not the build template's @apps token.
python3 - "$T/etc/inject.conf" "$REL/rootfs" <<'PY'
import pathlib, plistlib, sys
conf, root = map(pathlib.Path, sys.argv[1:])
names = set()
for base in ('Applications', 'private/var/staged_system_apps'):
    for info in (root / base).glob('*.app/Info.plist'):
        with info.open('rb') as f:
            info = plistlib.load(f)
            name = info.get('CFBundleExecutable') if 'hidden' not in info.get('SBAppTags', []) else None
        if name:
            names.add(name)
lines = []
for line in conf.read_text().splitlines():
    words = line.split()
    if '@apps' in words:
        words = [word for word in words if word != '@apps'] + sorted(names)
    lines.append(' '.join(words))
text = '\n'.join(lines) + '\n'
if len(text.encode()) >= 4096:
    raise SystemExit('inject.conf exceeds kern.exec_inject capacity')
conf.write_text(text)
PY
# chronod's CoreDuet store cannot create its missing parent directories in the fresh RAM /var.
printf 'd\t755\t501\t501\t/private/var/mobile/Library/CoreDuet\nd\t755\t501\t501\t/private/var/mobile/Library/CoreDuet/Knowledge\n' >> $T/etc/var.skel
# CONSOLE_LOG="NAME ...": these programs get libhakios-console.dylib (standard error on the console; with
# boot-arg exec_env=NAME,...:OS_ACTIVITY_DT_MODE=1 their os_log as well)
if [ -n "${CONSOLE_LOG:-}" ]; then
	echo "/hakios/lib/libhakios-console.dylib $CONSOLE_LOG" >> $T/etc/inject.conf
fi
# LOGGING_DEBUG="SUBSYSTEM ...": os_log of those subsystems at every level, Debug included (on the console
# with boot-arg exec_env=NAME:OS_ACTIVITY_DT_MODE=1); /Library/Preferences is /private/var/preferences
for subsystem in ${LOGGING_DEBUG:-}; do
	mkdir -p $T/var/preferences/Logging/Subsystems
	python3 - "$T/var/preferences/Logging/Subsystems/$subsystem.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'DEFAULT-OPTIONS': {'Level': {'Enable': 'Debug', 'Persist': 'Debug'}}}, f)
PY
	printf 'f\t644\t0\t0\t/private/var/preferences/Logging/Subsystems/%s.plist\t/var/preferences/Logging/Subsystems/%s.plist\n' \
		"$subsystem" "$subsystem" >> $T/etc/var.skel
done
# what is running, at the start and twice later (net_test waits out its 60 s with no Wi-Fi);
# RC_JOBS=FILE: that file instead (darwin-renoir's convention, e.g. gestures by rwtouch)
if [ -n "${RC_JOBS:-}" ]; then
	cp "$RC_JOBS" $T/etc/rc.jobs
else
cat > $T/etc/rc.jobs <<'EOF'
# the 26 release: towards SpringBoard
/bin/ps -A
/hakios/bin/net_test waitaddr en0 60
/bin/ps -A
/hakios/bin/net_test waitaddr en0 60
/bin/ps -A
EOF
fi
# the first phone's CONNSYS firmware (Wi-Fi, Bluetooth), the phone's own /vendor/firmware taken off it into
# kernel/deps/build/connsys-fw (never in the repository): the 27 release's system image is made without it, so it is
# here, /hakios/fw/connsys, where bsd/dev/mtk_connsys.c looks after /fw/connsys (1.5 MB; connsys_step=5 and on)
rm -rf $T/fw/connsys
if [ -d "$RN/../kernel/deps/build/connsys-fw/firmware" ]; then
	mkdir -p $T/fw/connsys
	cp "$RN"/../kernel/deps/build/connsys-fw/firmware/* $T/fw/connsys/
	echo "  fw/connsys: $(ls -1 $T/fw/connsys | wc -l | tr -d ' ') files"
fi
cd $DR/userland
# the firmware directories (fw, fw-wifi-..., fw-audio-...) stay uncompressed: the kernel reads them as they lie
plain=(); for d in "$T"/fw*/; do if [ -d "$d" ]; then plain+=(--plain "$(basename "$d")"); fi; done
python3 ../userland/tools/apfs/mkapfs.py -z "${plain[@]}" -o $REL/root26-sys.apfs hakios=$T | tail -1
ls -la $REL/root26-sys.apfs
