# Удалённый агент за NAT

ESP32 не принимает входящие соединения. Он сам ходит наружу:

1. **ntfy.sh** — живые команды и телеметрия (основной канал)
2. **version.json** в этом репозитории — номер прошивки + URL `.bin`
3. **inbox.json** — запасной канал (GitHub raw кэширует, не для срочного OTA)
4. **Telegram** — `/ota <url>`, `/ntfy`, `/remotekey`, `/remoteon`

## Первый запуск на коробке

Локально или в Telegram после прошивки v6.3:

```
/ntfy gb-ck-ВАШ-СЕКРЕТ
/remotekey rk_ВАШ-КЛЮЧ
/remoteon
/pull
```

Топик и ключ не коммитить. Они живут в NVS контроллера и в локальном `.remote.env`.

## Команды агента (ntfy, plaintext)

```
ping KEY
status KEY
ota https://.../firmware.bin KEY
flash KEY
checkota KEY
stage veg|bloom|dry KEY
mode light:on KEY
water 0 KEY
auto KEY
reboot KEY
```

Либо JSON: `{"cmd":"ping","arg":"","key":"KEY"}`.

Свои статусы ESP32 публикует с заголовком `Title: status` — сам их игнорирует.

## Скрипты

```bash
cp .remote.env.example .remote.env   # заполни NTFY_TOPIC и REMOTE_KEY
python3 tools/remote_cmd.py ping
python3 tools/remote_cmd.py status
python3 tools/remote_cmd.py ota https://github.com/JoTalbot/GrowBox/releases/download/v6.3/firmware.bin
python3 tools/remote_status.py
```

## Сборка .bin для OTA

- GitHub → Actions → **Build firmware** → artifact `firmware-bin` (`GrowBox-v6.3.bin` + сервис)
- или локально: `pio run -e esp32dev` и `pio run -d firmware/Growbox_OTA_Service -e ota_service`
- тег `v6.3.1` создаёт GitHub Release с обоими `.bin`

Положи бинарник в GitHub Release и пропиши URL в `firmware/remote/version.json`.  
OTA принимает и `https://`, и `http://`. 
Команда `flash` или галочка «Автопрошивка» заставит коробку скачать и перезалиться.
