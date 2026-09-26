#!/usr/bin/env python3
# dbgclient.py — client for the port's live, non-blocking debug server (runtime/psx/dbg_server.cpp).
#
# The port must be launched with PSXPORT_DEBUG_SERVER=1 (default port 5959) or =<port>. Unlike the
# blocking PSXPORT_REPL FIFO, this does NOT pause the game — the game keeps presenting while a command
# is marshalled to the main thread and serviced once per frame at a safe point, which is what makes it
# usable to PLAY a running game and watch what it does.
#
#   tools/dbgclient.py scene                 # one command, print reply, exit
#   tools/dbgclient.py provat 160 120
#   tools/dbgclient.py shot scratch/screenshots/puddle.ppm
#   tools/dbgclient.py                       # interactive prompt (REPL); 'quit' to exit
#   tools/dbgclient.py --port 5959 stage
#
# Each reply is terminated by a line "---END---".
#
# `LiveClient` is the same protocol as a class, for a driver that needs to keep a session: a tool that
# plays the game, samples guest state and captures frames has no business re-implementing the socket
# handshake, and a second copy would be free to disagree about when a reply has arrived.
import socket
import sys

DEFAULT_PORT = 5959
END_MARKER = b"---END---\n"


class LiveClient:
    """One connection to the live endpoint. A fresh connection per command is allowed and is what the
    CLI does, because the server handles them independently; a driver that interleaves reads with
    input keeps one connection so its commands are ordered against each other."""

    def __init__(self, port: int = DEFAULT_PORT, host: str = "127.0.0.1", timeout: float = 120.0):
        self.port = port
        self.host = host
        self.timeout = timeout
        self._sock = socket.create_connection((host, port), timeout=timeout)

    def send(self, line: str) -> str:
        """One command, one reply, without the terminator."""
        self._sock.sendall((line + "\n").encode())
        buf = b""
        while END_MARKER not in buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf.split(END_MARKER)[0].decode(errors="replace")

    def word(self, address: int) -> int:
        """One guest RAM word. The reply is `ADDR: HEXHEX...`; a short or unparsable reply is an
        error rather than a silent zero, because a driver acting on a wrong address is worse than one
        that stops."""
        reply = self.send(f"rw {address:08X} 1")
        try:
            return int(reply.rsplit(":", 1)[1].split()[0], 16)
        except (IndexError, ValueError) as error:
            raise RuntimeError(f"rw {address:08X} returned {reply.strip()!r}") from error

    def words(self, address: int, count: int) -> list[int]:
        reply = self.send(f"rw {address:08X} {count}")
        try:
            return [int(value, 16) for value in reply.rsplit(":", 1)[1].split()]
        except (IndexError, ValueError) as error:
            raise RuntimeError(f"rw {address:08X} {count} returned {reply.strip()!r}") from error

    def frame(self) -> int:
        """The product's present-frame counter, parsed out of `frame`'s reply."""
        reply = self.send("frame")
        for token in reply.split():
            if token.startswith("frame="):
                return int(token.split("=", 1)[1])
        raise RuntimeError(f"frame reply carried no counter: {reply.strip()!r}")

    def tap(self, button: str, frames: int = 4) -> str:
        """Press and release across `frames` presented frames. A pad EDGE has to span a frame the guest
        samples: a `press` immediately followed by a `release` can be serviced inside one frame and be
        invisible to the game, which is a real hazard when driving over a non-blocking endpoint."""
        return self.send(f"tap {button} {frames}")

    def press(self, button: str) -> str:
        return self.send(f"press {button}")

    def release(self, button: str) -> str:
        return self.send(f"release {button}")

    def shot(self, path: str) -> str:
        return self.send(f"shot {path}")

    def quit(self) -> None:
        try:
            self.send("quit")
        except OSError:
            pass  # the product may close the socket as it exits; the reply is not needed to leave
        finally:
            self.close()

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self) -> "LiveClient":
        return self

    def __exit__(self, *exception) -> None:
        self.close()


def main() -> int:
    args = sys.argv[1:]
    port = DEFAULT_PORT
    if len(args) >= 2 and args[0] == "--port":
        port = int(args[1])
        args = args[2:]
    with LiveClient(port) as client:
        if args:
            sys.stdout.write(client.send(" ".join(args)))
            return 0
        print(f"[dbgclient] connected to 127.0.0.1:{port}; 'help' for commands, 'quit' to exit")
        while True:
            try:
                line = input("dbg> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if line in ("quit", "q", "exit"):
                return 0
            if line:
                sys.stdout.write(client.send(line))


if __name__ == "__main__":
    raise SystemExit(main())
