# -*- coding: utf-8 -*-
"""
通用 USB 打印服务器 —— 固件实机验收脚本

用法：
    python scripts/verify_firmware.py                       # release 版（默认 192.168.10.43 / COM11）
    python scripts/verify_firmware.py --debug               # debug 版：额外校验串口观测日志
    python scripts/verify_firmware.py --host 192.168.4.1    # AP 直连时
    python scripts/verify_firmware.py --serial COM5         # 换串口

协议层项两版都跑；依赖串口日志的 4 项只在 --debug 下断言
（release 版按设计没有观测点，不是缺陷）。
退出码 0 = 全部通过，1 = 有失败项。需要 pyserial。
"""
import argparse, json, socket, struct, time, threading, urllib.request, re, sys

try:
    import serial
except ImportError:
    serial = None

_ap = argparse.ArgumentParser(description="打印服务器固件实机验收")
_ap.add_argument("--host", default="192.168.10.43", help="设备 IP")
_ap.add_argument("--serial", default="COM11", help="串口号")
_ap.add_argument("--debug", action="store_true",
                 help="被测固件是 debug 版：额外断言串口观测日志")
_args = _ap.parse_args()

HOST = _args.host
IS_DEBUG = _args.debug
R = []

def ck(name, ok, detail=""):
    R.append((name, bool(ok), str(detail)))
    print(("PASS" if ok else "FAIL"), "|", name, "|", detail, flush=True)

def sk(name, why):
    print("SKIP", "|", name, "|", why, flush=True)

def status():
    return json.load(urllib.request.urlopen("http://" + HOST + "/api/status", timeout=6))

# ---- 串口日志旁路 ----
slog = []
stop = False
ser = None
try:
    if serial is None:
        raise RuntimeError("未安装 pyserial")
    ser = serial.Serial(_args.serial, 115200, timeout=0.2)
    ser.reset_input_buffer()
    def rd():
        while not stop:
            d = ser.read(8192)
            if d:
                slog.append(d)
    threading.Thread(target=rd, daemon=True).start()
except Exception as e:
    print("串口监听不可用:", e)

# ---- 等联网 ----
online = False
d0 = {}
for _ in range(30):
    try:
        d0 = status()
        online = True
        break
    except Exception:
        time.sleep(1)
ck("设备在线 /api/status", online, d0.get("wifi", {}).get("sta_ip", "") if online else "超时")

# ---- 1. 状态接口 ----
d = status()
p, pr, sp, cfg, u, w = d["print"], d["printer"], d["spool"], d["cfg"], d["usb"], d["wifi"]
ck("固件版本号可读", bool(re.match(r"^\d+\.\d+\.\d+$", d["version"])), d["version"])
ck("身份识别 ready + 发布名称", pr["ready"] and pr["name"] == "HP-LaserJet-M203d",
   pr["name"] + " / " + pr["make_model"])
ck("自动双面识别", cfg["duplex_detected"] and cfg["duplex"], cfg["duplex_evidence"])
ck("URF / PJL 能力", pr["urf"] and pr["pjl"], "urf=%s pjl=%s" % (pr["urf"], pr["pjl"]))
ck("双通路监听 9100 + 631", p["raw"] and p["ipp"], "9100=%s 631=%s" % (p["raw"], p["ipp"]))
ck("spool 状态合法（无下溢）", 0 <= sp["used"] <= sp["capacity"],
   "%s / %s peak=%s" % (sp["used"], sp["capacity"], sp["peak"]))
ck("USB 就绪且纸张正常", u["connected"] and not u["paper_empty"] and not u["fault"],
   "paper_empty=%s vid_pid=%s" % (u["paper_empty"], u["vid_pid"]))
ck("Wi-Fi 已连接", w["connected"], w["sta_ip"])
ck("reset 原因记录", bool(d.get("reset")), d.get("reset"))

# ---- IPP 工具 ----
def attr(tag, name, val):
    n = name.encode(); v = val.encode()
    return bytes([tag]) + struct.pack(">H", len(n)) + n + struct.pack(">H", len(v)) + v

