#!/usr/bin/env python3
"""Проверка готовности системы к пассивному сниффингу COM-порта."""

from hhd_backend import (
    driver_installed,
    fallback_ports_from_pyserial,
    is_admin,
    list_serial_devices,
    spmc_available,
)


def main() -> int:
    print("=== Проверка COM Port Sniffer ===\n")

    print(f"Администратор: {'да' if is_admin() else 'нет'}")
    print(f"Драйвер hhdserial64.sys: {'установлен' if driver_installed() else 'НЕ найден'}")

    ok, msg = spmc_available()
    print(f"SPMC API: {'OK' if ok else 'НЕТ'}")
    if not ok:
        print(f"  -> {msg}")

    print("\n--- Порты ---")
    if ok:
        for dev in list_serial_devices():
            state = "подключён" if dev.present else "отключён"
            print(f"  {dev.port or dev.name:8}  {state:12}  {dev.description}")
    else:
        ports = list(fallback_ports_from_pyserial())
        if ports:
            print("  (через pyserial, только свободные порты)")
            for port in ports:
                print(f"  {port}")
        else:
            print("  Порты не найдены")

    print()
    if driver_installed() and ok:
        print("Система готова. Запуск: python com_port_sniffer.py")
        return 0

    if driver_installed() and not ok:
        print(
            "Драйвер есть, но нет SPMC SDK.\n"
            "Установите Serial Port Monitoring Control от HHD Software —\n"
            "он использует уже установленный драйвер hhdserial64.sys."
        )
        return 1

    print("Установите Device Monitoring Studio или SPMC (вместе с драйвером).")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
