"""A minimal RESP client, used by the integration suite.

Written from scratch for the same reason the server is: it means the tests
exercise the wire protocol itself rather than a library's idea of it. It speaks
both RESP2 and RESP3 so the protocol-dependent replies can be checked directly.
"""

import socket


class Error(str):
    """An error reply. Subclasses str so comparisons read naturally."""


class Client:
    def __init__(self, host="127.0.0.1", port=6379, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # --- sending ---

    @staticmethod
    def encode(*args):
        out = b"*%d\r\n" % len(args)
        for arg in args:
            if isinstance(arg, str):
                arg = arg.encode()
            elif not isinstance(arg, (bytes, bytearray)):
                arg = str(arg).encode()
            out += b"$%d\r\n%s\r\n" % (len(arg), arg)
        return out

    def send_raw(self, data):
        self.sock.sendall(data)

    def cmd(self, *args):
        self.sock.sendall(self.encode(*args))
        return self.read_reply()

    # --- receiving ---

    def _fill(self):
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise EOFError("server closed the connection")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]

        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return Error(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"_":            # RESP3 null
            return None
        if kind == b",":            # RESP3 double
            if rest == b"inf":
                return float("inf")
            if rest == b"-inf":
                return float("-inf")
            return float(rest)
        if kind == b"#":            # RESP3 boolean
            return rest == b"t"
        if kind in (b"$", b"="):    # bulk / verbatim
            length = int(rest)
            if length == -1:
                return None
            while len(self.buf) < length + 2:
                self._fill()
            data, self.buf = self.buf[:length], self.buf[length + 2:]
            if kind == b"=":
                data = data[4:]     # strip the "txt:" prefix
            return data.decode(errors="replace")
        if kind in (b"*", b"~", b">"):  # array / set / push
            count = int(rest)
            if count == -1:
                return None
            return [self.read_reply() for _ in range(count)]
        if kind == b"%":            # RESP3 map
            count = int(rest)
            result = {}
            for _ in range(count):
                key = self.read_reply()
                result[tuple(key) if isinstance(key, list) else key] = self.read_reply()
            return result

        raise ValueError("unknown reply type %r in %r" % (kind, line))
