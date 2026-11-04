#!/usr/bin/env python3
"""Minimal GDB remote serial protocol client for the QEMU gdb stub.

Written because no gdb is installed in this environment and installing one
needs sudo, which times out here. The protocol is simple enough to speak
directly: packets are "$<payload>#<two hex checksum digits>" and the peer
acknowledges each one with "+".

Intended use is answering a single control-flow question about a guest
user-mode address without a full debugger, for example whether the Direct3D 9
format-list builder reaches its capability gate.

  python3 tools/gdb-rsp.py --port 49200 --hbreak 0x4fdfb6ff --hbreak 0x4fdfb874

Hardware breakpoints (Z1) are used rather than software ones (Z0) on purpose:
Z0 writes a trap byte into guest memory, which only works if the page is mapped
in whatever address space happens to be current when the breakpoint is set. The
target address lives in a DLL inside one user-mode process, so at attach time it
is usually not mapped. A debug-register breakpoint needs no such write.
"""

import argparse
import socket
import sys
import time


class RspError(Exception):
    pass


class Rsp(object):
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b''

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _recv_some(self):
        chunk = self.sock.recv(4096)
        if not chunk:
            raise RspError('connection closed by the stub')
        self.buf += chunk

    def send(self, payload):
        body = payload.encode('ascii')
        checksum = sum(body) & 0xff
        packet = b'$' + body + b'#' + ('%02x' % checksum).encode('ascii')
        self.sock.sendall(packet)
        # The stub acknowledges with '+'. Tolerate leading noise.
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if b'+' in self.buf:
                self.buf = self.buf.replace(b'+', b'', 1)
                return
            self._recv_some()
        raise RspError('no acknowledgement for %r' % payload)

    def recv(self, timeout=None):
        """Read one packet payload, or None when nothing arrives in time."""
        if timeout is not None:
            self.sock.settimeout(timeout)
        deadline = None if timeout is None else time.time() + timeout
        while True:
            start = self.buf.find(b'$')
            end = self.buf.find(b'#', start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buf) >= end + 3:
                payload = self.buf[start + 1:end]
                self.buf = self.buf[end + 3:]
                self.sock.sendall(b'+')
                return payload.decode('ascii', 'replace')
            if deadline is not None and time.time() > deadline:
                return None
            try:
                self._recv_some()
            except socket.timeout:
                if deadline is not None and time.time() > deadline:
                    return None
            except RspError:
                return None

    def command(self, payload, timeout=5.0):
        self.send(payload)
        return self.recv(timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--hbreak', action='append', default=[],
                        help='hardware breakpoint address, e.g. 0x4fdfb6ff')
    parser.add_argument('--sbreak', action='append', default=[],
                        help='software breakpoint address (Z0)')
    parser.add_argument('--watch', action='append', default=[],
                        help='write watchpoint address (Z2); stops on the '
                             'instruction that writes the location, which '
                             'identifies a writer without having to guess '
                             'which function it lives in')
    parser.add_argument('--watch-len', default='4',
                        help='watchpoint length in bytes (default 4)')
    parser.add_argument('--rip-range', default=None, metavar='LO:HI',
                        help='only report stops whose RIP is in [LO,HI); other '
                             'stops are resumed without consuming the hit '
                             'budget. A watchpoint on a heap address catches '
                             'unrelated code reusing that memory, which floods '
                             'the budget with noise.')
    parser.add_argument('--trace-until', default=None, metavar='ADDR',
                        help='after the first stop, single-step until this '
                             'address is reached, then print the path taken. '
                             'Turns a multi-run bisection of a long validation '
                             'routine into one run.')
    parser.add_argument('--trace-max', type=int, default=4000,
                        help='maximum single-steps while tracing')
    parser.add_argument('--trace-tail', type=int, default=48,
                        help='how many trailing addresses to print')
    parser.add_argument('--max-stops', type=int, default=4000,
                        help='safety cap on total stops examined')
    parser.add_argument('--mem', action='append', default=[],
                        metavar='REG+OFF:LEN',
                        help='at each stop, dump LEN bytes of guest memory at '
                             'register REG plus optional hex offset, e.g. '
                             'rcx:16 or rcx+0x6c:8')
    parser.add_argument('--hits', type=int, default=1,
                        help='continue through up to this many stops, '
                             'reporting each; the window is shared')
    parser.add_argument('--wait', type=float, default=600.0,
                        help='seconds to wait for a breakpoint hit')
    parser.add_argument('--connect-timeout', type=float, default=120.0)
    args = parser.parse_args()

    deadline = time.time() + args.connect_timeout
    rsp = None
    while rsp is None:
        try:
            rsp = Rsp(args.host, args.port, 10.0)
        except OSError as exc:
            if time.time() > deadline:
                print('CONNECT FAILED %s' % exc)
                return 2
            time.sleep(1.0)
    print('CONNECTED %s:%d' % (args.host, args.port))

    # Attaching halts the guest. Announce no special features; the stub copes.
    reply = rsp.command('?')
    print('STOP-REASON %s' % reply)

    armed = 0
    for kind, specs in (('Z1', args.hbreak), ('Z0', args.sbreak),
                        ('Z2', args.watch)):
        for spec in specs:
            addr = int(spec, 16)
            length = int(args.watch_len, 0) if kind == 'Z2' else 1
            reply = rsp.command('%s,%x,%x' % (kind, addr, length))
            if reply == 'OK':
                armed += 1
                print('BREAK %s 0x%08x OK' % (kind, addr))
            else:
                # An empty packet is the protocol's "unsupported" answer.
                why = 'UNSUPPORTED' if reply == '' else ('ERROR %r' % reply)
                print('BREAK %s 0x%08x %s' % (kind, addr, why))
    if armed == 0:
        # Attaching halts the guest, so we MUST resume it before leaving.
        # An earlier version returned here without detaching and left the
        # virtual machine frozen, stalling the test cycle that owned it.
        print('NO-BREAKPOINTS none could be armed; detaching to resume guest')
        try:
            rsp.command('D', timeout=5.0)
        except (OSError, RspError):
            pass
        rsp.close()
        return 3

    # Stopping at the first hit answers only "was this reached".  The useful
    # question is usually which of several armed addresses are reached and in
    # what order, so continue through up to args.hits stops and report each.
    lo = hi = None
    if args.rip_range:
        bounds = args.rip_range.split(':')
        lo, hi = int(bounds[0], 0), int(bounds[1], 0)

    deadline = time.time() + args.wait
    reported = 0
    skipped = 0
    stops = 0
    while reported < args.hits and stops < args.max_stops:
        remaining = deadline - time.time()
        if remaining <= 0:
            print('WINDOW-EXPIRED after %d reported, %d skipped'
                  % (reported, skipped))
            break
        try:
            rsp.send('c')
            stop = rsp.recv(remaining)
        except (OSError, RspError) as exc:
            print('DISCONNECTED %s' % exc)
            rsp.close()
            return 1

        if stop is None:
            print('NO-HIT window elapsed; %d reported, %d skipped'
                  % (reported, skipped))
            break
        if stop[:1] in ('W', 'X'):
            print('GUEST-EXITED %s after %d reported, %d skipped'
                  % (stop, reported, skipped))
            break
        stops += 1
        try:
            regs = rsp.command('g', timeout=10.0)
        except (OSError, RspError) as exc:
            print('HIT but registers unavailable: %s' % exc)
            continue
        rip = register_value(regs, 16)
        if lo is not None and rip is not None and not (lo <= rip < hi):
            skipped += 1
            continue
        reported += 1
        print('CONTINUE (%d/%d) %d skipped so far'
              % (reported, args.hits, skipped))
        report_stop(rsp, stop, args, regs)
        if args.trace_until:
            trace_to(rsp, args)
            break

    # Always detach so the guest resumes; a frozen guest stalls the owning run.
    print('DETACHING to resume guest')
    try:
        rsp.command('D', timeout=5.0)
    except (OSError, RspError):
        pass
    rsp.close()
    return 0


