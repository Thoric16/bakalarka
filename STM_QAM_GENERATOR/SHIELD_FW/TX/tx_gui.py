#!/usr/bin/env python3
"""Serial transmitter GUI for the QPSK TX firmware."""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
MAX_LOG_LINES = 1000
MAX_LINES_PER_UPDATE = 100


class TxGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("QPSK transmitter")
        self.geometry("920x560")
        self.serial_port: serial.Serial | None = None
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.lines: queue.Queue[str] = queue.Queue()
        self.port_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Disconnected")
        self.message_var = tk.StringVar()
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

        editor = ttk.LabelFrame(self, text="Message", padding=12)
        editor.pack(fill=tk.X, padx=8, pady=8)
        ttk.Label(editor, text="UTF-8 text:").pack(anchor=tk.W)
        entry = ttk.Entry(editor, textvariable=self.message_var)
        entry.pack(fill=tk.X, pady=(4, 8))
        entry.bind("<Return>", lambda _event: self.send_message())
        ttk.Button(editor, text="Send QPSK frame", command=self.send_message).pack(anchor=tk.E)
        self.log = tk.Text(self, height=18, state=tk.DISABLED)
        self.log.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

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

    def send_message(self) -> None:
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showwarning("Not connected", "Connect to the TX Nucleo first.")
            return
        message = self.message_var.get()
        payload = message.encode("utf-8")
        if len(payload) > 128:
            messagebox.showwarning("Message too long", "The payload limit is 128 UTF-8 bytes.")
            return
        try:
            self.serial_port.write(b"SEND " + payload.hex().encode("ascii") + b"\n")
        except serial.SerialException as error:
            self.disconnect()
            messagebox.showerror("Send failed", str(error))
            return
        self._append_log(f"TX request: {message}")
        self.message_var.set("")

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
        self.after(50, self._drain_serial)

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
    TxGui().mainloop()
