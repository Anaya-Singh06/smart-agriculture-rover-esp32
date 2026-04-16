#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────
//  WiFi
// ─────────────────────────────────────────
const char* ssid     = "ESP32_CAR";
const char* password = "12345678";
WebServer server(80);

// ─────────────────────────────────────────
//  Sensors
// ─────────────────────────────────────────
#define DHTPIN    4
#define DHTTYPE   DHT11
DHT dht(DHTPIN, DHTTYPE);
#define SOIL_PIN  34

// ─────────────────────────────────────────
//  Servo
// ─────────────────────────────────────────
Servo myServo;
#define SERVO_PIN 5

// ─────────────────────────────────────────
//  Motor pins  (TB6612FNG)
// ─────────────────────────────────────────
#define IN1  13   // AIN1
#define IN2  23   // AIN2
#define IN3  14   // BIN1
#define IN4  27   // BIN2
#define PWMA 26
#define PWMB 25
#define STBY 33

// ─────────────────────────────────────────
//  Auto-navigation state machine
// ─────────────────────────────────────────
/*
  States:
    0 = IDLE / MANUAL
    1 = RUNNING  – executing a move step
    2 = PROBING  – servo deploy + sensor read
    3 = DONE     – mission complete, stop
*/

struct Waypoint {
  char  dir;        // 'F','B','L','R','S'
  unsigned long dur; // milliseconds to run
  bool  probe;      // deploy servo at end of this step
};

#define MAX_WP 20
Waypoint waypoints[MAX_WP];
int  wpCount    = 0;
int  wpIndex    = 0;
int  autoState  = 0;           // 0=IDLE, 1=RUNNING, 2=PROBING, 3=DONE
unsigned long stepStart   = 0;
unsigned long probeStart  = 0;
bool  missionActive = false;

// Store last sensor reading taken at each probe point
struct ProbeResult {
  float temp;
  float hum;
  int   soil;
  String suggestion;
};
#define MAX_RESULTS 10
ProbeResult probeResults[MAX_RESULTS];
int probeResultCount = 0;

// ─────────────────────────────────────────
//  Motor helpers
// ─────────────────────────────────────────
void moveForward() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}
void moveBack() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}
void moveLeft() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}
void moveRight() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}
void stopBot() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void driveDir(char d) {
  switch(d) {
    case 'F': moveForward(); break;
    case 'B': moveBack();    break;
    case 'L': moveLeft();    break;
    case 'R': moveRight();   break;
    default:  stopBot();     break;
  }
}

// ─────────────────────────────────────────
//  Servo probe (non-blocking via state)
// ─────────────────────────────────────────
// Called once to begin probe; loop() handles timing
void beginProbe() {
  myServo.write(90);
  probeStart = millis();
  autoState  = 2;
}

// ─────────────────────────────────────────
//  Smart logic
// ─────────────────────────────────────────
String getSuggestion(float t, float h, int s) {
  if (s < 2000) return "Water Plant Required";
  if (t > 26)   return "Temperature Critical";
  if (h < 40)   return "Low Humidity Warning";
  return "All Systems Optimal";
}

// ─────────────────────────────────────────
//  HTTP handlers
// ─────────────────────────────────────────

// Manual command: /cmd?go=F|B|L|R|S|C
void handleCommand() {
  if (missionActive) {
    // Abort auto-nav if any manual command arrives
    missionActive = false;
    autoState = 0;
    wpCount = 0;
  }
  String cmd = server.arg("go");
  if      (cmd == "F") moveForward();
  else if (cmd == "B") moveBack();
  else if (cmd == "L") moveLeft();
  else if (cmd == "R") moveRight();
  else if (cmd == "S") stopBot();
  else if (cmd == "C") {
    myServo.write(90);
    delay(1500);   // manual probe: blocking is OK, 1.5 s only
    myServo.write(0);
  }
  server.send(200, "text/plain", "OK");
}

// Receive mission JSON: POST /mission
// Body: {"waypoints":[{"dir":"F","dur":2000,"probe":false}, ...]}
void handleMission() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "POST only");
    return;
  }
  String body = server.arg("plain");
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "Bad JSON");
    return;
  }
  JsonArray arr = doc["waypoints"].as<JsonArray>();
  wpCount = 0;
  probeResultCount = 0;
  for (JsonObject wp : arr) {
    if (wpCount >= MAX_WP) break;
    waypoints[wpCount].dir   = wp["dir"].as<String>()[0];
    waypoints[wpCount].dur   = wp["dur"].as<unsigned long>();
    waypoints[wpCount].probe = wp["probe"].as<bool>();
    wpCount++;
  }
  wpIndex       = 0;
  autoState     = 1;
  missionActive = true;
  stepStart     = millis();
  driveDir(waypoints[0].dir);
  server.send(200, "text/plain", "Mission started");
}

// Abort mission: GET /abort
void handleAbort() {
  missionActive = false;
  autoState = 0;
  wpCount = 0;
  stopBot();
  myServo.write(0);
  server.send(200, "text/plain", "Aborted");
}

