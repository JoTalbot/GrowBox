# Home Assistant

Два способа. Можно оба сразу.

## 1. REST прямо сейчас (прошивка 6.3.1, без MQTT)

Коробка в LAN: `http://192.168.1.11/api/data` или `http://growbox.local/api/data`.

1. В `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

2. Скопируй [`homeassistant/growbox.yaml`](../homeassistant/growbox.yaml) в `/config/packages/growbox.yaml`.
3. Если IP другой — поправь его в файле.
4. Developer Tools → YAML → Check + Restart.

Появятся сенсоры климата/почвы и `rest_command` на полив и стадии.

## 2. MQTT Discovery (прошивка 6.3.2+)

Нужен брокер Mosquitto в HA (Add-on **Mosquitto broker**).

1. Создай пользователя MQTT (или используй логин аддона).
2. На дашборде коробки блок **Home Assistant (MQTT)**:
   - брокер: IP Home Assistant (например `192.168.1.10`) или `homeassistant.local`
   - порт `1883`
   - логин / пароль
   - галочка **Включить MQTT Discovery** → Сохранить
3. Бейдж MQTT → **ONLINE**.
4. В HA: Настройки → Устройства → **GrowBox**.

Сущности: температура, RH, VPD, почва, стадии, режимы реле (auto/on/off), кнопки полива.

Discovery-топики: `homeassistant/+/growbox_<DeviceID>/#`  
Состояние: `growbox/<DeviceID>/state`
