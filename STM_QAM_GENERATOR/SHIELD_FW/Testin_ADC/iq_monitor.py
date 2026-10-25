#!/usr/bin/env python3
"""Serial monitor and real-time ADC plotter for the STM firmware."""

from __future__ import annotations

import queue
import re
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

BAUD_RATE = 115200
MAX_POINTS = 100_000
GRAPH_LIMIT_MV = 200


class SampleParser:
    """Parse the text lines emitted by the ADC test firmware."""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.sample_pattern = re.compile(
            rb"ADC_I \(PA0\):\s*\d+\s*bits,\s*"
            rb"([+-]?\d+(?:\.\d+)?)\s*mV\s+"
            rb"ADC_Q \(PA1\):\s*\d+\s*bits,\s*"
            rb"([+-]?\d+(?:\.\d+)?)\s*mV"
        )

    def feed(self, data: bytes) -> list[tuple[float, float]]:
        self.buffer.extend(data)
        samples: list[tuple[float, float]] = []
        while b"\n" in self.buffer:
            line_end = self.buffer.index(b"\n")
            line = bytes(self.buffer[:line_end])
            del self.buffer[: line_end + 1]
            match = self.sample_pattern.search(line)
            if match is None:
                continue
            samples.append((float(match.group(1)), float(match.group(2))))
        return samples


class IQMonitor(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("STM ADC monitor")
        self.geometry("980x700")
        self.minsize(760, 520)

        self.serial_port: serial.Serial | None = None
        self.reader_stop = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.sample_queue: queue.Queue[tuple[float, float]] = queue.Queue()
        self.parser = SampleParser()
        self.i_values: list[float] = []
        self.q_values: list[float] = []
        self.sample_count = 0

        self.port_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Disconnected")
        self.count_var = tk.StringVar(value="Samples: 0")
        self._build_controls()
        self._build_plot()
        self.refresh_ports()
        self.after(30, self._update_plot)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def _build_controls(self) -> None:
        controls = ttk.Frame(self, padding=8)
        controls.pack(fill=tk.X)

        ttk.Label(controls, text="Serial port:").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(controls, textvariable=self.port_var, width=24, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(5, 5))
        ttk.Button(controls, text="Refresh", command=self.refresh_ports).pack(side=tk.LEFT)
        self.connect_button = ttk.Button(controls, text="Connect", command=self.toggle_connection)
        self.connect_button.pack(side=tk.LEFT, padx=(5, 15))

        modes = ttk.Frame(self, padding=(8, 0, 8, 8))
        modes.pack(fill=tk.X)
        ttk.Button(modes, text="Clear plot", command=self.clear_plot).pack(side=tk.LEFT)
        ttk.Label(modes, textvariable=self.count_var).pack(side=tk.RIGHT)
        ttk.Label(modes, textvariable=self.status_var).pack(side=tk.RIGHT, padx=15)

    def _build_plot(self) -> None:
        figure = Figure(figsize=(8, 6), dpi=100)
        self.axis = figure.add_subplot(111)
        self.axis.set_title("Received ADC voltages")
        self.axis.set_xlabel("ADC I voltage (mV)")
        self.axis.set_ylabel("ADC Q voltage (mV)")
        self.axis.set_xlim(-GRAPH_LIMIT_MV, GRAPH_LIMIT_MV)
        self.axis.set_ylim(-GRAPH_LIMIT_MV, GRAPH_LIMIT_MV)
        self.axis.grid(True, alpha=0.3)
        self.points = self.axis.scatter([], [], s=12, alpha=0.7, color="#1769aa")
        self.canvas = FigureCanvasTkAgg(figure, master=self)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

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
            self.serial_port = serial.Serial(self.port_var.get(), BAUD_RATE, timeout=0.1)
        except serial.SerialException as error:
            messagebox.showerror("Connection failed", str(error))
            return
        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._read_serial, daemon=True)
        self.reader_thread.start()
        self.connect_button.configure(text="Disconnect")
        self.status_var.set(f"Connected at {BAUD_RATE} baud")

    def disconnect(self) -> None:
        self.reader_stop.set()
        if self.serial_port:
            self.serial_port.close()
        self.serial_port = None
        self.connect_button.configure(text="Connect")
        self.status_var.set("Disconnected")

    def _read_serial(self) -> None:
        while not self.reader_stop.is_set():
            port = self.serial_port
            if not port or not port.is_open:
                return
            try:
                waiting = port.in_waiting
                read_size = waiting if isinstance(waiting, int) and waiting > 0 else 1
                data = port.read(read_size)
            except (OSError, TypeError, serial.SerialException):
                return
            for sample in self.parser.feed(data):
                self.sample_queue.put(sample)

    def clear_plot(self) -> None:
        self.i_values.clear()
        self.q_values.clear()
        self.sample_count = 0
        self.points.set_offsets([])
        self.count_var.set("Samples: 0")
        self.canvas.draw_idle()

    def _update_plot(self) -> None:
        changed = False
        while True:
            try:
                i_value, q_value = self.sample_queue.get_nowait()
            except queue.Empty:
                break
            self.i_values.append(i_value)
            self.q_values.append(q_value)
            self.sample_count += 1
            changed = True
        if len(self.i_values) > MAX_POINTS:
            del self.i_values[:-MAX_POINTS]
            del self.q_values[:-MAX_POINTS]
        if changed:
            self.points.set_offsets(list(zip(self.i_values, self.q_values)))
            self.count_var.set(f"Samples: {self.sample_count}")
            self.canvas.draw_idle()
        self.after(30, self._update_plot)

    def close(self) -> None:
        self.disconnect()
        self.destroy()


if __name__ == "__main__":
    IQMonitor().mainloop()
