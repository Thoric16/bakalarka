import tkinter as tk
from tkinter import scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import time

class DeviceController:
    def __init__(self, root):
        self.root = root
        self.root.title("Dual Channel SPI Controller")
        self.root.geometry("450x400")
        
        self.ser = None
        self.is_running = True
        self.min_freq_mhz = 10300
        self.max_freq_mhz = 11500
        
        # State Variables
        self.ch1_val = tk.IntVar(value=self.min_freq_mhz)
        self.ch2_val = tk.IntVar(value=10600)
        
        self.setup_gui()
        self.auto_connect()

    def setup_gui(self):
        # --- Control Panel ---
        ctrl_frame = tk.Frame(self.root)
        ctrl_frame.pack(pady=10)

        # Channel 1
        tk.Label(ctrl_frame, text="Channel 1 (MHz):", font=("Arial", 10, "bold")).grid(row=0, column=0, padx=5, pady=5)
        tk.Entry(ctrl_frame, textvariable=self.ch1_val, width=8, font=("Arial", 10)).grid(row=0, column=1, padx=5)
        tk.Button(ctrl_frame, text="▲ +25", command=lambda: self.step_value(1, 25)).grid(row=0, column=2, padx=2)
        tk.Button(ctrl_frame, text="▼ -25", command=lambda: self.step_value(1, -25)).grid(row=0, column=3, padx=2)
        tk.Button(ctrl_frame, text="Send", command=lambda: self.send_channel(1)).grid(row=0, column=4, padx=5)

        # Channel 2
        tk.Label(ctrl_frame, text="Channel 2 (MHz):", font=("Arial", 10, "bold")).grid(row=1, column=0, padx=5, pady=5)
        tk.Entry(ctrl_frame, textvariable=self.ch2_val, width=8, font=("Arial", 10)).grid(row=1, column=1, padx=5)
        tk.Button(ctrl_frame, text="▲ +25", command=lambda: self.step_value(2, 25)).grid(row=1, column=2, padx=2)
        tk.Button(ctrl_frame, text="▼ -25", command=lambda: self.step_value(2, -25)).grid(row=1, column=3, padx=2)
        tk.Button(ctrl_frame, text="Send", command=lambda: self.send_channel(2)).grid(row=1, column=4, padx=5)

        # Save Button
        tk.Button(self.root, text="💾 Save to EEPROM", font=("Arial", 10, "bold"), bg="#4CAF50", fg="white", 
                  command=self.save_eeprom).pack(pady=10)

        # --- Terminal Window ---
        tk.Label(self.root, text="Communication Terminal (TX / RX)").pack()
        self.terminal = scrolledtext.ScrolledText(self.root, height=12, width=50, bg="black", fg="#00FF00", font=("Consolas", 9))
        self.terminal.pack(padx=10, pady=5)

    def auto_connect(self):
        """Finds and connects to the FT232 device automatically."""
        ports = serial.tools.list_ports.comports()
        target_port = None
        
        for port in ports:
            # Look for FTDI chips or standard USB Serial descriptions
            if "FTDI" in (port.manufacturer or "") or "USB" in (port.description or ""):
                target_port = port.device
                break
                
        if target_port:
            try:
                # 9600 is standard, adjust if your device uses 115200
                self.ser = serial.Serial(target_port, baudrate=19200, timeout=1)
                self.log_terminal(f"SYSTEM: Connected to {target_port}", "sys")
                
                # Start background thread to listen for incoming data
                self.rx_thread = threading.Thread(target=self.read_serial_loop, daemon=True)
                self.rx_thread.start()
            except Exception as e:
                self.log_terminal(f"SYSTEM: Failed to connect to {target_port} - {e}", "sys")
        else:
            self.log_terminal("SYSTEM: No FT232/USB Serial device found.", "sys")

    def generate_command(self, channel_num, target_value):
        """Uses the previously decoded protocol to build the hex string."""
        channel_char = "A" if channel_num == 1 else "B"
        raw_val = target_value // 25
        high_byte = raw_val >> 4
        low_byte = raw_val & 0x0F
        value_str = f"{high_byte:02X}0{low_byte:X}"
        base_cmd = f"{channel_char}{value_str}00000000"
        
        char1_val = int(base_cmd[0], 16)
        char3_val = int(base_cmd[2], 16)
        char5_val = int(base_cmd[4], 16)
        checksum = char1_val + char3_val + char5_val + 35
        
        return f"#{base_cmd}{checksum:02d}"

    def clamp_frequency(self, freq):
        return max(self.min_freq_mhz, min(self.max_freq_mhz, freq))

    def validate_frequency(self, freq):
        if freq < self.min_freq_mhz or freq > self.max_freq_mhz:
            messagebox.showwarning(
                "Frequency limit",
                f"Frequency must be between {self.min_freq_mhz / 1000:.3f} GHz and {self.max_freq_mhz / 1000:.3f} GHz."
            )
            return False
        return True

    def step_value(self, channel, amount):
        if channel == 1:
            new_val = self.clamp_frequency(self.ch1_val.get() + amount)
            self.ch1_val.set(new_val)
            self.send_channel(1)
        else:
            new_val = self.clamp_frequency(self.ch2_val.get() + amount)
            self.ch2_val.set(new_val)
            self.send_channel(2)

    def send_channel(self, channel):
        if not self.ser or not self.ser.is_open:
            self.log_terminal("SYSTEM: Device not connected!", "sys")
            return
            
        val = self.ch1_val.get() if channel == 1 else self.ch2_val.get()
        if not self.validate_frequency(val):
            val = self.clamp_frequency(val)
            if channel == 1:
                self.ch1_val.set(val)
            else:
                self.ch2_val.set(val)
            self.log_terminal(
                f"SYSTEM: Value out of range, clamped to {val} MHz.",
                "sys"
            )
        cmd_str = self.generate_command(channel, val)
        self.send_serial(cmd_str)

    def save_eeprom(self):
        self.send_serial("#S")

    def send_serial(self, data_str):
        if self.ser and self.ser.is_open:
            # Ensure the string is formatted exactly as the device expects (often needs \r or \n)
            payload = f"{data_str}\r\n".encode('ascii')
            self.ser.write(payload)
            self.log_terminal(f"TX: {data_str}", "tx")

    def read_serial_loop(self):
        """Continuously listens for responses from the device."""
        while self.is_running and self.ser and self.ser.is_open:
            try:
                if self.ser.in_waiting > 0:
                    rx_data = self.ser.readline().decode('ascii', errors='ignore').strip()
                    if rx_data:
                        # Schedule the UI update safely from the background thread
                        self.root.after(0, self.log_terminal, f"RX: {rx_data}", "rx")
            except Exception:
                break
            time.sleep(0.05)

    def log_terminal(self, message, msg_type):
        """Appends text to the terminal widget."""
        self.terminal.insert(tk.END, message + "\n")
        self.terminal.see(tk.END) # Auto-scroll to bottom

    def on_close(self):
        self.is_running = False
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = DeviceController(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()