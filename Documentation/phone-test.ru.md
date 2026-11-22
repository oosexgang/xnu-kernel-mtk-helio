# Проверка на телефоне

План одного дня с телефоном: какие прогоны в каком порядке, что каждый должен сказать в журнале и что прислать
обратно, если что-то не так. Команды — те же, что в [building.md](building.md); здесь — порядок и признаки успеха.

Каждый прогон переписывает `boot_a` (загрузчик, упакованный в ваш рутованный образ) и хвост системного раздела
(ядро и малый образ, последние 96 МиБ). Слот `b` не трогается никогда. Телефон сам возвращается в Android через
`XNU_SECS` секунд и отдаёт журнал через pstore. Зависшее ядро стоит одного выключения питания: образ грузит Android,
если XNU не попросили, а просят его ровно один раз.

## 0. Подготовка

```sh
make check                                   # всё, что проверяется без телефона
tools/device.py add x6826b                   # пакет телефона: arch/arm64/boot/dts/x6826b
kernel/assemble.sh ../xnu-12377-hakios && kernel/build.sh -j8 -k
make defconfigs                              # какие бывают прогоны
```

Образы 27-го выпуска собираются по [building.md](building.md): `darwin/mtk27.sh all`, `mali`, кэш стадий, `root`.
Системный образ пишется на телефон один раз на образ (`tools/run/system-flash.sh`), малый — с каждым прогоном:

```sh
export HAKIOS_SERIAL=<серийный номер из adb devices>     # только в окружении, никогда в файле
HAKIOS_RAMDISK="$REL/root26-sys.apfs" tools/run/mkpayload.sh
```

Перед первой записью убедитесь, что до режима загрузки boot ROM (mtkclient) можно добраться: это путь назад, если
`boot_a` окажется сломан.

## 1. Прогоны по порядку

Каждый следующий имеет смысл, только если прошёл предыдущий. Общая часть команды:

```sh
HAKIOS_PAYLOAD=flash XNU_SECS=300 XNU_LOG_OUT=logs/ИМЯ.log XNU_DEFCONFIG=ИМЯ tools/run/xnu-cycle.sh
```

| № | `XNU_DEFCONFIG` | Список заданий (`RC_JOBS` шага `root`) | Что должно быть в журнале | Если нет |
| --- | --- | --- | --- | --- |
| 1 | `phone_darwin27` | `rc.jobs.nativeui` | `board: x6826b (...)` с функциями платы; `Darwin Kernel Version 12377.121.6`; `=== 36 checks, 0 failed ===`; раз в 30 секунд `rwn backboardd: 30s: fragment jobs N checked, 0 failed`; в конце `=== end of XNU log ===` и `starting Android` | Последние строки журнала: на какой из этих строк он остановился |
| 2 | `phone_radios` | `rc.jobs.net` (Wi-Fi), `rc.jobs.bluetooth`; шаг `root` с `JOBCACHE=1 ALLOW="wifi WiFi"` | `connsys: step 17 ...: association accepted`, затем `EAPOL ...: message 3, answered; the keys go in`; `net_test: tcp example.com port 80: connected in ...`; `connsys: bt control connected by bluetoothd`; `hakios: orientation: from kern.hakios.accel` | Строки `connsys: step 17` целиком: в них сказано, на каком шаге подключения он остановился |
| 3 | `phone_camera` | `rc.jobs.camraw` | `mtkcam: /dev/hakios-camera opened by camera_test ...`; `!!camera_test: 30 frames of 30 (0 skipped)` | Строки `mtkcam:` и `!!camera_test:` |
| 4 | `phone_bench` | `rc.jobs.nativeui` | Строки `!!rwn backboardd: 30s:` — `GPU busy N%`, `command buffers each second: ...`; `stage cache: N stages taken from it, M compiled` | Журнал целиком: по нему видно, где кадры просели |

Камеру и радио вместе не запускайте: у них общий канал PMIC. С 27-м выпуском никогда не передавайте
`aks_keys=1`: launchd сразу перезагружает телефон.

Аргументы прогона можно поменять, не трогая defconfig: `XNU_ARGS` заменяет одноимённые аргументы или добавляет
новые (`XNU_ARGS="cpus=4 mali_mhz=800"`). Что получит ядро, печатает `tools/defconfig.py ИМЯ`.

## 2. Что прислать

- Журнал прогона (`logs/ИМЯ.log`) целиком. Он хранит начало и последние 256 КБ; всё важное программы говорят
  строками `!!`, и такие строки приходят целиком, до 64 КБ.
- Снимки экрана, если в списке заданий есть `fbgrab -keep ИМЯ`: `python3 tools/log/fbjpg.py logs/ИМЯ.log
  снимок.jpg ИМЯ`.
- Что было видно на экране и что вы делали пальцами — словами.

Не присылайте содержимое `fw-wifi/network` (имя и пароль сети) и серийный номер телефона.

## 3. Если что-то не так

| Признак | Что это |
| --- | --- |
| В pstore только `starting Android` | Загрузчик не увидел просьбы загрузить XNU. Запустите ещё раз |
| `payload: ITS SUM IS NOT ITS HEADER'S: not used` | Нагрузка в хвосте раздела повреждена: запись прервалась. `tools/run/payload-flash.sh` |
| `xnu-cycle.sh: the phone says it is "...", the board x6826b is "Infinix-X6826B": not flashing it` | Подключён другой телефон: скрипт ничего не записал |
| `defconfig.py: no defconfig ...` | Опечатка в `XNU_DEFCONFIG`: `make defconfigs` перечисляет имена. Ничего не собрано и не записано |
| Значки и текст слишком крупные | В `XNU_ARGS` без defconfig нет `fbscale=2 fbfit=3` |

Остальное — в [troubleshooting.md](troubleshooting.md).
