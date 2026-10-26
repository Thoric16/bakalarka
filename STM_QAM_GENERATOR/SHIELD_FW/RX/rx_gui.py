#!/usr/bin/env python3
"""Serial receiver and message viewer for the QPSK RX firmware."""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
MAX_LOG_LINES = 1000
MAX_MESSAGES = 500
MAX_LINES_PER_UPDATE = 100


class RxGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("QPSK receiver")
        self.geometry("920x620")
        self.serial_port: serial.Serial | None = None
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.lines: queue.Queue[str] = queue.Queue()
        self.port_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Disconnected")
        self.rate_var = tk.StringVar(value="50")
        self.clock_var = tk.StringVar(value="INTERNAL")
        self.debug_var = tk.BooleanVar(value=True)
        self.raw_var = tk.BooleanVar(value=True)
        self._build_ui()
        self.refresh_ports()
        self.after(50, self._drain_serial)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self) -> None:
        controls = ttk.Frame(self, padding=8)
        controls.pack(fill=tk.X)
        ttk.Label(controls, text="Serial port:").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(
            controls, textvariable=self.port_var, width=24, state="readonly"
        )
        self.port_combo.pack(side=tk.LEFT, padx=5)
        ttk.Button(controls, text="Refresh", command=self.refresh_ports).pack(side=tk.LEFT)
        self.connect_button = ttk.Button(
            controls, text="Connect", command=self.toggle_connection
        )
        self.connect_button.pack(side=tk.LEFT, padx=(5, 15))
        ttk.Label(controls, textvariable=self.status_var).pack(side=tk.LEFT)
        ttk.Label(controls, text="Symbol rate:").pack(side=tk.LEFT, padx=(15, 2))
        ttk.Entry(controls, textvariable=self.rate_var, width=7).pack(side=tk.LEFT)
        ttk.Button(controls, text="Set", command=self._set_rate).pack(side=tk.LEFT, padx=2)
        ttk.Label(controls, text="Clock:").pack(side=tk.LEFT, padx=(8, 2))
        ttk.Combobox(
            controls, textvariable=self.clock_var, values=("INTERNAL", "EXTERNAL"),
            state="readonly", width=9,
        ).pack(side=tk.LEFT)
        ttk.Button(controls, text="Apply", command=self._set_clock).pack(side=tk.LEFT, padx=2)
        ttk.Checkbutton(
            controls, text="Firmware debug", variable=self.debug_var,
            command=self._send_debug_setting,
        ).pack(side=tk.LEFT, padx=(15, 0))
        ttk.Checkbutton(
            controls, text="Raw symbol trace", variable=self.raw_var,
            command=self._send_raw_setting,
        ).pack(side=tk.LEFT, padx=5)

        self.messages = ttk.Treeview(
            self,
            columns=("status", "sequence", "type", "length", "raw", "text"),
            show="headings",
        )
        for column, title, width in (
            ("status", "Status", 80),
            ("sequence", "Sequence", 90),
            ("type", "Type", 80),
            ("length", "Bytes", 70),
            ("raw", "Raw hex", 260),
            ("text", "Message", 360),
        ):
            self.messages.heading(column, text=title)
            self.messages.column(column, width=width, anchor=tk.W)
        self.messages.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))
        self.log = tk.Text(self, height=8, state=tk.DISABLED)
        self.log.pack(fill=tk.X, padx=8, pady=(0, 8))

    def refresh_ports(self) -> None:
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])

    def toggle_connection(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            self.disconnect()
            return
        if not self.port_var.get():
            messagebox.showwarning("No serial port", "Select a serial port first.")
            return
        try:
            port = serial.Serial(self.port_var.get(), BAUD_RATE, timeout=0.1)
        except serial.SerialException as error:
            messagebox.showerror("Connection failed", str(error))
            return
        self.serial_port = port
        self.stop_event.clear()
        self.reader_thread = threading.Thread(
            target=self._read_serial, args=(port,), daemon=True
        )
        self.reader_thread.start()
        port.write(b"DEBUG ON\n")
        port.write(b"RAW ON\n")
        port.write(f"RATE {self.rate_var.get()}\n".encode("ascii"))
        port.write(f"CLOCK {self.clock_var.get()}\n".encode("ascii"))
        self.connect_button.configure(text="Disconnect")
        self.status_var.set(f"Connected at {BAUD_RATE} baud")

    def disconnect(self) -> None:
        self.stop_event.set()
        port = self.serial_port
        self.serial_port = None
        if port:
            port.close()
        if self.reader_thread and self.reader_thread is not threading.current_thread():
            self.reader_thread.join(timeout=0.3)
        self.reader_thread = None
        self.connect_button.configure(text="Connect")
        self.status_var.set("Disconnected")

    def _read_serial(self, port: serial.Serial) -> None:
        while not self.stop_event.is_set():
            if not port.is_open:
                return
            try:
                line = port.readline()
            except (OSError, serial.SerialException):
                return
            if line:
                self.lines.put(line.decode("utf-8", errors="replace").strip())

    def _drain_serial(self) -> None:
        for _ in range(MAX_LINES_PER_UPDATE):
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            self._append_log(line)
            if line.startswith("RAW_FRAME "):
                self._insert_raw_frame(line)
            elif line.startswith("MSG "):
                self._insert_message(line)
            elif line.startswith("RX_ERR "):
                self.status_var.set(line)
            elif line.startswith("RX_READY "):
                self.status_var.set(line)
        self.after(50, self._drain_serial)

    def _insert_message(self, line: str) -> None:
        fields = dict(item.split("=", 1) for item in line[4:].split() if "=" in item)
        hex_data = fields.get("hex", "")
        try:
            text = bytes.fromhex(hex_data).decode("utf-8", errors="replace")
        except ValueError:
            text = "<invalid payload>"
        self.messages.insert(
            "", tk.END,
            values=("OK", fields.get("seq", "?"), fields.get("type", "?"),
                    len(hex_data) // 2, hex_data, text),
        )
        message_items = self.messages.get_children()
        if len(message_items) > MAX_MESSAGES:
            self.messages.delete(message_items[0])

    def _insert_raw_frame(self, line: str) -> None:
        fields = dict(item.split("=", 1) for item in line[10:].split() if "=" in item)
        hex_data = fields.get("payload", "")
        try:
            text = bytes.fromhex(hex_data).decode("utf-8", errors="replace")
        except ValueError:
            text = "<invalid payload>"
        self.messages.insert(
            "", tk.END,
            values=("CRC ERR", "?", "?", len(hex_data) // 2, hex_data, text),
        )
        message_items = self.messages.get_children()
        if len(message_items) > MAX_MESSAGES:
            self.messages.delete(message_items[0])

    def _send_debug_setting(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            command = b"DEBUG ON\n" if self.debug_var.get() else b"DEBUG OFF\n"
            try:
                self.serial_port.write(command)
            except serial.SerialException:
                self.disconnect()

    def _send_raw_setting(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            command = b"RAW ON\n" if self.raw_var.get() else b"RAW OFF\n"
            try:
                self.serial_port.write(command)
            except serial.SerialException:
                self.disconnect()

    def _set_rate(self) -> None:
        try:
            rate = int(self.rate_var.get())
        except ValueError:
            messagebox.showwarning("Invalid symbol rate", "Enter an integer from 1 to 1000.")
            return
        if not 1 <= rate <= 1000:
            messagebox.showwarning("Invalid symbol rate", "Enter an integer from 1 to 1000.")
            return
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.write(f"RATE {rate}\n".encode("ascii"))

    def _set_clock(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.write(f"CLOCK {self.clock_var.get()}\n".encode("ascii"))

    def _append_log(self, line: str) -> None:
        self.log.configure(state=tk.NORMAL)
        self.log.insert(tk.END, line + "\n")
        line_count = int(self.log.index("end-1c").split(".")[0])
        if line_count > MAX_LOG_LINES:
            self.log.delete("1.0", f"{line_count - MAX_LOG_LINES}.0")
        self.log.see(tk.END)
        self.log.configure(state=tk.DISABLED)

    def close(self) -> None:
        self.disconnect()
        self.destroy()


if __name__ == "__main__":
    RxGui().mainloop()
