
import socket
import time

HOST = "127.0.0.1"
PORT = 6666

with socket.create_connection((HOST, PORT), timeout=5) as sock:
    print("Connected to FrameParser TCP server")

    sock.settimeout(10)

    while True:
        try:
            data = sock.recv(4096)
            if not data:
                print("Server closed the connection")
                break

            print(f"Received {len(data)} bytes: {data.hex()}")

        except socket.timeout:
            print("No data received during this interval")
