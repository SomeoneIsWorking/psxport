#!/usr/bin/env python3
# dbgclient.py — client for the port's live, non-blocking debug server (runtime/recomp/dbg_server.c).
#
# The port must be launched with PSXPORT_DEBUG_SERVER=1 (default port 5959) or =<port>. Unlike the
# blocking PSXPORT_REPL FIFO, this does NOT pause the game — drive it WHILE the user plays live.
#
#   tools/dbgclient.py scene                 # one command, print reply, exit
#   tools/dbgclient.py provat 160 120
#   tools/dbgclient.py shot scratch/screenshots/puddle.ppm
#   tools/dbgclient.py                       # interactive prompt (REPL); 'quit' to exit
#   tools/dbgclient.py --port 5959 stage
#
# Each reply is terminated by a line "---END---".
import socket, sys

def send(sock, line):
    sock.sendall((line + "\n").encode())
    buf = b""
    while b"---END---\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.split(b"---END---\n")[0].decode(errors="replace")

def main():
    args = sys.argv[1:]
    port = 5959
    if len(args) >= 2 and args[0] == "--port":
        port = int(args[1]); args = args[2:]
    sock = socket.create_connection(("127.0.0.1", port))
    if args:
        sys.stdout.write(send(sock, " ".join(args)))
        return
    print(f"[dbgclient] connected to 127.0.0.1:{port}; 'help' for commands, 'quit' to exit")
    try:
        while True:
            line = input("dbg> ").strip()
            if line in ("quit", "q", "exit"):
                break
            if not line:
                continue
            sys.stdout.write(send(sock, line))
    except (EOFError, KeyboardInterrupt):
        print()

if __name__ == "__main__":
    main()