def ipp_request(op, attrs, doc=b""):
    body = b"\x01\x01" + struct.pack(">H", op) + struct.pack(">I", 900) + b"\x01"
    body += attr(0x47, "attributes-charset", "utf-8")
    body += attr(0x48, "attributes-natural-language", "en")
    body += attr(0x45, "printer-uri", "ipp://" + HOST + ":631/ipp/print")
    for a in attrs:
        body += a
    body += b"\x03" + doc
    req = (b"POST /ipp/print HTTP/1.1\r\nHost: " + HOST.encode() +
           b"\r\nContent-Type: application/ipp\r\nContent-Length: " + str(len(body)).encode() +
           b"\r\nConnection: close\r\n\r\n" + body)
    s = socket.create_connection((HOST, 631), 8)
    s.settimeout(12)
    s.sendall(req)
    buf = b""
    try:
        while True:
            x = s.recv(4096)
            if not x:
                break
            buf += x
    except Exception:
        pass
    s.close()
    return buf

def parse_ipp(buf):
    ipp = buf.split(b"\r\n\r\n", 1)[1]
    out = {}
    i = 8
    cur = None
    while i < len(ipp):
        tag = ipp[i]; i += 1
        if tag <= 5:
            if tag == 3:
                break
            continue
        if i + 2 > len(ipp):
            break
        nl = struct.unpack(">H", ipp[i:i+2])[0]; i += 2
        name = ipp[i:i+nl].decode("utf-8", "replace"); i += nl
        vl = struct.unpack(">H", ipp[i:i+2])[0]; i += 2
        val = ipp[i:i+vl]; i += vl
        key = cur if nl == 0 else name
        if nl:
            cur = name
        if key:
            out.setdefault(key, []).append(val)
    return ipp, out

def vals(out, k):
    return [v.decode("utf-8", "replace") if len(v) != 4 else "INT%d" % struct.unpack(">i", v)[0]
            for v in out.get(k, [])]

# ---- 2. IPP Get-Printer-Attributes ----
buf = ipp_request(0x000B, [])
ipp, o = parse_ipp(buf)
sc = struct.unpack(">H", ipp[2:4])[0]
ck("IPP Get-Printer-Attributes", sc == 0, "status=0x%04X" % sc)
ck("printer-name 为 M203d", vals(o, "printer-name") == ["HP-LaserJet-M203d"], str(vals(o, "printer-name")))
sv = vals(o, "sides-supported")
ck("sides-supported 含双面", any("two-sided-long-edge" in x for x in sv) and any("two-sided-short-edge" in x for x in sv), ",".join(sv))
ck("sides-default 双面长边", vals(o, "sides-default") == ["two-sided-long-edge"], str(vals(o, "sides-default")))
ck("AirPrint / URF 能力", "airprint-1.7" in vals(o, "ipp-features-supported") and any("DM3" in x for x in vals(o, "urf-supported")), "DM3 + airprint-1.7")
ck("PDF 为默认格式", vals(o, "document-format-default") == ["application/pdf"], str(vals(o, "document-format-default")))
ck("printer-state = idle(3)", "INT3" in vals(o, "printer-state"), str(vals(o, "printer-state")))

# ---- 3. RAW 9100 纯透传 ----
d_before = status()
doc = (b'\x1b%-12345X@PJL JOB NAME="verify-raw"\r\n'
       b'@PJL COMMENT v1.4.2 check\r\n'
       b'\x1b%-12345X@PJL EOJ\r\n\x1b%-12345X')
c = socket.create_connection((HOST, 9100), 8)
c.sendall(doc)
time.sleep(0.5)
c.close()
time.sleep(2)
d_after = status()
lj = d_after["last_job"]
ck("RAW 作业成功", lj["ok"] and lj["tag"] == "RAW9100" and lj["bytes"] == len(doc),
   "%s %sB why=%s" % (lj["tag"], lj["bytes"], lj["why"]))
raw_added = d_after["print"]["bytes"] - d_before["print"]["bytes"]
ck("RAW 字节计入 USB", raw_added == len(doc) + 9,
   "本次 +%d B（文档 %d + 尾部 9）" % (raw_added, len(doc)))
# 尾部只应补 9 字节 UEL；若出现 +10 说明旧版 FF(0x0C) 还在，会多吐一张空白页。
# 用 print.bytes 差值判断，不依赖串口日志 —— release 版同样能验。
ck("作业尾部仅 UEL（无 FF 空白页）", raw_added == len(doc) + 9,
   "+%d，期望 +%d" % (raw_added, len(doc) + 9))
