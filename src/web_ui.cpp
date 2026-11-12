#include "web_ui.h"
#include "config.h"
#include "printer_cfg.h"
#include "printer_identity.h"
#include "sysmon.h"
#include "wifi_manager.h"
#include "usb_printer.h"
#include "net_print.h"
#include "snmp_agent.h"

#include <Arduino.h>
#include <WebServer.h>
#include <stdarg.h>

namespace {
WebServer web(80);

bool rejectWhilePrinting() {
  if (!netPrintBusy()) return false;
  web.send(409, "text/plain; charset=utf-8", "打印中，暂不修改配置");
  return true;
}

// /api/status 每 5 秒被前端拉一次。原来用 String 反复 `+=` 拼接，一次请求要做
// 几十次 malloc/free —— 这是长期运行下内部 RAM 碎片（进而 OOM）的主要来源之一。
// 改成「固定缓冲 + 追加写入」：零堆分配、长度可控、失败也只是截断不会崩。
struct JsonOut {
  char  *buf;
  size_t cap;
  size_t len = 0;
  bool   over = false;
};

void jAppend(JsonOut &o, const char *fmt, ...) {
  if (o.len + 1 >= o.cap) { o.over = true; return; }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(o.buf + o.len, o.cap - o.len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= o.cap - o.len) {
    o.over = true;
    o.len = o.cap - 1;
    return;
  }
  o.len += (size_t)n;
}

// 转义后的 JSON 字符串（含引号）
void jStr(JsonOut &o, const char *s) {
  // 1284 Device ID 最长 512 B，缓冲太小会被截断成半截字符串
  char tmp[640];
  size_t k = 0;
  if (!s) s = "";
  for (; *s && k + 7 < sizeof(tmp); ++s) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { tmp[k++] = '\\'; tmp[k++] = (char)c; }
    else if (c < 0x20) { k += (size_t)snprintf(tmp + k, sizeof(tmp) - k, "\\u%04x", c); }
    else tmp[k++] = (char)c;
  }
  tmp[k] = '\0';
  jAppend(o, "\"%s\"", tmp);
}

const char *b2s(bool v) { return v ? "true" : "false"; }

