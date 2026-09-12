/*
 * ============================================================
 *  SMART PATIENT TOKEN SYSTEM  (v2)
 *  Hardware : ESP32 Dev Module
 *             SH1106 128x64 OLED  (SDA=21, SCL=22)
 *             Push Button         (GPIO12, active HIGH)
 *  Libraries: U8g2, WiFi, WebServer, WebSocketsServer
 *
 *  NEW IN THIS VERSION
 *  --------------------
 *  1. "Reset System" button on the web dashboard
 *       - Clears every token and restarts numbering from 1.
 *
 *  2. Active-queue limit (ACTIVE_LIMIT = 10)
 *       - Token numbers keep incrementing forever
 *         (1,2,3...10,11,12,13...) even after tokens are
 *         completed.
 *       - But if 10 tokens are CURRENTLY
 *         Waiting / Serving / Missed / Recalled at the same
 *         time, pressing the button shows "Appointments Full"
 *         on the OLED and on the dashboard.
 *
 *  3. Cancelled-token notification
 *       - If a recalled token is missed a second time it is
 *         marked REMOVED ("Cancelled").
 *       - The OLED shows "Token #X CANCELLED - Take New Token"
 *         for 2 seconds.
 *       - The web dashboard shows a matching pop-up toast.
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>

// ─────────────────────────────────────────────
//  OLED
// ─────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ─────────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────────
const char* ssid     = "Energy";
const char* password = "123456789";

// ─────────────────────────────────────────────
//  NTP (Date / Day)
// ─────────────────────────────────────────────
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec       = 19800;   // India Standard Time = UTC +5:30
const int   daylightOffset_sec  = 0;

// ─────────────────────────────────────────────
//  Doctor Info
// ─────────────────────────────────────────────
const char* doctorName = "Dr. Nithin M R";

// ─────────────────────────────────────────────
//  Servers
// ─────────────────────────────────────────────
WebServer       server(80);
WebSocketsServer webSocket(81);

// ─────────────────────────────────────────────
//  Hardware
// ─────────────────────────────────────────────
#define BUTTON_PIN 12

// ─────────────────────────────────────────────
//  Token constants & state
// ─────────────────────────────────────────────
// Max tokens allowed to be "active" (Waiting/Serving/Missed/Recalled)
// in the queue at the same time.
#define ACTIVE_LIMIT   10

// Total tokens that can be stored before a Reset is required.
// Numbering keeps climbing past ACTIVE_LIMIT (11,12,13...) as
// long as the active queue stays under ACTIVE_LIMIT.
#define TOKEN_STORAGE  500

// Token status values
#define STATUS_WAITING    0
#define STATUS_SERVING    1
#define STATUS_MISSED     2
#define STATUS_RECALLED   3
#define STATUS_COMPLETED  4
#define STATUS_REMOVED    5

struct Token {
  int  number;
  int  status;
  int  missCount;   // how many times this token has been missed
};

Token tokens[TOKEN_STORAGE + 1];   // index 1..TOKEN_STORAGE
int   tokenCount   = 0;            // highest token number issued
int   currentToken = 0;            // token currently being served (0 = none)

// Cumulative session counters (reset only via "Reset System")
int totalServed       = 0;   // number of tokens marked Completed
int totalMissedCount  = 0;   // number of "missed" events (incl. cancellations)

// ─────────────────────────────────────────────
//  Helper – status name
// ─────────────────────────────────────────────
const char* statusName(int s) {
  switch (s) {
    case STATUS_WAITING:   return "Waiting";
    case STATUS_SERVING:   return "Serving";
    case STATUS_MISSED:    return "Missed";
    case STATUS_RECALLED:  return "Recalled";
    case STATUS_COMPLETED: return "Completed";
    case STATUS_REMOVED:   return "Removed";
    default:               return "Unknown";
  }
}

// ─────────────────────────────────────────────
//  Counters (derived on demand)
// ─────────────────────────────────────────────
int countWaiting() {
  int c = 0;
  for (int i = 1; i <= tokenCount; i++)
    if (tokens[i].status == STATUS_WAITING) c++;
  return c;
}
int countMissed() {
  int c = 0;
  for (int i = 1; i <= tokenCount; i++)
    if (tokens[i].status == STATUS_MISSED || tokens[i].status == STATUS_RECALLED) c++;
  return c;
}
// Tokens that currently occupy a slot in the queue
int countActive() {
  int c = 0;
  for (int i = 1; i <= tokenCount; i++) {
    int s = tokens[i].status;
    if (s == STATUS_WAITING || s == STATUS_SERVING ||
        s == STATUS_MISSED  || s == STATUS_RECALLED) c++;
  }
  return c;
}

// ─────────────────────────────────────────────
//  Date / Day (from NTP)
// ─────────────────────────────────────────────
String getDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return "--/--/----";
  char buf[16];
  strftime(buf, sizeof(buf), "%d-%m-%Y", &timeinfo);
  return String(buf);
}

String getDayString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return "---";
  char buf[16];
  strftime(buf, sizeof(buf), "%A", &timeinfo);
  return String(buf);
}

// ─────────────────────────────────────────────
//  OLED display
// ─────────────────────────────────────────────
void updateDisplay() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Title
  u8g2.drawStr(20, 10, "SMART TOKEN");
  u8g2.drawLine(0, 14, 127, 14);

  // Serving
  String servingText = "Serving: ";
  if (currentToken == 0) servingText += "None";
  else                   servingText += String(currentToken);
  u8g2.drawStr(0, 28, servingText.c_str());

  // Waiting
  String waitingText = "Waiting: ";
  waitingText += String(countWaiting());
  u8g2.drawStr(0, 42, waitingText.c_str());

  // Missed / Active-Limit
  String missedText = "Missed:" + String(countMissed());
  missedText += "  Q:" + String(countActive()) + "/" + String(ACTIVE_LIMIT);
  u8g2.drawStr(0, 56, missedText.c_str());

  u8g2.sendBuffer();
}

// Temporary screen: queue is full
void showFullMessage() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(5, 22, "Appointments Full!");
  String line = "Max " + String(ACTIVE_LIMIT) + " active tokens";
  u8g2.drawStr(5, 38, line.c_str());
  u8g2.drawStr(5, 54, "Please wait...");
  u8g2.sendBuffer();
  delay(2000);
  updateDisplay();
}

// Temporary screen: a token was cancelled (missed twice)
void showCancelledMessage(int tokenNum) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 18, "TOKEN CANCELLED");
  String line = "Token #" + String(tokenNum);
  u8g2.drawStr(8, 36, line.c_str());
  u8g2.drawStr(8, 52, "Take New Token");
  u8g2.sendBuffer();
  delay(2000);
  updateDisplay();
}

// Temporary screen: system reset
void showResetMessage() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(20, 26, "SYSTEM RESET");
  u8g2.drawStr(15, 44, "Tokens start at 1");
  u8g2.sendBuffer();
  delay(1500);
  updateDisplay();
}

// ─────────────────────────────────────────────
//  WebSocket broadcast (send state to all clients)
// ─────────────────────────────────────────────
void broadcastState() {
  // Build a compact JSON payload
  // { "current":X, "waiting":W, "missed":M, "active":A, "limit":L,
  //   "tokens":[{n,s,mc},...] }
  // NOTE: "tokens" only includes currently ACTIVE tokens
  // (Waiting/Serving/Missed/Recalled) so the dashboard grid
  // doesn't fill up with the whole day's completed tokens.
  String json = "{";
  json += "\"current\":"  + String(currentToken) + ",";
  json += "\"waiting\":"  + String(countWaiting()) + ",";
  json += "\"missed\":"   + String(countMissed()) + ",";
  json += "\"active\":"   + String(countActive()) + ",";
  json += "\"limit\":"    + String(ACTIVE_LIMIT) + ",";
  json += "\"served\":"   + String(totalServed) + ",";
  json += "\"missedTotal\":" + String(totalMissedCount) + ",";
  json += "\"date\":\""   + getDateString() + "\",";
  json += "\"day\":\""    + getDayString() + "\",";
  json += "\"doctor\":\"" + String(doctorName) + "\",";
  json += "\"tokens\":[";
  bool first = true;
  for (int i = 1; i <= tokenCount; i++) {
    int s = tokens[i].status;
    if (s == STATUS_WAITING || s == STATUS_SERVING ||
        s == STATUS_MISSED  || s == STATUS_RECALLED) {
      if (!first) json += ",";
      json += "{\"n\":" + String(tokens[i].number)
            + ",\"s\":"  + String(tokens[i].status)
            + ",\"mc\":" + String(tokens[i].missCount)
            + "}";
      first = false;
    }
  }
  json += "]}";
  webSocket.broadcastTXT(json);
}

// Send a one-off event to the dashboard (cancelled / full / reset)
void broadcastEvent(const char* evt, int tokenNum) {
  String json = "{\"event\":\"";
  json += evt;
  json += "\",\"token\":" + String(tokenNum);
  json += ",\"limit\":" + String(ACTIVE_LIMIT);
  json += "}";
  webSocket.broadcastTXT(json);
}

// ─────────────────────────────────────────────
//  Actions
// ─────────────────────────────────────────────

// Move to next waiting token (called after "Next Patient" or after recall)
void serveNextWaiting() {
  for (int i = 1; i <= tokenCount; i++) {
    if (tokens[i].status == STATUS_WAITING) {
      currentToken = i;
      tokens[i].status = STATUS_SERVING;
      Serial.printf("Token %d now Serving\n", i);
      return;
    }
  }
  // No waiting tokens
  currentToken = 0;
}

// "Next Patient" button
void actionNext() {
  if (currentToken > 0) {
    tokens[currentToken].status = STATUS_COMPLETED;
    totalServed++;
    Serial.printf("Token %d Completed (Total Served: %d)\n", currentToken, totalServed);
    currentToken = 0;
  }
  serveNextWaiting();
  updateDisplay();
  broadcastState();
}

// "Miss Token" button
void actionMiss() {
  if (currentToken == 0) return;

  int t = currentToken;
  tokens[t].missCount++;
  tokens[t].status = STATUS_MISSED;
  Serial.printf("Token %d Missed (miss count=%d)\n", t, tokens[t].missCount);
  currentToken = 0;

  serveNextWaiting();
  updateDisplay();
  broadcastState();
}

// "Recall Missed Token" button – recall oldest missed token
void actionRecall() {
  for (int i = 1; i <= tokenCount; i++) {
    if (tokens[i].status == STATUS_MISSED) {

      // If there is currently a serving token, put it back to waiting
      if (currentToken > 0) {
        tokens[currentToken].status = STATUS_WAITING;
      }

      currentToken     = i;
      tokens[i].status = STATUS_RECALLED;
      Serial.printf("Token %d Recalled\n", i);

      updateDisplay();
      broadcastState();
      return;
    }
  }
  Serial.println("No missed tokens to recall");
  broadcastState();
}

// When a recalled token is missed again → REMOVE it permanently (Cancelled)
void actionMissRecalled() {
  if (currentToken == 0) return;
  if (tokens[currentToken].status != STATUS_RECALLED) {
    actionMiss();   // normal miss
    return;
  }

  int t = currentToken;
  tokens[t].status = STATUS_REMOVED;
  totalMissedCount++;
  Serial.printf("Token %d Cancelled (missed twice, Total Missed: %d)\n", t, totalMissedCount);
  currentToken = 0;

  serveNextWaiting();
  updateDisplay();
  broadcastState();

  // Show cancellation notice on OLED and dashboard
  broadcastEvent("cancelled", t);
  showCancelledMessage(t);
  broadcastState();   // refresh state after the temporary screen
}

// "Reset System" – wipe everything, restart numbering from 1
void actionReset() {
  for (int i = 0; i <= TOKEN_STORAGE; i++) {
    tokens[i].number   = 0;
    tokens[i].status   = STATUS_WAITING;
    tokens[i].missCount = 0;
  }
  tokenCount   = 0;
  currentToken = 0;
  totalServed      = 0;
  totalMissedCount = 0;

  Serial.println("=== SYSTEM RESET — token numbering restarted from 1 ===");

  updateDisplay();
  broadcastState();
  broadcastEvent("reset", 0);
  showResetMessage();
  broadcastState();
}

// ─────────────────────────────────────────────
//  HTTP handlers
// ─────────────────────────────────────────────

// Inline HTML+CSS+JS – served once; WebSocket keeps it live
String getHTML() {
  String ip = WiFi.localIP().toString();
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Token System</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@400;700;900&display=swap');

  :root{
    --bg:#0a0e1a;--panel:#111827;--border:#1e2d45;
    --green:#00e676;--red:#ff1744;--gray:#546e7a;
    --yellow:#ffd740;--blue:#40c4ff;--white:#e0f2f1;
    --serving-glow: 0 0 24px #00e67688;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--white);
       font-family:'Share Tech Mono',monospace;min-height:100vh;
       background-image:
         radial-gradient(ellipse at 20% 20%,#0d2137 0%,transparent 60%),
         radial-gradient(ellipse at 80% 80%,#0d1f0f 0%,transparent 60%);
  }
  header{
    text-align:center;padding:28px 16px 16px;
    border-bottom:1px solid var(--border);
  }
  header h1{
    font-family:'Orbitron',sans-serif;font-size:clamp(1.2rem,5vw,2rem);
    font-weight:900;letter-spacing:.15em;color:var(--green);
    text-shadow:0 0 18px #00e67666;
  }
  header p{font-size:.75rem;color:var(--gray);margin-top:6px;letter-spacing:.1em}
  #doctorLine{color:var(--blue);font-weight:bold;font-size:.85rem;letter-spacing:.12em}
  #dateLine{font-size:.7rem;margin-top:4px}

  .stats{
    display:flex;flex-wrap:wrap;gap:12px;
    justify-content:center;padding:20px 16px;
  }
  .stat-card{
    background:var(--panel);border:1px solid var(--border);
    border-radius:12px;padding:18px 28px;text-align:center;
    min-width:120px;flex:1;max-width:200px;
    transition:box-shadow .3s;
  }
  .stat-card.serving{border-color:var(--green);box-shadow:var(--serving-glow)}
  .stat-card.missed-card{border-color:var(--red)}
  .stat-card.active-card{border-color:var(--blue)}
  .stat-card.served-card{border-color:var(--green)}
  .stat-card.total-missed-card{border-color:var(--red)}
  .stat-label{font-size:.65rem;letter-spacing:.12em;color:var(--gray);text-transform:uppercase}
  .stat-value{
    font-family:'Orbitron',sans-serif;font-size:2.4rem;
    font-weight:700;margin-top:6px;line-height:1;
  }
  .serving .stat-value{color:var(--green);text-shadow:0 0 12px #00e676}
  .missed-card .stat-value{color:var(--red)}
  .waiting-card .stat-value{color:var(--yellow)}
  .active-card .stat-value{color:var(--blue);text-shadow:0 0 12px #40c4ff66}
  .served-card .stat-value{color:var(--green)}
  .total-missed-card .stat-value{color:var(--red)}

  .actions{
    display:flex;flex-wrap:wrap;gap:10px;
    justify-content:center;padding:0 16px 12px;
  }
  button{
    font-family:'Share Tech Mono',monospace;
    font-size:.82rem;letter-spacing:.08em;
    padding:12px 22px;border:none;border-radius:8px;
    cursor:pointer;transition:all .18s;font-weight:bold;
    text-transform:uppercase;
  }
  button:active{transform:scale(.96)}
  .btn-next{background:var(--green);color:#000}
  .btn-next:hover{box-shadow:0 0 16px #00e67688}
  .btn-miss{background:var(--red);color:#fff}
  .btn-miss:hover{box-shadow:0 0 16px #ff174488}
  .btn-recall{background:var(--yellow);color:#000}
  .btn-recall:hover{box-shadow:0 0 16px #ffd74088}
  .btn-reset{background:transparent;color:var(--red);border:2px solid var(--red)}
  .btn-reset:hover{background:var(--red);color:#fff;box-shadow:0 0 16px #ff174488}

  .reset-row{padding:0 16px 20px;display:flex;justify-content:center}

  .queue-section{padding:0 16px 32px}
  .queue-title{
    font-family:'Orbitron',sans-serif;font-size:.75rem;
    letter-spacing:.15em;color:var(--gray);
    text-transform:uppercase;margin-bottom:12px;
    padding-bottom:6px;border-bottom:1px solid var(--border);
  }
  .token-grid{
    display:flex;flex-wrap:wrap;gap:8px;
  }
  .token-chip{
    width:52px;height:52px;border-radius:10px;
    display:flex;align-items:center;justify-content:center;
    font-family:'Orbitron',sans-serif;font-weight:700;font-size:1rem;
    border:2px solid transparent;transition:all .25s;
    position:relative;
  }
  .chip-waiting  {background:#1a2535;border-color:#1e2d45;color:var(--white)}
  .chip-serving  {background:#0a2e1a;border-color:var(--green);color:var(--green);
                  box-shadow:var(--serving-glow);animation:pulse 2s infinite}
  .chip-missed   {background:#2e0a0a;border-color:var(--red);color:var(--red)}
  .chip-recalled {background:#2e2000;border-color:var(--yellow);color:var(--yellow)}
  .chip-completed{background:#0d0d0d;border-color:#263238;color:var(--gray)}
  .chip-removed  {background:#0d0d0d;border-color:#263238;color:#37474f;
                  text-decoration:line-through}

  @keyframes pulse{
    0%,100%{box-shadow:0 0 12px #00e67644}
    50%     {box-shadow:0 0 28px #00e67699}
  }

  .status-bar{
    text-align:center;font-size:.7rem;color:var(--gray);
    padding:10px 16px 24px;letter-spacing:.08em;
  }
  .dot{
    display:inline-block;width:8px;height:8px;border-radius:50%;
    background:var(--green);margin-right:6px;
    animation:blink 1.4s infinite;
  }
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
  .legend{display:flex;flex-wrap:wrap;gap:10px;
          justify-content:center;padding:0 16px 20px;font-size:.68rem}
  .leg{display:flex;align-items:center;gap:6px;color:var(--gray)}
  .leg-dot{width:12px;height:12px;border-radius:3px}

  .toast{
    position:fixed;bottom:24px;left:50%;
    transform:translateX(-50%) translateY(120px);
    background:var(--panel);border:1px solid var(--yellow);
    color:var(--yellow);padding:14px 24px;border-radius:10px;
    font-size:.8rem;letter-spacing:.05em;text-align:center;
    max-width:90%;box-shadow:0 4px 24px rgba(0,0,0,.6);
    opacity:0;transition:transform .3s ease,opacity .3s ease;
    z-index:999;font-weight:bold;
  }
  .toast.show{transform:translateX(-50%) translateY(0);opacity:1}
  .toast.danger{border-color:var(--red);color:var(--red)}
  .toast.info{border-color:var(--blue);color:var(--blue)}
</style>
</head>
<body>

<header>
  <h1>&#9874; SMART TOKEN SYSTEM</h1>
  <p id="doctorLine">PATIENT QUEUE MANAGEMENT</p>
  <p id="dateLine">&#8212;</p>
</header>

<div class="stats">
  <div class="stat-card serving">
    <div class="stat-label">Now Serving</div>
    <div class="stat-value" id="cur">—</div>
  </div>
  <div class="stat-card waiting-card">
    <div class="stat-label">Waiting</div>
    <div class="stat-value" id="wait">0</div>
  </div>
  <div class="stat-card missed-card">
    <div class="stat-label">Missed</div>
    <div class="stat-value" id="miss">0</div>
  </div>
  <div class="stat-card active-card">
    <div class="stat-label">Queue</div>
    <div class="stat-value" id="active">0/10</div>
  </div>
  <div class="stat-card served-card">
    <div class="stat-label">Total Served</div>
    <div class="stat-value" id="served">0</div>
  </div>
  <div class="stat-card total-missed-card">
    <div class="stat-label">Total Missed</div>
    <div class="stat-value" id="missedTotal">0</div>
  </div>
</div>

<div class="actions">
  <button class="btn-next"   onclick="send('next')">&#10003; Next Patient</button>
  <button class="btn-miss"   onclick="send('miss')">&#10007; Miss Token</button>
  <button class="btn-recall" onclick="send('recall')">&#8617; Recall Missed</button>
</div>

<div class="reset-row">
  <button class="btn-reset" onclick="confirmReset()">&#10227; Reset System</button>
</div>

<div class="queue-section">
  <div class="queue-title">&#9632; Token Queue</div>
  <div class="token-grid" id="grid"></div>
</div>

<div class="legend">
  <div class="leg"><div class="leg-dot" style="background:#1a2535;border:1px solid #1e2d45"></div>Waiting</div>
  <div class="leg"><div class="leg-dot" style="background:#00e676"></div>Serving</div>
  <div class="leg"><div class="leg-dot" style="background:#ff1744"></div>Missed</div>
  <div class="leg"><div class="leg-dot" style="background:#ffd740"></div>Recalled</div>
  <div class="leg"><div class="leg-dot" style="background:#263238"></div>Completed</div>
  <div class="leg"><div class="leg-dot" style="background:#37474f"></div>Cancelled</div>
</div>

<div class="status-bar">
  <span class="dot"></span>
  <span id="wsStatus">Connecting…</span>
</div>

<div id="toast" class="toast"></div>

<script>
const STATUS = ['Waiting','Serving','Missed','Recalled','Completed','Removed'];
const CHIP   = ['chip-waiting','chip-serving','chip-missed','chip-recalled','chip-completed','chip-removed'];

)rawhtml";

  html += "var wsUrl = 'ws://" + ip + ":81/';\n";

  html += R"rawhtml(
var ws;
function connect(){
  ws = new WebSocket(wsUrl);
  ws.onopen  = ()=>{ document.getElementById('wsStatus').textContent='Connected to )rawhtml";
  html += ip;
  html += R"rawhtml('; };
  ws.onclose = ()=>{ document.getElementById('wsStatus').textContent='Reconnecting…';
                     setTimeout(connect,2000); };
  ws.onerror = ()=>{ ws.close(); };
  ws.onmessage = e => {
    var d = JSON.parse(e.data);
    if (d.event === 'cancelled') {
      showToast('Token #' + d.token + ' CANCELLED — Please take a new appointment', 'danger');
    } else if (d.event === 'full') {
      showToast('Appointments Full — Max ' + d.limit + ' active tokens reached', 'danger');
    } else if (d.event === 'reset') {
      showToast('System Reset — Token numbering restarted from 1', 'info');
    } else {
      update(d);
    }
  };
}
connect();

function send(cmd){
  if(ws && ws.readyState===1) ws.send(cmd);
}

function confirmReset(){
  if(confirm('Reset the entire token queue?\nToken numbering will restart from 1.\nThis cannot be undone.')){
    send('reset');
  }
}

var toastTimer = null;
function showToast(msg, cls){
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show' + (cls ? (' ' + cls) : '');
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>{ t.className = 'toast'; }, 4000);
}

function update(d){
  document.getElementById('cur').textContent  = d.current>0 ? d.current : '—';
  document.getElementById('wait').textContent = d.waiting;
  document.getElementById('miss').textContent = d.missed;
  document.getElementById('active').textContent = d.active + '/' + d.limit;
  document.getElementById('served').textContent = d.served;
  document.getElementById('missedTotal').textContent = d.missedTotal;
  document.getElementById('doctorLine').textContent = d.doctor + ' — Patient Queue Management';
  document.getElementById('dateLine').textContent = d.day + ', ' + d.date;

  var grid = document.getElementById('grid');
  grid.innerHTML = '';
  d.tokens.forEach(t=>{
    var chip = document.createElement('div');
    chip.className = 'token-chip ' + CHIP[t.s];
    chip.textContent = t.n;
    chip.title = '#'+t.n+' – '+STATUS[t.s]+(t.mc>0?' (missed '+t.mc+'x)':'');
    grid.appendChild(chip);
  });
}
</script>
</body>
</html>
)rawhtml";

  return html;
}

void handleRoot() {
  server.send(200, "text/html", getHTML());
}

// ─────────────────────────────────────────────
//  WebSocket event handler
// ─────────────────────────────────────────────
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    msg.trim();

    if (msg == "next") {
      actionNext();
    }
    else if (msg == "miss") {
      // If the current token is in RECALLED state → cancel it permanently
      if (currentToken > 0 && tokens[currentToken].status == STATUS_RECALLED) {
        actionMissRecalled();
      } else {
        actionMiss();
      }
    }
    else if (msg == "recall") {
      actionRecall();
    }
    else if (msg == "reset") {
      actionReset();
    }
  }
  else if (type == WStype_CONNECTED) {
    Serial.printf("WebSocket client #%u connected\n", num);
    broadcastState();   // send current state to newly connected client
  }
  else if (type == WStype_DISCONNECTED) {
    Serial.printf("WebSocket client #%u disconnected\n", num);
  }
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Button
  pinMode(BUTTON_PIN, INPUT);

  // OLED
  Wire.begin(21, 22);
  u8g2.begin();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(30, 32, "OLED OK");
  u8g2.sendBuffer();
  delay(1500);

  // WiFi
  u8g2.clearBuffer();
  u8g2.drawStr(5, 32, "Connecting WiFi...");
  u8g2.sendBuffer();

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  // Sync date/time via NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  struct tm timeinfo;
  int ntpTries = 0;
  while (!getLocalTime(&timeinfo, 500) && ntpTries < 10) {
    Serial.print(".");
    ntpTries++;
  }
  Serial.println();
  if (ntpTries < 10) {
    Serial.println("Time synced: " + getDayString() + ", " + getDateString());
  } else {
    Serial.println("Time sync failed (will retry in background)");
  }

  // Show IP on OLED
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "WiFi Connected!");
  u8g2.drawStr(0, 28, "IP:");
  String ip = WiFi.localIP().toString();
  u8g2.drawStr(0, 42, ip.c_str());
  u8g2.drawStr(0, 56, "Port 80 / WS:81");
  u8g2.sendBuffer();
  delay(3000);

  updateDisplay();

  // HTTP server
  server.on("/", handleRoot);
  server.begin();
  Serial.println("HTTP server started on port 80");

  // WebSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  server.handleClient();
  webSocket.loop();

  // Physical button → generate new token
  if (digitalRead(BUTTON_PIN) == HIGH) {
    // Wait for release
    while (digitalRead(BUTTON_PIN) == HIGH) delay(10);
    delay(200);   // debounce

    if (countActive() >= ACTIVE_LIMIT) {
      // Active queue is full (10 tokens currently
      // Waiting/Serving/Missed/Recalled) — refuse new token
      Serial.println("Appointments Full! (active queue at limit)");
      broadcastEvent("full", 0);
      showFullMessage();

    } else if (tokenCount >= TOKEN_STORAGE) {
      // Extremely unlikely (500 tokens issued since last reset)
      Serial.println("Token storage full — please Reset the system");
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(5, 24, "Storage Full!");
      u8g2.drawStr(5, 40, "Please Reset System");
      u8g2.sendBuffer();
      delay(2000);
      updateDisplay();

    } else {
      // Generate a new token (number keeps climbing: 1,2,3...11,12,13...)
      tokenCount++;
      tokens[tokenCount].number    = tokenCount;
      tokens[tokenCount].status    = STATUS_WAITING;
      tokens[tokenCount].missCount = 0;

      Serial.printf("Token Generated: %d\n", tokenCount);

      // If nothing is being served right now, serve this token immediately
      if (currentToken == 0) {
        currentToken = tokenCount;
        tokens[tokenCount].status = STATUS_SERVING;
        Serial.printf("Token %d now Serving (auto)\n", tokenCount);
      }

      updateDisplay();
      broadcastState();
    }
  }
}
