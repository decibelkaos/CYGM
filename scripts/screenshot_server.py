#!/usr/bin/env python3
"""
CYGM Screenshot Server
Receives screenshots from ESP32 device via HTTP POST
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime
import threading
import socket
import os

class ScreenshotHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == '/upload':
            try:
                # Get content length
                content_length = int(self.headers['Content-Length'])

                # Read the file data
                file_data = self.rfile.read(content_length)

                # Generate filename with timestamp
                timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
                filename = f'screenshot_{timestamp}.bmp'

                # Save to screenshots directory
                screenshots_dir = 'screenshots'
                os.makedirs(screenshots_dir, exist_ok=True)
                filepath = os.path.join(screenshots_dir, filename)

                with open(filepath, 'wb') as f:
                    f.write(file_data)

                # Log to GUI
                if hasattr(self.server, 'log_callback'):
                    self.server.log_callback(f'✓ Saved {filename} ({len(file_data):,} bytes)')

                # Send response
                self.send_response(200)
                self.send_header('Content-type', 'text/plain')
                self.end_headers()
                self.wfile.write(b'OK')

            except Exception as e:
                if hasattr(self.server, 'log_callback'):
                    self.server.log_callback(f'✗ Error: {str(e)}')
                self.send_response(500)
                self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        # Suppress default HTTP logging
        pass


class ScreenshotServerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("CYGM Screenshot Server")
        self.root.geometry("600x450")
        self.root.resizable(True, True)

        self.server = None
        self.server_thread = None
        self.running = False

        # Get local IP
        self.local_ip = self.get_local_ip()

        self.setup_ui()

    def get_local_ip(self):
        """Get the local IP address"""
        try:
            # Create a socket to determine local IP
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            return local_ip
        except:
            return "Unable to detect"

    def setup_ui(self):
        # Main container
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(0, weight=1)
        main_frame.rowconfigure(3, weight=1)

        # Title
        title_label = ttk.Label(main_frame, text="CYGM Screenshot Server",
                               font=('Arial', 16, 'bold'))
        title_label.grid(row=0, column=0, pady=(0, 10))

        # Status frame
        status_frame = ttk.LabelFrame(main_frame, text="Server Configuration", padding="10")
        status_frame.grid(row=1, column=0, sticky=(tk.W, tk.E), pady=(0, 10))
        status_frame.columnconfigure(1, weight=1)

        # Local IP
        ttk.Label(status_frame, text="Your Computer IP:",
                 font=('Arial', 10, 'bold')).grid(row=0, column=0, sticky=tk.W, pady=5)
        self.ip_label = ttk.Label(status_frame, text=self.local_ip,
                                  font=('Arial', 11), foreground='blue')
        self.ip_label.grid(row=0, column=1, sticky=tk.W, pady=5)

        # Port
        ttk.Label(status_frame, text="Port:",
                 font=('Arial', 10, 'bold')).grid(row=1, column=0, sticky=tk.W, pady=5)
        ttk.Label(status_frame, text="8080",
                 font=('Arial', 11)).grid(row=1, column=1, sticky=tk.W, pady=5)

        # ESP32 Configuration
        ttk.Label(status_frame, text="ESP32 Target URL:",
                 font=('Arial', 10, 'bold')).grid(row=2, column=0, sticky=tk.W, pady=5)
        self.url_label = ttk.Label(status_frame,
                                   text=f"http://{self.local_ip}:8080/upload",
                                   font=('Arial', 10), foreground='green')
        self.url_label.grid(row=2, column=1, sticky=tk.W, pady=5)

        # Status indicator
        ttk.Label(status_frame, text="Status:",
                 font=('Arial', 10, 'bold')).grid(row=3, column=0, sticky=tk.W, pady=5)
        self.status_label = ttk.Label(status_frame, text="⚫ Stopped",
                                     font=('Arial', 10), foreground='red')
        self.status_label.grid(row=3, column=1, sticky=tk.W, pady=5)

        # Control buttons
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=2, column=0, pady=(0, 10))

        self.start_button = ttk.Button(button_frame, text="▶ Start Server",
                                       command=self.start_server, width=15)
        self.start_button.grid(row=0, column=0, padx=5)

        self.stop_button = ttk.Button(button_frame, text="⏹ Stop Server",
                                      command=self.stop_server, width=15, state=tk.DISABLED)
        self.stop_button.grid(row=0, column=1, padx=5)

        ttk.Button(button_frame, text="📁 Open Folder",
                  command=self.open_screenshots_folder, width=15).grid(row=0, column=2, padx=5)

        # Log area
        log_frame = ttk.LabelFrame(main_frame, text="Activity Log", padding="5")
        log_frame.grid(row=3, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=15, width=70,
                                                  font=('Consolas', 9))
        self.log_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        # Initial log message
        self.log("Screenshot server ready")
        self.log(f"Configure your ESP32 to POST to: http://{self.local_ip}:8080/upload")
        self.log("Screenshots will be saved to: ./screenshots/")
        self.log("")

    def log(self, message):
        """Add a message to the log"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        self.log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.log_text.see(tk.END)
        self.root.update_idletasks()

    def start_server(self):
        """Start the HTTP server"""
        if self.running:
            return

        try:
            # Create server
            self.server = HTTPServer(('0.0.0.0', 8080), ScreenshotHandler)
            self.server.log_callback = self.log

            # Start server in background thread
            self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
            self.server_thread.start()

            self.running = True
            self.status_label.config(text="🟢 Running", foreground='green')
            self.start_button.config(state=tk.DISABLED)
            self.stop_button.config(state=tk.NORMAL)

            self.log("✓ Server started successfully")
            self.log(f"  Listening on {self.local_ip}:8080")
            self.log("  Waiting for screenshots from ESP32...")
            self.log("")

        except Exception as e:
            self.log(f"✗ Failed to start server: {str(e)}")

    def stop_server(self):
        """Stop the HTTP server"""
        if not self.running:
            return

        try:
            if self.server:
                self.server.shutdown()
                self.server = None

            self.running = False
            self.status_label.config(text="⚫ Stopped", foreground='red')
            self.start_button.config(state=tk.NORMAL)
            self.stop_button.config(state=tk.DISABLED)

            self.log("Server stopped")
            self.log("")

        except Exception as e:
            self.log(f"Error stopping server: {str(e)}")

    def open_screenshots_folder(self):
        """Open the screenshots folder in file explorer"""
        screenshots_dir = os.path.abspath('screenshots')
        os.makedirs(screenshots_dir, exist_ok=True)

        # Open folder based on OS
        import platform
        system = platform.system()

        try:
            if system == 'Windows':
                os.startfile(screenshots_dir)
            elif system == 'Darwin':  # macOS
                os.system(f'open "{screenshots_dir}"')
            else:  # Linux
                os.system(f'xdg-open "{screenshots_dir}"')

            self.log(f"Opened folder: {screenshots_dir}")
        except Exception as e:
            self.log(f"Could not open folder: {str(e)}")

    def on_closing(self):
        """Handle window close"""
        if self.running:
            self.stop_server()
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    app = ScreenshotServerGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()
