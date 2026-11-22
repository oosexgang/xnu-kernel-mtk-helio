# Darwin 27 на телефоне с MediaTek Helio: что сделано и как повторить

Эта страница по-русски и по шагам: что проект уже умеет на первом телефоне (MT6768, Helio G85, Mali-G52)
и как с Mac дойти до домашнего экрана Darwin 27 на телефоне, у которого есть пакет в ветке `devices`.
Английские страницы подробнее: [building.md](building.md) — те же команды, [darwin27-boot.md](darwin27-boot.md)
— как всё поднималось, запуск за запуском, [troubleshooting.md](troubleshooting.md) — что значит сообщение
в журнале.

## Что сделано

- **Ядро.** Открытое ядро Apple xnu-12377.121.6 (ядро 27-го выпуска) с драйверами MediaTek: eMMC (чтение
  через DMA), клавиши и частоты, аккумулятор, Mali-G52, CONNSYS (Wi-Fi и Bluetooth), сенсорный хаб с
  акселерометром, камера, сенсорный экран.
- **Система.** Пользовательское окружение релиза 27.2 (24B119) в том виде, в каком его выпускает Apple,
  кроме аутентификации указателей: её у этого процессора нет, и она переписывается в обычные инструкции.
  Домашний экран, шторка, Пункт управления, приложения, жесты пальцем.
- **Сеть и радио.** Wi-Fi подключается к заданной сети (DHCP, DNS, HTTP), «Настройки» показывают список
  сетей; Bluetooth: контроллер, bluetoothd, CoreBluetooth включён и сканирует.
- **Metal на Mali.** Собственная библиотека пишет дескрипторы заданий GPU сама; шейдеры CoreAnimation
  переводятся из Apple AIR в NIR и компилируются компилятором Bifrost из Mesa. Стадии, нужные оконному
  серверу, заранее собираются на Mac (кэш стадий), а то, чего в кэше нет, телефон компилирует примерно
  за 60 мс. Перелистывание экранов — 25–29 кадров в секунду, шторка и Пункт управления — до 36.
- **Чего пока нет.** Размытие под меню получается плоским зелёным; первые кадры открытия приложения
  упираются в GPU; панель Bluetooth в «Настройках» без текста до нового системного образа; Camera.app;
  `/private/var` не сохраняется между запусками.

## Что понадобится

