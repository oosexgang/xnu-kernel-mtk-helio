# Быстрый старт

Самый короткий путь от клона до первого прогона на телефоне, у которого есть пакет в ветке `devices`. Каждая
команда подробнее — в [darwin27.ru.md](darwin27.ru.md) (по-русски) и [building.md](building.md); где что лежит —
[layout.md](layout.md).

## 1. Без телефона

```sh
git clone https://github.com/oosexgang/xnu-kernel-mtk-helio.git
cd xnu-kernel-mtk-helio
make check                        # линт, платы, defconfig'и, тесты на этой машине
tools/device.py available         # для каких телефонов есть пакеты
tools/device.py add x6826b        # пакет телефона: arch/arm64/boot/dts/x6826b (git его не видит)
tools/boards.py status x6826b     # что у платы включено и что можно включить следующим
make defconfigs                   # какие бывают прогоны
```

## 2. Ядро и загрузчик

```sh
kernel/assemble.sh ../xnu-12377-hakios   # дерево: выпуск Apple + наши файлы + патчи, рядом с клоном
kernel/deps/setup.sh                     # LLVM 19.1.7, mig 138, заголовки iig (один раз)
kernel/build.sh -j8 -k                   # около 4 минут; -k нужен (безобидная ошибка lldbmacros)
tools/get-zig.sh                         # компоновщик для загрузчика (один раз)
make loader                              # arch/arm64/boot/build.sh: загрузчик для этого телефона
```

Наши файлы ядра лежат по каталогам, как у Linux: драйверы в `drivers/board/`, APFS в `fs/apfs/`, платформа в
`arch/arm64/kernel/`. Правите файл там, `kernel/build.sh` сам переносит его в дерево.

## 3. Образы

Нужен распакованный IPSW выпуска 27.2 в каталоге `REL` и SDK в `$THEOS/sdks`: что именно и откуда — в
[darwin27.ru.md](darwin27.ru.md#что-понадобится).

```sh
darwin/system/fetch.sh && userland/build.sh        # малый корень прежней системы, с него начинается новый
darwin/mtk27.sh env && darwin/mtk27.sh all && darwin/mtk27.sh mali
RC_JOBS=$PWD/userland/jobs/rc.jobs.nativeui darwin/mtk27.sh root
```

Кэш стадий (шейдеры оконного сервера, собранные на Mac) делает открытие приложений быстрее; как его собрать —
в [building.md](building.md).

## 4. Телефон

```sh
export HAKIOS_SERIAL=<серийный номер из adb devices>      # только в окружении, никогда в файле
HAKIOS_PAYLOAD=flash tools/run/system-flash.sh "$REL/system27-prune.apfs"   # раз на образ
HAKIOS_RAMDISK="$REL/root26-sys.apfs" tools/run/mkpayload.sh               # каждый прогон
HAKIOS_PAYLOAD=flash XNU_SECS=300 XNU_LOG_OUT=logs/run1.log XNU_DEFCONFIG=phone_darwin27 tools/run/xnu-cycle.sh
```

Пишутся только `boot_a` и системный раздел, слот `b` — никогда. Перед первой записью проверьте, что до режима
загрузки boot ROM (mtkclient) можно добраться. Телефон возвращается в Android сам; журнал — в `logs/run1.log`.

## 5. Дальше

- Порядок прогонов и что в журнале значит успех: [phone-test.ru.md](phone-test.ru.md).
- Свой телефон: [porting.md](porting.md).
- Что значит сообщение в журнале: [troubleshooting.md](troubleshooting.md).
