#!/usr/bin/env python3
"""
CYGM Serial Screenshot Tool
Captures screenshots from ESP32 via serial port
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
from PIL import Image, ImageTk
import serial
import serial.tools.list_ports
import threading
import time
import struct
import os
import base64
from datetime import datetime
from io import BytesIO

class SerialScreenshotGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("CYGM Serial Screenshot")
        self.root.geometry("1000x800")  # Larger window for better layout

        self.serial_port = None
        self.connected = False
        self.receiving = False
        self.last_screenshot = None
        self.monitor_enabled = False
        self.monitor_thread = None
        self.monitor_running = False

        self.setup_ui()
        self.refresh_ports()

    def setup_ui(self):
        # Main container
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(2, weight=1)

        # Title
        title_label = ttk.Label(main_frame, text="CYGM Serial Screenshot",
                               font=('Arial', 16, 'bold'))
        title_label.grid(row=0, column=0, columnspan=3, pady=(0, 10))

        # Connection frame
        conn_frame = ttk.LabelFrame(main_frame, text="Serial Connection", padding="10")
        conn_frame.grid(row=1, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=(0, 10))
        conn_frame.columnconfigure(1, weight=1)

        # Port selection
        ttk.Label(conn_frame, text="Port:").grid(row=0, column=0, sticky=tk.W, padx=(0, 5))

        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, state='readonly')
        self.port_combo.grid(row=0, column=1, sticky=(tk.W, tk.E), padx=5)

        ttk.Button(conn_frame, text="🔄 Refresh", command=self.refresh_ports, width=10).grid(row=0, column=2, padx=5)

        # Baud rate
        ttk.Label(conn_frame, text="Baud:").grid(row=1, column=0, sticky=tk.W, padx=(0, 5), pady=(5, 0))

        self.baud_var = tk.StringVar(value="115200")
        baud_combo = ttk.Combobox(conn_frame, textvariable=self.baud_var,
                                  values=["9600", "19200", "38400", "57600", "115200",
                                         "230400", "460800", "921600"],
                                  state='readonly', width=10)
        baud_combo.grid(row=1, column=1, sticky=tk.W, padx=5, pady=(5, 0))

        # Connection status
        ttk.Label(conn_frame, text="Status:").grid(row=2, column=0, sticky=tk.W, padx=(0, 5), pady=(5, 0))
        self.status_label = ttk.Label(conn_frame, text="⚫ Disconnected", foreground='red')
        self.status_label.grid(row=2, column=1, sticky=tk.W, padx=5, pady=(5, 0))

        # Control buttons
        btn_frame = ttk.Frame(conn_frame)
        btn_frame.grid(row=3, column=0, columnspan=3, pady=(10, 0))

        self.connect_btn = ttk.Button(btn_frame, text="Connect", command=self.connect, width=12)
        self.connect_btn.grid(row=0, column=0, padx=5)

        self.disconnect_btn = ttk.Button(btn_frame, text="Disconnect", command=self.disconnect,
                                        width=12, state=tk.DISABLED)
        self.disconnect_btn.grid(row=0, column=1, padx=5)

        self.screenshot_btn = ttk.Button(btn_frame, text="📷 Take Screenshot",
                                        command=self.take_screenshot, width=15, state=tk.DISABLED)
        self.screenshot_btn.grid(row=0, column=2, padx=5)

        # Monitor checkbox
        self.monitor_var = tk.BooleanVar(value=False)
        monitor_check = ttk.Checkbutton(btn_frame, text="🔍 Serial Monitor",
                                        variable=self.monitor_var, command=self.toggle_monitor)
        monitor_check.grid(row=0, column=3, padx=5)

        # Two column layout for preview and log
        # Left: Preview (actual device size)
        preview_frame = ttk.LabelFrame(main_frame, text="Preview (320x240)", padding="5")
        preview_frame.grid(row=2, column=0, sticky=(tk.N), padx=(0, 5))

        # Canvas for fixed pixel size preview
        self.preview_canvas = tk.Canvas(preview_frame, width=320, height=240,
                                       background='black', highlightthickness=1)
        self.preview_canvas.grid(row=0, column=0)

        # Text on canvas initially
        self.preview_canvas.create_text(160, 120, text="No screenshot yet",
                                        fill='white', font=('Arial', 12),
                                        tags='placeholder')

        ttk.Button(preview_frame, text="💾 Save Last Screenshot",
                  command=self.save_last_screenshot).grid(row=1, column=0, pady=(5, 0))

        # Right: Log
        log_frame = ttk.LabelFrame(main_frame, text="Activity Log", padding="5")
        log_frame.grid(row=2, column=1, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S))
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=35, width=60,
                                                  font=('Consolas', 9))
        self.log_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        ttk.Button(log_frame, text="🗑️ Clear Log", command=self.clear_log).grid(row=1, column=0, pady=(5, 0))

        # Serial Monitor (initially hidden)
        self.monitor_frame = ttk.LabelFrame(main_frame, text="Serial Monitor (ESP32 Output)", padding="5")
        self.monitor_text = scrolledtext.ScrolledText(self.monitor_frame, height=10, width=100,
                                                      font=('Consolas', 8), background='black',
                                                      foreground='#00ff00')
        self.monitor_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), columnspan=2)

        # Buttons for serial monitor
        ttk.Button(self.monitor_frame, text="💾 Save Serial Log",
                  command=self.copy_serial_to_clipboard).grid(row=1, column=0, pady=(5, 0), padx=(0, 5))
        ttk.Button(self.monitor_frame, text="🗑️ Clear Monitor",
                  command=self.clear_monitor).grid(row=1, column=1, pady=(5, 0))

        self.monitor_frame.rowconfigure(0, weight=1)
        self.monitor_frame.columnconfigure(0, weight=1)
        self.monitor_frame.columnconfigure(1, weight=1)

        # Initial log
        self.log("Serial screenshot tool ready")
        self.log("1. Select your ESP32 COM port")
        self.log("2. Click Connect")
        self.log("3. Click 'Take Screenshot'")
        self.log("")

    def log(self, message):
        timestamp = datetime.now().strftime('%H:%M:%S')
        self.log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.log_text.see(tk.END)
        self.root.update_idletasks()

    def clear_log(self):
        self.log_text.delete('1.0', tk.END)

    def copy_serial_to_clipboard(self):
        """Save serial monitor output to file"""
        content = self.monitor_text.get('1.0', tk.END)

        # Create screenshots folder if it doesn't exist
        os.makedirs('screenshots', exist_ok=True)

        # Save to serial.txt
        filepath = 'screenshots/serial.txt'
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)

        self.log(f"✓ Serial output saved to {filepath}")

    def clear_monitor(self):
        """Clear the serial monitor window"""
        self.monitor_text.delete('1.0', tk.END)
        self.log("✓ Serial monitor cleared")

    def toggle_monitor(self):
        if self.monitor_var.get():
            # Show monitor frame
            self.monitor_frame.grid(row=3, column=0, columnspan=3, sticky=(tk.W, tk.E, tk.N, tk.S), pady=(10, 0))
            self.log("✓ Serial monitor enabled")
            if self.connected:
                self.start_monitor()
        else:
            # Hide monitor frame
            self.monitor_frame.grid_forget()
            self.log("Serial monitor disabled")
            self.stop_monitor()

    def start_monitor(self):
        if not self.monitor_running:
            self.monitor_running = True
            self.monitor_thread = threading.Thread(target=self._monitor_serial, daemon=True)
            self.monitor_thread.start()

    def stop_monitor(self):
        self.monitor_running = False
        if self.monitor_thread:
            self.monitor_thread.join(timeout=1)

    def _monitor_serial(self):
        """Background thread to read and display serial data"""
        buffer = ""
        while self.monitor_running and self.connected:
            try:
                if self.serial_port and self.serial_port.in_waiting > 0:
                    # Read available data
                    if not self.receiving:
                        # Normal mode - read and display
                        data = self.serial_port.read(self.serial_port.in_waiting)
                        text = data.decode('utf-8', errors='ignore')
                        buffer += text

                        # Process complete lines
                        while '\n' in buffer:
                            line, buffer = buffer.split('\n', 1)
                            line = line.rstrip('\r')
                            if line:  # Only show non-empty lines
                                self.monitor_text.insert(tk.END, line + '\n')
                                self.monitor_text.see(tk.END)

                        # Limit buffer size
                        if int(self.monitor_text.index('end-1c').split('.')[0]) > 1000:
                            self.monitor_text.delete('1.0', '100.0')
                    # else: screenshot in progress, don't interfere

                time.sleep(0.05)
            except Exception as e:
                if self.monitor_running:
                    self.log(f"Monitor error: {e}")
                    break

    def refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        port_list = [port.device for port in ports]

        self.port_combo['values'] = port_list

        if port_list:
            # Try to select ESP32 port (usually has "USB" or "Serial" in description)
            for port in ports:
                if 'USB' in port.description or 'Serial' in port.description or 'CH340' in port.description:
                    self.port_var.set(port.device)
                    break
            else:
                self.port_var.set(port_list[0])

            self.log(f"Found {len(port_list)} port(s)")
        else:
            self.log("No serial ports found")

    def connect(self):
        port = self.port_var.get()
        baud = int(self.baud_var.get())

        if not port:
            self.log("❌ Please select a port")
            return

        try:
            # Longer timeout for slow transfers, no write timeout
            self.serial_port = serial.Serial(port, baud, timeout=5, write_timeout=None)
            time.sleep(2)  # Wait for connection to stabilize and ESP32 to boot

            # Flush any existing data
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()

            self.connected = True
            self.status_label.config(text="🟢 Connected", foreground='green')
            self.connect_btn.config(state=tk.DISABLED)
            self.disconnect_btn.config(state=tk.NORMAL)
            self.screenshot_btn.config(state=tk.NORMAL)

            self.log(f"✓ Connected to {port} at {baud} baud")

            # Start monitor if enabled
            if self.monitor_var.get():
                self.start_monitor()

        except Exception as e:
            self.log(f"❌ Connection failed: {str(e)}")

    def disconnect(self):
        # Stop monitor first
        self.stop_monitor()

        if self.serial_port:
            self.serial_port.close()
            self.serial_port = None

        self.connected = False
        self.status_label.config(text="⚫ Disconnected", foreground='red')
        self.connect_btn.config(state=tk.NORMAL)
        self.disconnect_btn.config(state=tk.DISABLED)
        self.screenshot_btn.config(state=tk.DISABLED)

        self.log("Disconnected")

    def take_screenshot(self):
        if not self.connected or self.receiving:
            return

        self.receiving = True
        self.screenshot_btn.config(state=tk.DISABLED)

        # Run in thread to not block UI
        thread = threading.Thread(target=self._capture_screenshot, daemon=True)
        thread.start()

    def _capture_screenshot(self):
        try:
            self.log("📷 Requesting screenshot...")

            # Send screenshot command (char by char to avoid buffer overflow)
            for char in b"screenshot\n":
                self.serial_port.write(bytes([char]))
                time.sleep(0.01)  # Small delay between characters
            self.serial_port.flush()

            # Give ESP32 time to start processing
            time.sleep(0.5)

            # Wait for start header
            header_found = False
            timeout = time.time() + 10

            while not header_found and time.time() < timeout:
                line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                if line == "SCREENSHOT_START":
                    header_found = True
                    self.log("✓ Screenshot started")

            if not header_found:
                self.log("❌ Timeout waiting for screenshot header")
                self.receiving = False
                self.screenshot_btn.config(state=tk.NORMAL)
                return

            # Read dimensions
            dim_line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
            try:
                width, height = map(int, dim_line.split())
                self.log(f"  Dimensions: {width}x{height}")
            except:
                self.log(f"❌ Failed to parse dimensions: {dim_line}")
                self.receiving = False
                self.screenshot_btn.config(state=tk.NORMAL)
                return

            # Check for binary mode
            mode_line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
            if mode_line != "BINARY":
                self.log(f"❌ Expected BINARY mode, got: {mode_line}")
                self.receiving = False
                self.screenshot_btn.config(state=tk.NORMAL)
                return

            # Create image to receive pixel data
            self.log("  Creating image buffer...")
            img = Image.new('RGB', (width, height))
            pixels = img.load()

            # Receive pixel data by area (LVGL flushes in sections)
            self.log("  Receiving pixel data areas...")
            total_pixels_received = 0
            total_pixels = width * height
            area_count = 0

            while total_pixels_received < total_pixels:
                # Read area header: "AREA x1 y1 x2 y2 bytes\n"
                area_line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()

                if area_line == "SCREENSHOT_END":
                    self.log(f"✓ Screenshot complete ({area_count} areas received)")
                    break

                if not area_line.startswith("AREA"):
                    self.log(f"⚠ Warning: Expected AREA, got: {area_line}")
                    break

                try:
                    parts = area_line.split()
                    x1, y1, x2, y2 = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
                    area_bytes = int(parts[5])  # ESP32 tells us exactly how many bytes to read
                    area_w = x2 - x1 + 1
                    area_h = y2 - y1 + 1
                    area_pixels = area_w * area_h

                    area_count += 1
                    self.log(f"  Area {area_count}: ({x1},{y1})-({x2},{y2}) = {area_w}x{area_h} ({area_bytes} bytes)")

                    # Read EXACTLY the number of bytes specified in the header
                    area_data = self.serial_port.read(area_bytes)
                    if len(area_data) != area_bytes:
                        self.log(f"⚠ Warning: Expected {area_bytes} bytes, got {len(area_data)}")
                        break

                    # Place pixels at correct position in image
                    pixel_idx = 0
                    for y in range(y1, y2 + 1):
                        for x in range(x1, x2 + 1):
                            offset = pixel_idx * 2
                            rgb565 = struct.unpack('<H', area_data[offset:offset+2])[0]

                            # Extract RGB components - display uses BGR565!
                            b5 = (rgb565 >> 11) & 0x1F  # Blue in high bits
                            g6 = (rgb565 >> 5) & 0x3F   # Green in middle
                            r5 = rgb565 & 0x1F           # Red in low bits

                            # Scale to 8-bit
                            r8 = (r5 << 3) | (r5 >> 2)
                            g8 = (g6 << 2) | (g6 >> 4)
                            b8 = (b5 << 3) | (b5 >> 2)

                            pixels[x, y] = (r8, g8, b8)
                            pixel_idx += 1

                    total_pixels_received += area_pixels

                except Exception as e:
                    self.log(f"❌ Error processing area: {e}")
                    break

            # If we didn't get the end marker in the loop, try to read it
            if total_pixels_received >= total_pixels:
                end_line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                if end_line == "SCREENSHOT_END":
                    self.log(f"✓ Screenshot transfer complete")
                elif end_line:
                    self.log(f"⚠ Warning: unexpected end marker: {end_line}")

            # Image is already built from area data
            self.log("  Finalizing image...")

            # Save screenshot
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            os.makedirs('screenshots', exist_ok=True)
            filename = f'screenshots/screenshot_{timestamp}.png'
            img.save(filename, 'PNG')

            self.log(f"✓ Saved: {filename}")

            # Update preview
            self.last_screenshot = img
            self.update_preview(img)

        except Exception as e:
            self.log(f"❌ Error: {str(e)}")
            import traceback
            traceback.print_exc()

        finally:
            self.receiving = False
            self.screenshot_btn.config(state=tk.NORMAL)

    def update_preview(self, img):
        # Display at actual size (320x240)
        photo = ImageTk.PhotoImage(img)

        # Clear canvas and show image
        self.preview_canvas.delete('all')
        self.preview_canvas.create_image(0, 0, anchor=tk.NW, image=photo)
        self.preview_canvas.image = photo  # Keep reference

    def save_last_screenshot(self):
        if self.last_screenshot is None:
            self.log("❌ No screenshot to save")
            return

        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        os.makedirs('screenshots', exist_ok=True)
        filename = f'screenshots/screenshot_{timestamp}.png'
        self.last_screenshot.save(filename, 'PNG')
        self.log(f"✓ Saved copy: {filename}")

    def on_closing(self):
        if self.connected:
            self.disconnect()
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    app = SerialScreenshotGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()