// Mission status: GET /status
void handleStatus() {
  String json = "{";
  json += "\"active\":" + String(missionActive ? "true" : "false") + ",";
  json += "\"state\":" + String(autoState) + ",";
  json += "\"step\":"  + String(wpIndex)   + ",";
  json += "\"total\":" + String(wpCount)   + ",";
  json += "\"results\":[";
  for (int i = 0; i < probeResultCount; i++) {
    if (i) json += ",";
    json += "{\"point\":" + String(i+1) + ",";
    json += "\"temp\":"   + String(probeResults[i].temp, 1) + ",";
    json += "\"hum\":"    + String(probeResults[i].hum,  1) + ",";
    json += "\"soil\":"   + String(probeResults[i].soil)    + ",";
    json += "\"msg\":\""  + probeResults[i].suggestion + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Sensor data: GET /data
void handleData() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int   s = analogRead(SOIL_PIN);
  String json = "{";
  json += "\"temp\":"  + String(t) + ",";
  json += "\"hum\":"   + String(h) + ",";
  json += "\"soil\":"  + String(s) + ",";
  json += "\"msg\":\"" + getSuggestion(t, h, s) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// Root page
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AgriRover OS</title>
<style>
:root{--bg:#0a0c10;--surface:#111318;--surface2:#181b22;--border:rgba(255,255,255,0.07);--border2:rgba(0,255,180,0.2);--accent:#00ffb3;--accent2:#00cfff;--accent3:#7b61ff;--text:#e8edf5;--muted:#6b7280;--danger:#ff4d6d;--warn:#f59e0b;--ok:#00ffb3;}
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:var(--bg);color:var(--text);min-height:100vh;overflow-x:hidden;}
.sidebar{position:fixed;left:0;top:0;bottom:0;width:60px;background:var(--surface);border-right:1px solid var(--border);display:flex;flex-direction:column;align-items:center;padding:18px 0;gap:6px;z-index:100;}
.logo{width:34px;height:34px;background:linear-gradient(135deg,var(--accent),var(--accent2));border-radius:10px;display:flex;align-items:center;justify-content:center;margin-bottom:14px;}
.logo svg{width:18px;height:18px;}
.nav-item{width:40px;height:40px;border-radius:10px;display:flex;align-items:center;justify-content:center;cursor:pointer;color:var(--muted);border:1px solid transparent;transition:all 0.2s;}
.nav-item:hover{background:var(--surface2);color:var(--text);}
.nav-item.active{background:rgba(0,255,179,0.1);color:var(--accent);border-color:rgba(0,255,179,0.2);}
.nav-item svg{width:16px;height:16px;}
.topbar{position:fixed;left:60px;right:0;top:0;height:54px;background:var(--surface);border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;padding:0 22px;z-index:99;}
.topbar-left{display:flex;align-items:center;gap:10px;}
.page-title{font-size:13px;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;color:var(--accent);}
.breadcrumb{font-size:11px;color:var(--muted);}
.topbar-right{display:flex;align-items:center;gap:14px;}
.status-pill{display:flex;align-items:center;gap:5px;background:var(--surface2);border:1px solid var(--border);border-radius:20px;padding:4px 11px;font-size:11px;}
.dot{width:6px;height:6px;border-radius:50%;}
.dot.green{background:var(--ok);box-shadow:0 0 6px var(--ok);}
.dot.red{background:var(--danger);}
.dot.amber{background:var(--warn);}
.batt{display:flex;align-items:center;gap:5px;font-size:11px;color:var(--muted);}
.batt-bar{width:26px;height:11px;border:1px solid var(--muted);border-radius:3px;position:relative;overflow:hidden;}
.batt-fill{height:100%;width:72%;background:linear-gradient(90deg,var(--ok),var(--accent2));border-radius:2px;}
.time-display{font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums;}
.main{margin-left:60px;margin-top:54px;padding:18px;display:grid;grid-template-columns:1fr 330px;gap:14px;min-height:calc(100vh - 54px);}
.left-col{display:flex;flex-direction:column;gap:14px;}
.card{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:18px;position:relative;overflow:hidden;}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:1px;background:linear-gradient(90deg,transparent,rgba(0,255,179,0.12),transparent);}
.card-title{font-size:10px;font-weight:700;letter-spacing:0.1em;text-transform:uppercase;color:var(--muted);margin-bottom:14px;display:flex;align-items:center;gap:7px;}
.card-title svg{width:11px;height:11px;}
.sensor-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;}
.sensor-card{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:18px;transition:border-color 0.3s;}
.sensor-card.ok{border-color:rgba(0,255,179,0.15);}
.sensor-card.warn{border-color:rgba(245,158,11,0.3);}
.sensor-card.danger{border-color:rgba(255,77,109,0.3);}
.sensor-label{font-size:9px;font-weight:700;letter-spacing:0.1em;text-transform:uppercase;color:var(--muted);margin-bottom:10px;}
.sensor-value{font-size:36px;font-weight:300;line-height:1;}
.sensor-value.ok{background:linear-gradient(135deg,var(--ok),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.sensor-value.warn{background:linear-gradient(135deg,var(--warn),#fb923c);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.sensor-value.danger{background:linear-gradient(135deg,var(--danger),#fb7185);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.gauge-track{height:4px;background:var(--surface2);border-radius:4px;overflow:hidden;margin-top:12px;}
.gauge-fill{height:100%;border-radius:4px;transition:width 0.9s cubic-bezier(.4,0,.2,1);}
.gauge-fill.ok{background:linear-gradient(90deg,var(--ok),var(--accent2));}
.gauge-fill.warn{background:linear-gradient(90deg,var(--warn),#fb923c);}
.gauge-fill.danger{background:linear-gradient(90deg,var(--danger),#fb7185);}
.sensor-sub{font-size:10px;color:var(--muted);margin-top:5px;display:flex;justify-content:space-between;}
.alert-card{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:14px 18px;display:flex;align-items:center;gap:14px;}
.alert-icon{width:42px;height:42px;border-radius:11px;display:flex;align-items:center;justify-content:center;flex-shrink:0;font-size:20px;}
.alert-icon.ok{background:rgba(0,255,179,0.1);}
.alert-icon.warn{background:rgba(245,158,11,0.1);}
.alert-icon.danger{background:rgba(255,77,109,0.1);}
.alert-label{font-size:9px;color:var(--muted);letter-spacing:0.08em;text-transform:uppercase;}
.alert-text{font-size:17px;font-weight:600;margin-top:2px;}
.alert-text.ok{color:var(--ok);}
.alert-text.warn{color:var(--warn);}
.alert-text.danger{color:var(--danger);}
.alert-timestamp{font-size:11px;color:var(--muted);margin-left:auto;}
.chart-area{height:56px;position:relative;overflow:hidden;margin-top:6px;}
canvas.mini-chart{width:100%;height:100%;}
.control-panel{display:flex;flex-direction:column;gap:14px;}
.direction-ring{width:110px;height:110px;border-radius:50%;border:2px solid var(--border);position:relative;margin:0 auto 10px;display:flex;align-items:center;justify-content:center;transition:all 0.3s;}
.dir-arrow{position:absolute;width:100%;height:100%;top:0;left:0;display:flex;align-items:center;justify-content:center;}
.dir-arrow svg{width:26px;height:26px;color:var(--accent);opacity:0;transition:opacity 0.2s,transform 0.3s;}
.dir-label{font-size:10px;color:var(--muted);letter-spacing:0.1em;text-transform:uppercase;}
.dpad{display:grid;grid-template-columns:repeat(3,52px);grid-template-rows:repeat(3,52px);gap:5px;place-content:center;margin:6px auto;}
.ctrl-btn{width:52px;height:52px;background:var(--surface2);border:1px solid var(--border);border-radius:11px;display:flex;align-items:center;justify-content:center;cursor:pointer;color:var(--muted);user-select:none;-webkit-user-select:none;transition:all 0.1s;}
.ctrl-btn:hover{border-color:rgba(0,255,179,0.3);color:var(--accent);}
.ctrl-btn.active,.ctrl-btn:active{background:rgba(0,255,179,0.12);border-color:var(--accent);color:var(--accent);transform:scale(0.93);box-shadow:0 0 14px rgba(0,255,179,0.18);}
.ctrl-btn svg{width:20px;height:20px;}
.ctrl-center{border-color:rgba(255,77,109,0.3);color:var(--danger);}
.ctrl-center:hover{border-color:var(--danger);background:rgba(255,77,109,0.1);}
.ctrl-center.active{background:rgba(255,77,109,0.18);border-color:var(--danger);}
.key-hints{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin-top:4px;}
.key-hint{display:flex;align-items:center;gap:5px;font-size:10px;color:var(--muted);}
.key-badge{background:var(--surface2);border:1px solid var(--border);border-radius:5px;padding:1px 5px;font-size:9px;font-weight:700;font-family:monospace;color:var(--text);min-width:20px;text-align:center;}
.actuator-btn{width:100%;padding:13px;background:var(--surface2);border:1px solid rgba(0,207,255,0.2);border-radius:11px;color:var(--accent2);font-size:12px;font-weight:700;letter-spacing:0.05em;cursor:pointer;transition:all 0.2s;display:flex;align-items:center;justify-content:center;gap:9px;}
.actuator-btn:hover{background:rgba(0,207,255,0.08);border-color:var(--accent2);}
.actuator-btn.triggered{background:rgba(0,207,255,0.15);border-color:var(--accent2);}
.actuator-btn svg{width:15px;height:15px;}
.deploy-anim{width:9px;height:9px;border-radius:50%;background:var(--accent2);display:none;}
.deploy-anim.active{display:block;animation:pulse 0.8s ease-in-out infinite;}
@keyframes pulse{0%,100%{transform:scale(1);opacity:1;}50%{transform:scale(1.6);opacity:0.4;}}
.telem-row{display:flex;align-items:center;justify-content:space-between;padding:7px 0;border-bottom:1px solid var(--border);font-size:11px;}
.telem-row:last-child{border-bottom:none;}
.telem-label{color:var(--muted);}
.telem-val{font-weight:600;display:flex;align-items:center;gap:5px;}
.signal{display:flex;align-items:flex-end;gap:2px;height:13px;}
.sig-bar{width:3px;background:var(--muted);border-radius:1px;}
.sig-bar.lit{background:var(--ok);}

/* ── Mode toggle ── */
.mode-toggle{display:flex;gap:0;border-radius:10px;overflow:hidden;border:1px solid var(--border);margin-bottom:14px;}
.mode-btn{flex:1;padding:9px 0;font-size:11px;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;cursor:pointer;border:none;background:var(--surface2);color:var(--muted);transition:all 0.2s;}
.mode-btn.active{background:rgba(0,255,179,0.12);color:var(--accent);}
.mode-btn.auto-active{background:rgba(123,97,255,0.14);color:var(--accent3);}

/* ── Mission planner ── */
.mission-panel{display:none;}
.mission-panel.visible{display:block;}
.wp-list{display:flex;flex-direction:column;gap:6px;margin-bottom:12px;max-height:210px;overflow-y:auto;}
.wp-row{display:flex;align-items:center;gap:7px;background:var(--surface2);border:1px solid var(--border);border-radius:9px;padding:8px 10px;font-size:11px;}
.wp-num{width:18px;height:18px;border-radius:5px;background:rgba(123,97,255,0.2);color:var(--accent3);display:flex;align-items:center;justify-content:center;font-size:10px;font-weight:700;flex-shrink:0;}
.wp-dir-badge{padding:2px 8px;border-radius:5px;font-size:10px;font-weight:700;background:rgba(0,255,179,0.1);color:var(--accent);}
.wp-dur{color:var(--muted);}
.wp-probe-badge{padding:2px 7px;border-radius:5px;font-size:10px;background:rgba(0,207,255,0.1);color:var(--accent2);}
.wp-del{margin-left:auto;background:none;border:none;color:var(--muted);cursor:pointer;font-size:14px;padding:0 2px;}
.wp-del:hover{color:var(--danger);}
.wp-add-row{display:flex;gap:6px;align-items:center;margin-bottom:10px;}
.wp-select,.wp-input{background:var(--surface2);border:1px solid var(--border);border-radius:8px;color:var(--text);padding:7px 9px;font-size:11px;}
.wp-select{width:76px;}
.wp-input{width:70px;}
.wp-checkbox-label{font-size:11px;color:var(--muted);display:flex;align-items:center;gap:5px;cursor:pointer;}
.wp-add-btn{padding:7px 12px;background:rgba(123,97,255,0.15);border:1px solid rgba(123,97,255,0.3);border-radius:8px;color:var(--accent3);font-size:11px;font-weight:700;cursor:pointer;transition:all 0.2s;}
.wp-add-btn:hover{background:rgba(123,97,255,0.25);}
.mission-actions{display:flex;gap:8px;}
.mission-run-btn{flex:2;padding:11px;background:rgba(123,97,255,0.15);border:1px solid rgba(123,97,255,0.3);border-radius:10px;color:var(--accent3);font-size:12px;font-weight:700;cursor:pointer;transition:all 0.2s;}
.mission-run-btn:hover{background:rgba(123,97,255,0.25);}
.mission-run-btn:disabled{opacity:0.4;cursor:not-allowed;}
.mission-abort-btn{flex:1;padding:11px;background:rgba(255,77,109,0.1);border:1px solid rgba(255,77,109,0.2);border-radius:10px;color:var(--danger);font-size:12px;font-weight:700;cursor:pointer;transition:all 0.2s;}
.mission-abort-btn:hover{background:rgba(255,77,109,0.2);}
.progress-bar-track{height:5px;background:var(--surface2);border-radius:5px;overflow:hidden;margin-bottom:10px;}
.progress-bar-fill{height:100%;background:linear-gradient(90deg,var(--accent3),var(--accent2));border-radius:5px;transition:width 0.4s ease;}
.probe-results{margin-top:10px;}
.probe-result-row{display:flex;gap:8px;align-items:center;padding:7px 10px;background:var(--surface2);border-radius:8px;font-size:11px;margin-bottom:5px;}
.probe-pt{width:20px;height:20px;border-radius:5px;background:rgba(0,207,255,0.12);color:var(--accent2);display:flex;align-items:center;justify-content:center;font-size:10px;font-weight:700;flex-shrink:0;}
.probe-val{color:var(--muted);}
.probe-val span{color:var(--text);font-weight:600;}
.probe-msg{margin-left:auto;font-size:10px;padding:2px 7px;border-radius:5px;}
.probe-msg.ok{background:rgba(0,255,179,0.1);color:var(--ok);}
.probe-msg.warn{background:rgba(245,158,11,0.1);color:var(--warn);}
.probe-msg.danger{background:rgba(255,77,109,0.1);color:var(--danger);}

/* ── Mission status banner ── */
.mission-status{display:none;align-items:center;gap:10px;padding:10px 14px;border-radius:10px;font-size:11px;border:1px solid rgba(123,97,255,0.3);background:rgba(123,97,255,0.08);margin-bottom:4px;}
.mission-status.visible{display:flex;}
.mission-spinner{width:12px;height:12px;border:2px solid rgba(123,97,255,0.3);border-top-color:var(--accent3);border-radius:50%;animation:spin 0.8s linear infinite;flex-shrink:0;}
@keyframes spin{to{transform:rotate(360deg);}}

@media(max-width:760px){
.main{grid-template-columns:1fr;}
.sensor-grid{grid-template-columns:1fr 1fr;}
.sidebar{display:none;}
.topbar{left:0;}
.main{margin-left:0;}
}
</style>
</head>
<body>

<nav class="sidebar">
  <div class="logo">
    <svg viewBox="0 0 20 20" fill="none" stroke="white" stroke-width="1.5"><path d="M10 2L18 7V13L10 18L2 13V7L10 2Z"/><circle cx="10" cy="10" r="2.5"/></svg>
  </div>
  <div class="nav-item active">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/></svg>
  </div>
  <div class="nav-item">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="9"/><path d="M12 8v4l3 3"/></svg>
  </div>
  <div class="nav-item">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
  </div>
  <div class="nav-item" style="margin-top:auto;">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
  </div>
</nav>

<header class="topbar">
  <div class="topbar-left">
    <span class="page-title">AgriRover OS</span>
    <span class="breadcrumb">/ Dashboard / Live Monitor</span>
  </div>
  <div class="topbar-right">
    <div class="status-pill">
      <div class="dot green" id="conn-dot"></div>
      <span id="conn-text" style="font-size:11px;">Connected</span>
    </div>
    <div class="batt">
      <div class="batt-bar"><div class="batt-fill"></div></div>
      <span>72%</span>
    </div>
    <div class="time-display" id="clock">--:--:--</div>
  </div>
</header>

<main class="main">
  <div class="left-col">

    <div class="sensor-grid">
      <div class="sensor-card ok" id="temp-card">
        <div class="sensor-label">Temperature</div>
        <span class="sensor-value ok" id="temp-val">--</span><span style="font-size:13px;color:var(--muted)"> &deg;C</span>
        <div class="gauge-track"><div class="gauge-fill ok" id="temp-gauge" style="width:0%"></div></div>
        <div class="sensor-sub"><span>0&deg;</span><span>50&deg;</span></div>
      </div>
      <div class="sensor-card ok" id="hum-card">
        <div class="sensor-label">Humidity</div>
        <span class="sensor-value ok" id="hum-val">--</span><span style="font-size:13px;color:var(--muted)"> %</span>
        <div class="gauge-track"><div class="gauge-fill ok" id="hum-gauge" style="width:0%"></div></div>
        <div class="sensor-sub"><span>0%</span><span>100%</span></div>
      </div>
      <div class="sensor-card ok" id="soil-card">
        <div class="sensor-label">Soil Moisture</div>
        <span class="sensor-value ok" id="soil-val">--</span><span style="font-size:13px;color:var(--muted)"> raw</span>
        <div class="gauge-track"><div class="gauge-fill ok" id="soil-gauge" style="width:0%"></div></div>
        <div class="sensor-sub"><span>0</span><span>4095</span></div>
      </div>
    </div>

    <div class="alert-card" id="alert-card">
      <div class="alert-icon ok" id="alert-icon">&#127807;</div>
      <div>
        <div class="alert-label">Smart Decision Engine</div>
        <div class="alert-text ok" id="alert-text">Awaiting sensor data...</div>
      </div>
      <div class="alert-timestamp" id="alert-time">--</div>
    </div>

    <div class="card">
      <div class="card-title">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
        Temperature History (last 60s)
      </div>
      <div class="chart-area">
        <canvas id="tempChart" class="mini-chart"></canvas>
      </div>
    </div>

    <!-- Probe results (auto mode) -->
    <div class="card" id="probe-results-card" style="display:none">
      <div class="card-title">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22V12M12 12C12 12 8 9 8 6a4 4 0 0 1 8 0c0 3-4 6-4 6z"/></svg>
        Probe Readings — Auto Mission
      </div>
      <div class="probe-results" id="probe-results-list"></div>
    </div>

  </div>

  <div class="control-panel">

    <!-- Mode toggle -->
    <div class="mode-toggle">
      <button class="mode-btn active" id="btn-mode-manual" onclick="setMode('manual')">&#9654; Manual</button>
      <button class="mode-btn" id="btn-mode-auto" onclick="setMode('auto')">&#9654;&#9654; Auto Nav</button>
    </div>

    <!-- ═══ MANUAL PANEL ═══ -->
    <div id="manual-panel">
      <div class="card">
        <div class="card-title">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 8v4l3 3"/></svg>
          Rover Direction
        </div>
        <div class="direction-ring" id="dir-ring">
          <div class="dir-arrow">
            <svg id="dir-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="12" y1="19" x2="12" y2="5"/><polyline points="5 12 12 5 19 12"/>
            </svg>
          </div>
          <div class="dir-label" id="dir-label">IDLE</div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 3l14 9-14 9V3z"/></svg>
          Directional Control
        </div>
        <div class="dpad">
          <div></div>
          <div class="ctrl-btn" id="btn-F" onpointerdown="send('F')" onpointerup="send('S')" onpointerleave="send('S')">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="19" x2="12" y2="5"/><polyline points="5 12 12 5 19 12"/></svg>
          </div>
          <div></div>
          <div class="ctrl-btn" id="btn-L" onpointerdown="send('L')" onpointerup="send('S')" onpointerleave="send('S')">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/></svg>
          </div>
          <div class="ctrl-btn ctrl-center" id="btn-S" onclick="send('S')">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>
          </div>
          <div class="ctrl-btn" id="btn-R" onpointerdown="send('R')" onpointerup="send('S')" onpointerleave="send('S')">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/></svg>
          </div>
          <div></div>
          <div class="ctrl-btn" id="btn-B" onpointerdown="send('B')" onpointerup="send('S')" onpointerleave="send('S')">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"/><polyline points="19 12 12 19 5 12"/></svg>
          </div>
          <div></div>
        </div>
        <div class="key-hints">
          <div class="key-hint"><span class="key-badge">W</span> Forward</div>
          <div class="key-hint"><span class="key-badge">S</span> Back</div>
          <div class="key-hint"><span class="key-badge">A</span> Left</div>
          <div class="key-hint"><span class="key-badge">D</span> Right</div>
          <div class="key-hint"><span class="key-badge">SPC</span> Stop</div>
          <div class="key-hint"><span class="key-badge">C</span> Probe</div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v6M12 16v6M4.93 4.93l4.24 4.24M14.83 14.83l4.24 4.24M2 12h6M16 12h6M4.93 19.07l4.24-4.24M14.83 9.17l4.24-4.24"/></svg>
          Actuator Control
        </div>
        <button class="actuator-btn" id="probe-btn" onclick="triggerProbe()">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22V12M12 12C12 12 8 9 8 6a4 4 0 0 1 8 0c0 3-4 6-4 6z"/></svg>
          Deploy Soil Probe
          <div class="deploy-anim" id="deploy-anim"></div>
        </button>
      </div>
    </div>

    <!-- ═══ AUTO NAV PANEL ═══ -->
    <div id="auto-panel" class="mission-panel">

      <div class="card">
        <div class="card-title">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 12h18M3 6h18M3 18h18"/></svg>
          Mission Planner
        </div>

        <div class="mission-status" id="mission-status">
          <div class="mission-spinner"></div>
          <span id="mission-status-text">Running step 1 of 3…</span>
        </div>

        <div class="progress-bar-track"><div class="progress-bar-fill" id="progress-fill" style="width:0%"></div></div>

        <div class="wp-list" id="wp-list">
          <div style="text-align:center;color:var(--muted);font-size:11px;padding:14px 0;">No waypoints yet — add steps below</div>
        </div>

        <div class="wp-add-row">
          <select class="wp-select" id="wp-dir">
            <option value="F">Forward</option>
            <option value="B">Back</option>
            <option value="L">Left</option>
            <option value="R">Right</option>
            <option value="S">Stop</option>
          </select>
          <input class="wp-input" type="number" id="wp-dur" placeholder="ms" value="2000" min="100" max="30000">
          <label class="wp-checkbox-label">
            <input type="checkbox" id="wp-probe"> Probe
          </label>
          <button class="wp-add-btn" onclick="addWaypoint()">+ Add</button>
        </div>

        <div class="mission-actions">
          <button class="mission-run-btn" id="run-btn" onclick="runMission()">&#9654; Run Mission</button>
          <button class="mission-abort-btn" onclick="abortMission()">&#9632; Abort</button>
        </div>
      </div>

    </div>

    <!-- Telemetry (always visible) -->
    <div class="card">
      <div class="card-title">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M1 6s4-2 11-2 11 2 11 2"/><path d="M1 12s4-2 11-2 11 2 11 2"/><line x1="12" y1="22" x2="12" y2="17"/><path d="M7.5 17.5 12 22l4.5-4.5"/></svg>
        System Telemetry
      </div>
      <div class="telem-row">
        <span class="telem-label">ESP32 Hotspot</span>
        <span class="telem-val"><div class="dot green"></div><span id="esp-status">Online</span></span>
      </div>
      <div class="telem-row">
        <span class="telem-label">Mode</span>
        <span class="telem-val" id="telem-mode" style="color:var(--accent)">MANUAL</span>
      </div>
      <div class="telem-row">
        <span class="telem-label">Update Rate</span>
        <span class="telem-val" style="color:var(--accent2)">1 Hz</span>
      </div>
      <div class="telem-row">
        <span class="telem-label">Uptime</span>
        <span class="telem-val" id="uptime">00:00:00</span>
      </div>
      <div class="telem-row">
        <span class="telem-label">Packets</span>
        <span class="telem-val" id="pkt-count">0</span>
      </div>
    </div>

  </div>
</main>

<script>
// ── State ──────────────────────────────────────────────────────────
var tempHistory = [];
var packets = 0;
var uptimeSeconds = 0;
var failCount = 0;
var currentMode = 'manual';
var waypoints = [];  // [{dir,dur,probe}, ...]
var missionRunning = false;
var heldKey = null;

// ── Clock & uptime ────────────────────────────────────────────────
function updateClock(){
  document.getElementById('clock').textContent =
    new Date().toLocaleTimeString('en-US',{hour12:false});
}
setInterval(updateClock, 1000); updateClock();
setInterval(function(){
  uptimeSeconds++;
  var h = String(Math.floor(uptimeSeconds/3600)).padStart(2,'0');
  var m = String(Math.floor((uptimeSeconds%3600)/60)).padStart(2,'0');
  var s = String(uptimeSeconds%60).padStart(2,'0');
  document.getElementById('uptime').textContent = h+':'+m+':'+s;
}, 1000);

// ── Mode switch ───────────────────────────────────────────────────
function setMode(mode){
  currentMode = mode;
  document.getElementById('manual-panel').style.display = mode === 'manual' ? '' : 'none';
  var ap = document.getElementById('auto-panel');
  ap.style.display = mode === 'auto' ? '' : 'none';
  document.getElementById('btn-mode-manual').className = 'mode-btn' + (mode==='manual' ? ' active' : '');
  document.getElementById('btn-mode-auto').className   = 'mode-btn' + (mode==='auto'   ? ' auto-active' : '');
  document.getElementById('telem-mode').textContent    = mode === 'manual' ? 'MANUAL' : 'AUTO NAV';
  document.getElementById('telem-mode').style.color    = mode === 'manual' ? 'var(--accent)' : 'var(--accent3)';
  if(mode === 'auto') send('S');  // safety stop when switching to auto
}

// ── Manual command ────────────────────────────────────────────────
function send(cmd){
  if(currentMode !== 'manual' && cmd !== 'S') return;
  setActiveBtn(cmd);
  fetch('/cmd?go=' + cmd).catch(function(){});
}

var activeTimeout;
function setActiveBtn(cmd){
  document.querySelectorAll('.ctrl-btn').forEach(function(b){ b.classList.remove('active'); });
  var el = document.getElementById('btn-' + cmd);
  if(el) el.classList.add('active');
  updateDirection(cmd);
  clearTimeout(activeTimeout);
  if(cmd !== 'S'){
    activeTimeout = setTimeout(function(){ if(el) el.classList.remove('active'); }, 500);
  }
}

var dirMap = {
  F:{rot:0,lbl:'FORWARD'}, B:{rot:180,lbl:'REVERSE'},
  L:{rot:-90,lbl:'LEFT TURN'}, R:{rot:90,lbl:'RIGHT TURN'}, S:{rot:0,lbl:'IDLE'}
};
function updateDirection(cmd){
  var d = dirMap[cmd] || dirMap['S'];
  var arrow = document.getElementById('dir-svg');
  var ring  = document.getElementById('dir-ring');
  if(!arrow) return;
  document.getElementById('dir-label').textContent = d.lbl;
  if(cmd === 'S'){
    arrow.style.opacity = '0';
    ring.style.borderColor = 'var(--border)';
    ring.style.boxShadow = 'none';
  } else {
    arrow.style.opacity = '1';
    arrow.style.transform = 'rotate('+d.rot+'deg)';
    ring.style.borderColor = 'rgba(0,255,179,0.3)';
    ring.style.boxShadow = '0 0 18px rgba(0,255,179,0.07)';
  }
}

// ── WASD keyboard (fixed) ─────────────────────────────────────────
var keyMap = {'w':'F','s':'B','a':'L','d':'R',' ':'S','c':'C'};

document.addEventListener('keydown', function(e){
  if(currentMode !== 'manual') return;
  // Ignore if focus is inside an input/select
  var tag = document.activeElement ? document.activeElement.tagName : '';
  if(tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
  var key = e.key.toLowerCase();
  var cmd = keyMap[key];
  if(!cmd) return;
  e.preventDefault();
  if(heldKey === key) return;  // already pressed, no repeat
  heldKey = key;
  send(cmd);
});

document.addEventListener('keyup', function(e){
  var key = e.key.toLowerCase();
  if(heldKey === key){
    heldKey = null;
    // Only stop for movement keys, not probe
    if(['w','a','s','d'].includes(key)) send('S');
  }
});

// ── Manual probe ──────────────────────────────────────────────────
function triggerProbe(){
  var btn  = document.getElementById('probe-btn');
  var anim = document.getElementById('deploy-anim');
  btn.classList.add('triggered');
  anim.classList.add('active');
  fetch('/cmd?go=C').catch(function(){});
  setTimeout(function(){
    btn.classList.remove('triggered');
    anim.classList.remove('active');
  }, 3000);
}

// ── Waypoint builder ──────────────────────────────────────────────
var dirLabels = {F:'Forward',B:'Back',L:'Left',R:'Right',S:'Stop'};

function addWaypoint(){
  var dir   = document.getElementById('wp-dir').value;
  var dur   = parseInt(document.getElementById('wp-dur').value) || 2000;
  var probe = document.getElementById('wp-probe').checked;
  dur = Math.min(Math.max(dur, 100), 30000);
  waypoints.push({dir:dir, dur:dur, probe:probe});
  renderWpList();
}

function removeWaypoint(idx){
  waypoints.splice(idx, 1);
  renderWpList();
}

function renderWpList(){
  var list = document.getElementById('wp-list');
  if(waypoints.length === 0){
    list.innerHTML = '<div style="text-align:center;color:var(--muted);font-size:11px;padding:14px 0;">No waypoints yet — add steps below</div>';
    return;
  }
  list.innerHTML = waypoints.map(function(wp, i){
    return '<div class="wp-row">' +
      '<span class="wp-num">' + (i+1) + '</span>' +
      '<span class="wp-dir-badge">' + dirLabels[wp.dir] + '</span>' +
      '<span class="wp-dur">' + wp.dur + ' ms</span>' +
      (wp.probe ? '<span class="wp-probe-badge">&#8595; Probe</span>' : '') +
      '<button class="wp-del" onclick="removeWaypoint('+i+')">&#10005;</button>' +
      '</div>';
  }).join('');
}

// ── Run mission ───────────────────────────────────────────────────
function runMission(){
  if(waypoints.length === 0){ alert('Add at least one waypoint first.'); return; }
  document.getElementById('run-btn').disabled = true;
  document.getElementById('probe-results-card').style.display = 'none';
  document.getElementById('probe-results-list').innerHTML = '';
  missionRunning = true;

  var body = JSON.stringify({ waypoints: waypoints });
  fetch('/mission', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: body
  })
  .then(function(r){ return r.text(); })
  .then(function(t){
    console.log('Mission response:', t);
    pollMission();
  })
  .catch(function(e){
    alert('Could not send mission: ' + e);
    resetMissionUI();
  });
}

function abortMission(){
  fetch('/abort').catch(function(){});
  resetMissionUI();
}

function resetMissionUI(){
  missionRunning = false;
  document.getElementById('run-btn').disabled = false;
  document.getElementById('mission-status').classList.remove('visible');
  document.getElementById('progress-fill').style.width = '0%';
}

var pollTimer = null;
function pollMission(){
  if(pollTimer) clearTimeout(pollTimer);
  fetch('/status')
    .then(function(r){ return r.json(); })
    .then(function(d){
      var statusEl   = document.getElementById('mission-status');
      var statusText = document.getElementById('mission-status-text');
      var progFill   = document.getElementById('progress-fill');

      if(d.active){
        statusEl.classList.add('visible');
        var stateNames = {1:'Moving',2:'Probing',3:'Done'};
        var sn = stateNames[d.state] || 'Running';
        statusText.textContent = sn + ' — Step ' + (d.step+1) + ' of ' + d.total;
        var pct = d.total > 0 ? ((d.step / d.total) * 100) : 0;
        progFill.style.width = pct + '%';
      } else {
        // Done
        progFill.style.width = '100%';
        statusText.textContent = 'Mission complete ✓';
        resetMissionUI();
        statusEl.classList.add('visible');
        setTimeout(function(){ statusEl.classList.remove('visible'); }, 3000);
      }

      // Render probe results
      if(d.results && d.results.length > 0){
        document.getElementById('probe-results-card').style.display = '';
        var html = d.results.map(function(r){
          var cls = r.msg === 'All Systems Optimal' ? 'ok' :
                    r.msg === 'Water Plant Required' ? 'danger' : 'warn';
          return '<div class="probe-result-row">' +
            '<span class="probe-pt">P'+r.point+'</span>' +
            '<span class="probe-val">T:<span>'+r.temp+'°</span></span>' +
            '<span class="probe-val">H:<span>'+r.hum+'%</span></span>' +
            '<span class="probe-val">S:<span>'+r.soil+'</span></span>' +
            '<span class="probe-msg '+cls+'">'+r.msg+'</span>' +
            '</div>';
        }).join('');
        document.getElementById('probe-results-list').innerHTML = html;
      }

      if(d.active && missionRunning){
        pollTimer = setTimeout(pollMission, 800);
      }
    })
    .catch(function(){
      if(missionRunning) pollTimer = setTimeout(pollMission, 1500);
    });
}

// ── Sensor UI ────────────────────────────────────────────────────
function getSuggestion(t, h, s){
  if(s < 2000) return {text:'Water Plant Required', cls:'danger', icon:'&#128167;'};
  if(t > 26)   return {text:'Temperature Critical', cls:'warn',   icon:'&#127777;'};
  if(h < 40)   return {text:'Low Humidity Warning', cls:'warn',   icon:'&#128168;'};
  return {text:'All Systems Optimal', cls:'ok', icon:'&#127807;'};
}

function updateSensorCard(cardId, valId, gaugeId, value, pct, status){
  document.getElementById(cardId).className  = 'sensor-card '+status;
  document.getElementById(valId).className   = 'sensor-value '+status;
  document.getElementById(valId).textContent = value;
  document.getElementById(gaugeId).className = 'gauge-fill '+status;
  document.getElementById(gaugeId).style.width = Math.min(100,Math.max(0,pct))+'%';
}

function drawChart(){
  var canvas = document.getElementById('tempChart');
  if(!canvas || tempHistory.length < 2) return;
  var ctx = canvas.getContext('2d');
  canvas.width  = canvas.offsetWidth  * 2;
  canvas.height = canvas.offsetHeight * 2;
  ctx.scale(2, 2);
  var w = canvas.offsetWidth, h = canvas.offsetHeight;
  var d = tempHistory;
  var min = Math.min.apply(null,d)-2, max = Math.max.apply(null,d)+2;
  var toY = function(v){ return h - ((v-min)/(max-min))*(h-10) - 5; };
  var toX = function(i){ return (i/(d.length-1))*w; };
  ctx.clearRect(0,0,w,h);
  var grad = ctx.createLinearGradient(0,0,0,h);
  grad.addColorStop(0,'rgba(0,255,179,0.14)');
  grad.addColorStop(1,'rgba(0,255,179,0)');
  ctx.beginPath();
  ctx.moveTo(toX(0), toY(d[0]));
  for(var i=1;i<d.length;i++) ctx.lineTo(toX(i), toY(d[i]));
  ctx.lineTo(toX(d.length-1), h); ctx.lineTo(0, h); ctx.closePath();
  ctx.fillStyle = grad; ctx.fill();
  ctx.beginPath();
  ctx.moveTo(toX(0), toY(d[0]));
  for(var j=1;j<d.length;j++) ctx.lineTo(toX(j), toY(d[j]));
  ctx.strokeStyle='#00ffb3'; ctx.lineWidth=1.5; ctx.stroke();
  var lx = toX(d.length-1), ly = toY(d[d.length-1]);
  ctx.beginPath(); ctx.arc(lx,ly,3,0,Math.PI*2);
  ctx.fillStyle='#00ffb3'; ctx.fill();
}

// ── Data fetch ───────────────────────────────────────────────────
function fetchData(){
  fetch('/data')
    .then(function(r){ return r.json(); })
    .then(function(d){
      failCount = 0;
      document.getElementById('conn-dot').className   = 'dot green';
      document.getElementById('conn-text').textContent = 'Connected';
      document.getElementById('esp-status').textContent = 'Online';
      packets++;
      document.getElementById('pkt-count').textContent = packets;
      var t = isNaN(d.temp) ? '--' : parseFloat(d.temp).toFixed(1);
      var h = isNaN(d.hum)  ? '--' : parseFloat(d.hum).toFixed(1);
      var s = isNaN(d.soil) ? '--' : parseInt(d.soil);
      var tNum = parseFloat(t), hNum = parseFloat(h), sNum = parseInt(s);
      updateSensorCard('temp-card','temp-val','temp-gauge', t, (tNum/50)*100,
        tNum>35?'danger':tNum>28?'warn':'ok');
      updateSensorCard('hum-card','hum-val','hum-gauge', h, hNum,
        hNum<30?'danger':hNum<40?'warn':'ok');
      updateSensorCard('soil-card','soil-val','soil-gauge', s, (sNum/4095)*100,
        sNum<1500?'danger':sNum<2000?'warn':'ok');
      var sg = getSuggestion(tNum, hNum, sNum);
      document.getElementById('alert-text').textContent = sg.text;
      document.getElementById('alert-text').className   = 'alert-text '+sg.cls;
      document.getElementById('alert-icon').innerHTML   = sg.icon;
      document.getElementById('alert-icon').className   = 'alert-icon '+sg.cls;
      document.getElementById('alert-time').textContent =
        new Date().toLocaleTimeString('en-US',{hour12:false});
      if(!isNaN(tNum)){
        tempHistory.push(tNum);
        if(tempHistory.length > 60) tempHistory.shift();
        drawChart();
      }
    })
    .catch(function(){
      failCount++;
      if(failCount > 2){
        document.getElementById('conn-dot').className    = 'dot red';
        document.getElementById('conn-text').textContent  = 'Disconnected';
        document.getElementById('esp-status').textContent = 'Offline';
      }
    });
}

setInterval(fetchData, 1000);
window.addEventListener('resize', drawChart);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ─────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, HIGH);
  pinMode(PWMA, OUTPUT); pinMode(PWMB, OUTPUT);
  digitalWrite(PWMA, HIGH); digitalWrite(PWMB, HIGH);

  myServo.attach(SERVO_PIN);
  myServo.write(0);

  dht.begin();

  WiFi.softAP(ssid, password);
  Serial.println("Hotspot Ready!");
  Serial.println(WiFi.softAPIP());

  server.on("/",        handleRoot);
  server.on("/cmd",     handleCommand);
  server.on("/data",    handleData);
  server.on("/mission", handleMission);
  server.on("/abort",   handleAbort);
  server.on("/status",  handleStatus);
  server.begin();
}

// ─────────────────────────────────────────
//  Loop — non-blocking auto-nav FSM
// ─────────────────────────────────────────
void loop() {
  server.handleClient();

  if (!missionActive) return;

  unsigned long now = millis();

  // ── State 1: RUNNING a move step ──────────────────────────────
  if (autoState == 1) {
    if (wpIndex >= wpCount) {
      // All steps done
      stopBot();
      missionActive = false;
      autoState = 0;
      return;
    }
    // Check if this step's time has elapsed
    if (now - stepStart >= waypoints[wpIndex].dur) {
      stopBot();
      if (waypoints[wpIndex].probe) {
        beginProbe();   // → goes to state 2
      } else {
        // Advance to next waypoint
        wpIndex++;
        if (wpIndex < wpCount) {
          driveDir(waypoints[wpIndex].dir);
          stepStart = millis();
        } else {
          missionActive = false;
          autoState = 3;   // DONE
        }
      }
    }
  }

  // ── State 2: PROBING ──────────────────────────────────────────
  else if (autoState == 2) {
    if (now - probeStart >= 500 && myServo.read() == 90) {
      // Servo has been down 500 ms — read sensor
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      int   s = analogRead(SOIL_PIN);
      if (probeResultCount < MAX_RESULTS) {
        probeResults[probeResultCount].temp = t;
        probeResults[probeResultCount].hum  = h;
        probeResults[probeResultCount].soil = s;
        probeResults[probeResultCount].suggestion = getSuggestion(t, h, s);
        probeResultCount++;
      }
      Serial.printf("Probe %d: T=%.1f H=%.1f S=%d\n", probeResultCount, t, h, s);
    }
    if (now - probeStart >= 1500) {
      // Retract servo, advance waypoint
      myServo.write(0);
      wpIndex++;
      if (wpIndex < wpCount) {
        autoState = 1;
        driveDir(waypoints[wpIndex].dir);
        stepStart = millis();
      } else {
        autoState = 3;
        missionActive = false;
      }
    }
  }

  // ── State 3: DONE ─────────────────────────────────────────────
  else if (autoState == 3) {
    stopBot();
    missionActive = false;
    autoState = 0;
  }
}
