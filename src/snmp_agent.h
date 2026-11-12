#pragma once
// SNMP v1/v2c 只读状态代理（UDP 161）。
// 只实现 GET / GETNEXT，不开放 SET；community 见 config.h。
// 状态值统一取自 IPP 的 printer-state，不复用 9100 / 631 的任何 socket 或任务。
void snmpAgentBegin();
// UDP 161 是否已成功 bind（Web 的端口指示灯用真实状态，不再拿打印服务状态代替）
bool snmpAgentListening();
