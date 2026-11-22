#!/bin/bash
# Make the images of a run with more than the plain boot (userland/build.sh, then tools/run/mkpayload.sh):
#
#   tools/run/image.sh wifi        the system's Wi-Fi daemon, Settings' Wi-Fi page; fw/network (your network's name and
#                                  passphrase, fw-wifi/network) joined by the kernel. Boot args: connsys_step=17 connsys_wake=0
#   tools/run/image.sh bluetooth   ... and the key store (bluetoothd keeps its identity in the keychain), bluetoothd and
#                                  BlueTool with our library, src/bt_test.m's scan. Boot args: also connsys_bt=2 aks_keys=1
#   tools/run/image.sh metal       the GPU for CoreAnimation: Mesa's panfrost (userland/tools/mk-mesa.sh, built when missing) and the
#                                  Metal device; with the payload in boot_a, the cuts its 32 MB need: libOSMesa lives in the system
#                                  image and the small one has a link to it, the test programs and two unused libraries are
#                                  left out, our libraries are stripped (init then says 4 checks failed). With HAKIOS_PAYLOAD=flash
#                                  (the payload in the system partition) nothing is cut. Boot args: metal=1 fb_queue=1 fb_flip=1.
#                                  QC_METALLIB=FILE: QuartzCore's shaders in AIR (userland/tools/qc-simlib.py makes it from the
#                                  simulator's QuartzCore; never in the repository)
#
# All three need the phone's CONNSYS firmware in kernel/deps/build/connsys-fw/firmware for Wi-Fi and Bluetooth, and
# rebuild the system image (SYSTEM=1): write it with tools/run/system-flash.sh before the run. RC_JOBS=FILE (in userland/)
# is the run's job list: userland/jobs/rc.jobs.check, rc.jobs.bluetooth, rc.jobs.metalcheck are the checks with pictures.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/kernel/env.sh"
mode=${1:?usage: tools/run/image.sh wifi|bluetooth|metal}
cd "$ROOT/userland"
LDID=${LDID:-$(command -v ldid || echo "$ROOT/userland/tools/ldid-codesign.sh")}
case "$mode" in
wifi)
	SYSTEM=1 WIFID=1 RC_JOBS=${RC_JOBS:-rc.jobs.check} LDID=$LDID ./build.sh ;;
bluetooth)
	mkdir -p obj
	xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 -Wall -framework CoreFoundation -framework IOKit \
		-o obj/aks_restore src/aks_restore.c
	"$LDID" -Ssrc/aks_restore.entitlements obj/aks_restore
	INJECT="/hakios/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system com.apple.WebKit.GPU BlueTool Preferences bluetoothd
/hakios/lib/libhakios-sb.dylib SpringBoard
/hakios/lib/libhakios-media.dylib mediaserverd" \
	SYSTEM=1 BT=1 WIFID=1 RC=rc.launchd.aks RC_JOBS=${RC_JOBS:-rc.jobs.bluetooth} EXTRA="$PWD/obj/aks_restore" \
	MORE_JOBS="com.apple.tccd com.apple.bluetoothd com.apple.BlueTool com.apple.mobile.keybagd com.apple.securityd" \
	LDID=$LDID ./build.sh ;;
metal)
	[ -f ../kernel/deps/build/mesa/out/libOSMesa.8.dylib ] || DRIVERS=softpipe,panfrost ../userland/tools/mk-mesa.sh
	GL=1 METAL=1 SYSTEM=1 WIFID=1 RC=rc.launchd.metal RC_JOBS=${RC_JOBS:-rc.jobs.metalcheck} LDID=$LDID ./build.sh
	# with the payload in the system partition (HAKIOS_PAYLOAD=flash) the image is used as it is: nothing is left out
	if [ "${HAKIOS_PAYLOAD:-}" != flash ]; then
	for b in hello_dyn cputop apfs_mmap_test battery_test camera_avtest camera_test cg_test child cow_test cpu_test font_test \
		gl_test keychain_test metal_drv_test metal_test mg_test record_test shm_test snap_test sound_test surf_test svc_test \
		torch_test vol_ls wifipw_test net_test bt_test; do rm -f root/bin/$b; done
	rm -f root/lib/libhakios-tweaks.dylib root/lib/libhakios-app.dylib
	sed -i.bak -E '/libhakios-(tweaks|app)\.dylib/d' root/etc/inject.conf && rm -f root/etc/inject.conf.bak
	rm -f root/lib/libOSMesa.8.dylib
	ln -s /usr/lib/hakios/libOSMesa.8.dylib root/lib/libOSMesa.8.dylib
	rm -f root/share/pattern.bin                    # (init's 300 KB file test: it then says 4 checks failed)
	for f in root/lib/HakiosMetal.bundle/HakiosMetal root/lib/libhakios-gl.dylib root/lib/libhakios.dylib \
		root/lib/libhakios-sb.dylib root/lib/libhakios-media.dylib; do [ -f "$f" ] && strip -x "$f"; done
	rm -f root.apfs
	python3 ../userland/tools/apfs/mkapfs.py -z --plain fw -o root.apfs hakios=root | tail -1
	fi ;;
*) echo "image.sh: wifi, bluetooth or metal" >&2; exit 1 ;;
esac
"$ROOT/tools/run/mkpayload.sh"
