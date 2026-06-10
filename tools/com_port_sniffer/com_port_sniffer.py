#!/usr/bin/env python3
"""
Пассивный сниффер COM-порта (только приём, без отправки).

Работает через драйвер HHD hhdserial64.sys — тот же, что использует
Device Monitoring Studio. Позволяет видеть трафик порта, занятого другой программой.

Запуск:
    python com_port_sniffer.py
    python com_port_sniffer.py COM5

Требования:
    pip install -r requirements.txt
    Установленный драйвер hhdserial64.sys (идёт с Device Monitoring Studio)
    COM-библиотека SPMC hhdspmc.dll (Serial Port Monitoring Control SDK от HHD)
"""

from __future__ import annotations

import queue
import sys
import threading
from datetime import datetime
from pathlib import Path
from tkinter import (
    BOTH,
    END,
    DISABLED,
    NORMAL,
    Button,
    Frame,
    Label,
    Scrollbar,
    StringVar,
    Text,
    Tk,
    ttk,
    filedialog,
    messagebox,
)

from hhd_backend import (
    HhdComSniffer,
    SnifferFrame,
    driver_installed,
    fallback_ports_from_pyserial,
    format_ascii,
    format_hex,
    is_admin,
    list_serial_devices,
    spmc_available,
)

MAX_LOG_LINES = 10000