| | |
| --- | --- |
| Телефон | разблокированный загрузчик, root (Magisk), слот `a`, отладка по USB разрешена для этого компьютера |
| Раздел под систему | на первом телефоне `xnusys`, 4,3 ГБ, вырезан из `userdata` (`tools/gpt-carve.py` пишет новую таблицу **в файл**; записать её на телефон — ваш шаг) |
| Ваш рутованный загрузочный образ | `backup/boot_a_magisk.img`: тот, что стоит в `boot_a`, когда грузится Android. Загрузчик пакуется в него. Никогда не попадает в репозиторий |
| Mac | Intel, macOS 14, Command Line Tools; adb и fastboot в `PATH`; Python 3.9+, meson, ninja, bison, flex; 7-Zip 23+ (`7zz`) |
| SDK | `iPhoneOS16.5.sdk` в `$THEOS/sdks` (github.com/theos/sdks) |
| IPSW релиза 27.2 | распакованный в один каталог `REL`: `rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/` ([darwin27-boot.md](darwin27-boot.md#the-kernel-and-the-build) — как) |
| Прошивки телефона | для Wi-Fi и Bluetooth — файлы CONNSYS в `kernel/deps/build/connsys-fw/firmware`; для тача — `fw-touch/hdl_firmware.img`. Берутся с вашего же телефона |

## Путь назад — до первой записи

Каждый запуск переписывает `boot_a`. Если `boot_a` сломан, fastboot может не подняться, и тогда спасает
только режим загрузки boot ROM (mtkclient: `mtk.py w boot_a backup/boot_a_magisk.img`). **Проверьте, что
до него можно добраться, до первой записи.** Слот `b` ничто здесь не трогает никогда.

## Шаги

```sh
export HAKIOS_SERIAL=<серийный номер телефона из adb devices>   # только в окружении, никогда в файле
export REL=$PWD/build/ios27/ios27                                # распакованный IPSW

make check                        # всё, что проверяется без телефона
tools/device.py add x6826b        # пакет телефона: из него собираются таблица ядра и загрузчик

# ядро (около 4 минут, когда зависимости на месте)
kernel/assemble.sh ../xnu-12377-hakios
kernel/deps/setup.sh              # LLVM 19.1.7, mig 138, заголовки iig из SDK macOS 26.5
kernel/build.sh -j8 -k            # -k нужен: одна безобидная ошибка (lldbmacros) завершает make с кодом 2
tools/get-zig.sh                  # компоновщик для загрузчика

# малый корень прежней системы (20F66): с него начинается малый образ 27-го выпуска
darwin/system/fetch.sh            # файлы системы 20F66 с сервера Apple, около 30 минут
userland/build.sh

# образы 27-го выпуска
darwin/mtk27.sh env
darwin/tools/pacstrip cache "$REL/dyld-strip" "$REL"/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py "$REL/dyld-strip" STORED COMPUTED   # два значения сборки: где их взять, сказано в начале скрипта
darwin/mtk27.sh all               # Mesa для arm64e (в первый раз 10–20 минут), библиотеки, Metal, образы
darwin/mtk27.sh mali              # нативная библиотека Mali

# кэш стадий: шейдеры оконного сервера, собранные на Mac
userland/tools/air/mk-host-mac-llvm.sh                 # Mesa для этого Mac, один раз
export MESA_HOST_BUILD=$PWD/build/mesa-host-mac-llvm/build
userland/tools/rwacache.sh build/rwa-cache.bin
RWA_CACHE_BIN=$PWD/build/rwa-cache.bin RC_JOBS=$PWD/userland/jobs/rc.jobs.nativeui darwin/mtk27.sh root

# телефон
HAKIOS_PAYLOAD=flash tools/run/system-flash.sh "$REL/system27-prune.apfs"   # раз на образ; Android должен быть загружен
HAKIOS_RAMDISK="$REL/root26-sys.apfs" tools/run/mkpayload.sh               # каждый запуск: ядро + малый образ
HAKIOS_PAYLOAD=flash XNU_SECS=300 XNU_LOG_OUT=logs/run1.log XNU_DEFCONFIG=phone_darwin27 tools/run/xnu-cycle.sh
```

Аргументы загрузки берутся из `arch/arm64/configs/phone_darwin27_defconfig`: по одному на строку, у каждого
сказано, что он делает. `tools/defconfig.py phone_darwin27` печатает их так, как их получит ядро, а
`make defconfigs` перечисляет остальные виды запуска. Аргументы из `XNU_ARGS` заменяют одноимённые из
defconfig или добавляются к ним (`XNU_ARGS="cpus=4 mali_mhz=800"`).

Последняя команда собирает загрузчик, пакует его вместе с ядром Android в `build/boot_a.img`, пишет
нагрузку (ядро и малый образ) в последние 96 МиБ системного раздела и читает её обратно, прошивает
`boot_a`, ждёт Android, просит у него одну загрузку XNU и забирает журнал, когда Android вернётся.
Телефон возвращается в Android сам через `XNU_SECS` секунд.

С радио и акселерометром запускайте с `XNU_DEFCONFIG=phone_radios`, а к шагу `root` добавьте
`JOBCACHE=1 ALLOW="wifi WiFi"`; сеть, к которой подключаться, — `fw-wifi/network` (имя,
затем пароль; в репозиторий не попадает никогда). Камера — `XNU_DEFCONFIG=phone_camera` (`mtkcam=5`), без CONNSYS: у них общий
канал PMIC. С 27-м выпуском никогда не передавайте `aks_keys=1`: launchd сразу перезагружает телефон.

## Как понять, что всё получилось

В `logs/run1.log`, по порядку:

- `boot-args: ...` с вашими аргументами, `board: x6826b (...)` с функциями платы;
- `Darwin Kernel Version 12377.121.6`;
- проверки нашего init: `=== 36 checks, 0 failed ===`;
- каждые 5 секунд `mali: GPU busy N% of 5 s`;
- каждые 30 секунд строки нативной библиотеки `rwn backboardd: 30s: ...`: сколько стадий взято из кэша и
  сколько собрано на телефоне, команд-буферы по секундам, `fragment jobs N checked, 0 failed`;
- в конце `=== end of XNU log ===` и `starting Android`.

Журнал хранит начало и последние 256 КБ, середина длинного запуска теряется. Что важно, программа
говорит строкой, начинающейся с `!!`: такие строки ядро сохраняет для компьютера отдельно (до 64 КБ).
Картинки экрана: в списке заданий `fbgrab -keep ИМЯ`, на Mac — `python3 tools/log/fbjpg.py logs/run1.log
shot.jpg ИМЯ`.

## Если телефон не вернулся

Зависшее ядро стоит одного выключения питания: образ грузит Android, если XNU не попросили, и просят
его ровно один раз. Если сломан сам `boot_a` — режим загрузки boot ROM и mtkclient (см. выше).

## Куда дальше

- Свой телефон: [porting.md](porting.md) — порядок работы и проверка каждого шага.
- Драйвер для детали, у которой его нет: [writing-a-driver.md](writing-a-driver.md).
- Где сейчас Metal и стекло: [metal-glass.md](metal-glass.md).
- Слова: [glossary.md](glossary.md).
