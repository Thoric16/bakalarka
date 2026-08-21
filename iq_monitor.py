#!/usr/bin/env python3
"""Serial control and real-time IQ plotter for the STM firmware."""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

BAUD_RATE = 115200
ADC_MAX = 4095
ADC_VOLTAGE = 3.3
PACKET_HEADER = 0xA5
PACKET_SIZE = 5
MAX_POINTS = 100_000
MODULATIONS = ("QPSK", "16-QAM", "64-QAM", "256-QAM", "1024-QAM", "4096-QAM")
IDEAL_BORDER_OPTIONS = ("Off",) + MODULATIONS


class SampleParser:
    """Parse the firmware's fixed-size packets while ignoring status text."""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[tuple[float, float]]:
        self.buffer.extend(data)
        samples: list[tuple[float, float]] = []
        while True:
            try:
                header_index = self.buffer.index(PACKET_HEADER)
            except ValueError:
                self.buffer.clear()
                break
            if header_index:
                del self.buffer[:header_index]
            if len(self.buffer) < PACKET_SIZE:
                break
            i_raw = self.buffer[1] | (self.buffer[2] << 8)
            q_raw = self.buffer[3] | (self.buffer[4] << 8)
            del self.buffer[:PACKET_SIZE]
            samples.append((i_raw * ADC_VOLTAGE / ADC_MAX, q_raw * ADC_VOLTAGE / ADC_MAX))
        return samples


class IQMonitor(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("STM IQ monitor")
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
        self.modulation_var = tk.StringVar(value=MODULATIONS[0])
        self.border_var = tk.StringVar(value="Off")
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

        ttk.Label(controls, text="Modulation:").pack(side=tk.LEFT)
        self.modulation_combo = ttk.Combobox(
            controls, textvariable=self.modulation_var, values=MODULATIONS, state="readonly", width=12
        )
        self.modulation_combo.pack(side=tk.LEFT, padx=5)
        ttk.Button(controls, text="Apply TX", command=self.apply_modulation).pack(side=tk.LEFT)
        ttk.Button(controls, text="Next TX", command=lambda: self.send_command("NEXT")).pack(side=tk.LEFT, padx=5)

        ttk.Label(controls, text="Ideal borders:").pack(side=tk.LEFT, padx=(15, 0))
        self.border_combo = ttk.Combobox(
            controls, textvariable=self.border_var, values=IDEAL_BORDER_OPTIONS, state="readonly", width=12
        )
        self.border_combo.pack(side=tk.LEFT, padx=5)
        self.border_combo.bind("<<ComboboxSelected>>", self.update_borders)

        modes = ttk.Frame(self, padding=(8, 0, 8, 8))
        modes.pack(fill=tk.X)
        ttk.Button(modes, text="TX mode", command=lambda: self.send_command("TX")).pack(side=tk.LEFT)
        ttk.Button(modes, text="RX mode", command=lambda: self.send_command("RX")).pack(side=tk.LEFT, padx=5)
        ttk.Button(modes, text="Clear plot", command=self.clear_plot).pack(side=tk.LEFT, padx=(15, 0))
        ttk.Label(modes, textvariable=self.count_var).pack(side=tk.RIGHT)
        ttk.Label(modes, textvariable=self.status_var).pack(side=tk.RIGHT, padx=15)

    def _build_plot(self) -> None:
        figure = Figure(figsize=(8, 6), dpi=100)
        self.axis = figure.add_subplot(111)
        self.axis.set_title("Received IQ constellation")
        self.axis.set_xlabel("I voltage (V)")
        self.axis.set_ylabel("Q voltage (V)")
        self.axis.set_xlim(0, ADC_VOLTAGE)
        self.axis.set_ylim(0, ADC_VOLTAGE)
        self.axis.grid(True, alpha=0.3)
        self.points = self.axis.scatter([], [], s=12, alpha=0.7, color="#1769aa")
        self.border_lines: list[object] = []
        self.canvas = FigureCanvasTkAgg(figure, master=self)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))
        self.update_borders()

    def update_borders(self, _event: object = None) -> None:
        for line in self.border_lines:
            line.remove()
        self.border_lines.clear()

        if self.border_var.get() == "Off":
            self.canvas.draw_idle()
            return

        modulation_index = MODULATIONS.index(self.border_var.get())
        bits_per_axis = modulation_index + 1
        axis_level_count = 1 << bits_per_axis
        level_spacing = ADC_VOLTAGE / (axis_level_count - 1)
        border_color = "#d95f02"
        for level_index in range(axis_level_count - 1):
            border = (level_index + 0.5) * level_spacing
            self.border_lines.append(self.axis.axvline(border, color=border_color, linewidth=0.8, alpha=0.65))
            self.border_lines.append(self.axis.axhline(border, color=border_color, linewidth=0.8, alpha=0.65))
        self.canvas.draw_idle()

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
                data = port.read(port.in_waiting or 1)
            except serial.SerialException:
                return
            for sample in self.parser.feed(data):
                self.sample_queue.put(sample)

    def send_command(self, command: str) -> None:
        if not self.serial_port or not self.serial_port.is_open:
            self.status_var.set("Connect to the STM first")
            return
        try:
            self.serial_port.write((command + "\n").encode("ascii"))
            self.status_var.set(f"Sent {command}")
        except serial.SerialException as error:
            self.status_var.set(f"Serial error: {error}")

    def apply_modulation(self) -> None:
        self.send_command(f"MOD {MODULATIONS.index(self.modulation_var.get())}")

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