class ComPortSnifferApp:
    def __init__(self, default_port: str = ""):
        self.root = Tk()
        self.root.title("COM Port Sniffer — пассивный мониторинг")
        self.root.geometry("980x640")
        self.root.minsize(760, 480)

        self.port_var = StringVar(value=default_port)
        self.status_var = StringVar(value="Готов")
        self.filter_var = StringVar(value="Все")
        self.autoscroll_var = StringVar(value="1")

        self._sniffer: HhdComSniffer | None = None
        self._frame_queue: queue.Queue[SnifferFrame] = queue.Queue()
        self._log_lines = 0
        self._all_frames: list[tuple[str, str]] = []

        self._build_ui()
        self._refresh_status()
        self._refresh_ports()
        self.root.after(100, self._poll_frames)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        top = Frame(self.root, padx=8, pady=8)
        top.pack(fill=BOTH)

        Label(top, text="COM-порт:").pack(side="left")
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=14, state="readonly")
        self.port_combo.pack(side="left", padx=(4, 8))

        Button(top, text="Обновить", command=self._refresh_ports).pack(side="left", padx=2)
        self.start_btn = Button(top, text="Начать сниффинг", command=self._start_sniff, width=16)
        self.start_btn.pack(side="left", padx=8)
        self.stop_btn = Button(top, text="Остановить", command=self._stop_sniff, width=12, state=DISABLED)
        self.stop_btn.pack(side="left", padx=2)

        Label(top, text="Фильтр:").pack(side="left", padx=(16, 4))
        filter_box = ttk.Combobox(
            top,
            textvariable=self.filter_var,
            values=["Все", "RX", "TX", "IOCTL", "INFO"],
            width=8,
            state="readonly",
        )
        filter_box.pack(side="left")
        filter_box.bind("<<ComboboxSelected>>", lambda _e: self._rebuild_log())

        Button(top, text="Очистить", command=self._clear_log).pack(side="right", padx=2)
        Button(top, text="Сохранить...", command=self._save_log).pack(side="right", padx=2)

        info = Frame(self.root, padx=8)
        info.pack(fill=BOTH, pady=(0, 4))
        self.info_label = Label(info, text="", anchor="w", justify="left", fg="#333")
        self.info_label.pack(fill=BOTH)

        log_frame = Frame(self.root, padx=8, pady=4)
        log_frame.pack(fill=BOTH, expand=True)

        self.log_text = Text(log_frame, wrap="none", font=("Consolas", 10))
        scroll_y = Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        scroll_x = Scrollbar(log_frame, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=scroll_y.set, xscrollcommand=scroll_x.set)

        self.log_text.grid(row=0, column=0, sticky="nsew")
        scroll_y.grid(row=0, column=1, sticky="ns")
        scroll_x.grid(row=1, column=0, sticky="ew")
        log_frame.grid_rowconfigure(0, weight=1)
        log_frame.grid_columnconfigure(0, weight=1)

        self.log_text.tag_configure("RX", foreground="#006400")
        self.log_text.tag_configure("TX", foreground="#00008B")
        self.log_text.tag_configure("IOCTL", foreground="#8B4513")
        self.log_text.tag_configure("INFO", foreground="#555555")

        status_bar = Label(self.root, textvariable=self.status_var, anchor="w", relief="sunken", padx=8)
        status_bar.pack(fill=BOTH, side="bottom")

    def _refresh_status(self) -> None:
        lines = []
        if driver_installed():
            lines.append("Драйвер hhdserial64.sys: установлен")
        else:
            lines.append("Драйвер hhdserial64.sys: НЕ найден (нужен Device Monitoring Studio или SPMC)")

        ok, msg = spmc_available()
        if ok:
            lines.append("SPMC API: доступен")
        else:
            lines.append(f"SPMC API: {msg}")

        if not is_admin():
            lines.append("Подсказка: для установки драйвера может потребоваться запуск от администратора.")

        self.info_label.config(text="\n".join(lines))

    def _refresh_ports(self) -> None:
        ports: list[str] = []
        try:
            devices = list_serial_devices()
            for dev in devices:
                label = dev.port or dev.name
                if dev.present:
                    ports.append(label)
                else:
                    ports.append(f"{label} (отключён)")
        except Exception:
            ports = list(fallback_ports_from_pyserial())
            if ports:
                self.status_var.set(
                    "Список портов из pyserial (SPMC недоступен — сниффинг не запустится)"
                )
            else:
                self.status_var.set("Не удалось получить список портов")

        clean_ports = []
        for item in ports:
            port = item.split()[0]
            if port.upper().startswith("COM"):
                clean_ports.append(port.upper())

        if not clean_ports:
            clean_ports = ["COM1"]

        self.port_combo["values"] = clean_ports
        current = self.port_var.get().strip().upper()
        if current and current in clean_ports:
            self.port_var.set(current)
        elif clean_ports:
            self.port_var.set(clean_ports[0])

    def _on_frame(self, frame: SnifferFrame) -> None:
        self._frame_queue.put(frame)

    def _poll_frames(self) -> None:
        updated = False
        while True:
            try:
                frame = self._frame_queue.get_nowait()
            except queue.Empty:
                break
            self._append_frame(frame)
            updated = True

        if updated and self.autoscroll_var.get() == "1":
            self.log_text.see(END)

        self.root.after(50, self._poll_frames)

    def _frame_line(self, frame: SnifferFrame) -> tuple[str, str]:
        ts = frame.timestamp.strftime("%H:%M:%S.%f")[:-3]
        if frame.data:
            line = (
                f"[{ts}] {frame.direction:5} {len(frame.data):4} байт  "
                f"HEX: {format_hex(frame.data)}  ASCII: {format_ascii(frame.data)}"
            )
        else:
            detail = frame.detail or "(событие)"
            line = f"[{ts}] {frame.direction:5} {detail}"
        return frame.direction, line

    def _append_frame(self, frame: SnifferFrame) -> None:
        direction, line = self._frame_line(frame)
        self._all_frames.append((direction, line))
        if len(self._all_frames) > MAX_LOG_LINES:
            self._all_frames = self._all_frames[-MAX_LOG_LINES:]

        filt = self.filter_var.get()
        if filt != "Все" and direction != filt:
            return

        self._insert_line(direction, line)

    def _insert_line(self, direction: str, line: str) -> None:
        self.log_text.insert(END, line + "\n", direction)
        self._log_lines += 1
        if self._log_lines > MAX_LOG_LINES:
            self.log_text.delete("1.0", "2.0")
            self._log_lines = MAX_LOG_LINES

    def _rebuild_log(self) -> None:
        filt = self.filter_var.get()
        self.log_text.delete("1.0", END)
        self._log_lines = 0
        for direction, line in self._all_frames:
            if filt == "Все" or direction == filt:
                self._insert_line(direction, line)
        self.log_text.see(END)

    def _clear_log(self) -> None:
        self._all_frames.clear()
        self.log_text.delete("1.0", END)
        self._log_lines = 0

    def _save_log(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Сохранить лог",
            defaultextension=".txt",
            filetypes=[("Текстовый файл", "*.txt"), ("Все файлы", "*.*")],
        )
        if not path:
            return
        Path(path).write_text("\n".join(line for _, line in self._all_frames), encoding="utf-8")
        self.status_var.set(f"Лог сохранён: {path}")

    def _start_sniff(self) -> None:
        port = self.port_var.get().strip().split()[0].upper()
        if not port.startswith("COM"):
            messagebox.showerror("Ошибка", "Выберите корректный COM-порт.")
            return

        ok, msg = spmc_available()
        if not ok:
            messagebox.showerror(
                "SPMC недоступен",
                msg
                + "\n\n"
                "Для пассивного сниффинга занятого порта нужна COM-библиотека "
                "Serial Port Monitoring Control (hhdspmc.dll) от HHD Software.\n"
                "Драйвер hhdserial64.sys у вас уже установлен вместе с Device Monitoring Studio.",
            )
            return

        self._stop_sniff()
        self._sniffer = HhdComSniffer(self._on_frame)
        try:
            self._sniffer.start(port)
        except Exception as exc:  # noqa: BLE001
            self._sniffer = None
            messagebox.showerror("Ошибка запуска", str(exc))
            return

        self.start_btn.config(state=DISABLED)
        self.stop_btn.config(state=NORMAL)
        self.port_combo.config(state=DISABLED)
        self.status_var.set(f"Сниффинг {port} — пассивный режим (только просмотр)")

    def _stop_sniff(self) -> None:
        if self._sniffer is not None:
            self._sniffer.stop()
            self._sniffer = None
        self.start_btn.config(state=NORMAL)
        self.stop_btn.config(state=DISABLED)
        self.port_combo.config(state="readonly")
        self.status_var.set("Остановлено")

    def _on_close(self) -> None:
        self._stop_sniff()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def main() -> int:
    default_port = sys.argv[1].upper() if len(sys.argv) > 1 else ""
    app = ComPortSnifferApp(default_port=default_port)
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