REG_INDEX = {'rax': 0, 'rbx': 1, 'rcx': 2, 'rdx': 3,
             'rsi': 4, 'rdi': 5, 'rbp': 6, 'rsp': 7, 'rip': 16}


def dump_memory(rsp, regs, spec):
    """Dump guest memory at a register plus optional offset.  Reading the
    values a branch actually tests is the only way to tell a wrong value from a
    wrong assumption about which value is being tested."""
    target, _, length = spec.partition(':')
    name, _, offset = target.partition('+')
    index = REG_INDEX.get(name.strip().lower())
    if index is None:
        print('MEM-BAD-REG %s' % spec)
        return
    field = regs[index * 8 * 2:(index + 1) * 8 * 2]
    try:
        base = int.from_bytes(bytes.fromhex(field), 'little')
        addr = base + (int(offset, 0) if offset else 0)
        count = int(length, 0) if length else 16
    except ValueError:
        print('MEM-BAD-SPEC %s' % spec)
        return
    try:
        data = rsp.command('m%x,%x' % (addr, count), timeout=10.0)
    except (OSError, RspError) as exc:
        print('MEM 0x%08x unavailable: %s' % (addr, exc))
        return
    if not data or data.startswith('E'):
        print('MEM 0x%08x error %r' % (addr, data))
        return
    print('MEM 0x%08x %s' % (addr, data))
    # Also render as little-endian dwords, which is how the fields being
    # inspected here are actually laid out.
    words = []
    for pos in range(0, len(data) - 7, 8):
        try:
            words.append('0x%08x' % int.from_bytes(
                bytes.fromhex(data[pos:pos + 8]), 'little'))
        except ValueError:
            break
    if words:
        print('MEM-DWORDS %s' % ' '.join(words))