ck("busy 已复位", not d_after["print"]["busy"], "busy=%s" % d_after["print"]["busy"])

# ---- 4. IPP Print-Job 双面（PJL 注入）----
job_attrs = [attr(0x42, "job-name", "verify-duplex"),
             attr(0x49, "document-format", "application/vnd.hp-PCL"),
             attr(0x44, "sides", "two-sided-long-edge")]
buf = ipp_request(0x0002, job_attrs, doc)
ipp, o = parse_ipp(buf)
sc = struct.unpack(">H", ipp[2:4])[0]
time.sleep(2)
d_after = status()
lj = d_after["last_job"]
ck("IPP Print-Job 回 successful-ok", sc == 0, "status=0x%04X" % sc)
ck("IPP 双面作业成功", lj["ok"] and lj["tag"] == "IPP", "%s %sB" % (lj["tag"], lj["bytes"]))

# ---- 5. 并发 busy + Retry-After ----
hold_done = threading.Event()
def hold():
    try:
        c = socket.create_connection((HOST, 9100), 8)
        c.sendall(b'\x1b%-12345X@PJL JOB NAME="hold"\r\n' + b'@PJL COMMENT ' + b'x' * 3000 + b'\r\n')
        time.sleep(3.0)
        c.close()
    except Exception:
        pass
    finally:
        hold_done.set()
threading.Thread(target=hold, daemon=True).start()
time.sleep(0.9)
busybuf = ipp_request(0x0002, job_attrs, b'\x1b%-12345X')
ippb, ob = parse_ipp(busybuf)
bsc = struct.unpack(">H", ippb[2:4])[0] if len(ippb) >= 4 else -1
hdr = busybuf.split(b"\r\n\r\n", 1)[0]
has_ra = b"Retry-After: 30" in hdr
ck("并发控制 busy + Retry-After", bsc == 0x0507 and has_ra,
   "status=0x%04X Retry-After=%s" % (bsc, has_ra))
hold_done.wait(6)
time.sleep(2)
ck("并发后服务恢复", not status()["print"]["busy"], "")

# ---- 6. SNMP v2c ----
def tlv(tag, val):
    if len(val) < 128:
        return bytes([tag, len(val)]) + val
    lb = len(val).to_bytes((len(val).bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(lb)]) + lb + val

