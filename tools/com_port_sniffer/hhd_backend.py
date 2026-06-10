"""
Пассивный сниффер COM-порта через HHD Serial Port Monitoring Control (SPMC).

Использует драйвер hhdserial64.sys (тот же, что ставит Device Monitoring Studio).
Порт может быть занят другой программой — сниффер только слушает трафик.
"""

from __future__ import annotations

import ctypes
import os
import sys
import threading
import time
import winreg
from dataclasses import dataclass
from datetime import datetime
from typing import Callable, Iterable, List, Optional, Tuple

FrameCallback = Callable[["SnifferFrame"], None]

SPMC_PROGIDS = (
    "hhdspmc.SerialMonitor",
    "hhdspmc.SerialMonitor.1.2",
)

SPMC_DLL_CANDIDATES = (
    r"C:\Program Files\HHD Software\Serial Port Monitoring Control\bin\x64\hhdspmc.dll",
    r"C:\Program Files (x86)\HHD Software\Serial Port Monitoring Control\bin\x86\hhdspmc.dll",
)

DMS_DRIVER_PATH = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"),
    "System32",
    "drivers",
    "hhdserial64.sys",
)

_spmc_module_loaded = False


@dataclass(frozen=True)
class SerialDeviceInfo:
    name: str
    port: str
    present: bool
    description: str = ""


@dataclass(frozen=True)
class SnifferFrame:
    timestamp: datetime
    direction: str  # "RX" | "TX" | "IOCTL" | "INFO"
    data: bytes
    detail: str = ""


def driver_installed() -> bool:
    return os.path.isfile(DMS_DRIVER_PATH)


def find_hhdspmc_dll() -> Optional[str]:
    for path in SPMC_DLL_CANDIDATES:
        if os.path.isfile(path):
            return path

    try:
        key = winreg.OpenKey(
            winreg.HKEY_CLASSES_ROOT,
            r"hhdspmc.SerialMonitor\CLSID",
        )
        clsid, _ = winreg.QueryValueEx(key, "")
        server_key = winreg.OpenKey(
            winreg.HKEY_CLASSES_ROOT,
            rf"CLSID\{clsid}\InprocServer32",
        )
        dll_path, _ = winreg.QueryValueEx(server_key, "")
        if dll_path and os.path.isfile(dll_path):
            return dll_path
    except OSError:
        pass
    return None


def _ensure_spmc_typelib() -> str:
    global _spmc_module_loaded

    dll_path = find_hhdspmc_dll()
    if not dll_path:
        raise RuntimeError(
            "hhdspmc.dll не найден. Установите Serial Port Monitoring Control."
        )

    if not _spmc_module_loaded:
        from comtypes.client import GetModule

        GetModule(dll_path)
        _spmc_module_loaded = True
    return dll_path


def _create_serial_monitor():
    import pythoncom
    from comtypes.client import CreateObject

    _ensure_spmc_typelib()
    pythoncom.CoInitialize()
    last_error: Optional[Exception] = None
    for progid in SPMC_PROGIDS:
        try:
            return CreateObject(progid), None
        except Exception as exc:  # noqa: BLE001
            last_error = exc
    return None, last_error


def spmc_available() -> Tuple[bool, str]:
    if not sys.platform.startswith("win"):
        return False, "Только Windows."

    if not driver_installed():
        return (
            False,
            "Драйвер hhdserial64.sys не найден. Установите Device Monitoring Studio "
            "или SPMC (драйвер ставится вместе с ними).",
        )

    if not find_hhdspmc_dll():
        return (
            False,
            "COM-библиотека SPMC (hhdspmc.dll) не найдена. "
            "Установите Serial Port Monitoring Control от HHD Software.",
        )

    factory, err = _create_serial_monitor()
    if factory is None:
        return False, f"Не удалось создать SerialMonitor: {err}"
    return True, "SPMC доступен."


def list_serial_devices() -> List[SerialDeviceInfo]:
    ok, msg = spmc_available()
    if not ok:
        raise RuntimeError(msg)

    import pythoncom

    pythoncom.CoInitialize()
    factory, _ = _create_serial_monitor()
    if factory is None:
        raise RuntimeError("SerialMonitor недоступен.")

    devices = factory.Devices
    result: List[SerialDeviceInfo] = []

    for index in range(int(devices.Count)):
        device = devices.Item(index)
        name = str(getattr(device, "Name", "") or "")
        port = str(getattr(device, "Port", "") or "")
        if not port and name.upper().startswith("COM"):
            port = name
        present = bool(getattr(device, "Present", False))
        description = str(getattr(device, "Description", "") or name)
        result.append(
            SerialDeviceInfo(
                name=name,
                port=port,
                present=present,
                description=description,
            )
        )
    return result


def _safe_bytes_from_com_array(array_obj) -> bytes:
    if array_obj is None:
        return b""
    if isinstance(array_obj, (bytes, bytearray)):
        return bytes(array_obj)
    try:
        size = len(array_obj)
        if size <= 0:
            return b""
        return bytes(int(array_obj[i]) & 0xFF for i in range(size))
    except Exception:  # noqa: BLE001
        pass
    try:
        return bytes(bytearray(array_obj))
    except Exception:  # noqa: BLE001
        return b""


