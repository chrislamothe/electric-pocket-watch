"""Improv Wi-Fi serial client for the Electric Pocket Watch (stand-in for the web flasher).

usage:
  python wifi.py COM3 info
  python wifi.py COM3 scan
  python wifi.py COM3 set "<ssid>" "<password>"

Does not reset the watch (DTR/RTS held low). Prints the watch's log lines as they arrive.
"""
import sys
import time

import serial

HDR = b"IMPROV\x01"
STATES = {0x00: "stopped", 0x02: "ready", 0x03: "provisioning", 0x04: "provisioned"}
ERRORS = {0x00: "none", 0x01: "invalid RPC packet", 0x02: "unknown command",
          0x03: "unable to connect", 0xFF: "unknown error"}


def packet(rpc_cmd: int, data: bytes) -> bytes:
    body = HDR + bytes([0x03, len(data) + 2, rpc_cmd, len(data)]) + data
    return body + bytes([sum(body) & 0xFF, 0x0A])


def parse_strings(d: bytes):
    out, i = [], 0
    while i < len(d):
        n = d[i]
        out.append(d[i + 1:i + 1 + n].decode(errors="replace"))
        i += 1 + n
    return out


def main():
    port, cmd = sys.argv[1], sys.argv[2]
    if cmd == "info":
        pkt, wait = packet(0x03, b""), 5
    elif cmd == "scan":
        pkt, wait = packet(0x04, b""), 20
    elif cmd == "set":
        ssid, pw = sys.argv[3].encode(), sys.argv[4].encode()
        pkt, wait = packet(0x01, bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw), 35
    else:
        sys.exit(__doc__)

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr, s.rts = False, False
    s.open()
    s.write(pkt)
    buf = b""
    t0 = time.time()
    done = False
    while time.time() - t0 < wait and not done:
        buf += s.read(512)
        while True:
            i = buf.find(HDR)
            if i < 0:
                *lines, buf = buf.split(b"\n")
                for ln in lines:
                    if ln.strip():
                        print("  log:", ln.decode(errors="replace").rstrip())
                break
            for ln in buf[:i].split(b"\n"):
                if ln.strip():
                    print("  log:", ln.decode(errors="replace").rstrip())
            if len(buf) < i + 9:
                buf = buf[i:]
                break
            n = buf[i + 8]
            end = i + 9 + n + 1
            if len(buf) < end:
                buf = buf[i:]
                break
            typ, data = buf[i + 7], buf[i + 9:i + 9 + n]
            buf = buf[end:]
            if typ == 0x01:
                st = STATES.get(data[0], hex(data[0]))
                print("STATE:", st)
            elif typ == 0x02:
                print("ERROR:", ERRORS.get(data[0], hex(data[0])))
                if data[0] == 0x03:
                    done = True
            elif typ == 0x04:
                strs = parse_strings(data[2:2 + data[1]])
                print("RESULT:", strs)
                if cmd != "scan" or not strs:
                    done = True
    s.close()


if __name__ == "__main__":
    main()