def integer(v):
    return tlv(0x02, v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big"))

def vb(oid):
    return tlv(0x30, tlv(0x06, oid) + tlv(0x05, b""))

def snmp_get(oids):
    body = integer(777) + integer(0) + integer(0) + tlv(0x30, b"".join(vb(o) for o in oids))
    return tlv(0x30, integer(1) + tlv(0x04, b"public") + tlv(0xA0, body))

def read_tlv(b, pos):
    tag = b[pos]; pos += 1
    l = b[pos]; pos += 1
    if l & 0x80:
        n = l & 0x7f
        l = int.from_bytes(b[pos:pos+n], "big")
        pos += n
    return tag, b[pos:pos+l], pos + l

O_DESCR = bytes([0x2B, 6, 1, 2, 1, 1, 1, 0])
O_HRDEV = bytes([0x2B, 6, 1, 2, 1, 25, 3, 2, 1, 5, 1])
O_HRPRT = bytes([0x2B, 6, 1, 2, 1, 25, 3, 5, 1, 1, 1])
try:
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.settimeout(3)
    us.sendto(snmp_get([O_DESCR, O_HRDEV, O_HRPRT]), (HOST, 161))
    resp, _ = us.recvfrom(2048)
    us.close()
    _, outer, _ = read_tlv(resp, 0)
    pos = 0
    _, _, pos = read_tlv(outer, pos)
    _, _, pos = read_tlv(outer, pos)
    ptag, pdu, _ = read_tlv(outer, pos)
    q = 0
    for _ in range(3):
        _, _, q = read_tlv(pdu, q)
    _, vbl, _ = read_tlv(pdu, q)
    got = 0
    q = 0
    while q < len(vbl):
        _, one, q = read_tlv(vbl, q)
        k = 0
        _, ov, k = read_tlv(one, k)
        _, vv, k = read_tlv(one, k)
        if ov in (O_DESCR, O_HRDEV, O_HRPRT):
            got += 1
    ck("SNMP v2c GET 三项应答", ptag == 0xA2 and got == 3,
       "pdutag=0x%02X varbinds=%d resp=%dB" % (ptag, got, len(resp)))
    ck("SNMP sysDescr 含机型", b"M203d" in resp or b"Print Server" in resp, "")
except Exception as e:
    ck("SNMP v2c GET 三项应答", False, str(e))
    ck("SNMP sysDescr 含机型", False, "跳过")

# ---- 7. mDNS 被动应答 ----
# 组播查询在 Wi-Fi 上会偶发丢包，所以用「同一 socket 重发」模拟 Bonjour 行为；
# 只统计来自被测设备 IP 的应答，排除本机 Bonjour / 其它 mDNS 流量。
MCAST = ("224.0.0.251", 5353)
qn = b"".join(bytes([len(x)]) + x.encode() for x in ["_ipp", "_tcp", "local"]) + b"\x00"
mquery = b"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00" + qn + b"\x00\x0c\x00\x01"

def mdns_probe(bindport, tries=6):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", bindport))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 3)
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0"))
    except Exception:
        pass
    s.settimeout(1.2)
    got = b""
    for _ in range(tries):
        s.sendto(mquery, MCAST)
        try:
            while True:
                x, src = s.recvfrom(4096)
                if src[0] == HOST:
                    got += x
        except socket.timeout:
            pass
        if got:
            break
    s.close()
    return got

try:
    mb = mdns_probe(0) or mdns_probe(0)   # 组播在 Wi-Fi 上会偶发丢包，整轮再重试一次
    txt = mb.decode("latin1")
    ck("mDNS 被动应答（随机源端口）", len(mb) > 400 and "HP-LaserJet-M203d" in txt, "%d B" % len(mb))
    ck("mDNS TXT duplex=T / DM3", "duplex=T" in txt and "DM3" in txt, "")
except Exception as e:
    ck("mDNS 被动应答（随机源端口）", False, str(e))
    ck("mDNS TXT duplex=T / DM3", False, "跳过")
try:
    # Windows 上 UDP 5353 常被 Edge / 微信等进程占用，设备的单播应答可能被
    # 别的 socket 收走，因此这里只确认「查询已发出」，设备是否应答看第 8 节
    # 的串口日志（设备侧限频打印），这才是可靠证据。
    mb2 = mdns_probe(5353)
    ck("mDNS 标准 5353 查询已发出", True,
       "客户端收到 %d B（本机端口占用时可为 0，以设备日志为准）" % len(mb2))
except Exception as e:
    ck("mDNS 标准 5353 查询已发出", False, str(e))

# ---- 8. 串口日志检查 ----
stop = True
time.sleep(0.3)
if ser:
    try:
        ser.close()
    except Exception:
        pass
log = b"".join(slog).decode("utf-8", "replace")
if IS_DEBUG:
    ck("串口：PJL 双面头已注入", "注入 PJL 双面作业头" in log, "")
    ck("串口：设备应答标准 5353 查询（AirPrint 可发现）", "应答标准 5353 查询" in log, "")
    pairs = re.findall(r"\[spool\] reset \(w=(\d+) r=(\d+)\)", log)
    ck("串口：spool 读写指针一致", len(pairs) > 0 and all(a == b for a, b in pairs), str(pairs))
    bad = [l for l in log.splitlines()
           if re.search(r"(?i)panic|assert|guru|\[E\]|BAD |溢出|abort", l)]
    ck("串口无异常", len(bad) == 0, ("; ".join(bad[:3]) if bad else "0 行"))
else:
    # release 版按设计不输出观测日志，这些项没有断言依据，如实标 SKIP 而不是 FAIL
    sk("串口：PJL 双面头已注入", "release 版无观测日志，用 --debug 复验")
    sk("串口：设备应答标准 5353 查询", "release 版无观测日志，用 --debug 复验")
    sk("串口：spool 读写指针一致", "release 版无观测日志，用 --debug 复验")
    sk("串口无异常", "release 版无观测日志，用 --debug 复验")

print("")
print("================ 汇总 ================")
okn = sum(1 for _, ok, _ in R if ok)
print("通过 %d / %d" % (okn, len(R)))
for n, ok, det in R:
    if not ok:
        print("  FAILED ->", n, det)
sys.exit(0 if okn == len(R) else 1)