def trace_to(rsp, args):
    """Single-step until the target address, then print the path. Reading the
    branch actually taken beats inferring it from a static reading of many
    candidate branches."""
    target = int(args.trace_until, 0)
    path = []
    for step in range(args.trace_max):
        try:
            rsp.send('s')
            stop = rsp.recv(30.0)
        except (OSError, RspError) as exc:
            print('TRACE-DISCONNECTED after %d steps: %s' % (step, exc))
            return
        if stop is None or stop[:1] in ('W', 'X'):
            print('TRACE-ENDED %s after %d steps' % (stop, step))
            break
        try:
            regs = rsp.command('g', timeout=10.0)
        except (OSError, RspError):
            print('TRACE-REGS-LOST after %d steps' % step)
            break
        rip = register_value(regs, 16)
        if rip is None:
            break
        path.append(rip)
        if rip == target:
            print('TRACE-REACHED 0x%08x after %d steps' % (target, step + 1))
            break
    else:
        print('TRACE-MAX %d steps without reaching 0x%08x'
              % (args.trace_max, target))
    tail = path[-args.trace_tail:]
    print('TRACE-PATH (%d steps, last %d):' % (len(path), len(tail)))
    print(' '.join('%08x' % value for value in tail))


def register_value(regs, index, width=8):
    field = regs[index * width * 2:(index + 1) * width * 2]
    try:
        return int.from_bytes(bytes.fromhex(field), 'little')
    except ValueError:
        return None


def report_stop(rsp, stop, args=None, regs=None):
    """Print one breakpoint stop. Never closes the socket: the caller keeps
    continuing and must still be able to detach so the guest resumes."""
    print('HIT %s' % stop)
    if regs is None:
        try:
            regs = rsp.command('g', timeout=10.0)
        except (OSError, RspError) as exc:
            print('HIT but registers unavailable: %s' % exc)
            return 0
    # The 'g' reply is a hex string, so every register byte is two characters.
    # qemu-system-x86_64 serves the x86-64 layout even while the guest runs in
    # 32-bit mode: registers 0-15 are RAX..R15 at 8 bytes each, and RIP is
    # register 16.  Indexing this string by byte counts silently reads the
    # middle of another register, which is what made a real hit print zero.
    reg_bytes = 8
    rip_index = 16
    need = (rip_index + 1) * reg_bytes * 2
    if regs and len(regs) >= need:
        rip = regs[rip_index * reg_bytes * 2:need]
        try:
            value = int.from_bytes(bytes.fromhex(rip), 'little')
        except ValueError:
            print('REGS %s' % regs[:80])
            return 0
        print('RIP 0x%016x' % value)
        # x86-64 'g' order is RAX,RBX,RCX,RDX,RSI,RDI,RBP,RSP then R8-R15.
        # An earlier version used the i386 order and mislabelled RDX as RBX.
        for index, name in ((0, 'RAX'), (1, 'RBX'), (2, 'RCX'), (3, 'RDX'),
                            (4, 'RSI'), (5, 'RDI'), (6, 'RBP'), (7, 'RSP')):
            field = regs[index * reg_bytes * 2:(index + 1) * reg_bytes * 2]
            try:
                print('%s 0x%016x'
                      % (name, int.from_bytes(bytes.fromhex(field), 'little')))
            except ValueError:
                pass
    else:
        print('REGS-SHORT len=%d need=%d' % (len(regs or ''), need))
        return 0
    for spec in (getattr(args, 'mem', None) or []):
        dump_memory(rsp, regs, spec)
    return 0


if __name__ == '__main__':
    sys.exit(main())
