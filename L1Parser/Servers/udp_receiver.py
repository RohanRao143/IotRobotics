
import socket

HOST = "0.0.0.0"
PORT = 5555

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((HOST, PORT))

print(f"Listening for UDP datagrams on port {PORT}")

while True:
    data, addr = sock.recvfrom(65535)
    print(f"Received {len(data)} bytes from {addr}: {data.hex()}")
