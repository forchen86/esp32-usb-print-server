#pragma once
// RAW / PDL-Datastream（TCP 9100）通路。
// 一个 TCP 连接 = 一份作业，close 即 EOF；不做任何格式协商，字节原样透传。
// 与 IPP 631 共用同一个作业泵与环形缓冲，任一时刻只允许一份作业。
void rawPrintBegin();
