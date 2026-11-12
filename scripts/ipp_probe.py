#!/usr/bin/env python3
"""IPP 能力探测：连上设备热点 / 同一局域网后运行。

    python scripts/ipp_probe.py 192.168.4.1
    python scripts/ipp_probe.py 192.168.10.43 --job test.pcl   # 可选：顺带发一个作业

只依赖标准库。用于确认固件声明的 PDL 能被 IPP Everywhere / AirPrint 客户端识别。
"""
import argparse
import socket
import struct
import sys

# IPP value-tag
VT = {
    0x21: 'integer', 0x22: 'boolean', 0x23: 'enum', 0x30: 'range',
    0x31: 'datetime', 0x32: 'resolution', 0x41: 'text', 0x42: 'name',
    0x44: 'keyword', 0x45: 'uri', 0x47: 'charset', 0x48: 'language',
    0x49: 'mimeMediaType',
}


def attr(tag, name, value):
    n = name.encode('utf-8')
    v = value.encode('utf-8') if isinstance(value, str) else value
    return struct.pack('!BH', tag, len(n)) + n + struct.pack('!H', len(v)) + v


def build_request(op, req_id=1, extra=b''):
    body = struct.pack('!HHI', 0x0101, op, req_id)
    body += b'\x01'                                        # operation-attributes-tag
    body += attr(0x47, 'attributes-charset', 'utf-8')
    body += attr(0x48, 'attributes-natural-language', 'en')
    body += attr(0x45, 'printer-uri', 'ipp://localhost:631/ipp/print')
    body += attr(0x42, 'requesting-user-name', 'probe')
    body += extra
    body += b'\x03'                                        # end-of-attributes-tag
    return body


def http_post(host, port, path, payload, timeout=20):
    req = (f'POST {path} HTTP/1.1\r\n'
           f'Host: {host}:{port}\r\n'
           'Content-Type: application/ipp\r\n'
           f'Content-Length: {len(payload)}\r\n'
           'Connection: close\r\n\r\n').encode() + payload
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(req)
    chunks = []
    while True:
        try:
            b = s.recv(8192)
        except socket.timeout:
            break
        if not b:
            break
        chunks.append(b)
    s.close()
    return b''.join(chunks)


def split_http(raw):
    sep = raw.find(b'\r\n\r\n')
    if sep < 0:
        raise RuntimeError('HTTP 响应不完整')
    head = raw[:sep].decode('latin-1')
    body = raw[sep + 4:]
    clen = None
    for line in head.split('\r\n')[1:]:
        if line.lower().startswith('content-length:'):
            clen = int(line.split(':', 1)[1].strip())
    if clen is not None:
        body = body[:clen]
    return head.split('\r\n')[0], body


def parse_ipp(body):
    if len(body) < 8:
        raise RuntimeError('IPP 响应过短: %r' % body[:32])
    ver, status, rid = struct.unpack('!HHI', body[:8])
    out, i, last_name = [], 8, ''
    while i < len(body):
        tag = body[i]
        i += 1
        if tag == 0x03:
            break
        if tag <= 0x05:
            continue
        if i + 4 > len(body):
            break
        nlen = struct.unpack('!H', body[i:i + 2])[0]
        i += 2
        name = body[i:i + nlen].decode('utf-8', 'replace')
        i += nlen
        vlen = struct.unpack('!H', body[i:i + 2])[0]
        i += 2
        val = body[i:i + vlen]
        i += vlen
        if name:
            last_name = name
        else:
            name = last_name + ' (附加值)'
        if tag == 0x21:
            value = struct.unpack('!i', val)[0] if len(val) == 4 else val.hex()
        elif tag == 0x23:
            value = struct.unpack('!i', val)[0] if len(val) == 4 else val.hex()
        elif tag == 0x22:
            value = bool(val[0]) if val else False
        else:
            value = val.decode('utf-8', 'replace')
        out.append((name, VT.get(tag, hex(tag)), value))
    return ver, status, rid, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host', nargs='?', default='192.168.4.1')
    ap.add_argument('--port', type=int, default=631)
    ap.add_argument('--path', default='/ipp/print')
    ap.add_argument('--job', help='可选：把这个文件作为 Print-Job 文档数据发送')
    a = ap.parse_args()

    print(f'==> Get-Printer-Attributes  ipp://{a.host}:{a.port}{a.path}')
    req = build_request(0x000B, 1, attr(0x44, 'requested-attributes', 'all'))
    raw = http_post(a.host, a.port, a.path, req)
    if not raw:
        print('!! 没有响应，检查 IP / 端口 / 是否连上热点')
        return 1
    line, body = split_http(raw)
    print('    ', line)
    ver, status, rid, attrs = parse_ipp(body)
    print(f'     IPP {ver >> 8}.{ver & 0xff}  status=0x{status:04x}  request-id={rid}')
    d = {}
    for name, _kind, value in attrs:
        d.setdefault(name, []).append(value)

    for key in ('printer-name', 'printer-make-and-model', 'printer-device-id',
                'ipp-features-supported', 'document-format-default',
                'document-format-supported', 'urf-supported',
                'pwg-raster-document-type-supported', 'printer-state',
                'printer-state-reasons', 'printer-is-accepting-jobs'):
        if key in d:
            print(f'     {key}: {", ".join(str(v) for v in d[key])}')

    fmts = [str(v) for v in d.get('document-format-supported', [])]
    must = ('image/pwg-raster', 'image/urf', 'application/pdf')
    missing = [m for m in must if m not in fmts]
    print()
    if missing:
        print('!! 免驱能力声明不完整，缺少:', ', '.join(missing))
    else:
        print('OK: PWG Raster / URF / PDF 均已声明，IPP Everywhere + AirPrint 免驱条件成立')
    print(f'    共 {len(attrs)} 个响应属性，{len(body)} 字节')

    if a.job:
        data = open(a.job, 'rb').read()
        print(f'\n==> Print-Job  ({len(data)} 字节文档数据)')
        job_attr = attr(0x49, 'document-format', 'application/octet-stream')
        job = build_request(0x0002, 2, job_attr) + data
        r2 = http_post(a.host, a.port, a.path, job, timeout=120)
        if not r2:
            print('!! 无响应')
            return 1
        line2, body2 = split_http(r2)
        v2, s2, rid2, a2 = parse_ipp(body2)
        print('    ', line2)
        print(f'     status=0x{s2:04x}  (0x0000 = successful-ok)')
        for n, _k, v in a2:
            if n in ('job-id', 'job-state', 'job-uri', 'status-message'):
                print(f'     {n}: {v}')
    return 0 if not missing else 1


if __name__ == '__main__':
    sys.exit(main())