def _com_time_to_datetime(com_time) -> datetime:
    try:
        if hasattr(com_time, "year"):
            return datetime(
                com_time.year,
                com_time.month,
                com_time.day,
                com_time.hour,
                com_time.minute,
                com_time.second,
                com_time.microsecond // 1000 * 1000,
            )
    except Exception:  # noqa: BLE001
        pass
    return datetime.now()


class HhdComSniffer:
    """Пассивный мониторинг одного COM-порта через SPMC."""

    def __init__(self, on_frame: FrameCallback):
        self.on_frame = on_frame
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._start_error: Optional[str] = None
        self._port = ""
        self._monitor = None
        self._events = None

    @property
    def port(self) -> str:
        return self._port

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self, port: str, timeout: float = 10.0) -> None:
        if self.running:
            raise RuntimeError("Сниффер уже запущен.")

        ok, msg = spmc_available()
        if not ok:
            raise RuntimeError(msg)

        self._port = port.strip().upper()
        if not self._port:
            raise ValueError("Не указан COM-порт.")

        self._stop.clear()
        self._ready.clear()
        self._start_error = None
        self._thread = threading.Thread(target=self._worker, name="HhdComSniffer", daemon=True)
        self._thread.start()

        if not self._ready.wait(timeout):
            self.stop()
            raise TimeoutError("Таймаут запуска сниффера.")
        if self._start_error:
            err = self._start_error
            self.stop()
            raise RuntimeError(err)

    def stop(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)
        self._thread = None
        self._monitor = None
        self._events = None

    def _emit(self, frame: SnifferFrame) -> None:
        try:
            self.on_frame(frame)
        except Exception:  # noqa: BLE001
            pass

    def _worker(self) -> None:
        import pythoncom
        from comtypes.client import GetEvents

        pythoncom.CoInitialize()
        try:
            _ensure_spmc_typelib()
            from comtypes.gen.hhdspmcLib import _IMonitoringEvents

            factory, err = _create_serial_monitor()
            if factory is None:
                raise RuntimeError(f"SerialMonitor: {err}")

            monitor = factory.CreateMonitor()
            self._monitor = monitor
            sniffer = self

            class MonitorEvents:
                def OnConnection(self, time, state, name):  # noqa: N802, ARG002
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="INFO",
                            data=b"",
                            detail=f"Подключение: {name} ({state})",
                        )
                    )

                def OnOpen(self, time, name, process_id):  # noqa: N802
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="INFO",
                            data=b"",
                            detail=f"Порт открыт: {name}, PID={process_id}",
                        )
                    )

                def OnClose(self, time):  # noqa: N802
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="INFO",
                            data=b"",
                            detail="Порт закрыт",
                        )
                    )

                def OnRead(self, time, array):  # noqa: N802
                    data = _safe_bytes_from_com_array(array)
                    if not data:
                        return
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="RX",
                            data=data,
                        )
                    )

                def OnWrite(self, time, array):  # noqa: N802
                    data = _safe_bytes_from_com_array(array)
                    if not data:
                        return
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="TX",
                            data=data,
                        )
                    )

                def OnBaudRate(self, time, baud_rate, is_get):  # noqa: N802
                    action = "запрос" if is_get else "установка"
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="IOCTL",
                            data=b"",
                            detail=f"Скорость ({action}): {baud_rate}",
                        )
                    )

                def OnLineControl(self, time, *_args):  # noqa: N802, ARG002
                    sniffer._emit(
                        SnifferFrame(
                            timestamp=_com_time_to_datetime(time),
                            direction="IOCTL",
                            data=b"",
                            detail="Изменение параметров линии",
                        )
                    )

            self._events = GetEvents(monitor, MonitorEvents(), interface=_IMonitoringEvents)
            monitor.Connect(self._port)
            self._ready.set()

            while not self._stop.is_set():
                pythoncom.PumpWaitingMessages()
                time.sleep(0.01)
        except Exception as exc:  # noqa: BLE001
            self._start_error = str(exc)
            self._ready.set()
        finally:
            try:
                if self._monitor is not None:
                    self._monitor.Disconnect()
            except Exception:  # noqa: BLE001
                pass
            pythoncom.CoUninitialize()


def format_hex(data: bytes, max_len: int = 256) -> str:
    chunk = data[:max_len]
    text = chunk.hex(" ").upper()
    if len(data) > max_len:
        text += f" ... (+{len(data) - max_len} байт)"
    return text


def format_ascii(data: bytes, max_len: int = 256) -> str:
    chunk = data[:max_len]
    chars = []
    for byte in chunk:
        chars.append(chr(byte) if 32 <= byte < 127 else ".")
    text = "".join(chars)
    if len(data) > max_len:
        text += f" ... (+{len(data) - max_len} байт)"
    return text


def fallback_ports_from_pyserial() -> Iterable[str]:
    try:
        import serial.tools.list_ports
    except ImportError:
        return []
    return sorted({p.device for p in serial.tools.list_ports.comports()})


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:  # noqa: BLE001
        return False
