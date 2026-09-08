"""Un client survie minimal, dont le seul métier est de miner et de chronométrer.

Le serveur casse le bloc lui-même quand la progression atteint 1 : le client
n'envoie que « je commence ». Le nombre de ticks entre l'envoi et le Block
Update de retour est donc la durée que le jeu applique — pas une durée qu'on
lui aurait supposée.

Deux détails changent le résultat d'un facteur cinq chacun et sont donc tenus :
le joueur doit être annoncé **au sol**, et il ne doit pas être dans l'eau.
"""
import socket, struct, time, zlib

def varint(n):
    o = b""
    while True:
        b_ = n & 0x7F
        n >>= 7
        o += bytes([b_ | (0x80 if n else 0)])
        if not n:
            return o

def read_varint(buf, i):
    v = 0; sh = 0
    while True:
        b_ = buf[i]; i += 1
        v |= (b_ & 0x7F) << sh
        if not b_ & 0x80:
            return v, i
        sh += 7

def block_pos(x, y, z):
    v = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", v)

class Miner:
    def __init__(self, port, name, host="127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=60)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.threshold = None
        self.name = name
        self.pos = None
        # The server's own clock. Update Time carries the world age in ticks and
        # arrives every second; anchoring on it turns a wall-clock stopwatch
        # into exact tick counts, which is the difference between "about 150"
        # and a hardness we can write down.
        self.clock = None
        self.last_arrival = 0.0
        h = host.encode()
        self.send(0x00, varint(763) + varint(len(h)) + h + struct.pack(">H", port) + varint(2))
        self.send(0x00, varint(len(name.encode())) + name.encode() + bytes([0]))
        self.buf = b""
        while True:
            pid, p = self.read()
            if pid == 0x03 and self.threshold is None:
                self.threshold, _ = read_varint(p, 0)
            elif pid == 0x02:
                break

    def send(self, pid, payload):
        body = varint(pid) + payload
        if self.threshold is None:
            self.s.sendall(varint(len(body)) + body)
        else:
            inner = (varint(0) + body if len(body) < self.threshold
                     else varint(len(body)) + zlib.compress(body))
            self.s.sendall(varint(len(inner)) + inner)

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read(self):
        length = 0; shift = 0
        while True:
            b_ = self._recv_exact(1)[0]
            length |= (b_ & 0x7F) << shift
            if not b_ & 0x80:
                break
            shift += 7
        data = self._recv_exact(length)
        self.last_arrival = time.monotonic()
        i = 0
        if self.threshold is not None:
            size, i = read_varint(data, i)
            data = data[i:] if size == 0 else zlib.decompress(data[i:])
            i = 0
        pid, i = read_varint(data, i)
        return pid, data[i:]

    def pump(self, until=None, timeout=1.0):
        """Answer housekeeping until `until(pid, payload)` returns a value."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.s.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                pid, p = self.read()
            except (socket.timeout, TimeoutError):
                return None
            if pid == 0x5E and len(p) >= 8:       # update time
                self.clock = (struct.unpack_from(">q", p, 0)[0], time.monotonic())
            elif pid == 0x23:                     # keep alive
                self.send(0x12, p[:8])
            elif pid == 0x3C:                     # synchronize position
                x, y, z = struct.unpack_from(">ddd", p, 0)
                self.pos = (x, y, z)
                # X, Y, Z, yaw, pitch, flags, *then* the teleport id: 33 bytes
                # in, not 32. Confirming with the flags byte instead answers 0,
                # the server keeps waiting, and from then on it ignores every
                # movement **and every block placement** without a word. Digging
                # still works, which is what makes the mistake survive.
                tid, _ = read_varint(p, 33)
                self.send(0x00, varint(tid))
                self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))
            if until is not None:
                got = until(pid, p)
                if got is not None:
                    return got
        return None

    def tick_at(self, when, anchor=None):
        """The server tick a wall-clock instant falls in.

        The anchor must be the *same* for both ends of a measurement. Using
        whichever Update Time happened to arrive last leaves each end resting on
        a different, independently jittered reference, and the difference then
        carries both errors instead of cancelling them."""
        anchor = anchor or self.clock
        if anchor is None:
            return None
        age, at = anchor
        return age + round((when - at) / 0.05)

    def stand(self, x, y, z):
        """Announce a position, on the ground. Off the ground mines five times slower."""
        self.pos = (x, y, z)
        self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))

    def mine(self, x, y, z, timeout=30.0):
        """Start breaking, and return how long the server took, in seconds."""
        target = block_pos(x, y, z)
        self.send(0x1D, bytes([0]) + target + bytes([1]) + varint(1))
        start = time.monotonic()

        def broken(pid, p):
            if pid != 0x0A or p[:8] != target:
                return None
            state, _ = read_varint(p, 8)
            return state
        state = self.pump(until=broken, timeout=timeout)
        if state is None:
            return None
        return time.monotonic() - start

    def mine_timed(self, x, y, z, timeout=40.0):
        """Break a block the way the game does, and return the elapsed seconds.

        The server does not finish the job on its own: it waits for the client
        to say "done", then checks. Saying it immediately is below the 0.7
        threshold that would let a claim through, so the server falls back to
        its own clock and breaks the block at exactly the tick vanilla would —
        which is the number we came for, and one the client never had to know.
        """
        target = block_pos(x, y, z)
        anchor = self.clock
        # Sent mid-tick, the start lands in this tick or the next depending on
        # the millisecond, and the count comes out one short about half the
        # time. The server's own clock says where the boundaries are, so wait
        # for one and send just after it.
        if anchor is not None:
            age, at = anchor
            phase = (time.monotonic() - at) % 0.05
            time.sleep((0.05 - phase) + 0.006 if phase > 0.004 else 0.006 - phase)
        self.send(0x1D, bytes([0]) + target + bytes([1]) + varint(1))
        start = time.monotonic()
        self.send(0x1D, bytes([2]) + target + bytes([1]) + varint(2))
        start_tick = self.tick_at(start, anchor)

        broke_at = [None]

        def broken(pid, p):
            if pid != 0x0A or p[:8] != target:
                return None
            state, _ = read_varint(p, 8)
            # Any change at the target, not only "became air": ice breaks into
            # water, and filtering on air alone reads that as a block that never
            # broke at all.
            broke_at[0] = self.tick_at(self.last_arrival, anchor)
            return state if state is not None else 0

        if self.pump(until=broken, timeout=timeout) is None:
            return None
        if start_tick is None or broke_at[0] is None:
            return None
        return broke_at[0] - start_tick

