#!/usr/bin/env python3
# serve_packages.py — NexOS package index server
# Jalankan di host QEMU: python3 serve_packages.py
# Akses dari NexOS: npl update (→ GET http://10.0.2.2:80/packages)
#
# Format index: name|version|desc|size_kb
# Baris diawali '#' = komentar

from http.server import HTTPServer, BaseHTTPRequestHandler

PACKAGE_INDEX = """\
# NexOS Package Index v1
# Format: name|version|desc|size_kb
python|3.12.0|Python interpreter port|8192
lua|5.4.7|Lua scripting language|256
busybox|1.36.1|Unix tools collection|512
vim|9.1|Text editor|1024
curl|8.7.1|HTTP client tool|384
htop|3.3.0|Interactive process viewer|128
nsh|0.4.0|NexOS shell|16
nexfetch|2.0.0|System info tool|8
calc|1.1.0|Simple calculator|4
cat|1.0.1|Print file contents|2
ls|1.0.1|List directory|2
nano|7.2|Simple text editor|256
wget|1.0.0|HTTP downloader|64
"""

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/packages':
            body = PACKAGE_INDEX.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
            print(f"[+] Served /packages ({len(body)} bytes)")
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not found\n")

    def log_message(self, fmt, *args):
        print(f"[http] {self.address_string()} — {fmt % args}")

if __name__ == '__main__':
    server = HTTPServer(('0.0.0.0', 80), Handler)
    print("NexOS Package Server running on :80")
    print("Akses dari NexOS: npl update")
    print("Ctrl+C untuk stop\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