void noCache() { web.sendHeader("Cache-Control", "no-store"); }

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>USB 打印服务器</title>
<style>
:root{--bg:#f5f7fa;--card:#fff;--line:#e4e9f0;--txt:#111827;--sub:#6b7280;--muted:#9ca3af;--ok:#16a34a;--bad:#dc2626;--warn:#d97706;--brand:#2563eb;--soft:#eff6ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.55 system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
header{position:sticky;top:0;z-index:5;background:rgba(255,255,255,.94);backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:13px 20px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}
.brand{display:flex;align-items:center;gap:12px;min-width:0}
.mark{width:36px;height:36px;border-radius:10px;background:var(--brand);color:#fff;display:grid;place-items:center;font-weight:700;font-size:15px;flex:none}
h1{font-size:16px;margin:0}
.sub{color:var(--sub);font-size:12px;margin-top:1px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:62vw}
.badge{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--line);background:#fff;border-radius:999px;padding:6px 12px;font-size:12px;font-weight:600;white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);display:inline-block;flex:none}
.dot.on{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}.dot.blue{background:var(--brand)}
main{max-width:1160px;margin:0 auto;padding:18px 20px 44px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}
@media(max-width:840px){.grid{grid-template-columns:1fr}}
.span2{grid-column:1/-1}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px 18px;box-shadow:0 1px 2px rgba(16,24,40,.03)}
.card h2{font-size:12px;margin:0 0 14px;color:var(--sub);font-weight:700;letter-spacing:.7px;text-transform:uppercase;display:flex;justify-content:space-between;align-items:center;gap:10px}
.hint{font-size:11px;font-weight:500;color:var(--muted);text-transform:none;letter-spacing:0}
.kv{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px 22px}
.kv>div{display:flex;justify-content:space-between;gap:12px;border-bottom:1px dashed var(--line);padding-bottom:6px;min-width:0}
.k{color:var(--sub);flex:none}
.v{text-align:right;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
/* 长值（URL、状态消息）：独占一整行 + 自动换行，不再截断 */
.kv .wide{grid-column:1/-1}
.kv .wide .v{white-space:normal;overflow-wrap:anywhere;word-break:break-all;text-align:right;line-height:1.45}
.mono{font-family:ui-monospace,Consolas,monospace;font-size:12px}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.muted{color:var(--muted)}
.meter{height:10px;background:#eef2f7;border-radius:999px;overflow:hidden;margin:2px 0 16px}
.meter i{display:block;height:100%;width:0;background:var(--brand);transition:width .4s,background .3s}
.meter i.hi{background:var(--warn)}
.ports{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px}
.port{border:1px solid var(--line);border-radius:9px;padding:8px 11px;font-size:12px;display:flex;align-items:center;gap:7px;background:#fafbfd}
.port b{font-weight:700}
.port.off{opacity:.45}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
button{font:inherit;font-size:13px;font-weight:600;border:1px solid var(--line);background:#fff;color:var(--txt);border-radius:9px;padding:8px 13px;cursor:pointer}
button:hover{border-color:var(--brand);color:var(--brand)}
button.primary{background:var(--brand);border-color:var(--brand);color:#fff}
button.primary:hover{background:#1d4ed8;color:#fff}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
input,select{font:inherit;font-size:13px;width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:9px;background:#fff;color:var(--txt);margin-top:6px}
label{display:block;color:var(--sub);font-size:12px;margin-top:10px}
.msg{margin-top:12px;min-height:18px;color:var(--sub);font-size:12px;overflow-wrap:anywhere;word-break:break-all}
.note{background:var(--soft);border-left:3px solid var(--brand);border-radius:0 9px 9px 0;padding:10px 12px;color:#1e40af;font-size:12px;margin-top:14px}
pre{background:#0f172a;color:#e2e8f0;border-radius:9px;padding:11px 13px;overflow:auto;font-size:12px;margin:8px 0 0}
details{margin-top:12px;font-size:12px;color:var(--sub)}
summary{cursor:pointer;user-select:none}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
.chip{font-size:11px;padding:3px 8px;border-radius:6px;background:#f1f5f9;color:#475569;font-family:ui-monospace,monospace}
footer{text-align:center;color:var(--muted);font-size:11px;margin-top:22px;line-height:1.9}
</style></head>
<body>
<header>
  <div class="brand"><div class="mark">PS</div><div><h1>USB 打印服务器</h1><div class="sub" id="model">正在识别打印机…</div></div></div>
  <div class="badge"><span class="dot" id="bdot"></span><span id="btxt">读取中…</span></div>
</header>
<main>
<div class="grid">

  <section class="card span2">
    <h2>这台打印机 <span class="hint" id="idstate">识别中…</span></h2>
    <div class="kv">
      <div><span class="k">品牌</span><span class="v" id="p_make">-</span></div>
      <div class="wide"><span class="k">型号</span><span class="v" id="p_model">-</span></div>
      <div class="wide"><span class="k">完整型号</span><span class="v" id="p_des">-</span></div>
      <div class="wide" id="p_serial_row" style="display:none"><span class="k">序列号</span><span class="v mono" id="p_serial">-</span></div>
      <div><span class="k">USB 编号</span><span class="v mono" id="p_vid">-</span></div>
      <div><span class="k">USB 接口</span><span class="v" id="p_iface">-</span></div>
      <div><span class="k">数据通道</span><span class="v mono" id="p_ep">-</span></div>
      <div><span class="k">纸张</span><span class="v" id="p_paper">-</span></div>
      <div><span class="k">iPhone / Mac 免驱</span><span class="v" id="p_urf">-</span></div>
      <div><span class="k">彩色打印</span><span class="v" id="p_color">-</span></div>
    </div>
    <div class="chips" id="cmds"></div>
    <div class="chips" id="pdls"></div>
    <details><summary>打印机自报的身份信息（原始数据，排错时用）</summary><pre id="devid">-</pre></details>
  </section>

  <section class="card">
    <h2>网络 <span class="hint" id="apstate">-</span></h2>
    <div class="kv">
      <div><span class="k">联网状态</span><span class="v" id="wstate">-</span></div>
      <div><span class="k">信号强度</span><span class="v" id="rssi">-</span></div>
      <div><span class="k">路由器分配的地址</span><span class="v mono" id="staip">-</span></div>
      <div><span class="k">本机热点地址</span><span class="v mono" id="apip">-</span></div>
      <div class="wide"><span class="k">已连的 Wi-Fi</span><span class="v mono" id="ssid">-</span></div>
      <div><span class="k">本机热点名</span><span class="v mono" id="apssid">-</span></div>
    </div>
    <label>附近搜到的 Wi-Fi<select id="netlist"><option value="">— 也可以手动填写 —</option></select></label>
    <label>Wi-Fi 名称<input id="ssid_in" autocomplete="off" placeholder="只支持 2.4GHz"></label>
    <label>密码<input id="pass_in" type="password" placeholder="留空表示不加密"></label>
    <div class="bar"><button onclick="scan()">扫描</button><button class="primary" onclick="save()">保存并连接</button><button class="danger" onclick="forget()">清除配置</button></div>
    <div class="msg" id="msg"></div>
  </section>

  <section class="card">
    <h2>打印服务 <span class="hint">一次只打一份，第二份会被直接拒绝</span></h2>
    <div class="ports">
      <div class="port" id="port_raw"><span class="dot"></span><b>RAW 9100</b><span class="muted" style="font-size:11px">通用直连</span></div>
      <div class="port" id="port_ipp"><span class="dot"></span><b>IPP 631</b><span class="muted" style="font-size:11px">免驱</span></div>
      <div class="port" id="port_snmp"><span class="dot"></span><b>SNMP 161</b><span class="muted" style="font-size:11px">查状态</span></div>
    </div>
    <div class="kv">
      <div class="wide"><span class="k">打印地址（IPP）</span><span class="v mono" id="ippurl">-</span></div>
      <div><span class="k">正在打印</span><span class="v" id="busy">-</span></div>
      <div><span class="k">成功 / 失败次数</span><span class="v" id="jobs">-</span></div>
      <div><span class="k">两种方式的作业数</span><span class="v" id="jobsplit">-</span></div>
      <div><span class="k">已发给打印机</span><span class="v" id="ubytes">-</span></div>
      <div class="wide"><span class="k">最近状态</span><span class="v" id="pmsg">-</span></div>
    </div>
    <div class="bar"><button class="danger" onclick="abortJob()">中止当前作业</button></div>
    <div class="msg" id="lastjob">-</div>
  </section>

  <section class="card span2">
    <h2>数据缓冲区 <span class="hint" id="spstate">-</span></h2>
    <div class="meter"><i id="meterfill"></i></div>
    <div class="kv">
      <div><span class="k">已用 / 总量</span><span class="v" id="spused">-</span></div>
      <div><span class="k">最高占用</span><span class="v" id="sppeak">-</span></div>
      <div><span class="k">因来不及处理而暂停接收</span><span class="v" id="spbp">-</span></div>
      <div><span class="k">暂停 / 恢复阈值</span><span class="v mono">75% / 25%</span></div>
      <div><span class="k">剩余扩展内存</span><span class="v" id="psram">-</span></div>
      <div><span class="k">内存分配失败</span><span class="v" id="alloc">-</span></div>
    </div>
  </section>

  <section class="card">
    <h2>双面打印 <span class="hint" id="dpxhint">自动</span></h2>
    <div class="kv">
      <div><span class="k">打印机是否支持</span><span class="v" id="dpxdetected">-</span></div>
      <div><span class="k">当前设置</span><span class="v" id="dpxstate">-</span></div>
      <div class="wide"><span class="k">判断依据</span><span class="v" id="dpxevidence">-</span></div>
      <div><span class="k">自动补发双面指令</span><span class="v" id="pjlstate">-</span></div>
      <div><span class="k">作业结束标记</span><span class="v">只收尾，不多吐白纸</span></div>
      <div><span class="k">默认发送格式</span><span class="v mono" id="deffmt">-</span></div>
    </div>
    <div class="bar">
      <button id="dm_auto" onclick="setDuplexMode('auto')">自动</button>
      <button id="dm_on" onclick="setDuplexMode('on')">总是双面</button>
      <button id="dm_off" onclick="setDuplexMode('off')">总是单面</button>
      <button id="pjlbtn" onclick="togglePjl()">自动补发双面指令</button>
    </div>
    <div class="msg" id="optmsg"></div>
    <div class="note"><b>自动</b>：打印机支持双面就打开，不支持就关闭；也可以手动强制成「总是双面 / 总是单面」。<br>
      <b>为什么还要「自动补发双面指令」</b>：电脑选了双面之后，只是把「我要双面」这个要求告诉了本机，并不会写进打印内容里。本机得额外给打印机补一条它认得的双面命令，双面才会真的生效。<br>
      用 <b>9100 端口</b>打印时不会补：这种作业由电脑上的原厂驱动自己发指令，本机只负责原样转发。</div>
  </section>

  <section class="card">
    <h2>设备状态</h2>
    <div class="kv">
      <div><span class="k">固件版本</span><span class="v mono" id="fw">-</span></div>
      <div><span class="k">已连续运行</span><span class="v" id="uptime">-</span></div>
      <div><span class="k">剩余内存</span><span class="v" id="heap">-</span></div>
      <div><span class="k">开机次数 / 本次原因</span><span class="v mono" id="boot">-</span></div>
      <div class="wide"><span class="k">上次异常记录</span><span class="v" id="lastfault">-</span></div>
      <div><span class="k">看门狗超时</span><span class="v" id="wdt">-</span></div>
    </div>
    <div class="bar"><button class="danger" onclick="reboot()">重启设备</button></div>
  </section>

  <section class="card span2">
    <h2>怎么在电脑上添加</h2>
    <div class="note">本机只做「搬运工」：把电脑发来的打印数据原样转给 USB 打印机，<b>不改格式、不渲染、不压缩</b>。所以电脑上必须装这台打印机对应的驱动，否则发过来的内容打印机看不懂。</div>
    <p style="font-size:12px;color:var(--sub);margin:12px 0 4px"><b>方式一：RAW 9100（推荐，最省事）</b><br>
      在 Windows 里用「TCP/IP 地址」添加打印机 → 协议选 <b>RAW</b>、端口填 <b>9100</b>。<br>
      「设备类型」优先选 <b>Generic Network Card</b>：它不会去追问设备状态，能避开本机简化的状态服务被误判。如果下拉框里只有「TCP/IP Device」，就到端口设置里取消勾选「启用 SNMP 状态」，效果一样。<br>
      驱动选这台打印机的原厂 PCL / PS 驱动，不要选 Generic / Text Only。一次连接就是一份作业。</p>
    <p style="font-size:12px;color:var(--sub);margin:8px 0 4px"><b>方式二：IPP 631（适合 iPhone / Mac 和免驱机型）</b><br>
      本机已经按行业标准声明了免驱能力，但最后能不能解析还是看打印机自己（只会 PCL 的机型请用上面的方式一）。Windows 用「Microsoft IPP Class Driver」，先建端口再加打印机：</p>
    <pre id="pscmd">-</pre>
    <p style="font-size:12px;color:var(--sub);margin:10px 0 0"><b>SNMP 161（只读）</b>：给监控软件查缺纸、忙闲等状态用，只能读、不能改，community 是 <code>public</code>。这些端口只适合在内网用，不要映射到公网。</p>
  </section>
</div>
<footer>RAW 9100 + IPP 631 两条通路 · 256 KB 缓冲 · 被问才应答（mDNS）· 双核分工<br>本机只负责搬运数据，打印语言由打印机自己解释</footer>
</main>
<script>
const $=i=>document.getElementById(i);
function fmtBytes(n){n=Number(n)||0;if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';if(n<1073741824)return (n/1048576).toFixed(1)+' MB';return (n/1073741824).toFixed(2)+' GB';}
function dur(s){s=Number(s)||0;const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+'h '+m+'m':(m?m+'m '+(s%60)+'s':s+'s');}
function chips(el,arr){el.textContent='';if(!arr.length){const s=document.createElement('span');s.className='muted';s.textContent='未报告';el.appendChild(s);return;}arr.slice(0,20).forEach(x=>{const s=document.createElement('span');s.className='chip';s.textContent=x;el.appendChild(s);});}
function mark(id,on){const e=$(id);e.classList.toggle('off',!on);const d=e.querySelector('.dot');if(d)d.className='dot '+(on?'on':'');}
async function post(path,body){return fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body||''});}
function badge(d){const u=d.usb||{},p=d.print||{};let c='',t='读取中…';
 if(!u.connected){t='未连接打印机';}else if(u.fault){c='bad';t='打印机故障';}
 else if(u.paper_empty){c='bad';t='缺纸';}else if(p.busy){c='blue';t='打印中';}
 else{c='on';t='就绪';}
 $('bdot').className='dot '+c;$('btxt').textContent=t;}
async function status(){
 try{
  const d=await (await fetch('/api/status')).json();
  const u=d.usb||{},p=d.print||{},w=d.wifi||{},sp=d.spool||{},pr=d.printer||{},lj=d.last_job||{},cfg=d.cfg||{};
  badge(d);
  $('model').textContent=(pr.ready?pr.make_model:'正在识别打印机…')+(pr.guessed?'  ·  能力为推定值':'');
  $('idstate').textContent=pr.ready?(pr.guessed?'没读到身份信息，按 USB 编号推断':'已自动识别'):'识别中…';
  $('p_make').textContent=pr.make||'-';$('p_model').textContent=pr.model||'-';
  $('p_des').textContent=pr.description||'-';
  $('p_serial_row').style.display=pr.serial?'':'none';
  if(pr.serial)$('p_serial').textContent=pr.serial;
  $('p_vid').textContent=u.vid_pid||'-';$('p_iface').textContent=u.iface||'-';$('p_ep').textContent=u.out_ep||'-';
  $('p_paper').textContent=!u.connected?'-':(u.paper_empty?'缺纸':'正常');
  $('p_paper').className='v '+(u.paper_empty?'bad':(u.connected?'ok':''));
  $('p_urf').textContent=pr.urf?'支持':'不支持';$('p_color').textContent=pr.color?'彩色':'单色';
  $('deffmt').textContent=pr.default_format||'-';
  $('devid').textContent=pr.device_id||'-';
  chips($('cmds'),(pr.commands||'').split(',').filter(Boolean));
  chips($('pdls'),(pr.pdl||'').split(',').filter(Boolean));
  $('wstate').textContent=w.connected?'已连接':'未连接';
  $('rssi').textContent=w.connected?w.rssi+' dBm':'-';
  $('staip').textContent=w.sta_ip||'-';$('apip').textContent=w.ap_ip||'-';
  $('ssid').textContent=w.ssid||'-';$('apssid').textContent=w.ap_ssid||'-';
  $('apstate').textContent=w.ap_on?'热点：开':'热点：已关（省电）';
  mark('port_raw',p.raw);mark('port_ipp',p.ipp);mark('port_snmp',!!(d.snmp&&d.snmp.listening));
  $('ippurl').textContent=p.ipp_url||'-';
  $('busy').textContent=p.busy?'正在打印':'空闲';
  $('jobs').textContent=(p.ok||0)+' / '+(p.failed||0);
  $('jobsplit').textContent=(p.jobs_raw||0)+' / '+(p.jobs_ipp||0);
  $('ubytes').textContent=fmtBytes(p.bytes);
  $('pmsg').textContent=p.message||'-';
  $('lastjob').textContent=lj.tag?((lj.src?lj.src+' · ':'')+lj.tag+' · '+fmtBytes(lj.bytes)+' · '+(lj.ms/1000).toFixed(1)+'s · '+(lj.ok?'成功':('中止'+(lj.why?'：'+lj.why:'')))):'暂无作业';
  const cap=Number(sp.capacity)||1,used=Number(sp.used)||0;
  const pct=Math.min(100,Math.round(used*100/cap));
  $('meterfill').style.width=pct+'%';$('meterfill').classList.toggle('hi',pct>=75);
  $('spused').textContent=fmtBytes(used)+' / '+fmtBytes(cap);
  $('sppeak').textContent=fmtBytes(sp.peak);
  $('spbp').textContent=sp.backpressure+' 次';
  $('spstate').textContent=sp.forced?'内存紧张，已暂停接收':sp.paused?'处理不过来，已暂停接收':'正常';
  $('psram').textContent=fmtBytes(sp.psram_free);
  $('alloc').textContent=(d.alloc_fails||0)+' 次';
  const cm=cfg.duplex_mode||0;
  $('dpxhint').textContent=cm===0?'自动':(cm===1?'总是双面':'总是单面');
  $('dpxdetected').textContent=cfg.duplex_detected?'支持':'不支持或没识别出来';
  $('dpxstate').textContent=cfg.duplex?'双面':'单面';
  $('dpxevidence').textContent=cfg.duplex_evidence||'-';
  // 开关只是「允许」，能否真正注入还取决于打印机是否懂 PJL，这里如实显示
  $('pjlstate').textContent=cfg.pjl_duplex?(pr.pjl?'开启':'已开，但本机不支持 PJL（不生效）'):'关闭';
  ['auto','on','off'].forEach((k,i)=>{$('dm_'+k).classList.toggle('primary',cm===i);});
  $('pjlbtn').textContent=cfg.pjl_duplex?'关闭「自动补发双面指令」':'开启「自动补发双面指令」';
  window.__pjl=!!cfg.pjl_duplex;
  $('fw').textContent='v'+d.version;$('uptime').textContent=dur(d.uptime);
  $('heap').textContent=fmtBytes(d.heap);
  $('boot').textContent='#'+d.boots+' / '+d.reset;
  $('lastfault').textContent=d.last_fault||'无';
  $('wdt').textContent=d.wdt+' s';
  const ip=(w.sta_ip||w.ap_ip||'192.168.4.1');
  const ippPort='http://'+ip+':631/ipp/print';
  $('pscmd').textContent='Add-PrinterPort -Name "'+ippPort+'"\nAdd-Printer -Name "'+(pr.name||'USB Printer')+'" -DriverName "Microsoft IPP Class Driver" -PortName "'+ippPort+'"';
 }catch(e){$('msg').textContent='状态读取失败：'+e;}
}
async function scan(){$('msg').textContent='正在扫描…';try{
 let r={nets:[],pending:true};
 for(let i=0;i<15;i++){
  const x=await fetch('/api/scan');if(x.status===409){$('msg').textContent='打印中，暂不扫描';return;}
  r=await x.json();if(!r.pending)break;await new Promise(ok=>setTimeout(ok,700));
 }
 const sel=$('netlist');sel.textContent='';
 const o=document.createElement('option');o.value='';o.textContent='— 手动填写 SSID —';sel.appendChild(o);
 (r.nets||[]).forEach(n=>{const q=document.createElement('option');q.value=n.s;q.textContent=n.s+'  ('+n.r+' dBm'+(n.e?', 加密':'')+')';sel.appendChild(q);});
 $('msg').textContent=r.error?('扫描失败：'+r.error):'找到 '+(r.nets||[]).length+' 个网络';
}catch(e){$('msg').textContent='扫描失败：'+e;}}
$('netlist').addEventListener('change',e=>{if(e.target.value)$('ssid_in').value=e.target.value;});
async function save(){const s=$('ssid_in').value.trim();if(!s){$('msg').textContent='请填写 Wi-Fi 名称';return;}
 const r=await post('/api/wifi','ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent($('pass_in').value));
 $('msg').textContent=r.ok?'已保存，正在连接（可继续用 AP 访问本页面）':(r.status===409?'打印中，暂不修改配置':'保存失败：'+await r.text());}
async function forget(){const r=await post('/api/forget');$('msg').textContent=r.ok?'已清除，AP 会立刻重开供配网':'清除失败：'+await r.text();}
async function setDuplexMode(m){const r=await post('/api/duplex','mode='+m);
 $('optmsg').textContent=r.ok?'双面策略已更新':'保存失败：'+await r.text();status();}
async function togglePjl(){const on=!window.__pjl;
 const r=await post('/api/duplex','pjl='+(on?'1':'0'));
 $('optmsg').textContent=r.ok?('sides→PJL 注入已'+(on?'开启':'关闭')):'保存失败：'+await r.text();status();}
async function abortJob(){const r=await post('/api/abort');$('msg').textContent=r.ok?'已发送中止请求':'中止失败：'+await r.text();setTimeout(status,1200);}
async function reboot(){const r=await post('/api/reboot');$('msg').textContent=r.ok?'正在重启…':'重启失败：'+await r.text();}
status();setInterval(status,5000);
</script></body></html>
)HTML";

void handleRoot() {
  noCache();
  web.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleStatus() {
  const WifiStatus w = wifiManagerStatus();
  const UsbPrinterStatus u = usbPrinterStatus();
  const NetPrintStatus p = netPrintStatus();
  char vidpid[16], iface[16], ep[8], ippUrl[96];
  const char *host = w.staIp[0] ? w.staIp : w.apIp;
  snprintf(ippUrl, sizeof(ippUrl), "http://%s:%d%s", host, IPP_PORT, IPP_URI_PATH);
  snprintf(vidpid, sizeof(vidpid), "%04X:%04X", u.vid, u.pid);
  snprintf(iface, sizeof(iface), "%u / %s", u.iface,
           u.protocol == 2 ? "双向" : (u.protocol == 1 ? "单向" : "-"));
  snprintf(ep, sizeof(ep), "0x%02X", u.outEp);

  // 静态缓冲：Web 请求在 loopTask 里串行处理，不需要加锁，也不会产生堆分配。
  // 加入 printer 对象（含 512 B 的 1284 Device ID）后留足余量。
  static char json[5120];
  JsonOut o = {json, sizeof(json)};

  jAppend(o, "{\"version\":");
  jStr(o, FW_VERSION);
  jAppend(o, ",\"uptime\":%lu", (unsigned long)(millis() / 1000));
  jAppend(o, ",\"heap\":%u", (unsigned)ESP.getFreeHeap());
  jAppend(o, ",\"boots\":%lu", (unsigned long)sysMonBootCount());
  jAppend(o, ",\"reset\":");
  jStr(o, sysMonResetReason());
  jAppend(o, ",\"wdt\":%d", (int)WDT_TIMEOUT_S);
  jAppend(o, ",\"cfg\":{\"duplex\":%s", b2s(cfgDuplexEnabled()));
  jAppend(o, ",\"duplex_mode\":%d", cfgDuplexMode());
  jAppend(o, ",\"duplex_detected\":%s", b2s(cfgDuplexDetected()));
  jAppend(o, ",\"duplex_evidence\":");
  jStr(o, cfgDuplexEvidence());
  jAppend(o, ",\"pjl_duplex\":%s}", b2s(cfgPjlDuplexEnabled()));

  // 自动识别出来的打印机身份：型号、命令集、能力，全部来自 1284 Device ID
  jAppend(o, ",\"printer\":{\"ready\":%s", b2s(printerIdReady()));
  jAppend(o, ",\"make\":");        jStr(o, printerIdMake());
  jAppend(o, ",\"model\":");       jStr(o, printerIdMdl());
  jAppend(o, ",\"description\":"); jStr(o, printerIdDescription());
  jAppend(o, ",\"name\":");        jStr(o, printerIdName());
  jAppend(o, ",\"make_model\":");  jStr(o, printerIdMakeModel());
  jAppend(o, ",\"serial\":");      jStr(o, printerIdSerial());
  jAppend(o, ",\"device_id\":");   jStr(o, printerIdDeviceId());
  jAppend(o, ",\"commands\":");    jStr(o, printerIdCommands());
  jAppend(o, ",\"pdl\":");         jStr(o, printerIdPdl());
  jAppend(o, ",\"default_format\":"); jStr(o, printerIdDefaultFormat());
  jAppend(o, ",\"urf\":%s", b2s(printerIdSupportsUrf()));
  jAppend(o, ",\"color\":%s", b2s(printerIdSupportsColor()));
  jAppend(o, ",\"pjl\":%s", b2s(printerIdSupportsPjl()));
  jAppend(o, ",\"guessed\":%s}", b2s(printerIdGuessed()));

  jAppend(o, ",\"wifi\":{\"connected\":%s", b2s(w.staConnected));
  jAppend(o, ",\"ap_on\":%s", b2s(w.apOn));
  jAppend(o, ",\"ssid\":");
  jStr(o, w.ssid);
  jAppend(o, ",\"sta_ip\":");
  jStr(o, w.staIp);
  jAppend(o, ",\"ap_ip\":");
  jStr(o, w.apIp);
  jAppend(o, ",\"ap_ssid\":");
  jStr(o, w.apSsid);
  jAppend(o, ",\"rssi\":%d", (int)w.rssi);
  jAppend(o, ",\"retries\":%u", (unsigned)w.reconnectFails);
  jAppend(o, ",\"message\":");
  jStr(o, w.message);
  jAppend(o, "}");

  jAppend(o, ",\"usb\":{\"connected\":%s", b2s(u.connected));
  jAppend(o, ",\"fault\":%s", b2s(u.fault));
  jAppend(o, ",\"paper_empty\":%s", b2s(u.paperEmpty));
  jAppend(o, ",\"selected\":%s", b2s(u.selected));
  jAppend(o, ",\"vid_pid\":");
  jStr(o, vidpid);
  jAppend(o, ",\"iface\":");
  jStr(o, iface);
  jAppend(o, ",\"protocol\":%u", (unsigned)u.protocol);
  jAppend(o, ",\"out_ep\":");
  jStr(o, ep);
  jAppend(o, ",\"bytes\":%llu", (unsigned long long)u.bytesTotal);
  jAppend(o, ",\"errors\":%u", (unsigned)u.usbErrors);
  jAppend(o, ",\"message\":");
  jStr(o, u.message);
  jAppend(o, ",\"buf_corrupt\":%lu", (unsigned long)usbPrinterBufCorrupt());
  jAppend(o, "}");

  // SNMP 是独立服务，指示灯必须读真实 bind 状态，不能拿打印服务状态代替
  jAppend(o, ",\"snmp\":{\"listening\":%s,\"port\":%d}",
          b2s(snmpAgentListening()), SNMP_PORT);

  jAppend(o, ",\"print\":{\"listening\":%s", b2s(p.listening));
  jAppend(o, ",\"raw\":%s", b2s(p.rawListening));
  jAppend(o, ",\"ipp\":%s", b2s(p.ippListening));
  jAppend(o, ",\"ipp_url\":");
  jStr(o, ippUrl);
  jAppend(o, ",\"busy\":%s", b2s(p.busy));
  jAppend(o, ",\"ok\":%lu", (unsigned long)p.jobsOk);
  jAppend(o, ",\"failed\":%lu", (unsigned long)p.jobsFailed);
  jAppend(o, ",\"jobs_raw\":%lu", (unsigned long)p.jobsRaw);
  jAppend(o, ",\"jobs_ipp\":%lu", (unsigned long)p.jobsIpp);
  jAppend(o, ",\"bytes\":%llu", (unsigned long long)p.bytesTotal);
  jAppend(o, ",\"received\":%llu", (unsigned long long)p.bytesReceived);
  jAppend(o, ",\"sent\":%llu", (unsigned long long)p.bytesSent);
  jAppend(o, ",\"message\":");
  jStr(o, p.message);
  jAppend(o, "}");

  jAppend(o, ",\"spool\":{\"capacity\":%lu", (unsigned long)p.spoolCapacity);
  jAppend(o, ",\"used\":%lu", (unsigned long)p.spoolUsed);
  jAppend(o, ",\"peak\":%lu", (unsigned long)p.spoolPeak);
  jAppend(o, ",\"backpressure\":%lu", (unsigned long)p.backpressure);
  jAppend(o, ",\"paused\":%s", b2s(p.spoolPaused));
  jAppend(o, ",\"forced\":%s", b2s(p.spoolForced));
  jAppend(o, ",\"psram\":%s", b2s(p.spoolPsram));
  jAppend(o, ",\"psram_total\":%lu", (unsigned long)p.psramTotal);
  jAppend(o, ",\"psram_free\":%lu", (unsigned long)p.psramFree);
  jAppend(o, ",\"psram_largest\":%lu", (unsigned long)p.psramLargest);
  jAppend(o, ",\"psram_reserve\":%lu", (unsigned long)p.psramReserve);
  jAppend(o, "}");
  jAppend(o, ",\"alloc_fails\":%lu", (unsigned long)sysMonAllocFails());

  // 最近一次作业：通路、字节、耗时、结果、中止原因
  const LastJobInfo lj = netPrintLastJob();
  jAppend(o, ",\"last_job\":{\"tag\":");
  jStr(o, lj.tag);
  jAppend(o, ",\"bytes\":%lu,\"ms\":%lu,\"ok\":%s,\"aborted\":%s,\"why\":",
          (unsigned long)lj.bytes, (unsigned long)lj.ms, b2s(lj.ok), b2s(lj.aborted));
  jStr(o, lj.why);
  jAppend(o, ",\"src\":");
  jStr(o, lj.src);
  jAppend(o, ",\"at\":%lu}", (unsigned long)lj.atSec);

  jAppend(o, ",\"pending_restart\":");
  jStr(o, sysMonRestartReason());
  jAppend(o, ",\"last_fault\":");
  jStr(o, sysMonLastFault());
  jAppend(o, "}");
  if (o.over) {                    // 缓冲不够会截断成坏 JSON，宁可吵一句也别静默
    static bool warned = false;
    if (!warned) {
      warned = true;
      FWLOGE("[web] /api/status 超过 %u B，JSON 被截断（请调大缓冲）\n",
                    (unsigned)sizeof(json));
    }
  }
  noCache();
  web.send(200, "application/json; charset=utf-8", json);
}

// 双面：三态策略（auto / on / off）+ IPP sides→PJL 注入开关。
// 兼容旧前端的 on=1|0，映射为强制开 / 强制关。
void handleDuplex() {
  noCache();
  bool changed = false;
  if (web.hasArg("mode")) {
    const String v = web.arg("mode");
    int m = DUPLEX_AUTO;
    if (v == "on") m = DUPLEX_ON;
    else if (v == "off") m = DUPLEX_OFF;
    else if (v != "auto") {
      web.send(400, "text/plain; charset=utf-8", "mode 只能是 auto / on / off");
      return;
    }
    cfgSetDuplexMode(m);
    changed = true;
  } else if (web.hasArg("on")) {
    const String v = web.arg("on");
    cfgSetDuplexEnabled(v == "1" || v == "true" || v == "on");
    changed = true;
  }
  if (web.hasArg("pjl")) {
    const String v = web.arg("pjl");
    cfgSetPjlDuplexEnabled(v == "1" || v == "true" || v == "on");
    changed = true;
  }
  if (!changed) {
    web.send(400, "text/plain; charset=utf-8", "缺少 mode / on / pjl");
    return;
  }
  char out[96];
  snprintf(out, sizeof(out), "{\"mode\":%d,\"effective\":%s,\"pjl\":%s}",
           cfgDuplexMode(), b2s(cfgDuplexEnabled()), b2s(cfgPjlDuplexEnabled()));
  web.send(200, "application/json; charset=utf-8", out);
}

void handleWifi() {
  noCache();
  if (rejectWhilePrinting()) return;
  if (!web.hasArg("ssid")) { web.send(400, "text/plain; charset=utf-8", "缺少 ssid"); return; }
  if (!wifiManagerSetCredentials(web.arg("ssid").c_str(), web.arg("password").c_str())) {
    web.send(400, "text/plain; charset=utf-8", "SSID 或密码长度不合法");
    return;
  }
  web.send(200, "text/plain; charset=utf-8", "ok");
}

void handleScan() {
  noCache();
  if (rejectWhilePrinting()) return;

  // 扫描全部交给 monitorTask（WiFi API 单点收敛），这里只投递请求 / 取结果。
  char result[2048];
  if (wifiManagerScanReady()) {
    if (wifiManagerScanCopy(result, sizeof(result))) {
      web.send(200, "application/json; charset=utf-8", result);
    } else {
      web.send(200, "application/json; charset=utf-8", "{\"nets\":[],\"pending\":true}");
    }
    return;
  }
  wifiManagerRequestScan();
  web.send(200, "application/json; charset=utf-8", "{\"nets\":[],\"pending\":true}");
}
}  // namespace

void webUiBegin() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/api/status", HTTP_GET, handleStatus);
  web.on("/api/wifi", HTTP_POST, handleWifi);
  web.on("/api/duplex", HTTP_POST, handleDuplex);
  // 卡死逃生口：中止当前作业并复位 busy，不必断电
  web.on("/api/abort", HTTP_POST, []() {
    noCache();
    netPrintAbort();
    web.send(200, "text/plain; charset=utf-8", "ok");
  });
  web.on("/api/scan", HTTP_GET, handleScan);
  web.on("/api/forget", HTTP_POST, []() {
    noCache();
    if (rejectWhilePrinting()) return;
    wifiManagerForget();
    web.send(200, "text/plain; charset=utf-8", "ok");
  });
  web.on("/api/reboot", HTTP_POST, []() {
    noCache();
    if (rejectWhilePrinting()) return;
    web.send(200, "text/plain; charset=utf-8", "ok");
    sysMonRequestRestart("web ui");
  });
  web.onNotFound([]() { noCache(); web.send(404, "text/plain; charset=utf-8", "404"); });
  web.begin();
  FWLOGI_LN("[web] UI ready on port 80 (AP + LAN)");
}

void webUiLoop() { web.handleClient(); }
