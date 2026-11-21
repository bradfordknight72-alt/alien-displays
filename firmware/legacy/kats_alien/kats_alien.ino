// Kat's Alien - v1: animated eyes + Wi-Fi messages + over-the-air updates
// Board: ESP32-C3 SuperMini + MAX7219 FC-16 4-in-1 (32 x 8 dots)
// Arduino IDE: Board "ESP32C3 Dev Module", USB CDC On Boot: Enabled,
//              Partition Scheme: "Default 4MB with spiffs" (needed for OTA)
// Libraries (Library Manager): MD_Parola (+ MD_MAX72XX), WiFiManager by tzapu
//
// First boot: join the Wi-Fi network "Kats-Alien-Setup" on your phone,
// pick your home Wi-Fi, enter its password. After that, open
// http://kats-alien.local (or the IP it scrolls at startup) to send messages.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <Preferences.h>

// Declared up here so the IDE's auto-generated prototypes can see it
enum Mode { NORMAL, ANGRY, HEARTS };

// ---------- settings ----------
#define DIN_PIN  6
#define CLK_PIN  4
#define CS_PIN   7
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4
#define FLIP_X false
#define FLIP_Y false

const char *HOSTNAME     = "kats-alien";
const char *SETUP_AP     = "Kats-Alien-Setup";
const char *OTA_PASSWORD = "changeme";      // asked for when uploading over Wi-Fi

const float EYE_A = 7.8, EYE_B = 3.3, EYE_TILT = 18.0;
const float LEFT_CX = 7.5, RIGHT_CX = 23.5, EYE_CY = 3.5;
uint8_t brightness = 3;                      // 0-15, adjustable from the web page
const uint8_t SCROLL_SPEED = 45;             // ms per step, lower = faster

MD_Parola display(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx;
WebServer server(80);
WiFiManager wm;
Preferences prefs;      // survives power-off: holds the fixed-IP setting

// ---------- message queue ----------
struct Msg { String text; String mood; uint8_t repeats; };
const int QMAX = 5;
Msg queue[QMAX];
int qHead = 0, qCount = 0;
String scrollBuf;

bool enqueue(const String &text, const String &mood, int reps) {
  if (qCount >= QMAX) return false;
  queue[(qHead + qCount) % QMAX] = { text, mood, (uint8_t)constrain(reps, 1, 5) };
  qCount++;
  return true;
}

// ---------- background work, called during every pause ----------
void serviceNet() {
  server.handleClient();
  ArduinoOTA.handle();
}

void wait(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) { serviceNet(); delay(2); }
}

// ---------- eye drawing ----------
void px(int x, int y, bool on) {
  if (x < 0 || x > 31 || y < 0 || y > 7) return;
  mx->setPoint(FLIP_Y ? 7 - y : y, FLIP_X ? x : 31 - x, on);
}

const char *HEART[6] = { ".##.##.", "#######", "#######", ".#####.", "..###..", "...#..." };

void drawEye(bool rightEye, float open, float pdx, float pdy, Mode mode) {
  float cx = rightEye ? RIGHT_CX : LEFT_CX;
  int x0 = rightEye ? 16 : 0;
  float t = EYE_TILT * PI / 180.0, c = cos(t), s = sin(t);
  float b = EYE_B * open;

  for (int x = x0; x < x0 + 16; x++) {
    for (int y = 0; y < 8; y++) {
      bool on = false;
      if (mode == HEARTS) {
        int hx = x - (int)(cx - 3), hy = y - 1;
        on = (hx >= 0 && hx < 7 && hy >= 0 && hy < 6 && HEART[hy][hx] == '#');
      } else if (b > 0.35) {
        float dx = (x - cx) * (rightEye ? -1 : 1), dy = y - EYE_CY;
        float u = dx * c + dy * s, v = -dx * s + dy * c;
        on = (u * u) / (EYE_A * EYE_A) + (v * v) / (b * b) <= 1.0;
        if (on && open > 0.6) {
          float qx = x - (cx + pdx), qy = y - (EYE_CY + pdy);
          float r = (open > 1.1) ? 0.6 : 1.1;
          if (fabs(qx) <= r && fabs(qy) <= r) on = false;
        }
        if (on && mode == ANGRY) {
          float inner = rightEye ? (x0 + 15 - x) : (x - x0);
          if (y < 0.5 + inner * 0.22) on = false;
        }
      } else {
        float dx = (x - cx) * (rightEye ? -1 : 1);
        int yl = (int)round(EYE_CY + dx * tan(t) * 0.6);
        on = (y == yl && fabs(dx) < EYE_A - 1);
      }
      px(x, y, on);
    }
  }
}

void drawEyes2(float openL, float openR, float pdx = 0, float pdy = 0, Mode mode = NORMAL) {
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  drawEye(false, openL, pdx, pdy, mode);
  drawEye(true,  openR, pdx, pdy, mode);
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}
void drawEyes(float open, float pdx = 0, float pdy = 0, Mode mode = NORMAL) {
  drawEyes2(open, open, pdx, pdy, mode);
}

// ---------- expressions ----------
float pupilX = 0, pupilY = 0;

void blink() {
  const float steps[] = {0.7, 0.35, 0.0, 0.0, 0.35, 0.7, 1.0};
  for (float o : steps) { drawEyes(o, pupilX, pupilY); wait(28); }
}

void wink() {
  const float steps[] = {0.7, 0.35, 0.0, 0.0, 0.0, 0.0, 0.35, 0.7, 1.0};
  for (float o : steps) { drawEyes2(1.0, o, pupilX, pupilY); wait(45); }
}

void lookTo(float tx, float ty, int stepMs = 45) {
  while (pupilX != tx || pupilY != ty) {
    if (pupilX < tx) pupilX++; else if (pupilX > tx) pupilX--;
    if (pupilY < ty) pupilY++; else if (pupilY > ty) pupilY--;
    drawEyes(1.0, pupilX, pupilY);
    wait(stepMs);
  }
}

void glance() {
  lookTo(random(2) ? 3 : -3, random(-1, 2));
  wait(random(500, 1400));
  if (random(3) == 0) blink();
  lookTo(0, 0);
}

void lookAround() {
  lookTo(-3, 0); wait(600);
  lookTo(3, 0, 60); wait(600);
  lookTo(0, -1); wait(400);
  lookTo(0, 0);
}

void squint() {
  for (float o = 1.0; o >= 0.55; o -= 0.15) { drawEyes(o, pupilX, pupilY); wait(50); }
  wait(random(1200, 2200));
  for (float o = 0.55; o <= 1.0; o += 0.15) { drawEyes(o, pupilX, pupilY); wait(50); }
  drawEyes(1.0, pupilX, pupilY);
}

void surprise() { drawEyes(1.25, 0, 0); wait(1200); blink(); blink(); }

void angry() { drawEyes(1.0, 0, 1, ANGRY); wait(1800); drawEyes(1.0, pupilX, pupilY); }

void hearts() {
  drawEyes(1.0, 0, 0, HEARTS);
  for (int i = 0; i < 4; i++) {
    for (int b = brightness; b <= brightness + 5; b++) { mx->control(MD_MAX72XX::INTENSITY, min(b, 15)); wait(40); }
    for (int b = brightness + 5; b >= brightness; b--) { mx->control(MD_MAX72XX::INTENSITY, min(b, 15)); wait(40); }
  }
  mx->control(MD_MAX72XX::INTENSITY, brightness);
  blink();
}

void wakeUp() { for (float o = 0; o <= 1.0; o += 0.1) { drawEyes(o); wait(60); } blink(); }

void playMood(const String &m) {
  if      (m == "hearts")   hearts();
  else if (m == "surprise") surprise();
  else if (m == "angry")    angry();
  else if (m == "squint")   squint();
  else if (m == "look")     lookAround();
  else if (m == "wink")     wink();
}

// ---------- scrolling text ----------
void scrollText(const String &text, uint8_t repeats) {
  scrollBuf = text;
  display.displayClear();
  for (uint8_t i = 0; i < repeats; i++) {
    display.displayText(scrollBuf.c_str(), PA_LEFT, SCROLL_SPEED, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    while (!display.displayAnimate()) serviceNet();
    wait(300);
  }
  display.displayClear();
}

void showMessage(const Msg &m) {
  Serial.printf("Message: \"%s\" mood=%s x%d\n", m.text.c_str(), m.mood.c_str(), m.repeats);
  blink();
  playMood(m.mood);
  if (m.text.length()) scrollText(m.text, m.repeats);
  pupilX = pupilY = 0;
  wakeUp();
  playMood(m.mood);
}

// ---------- web page ----------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kat's Alien</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:420px;margin:auto;padding:16px}
h1{font-size:22px;color:#f55;margin:8px 0 16px}
textarea,select,button,input{width:100%;box-sizing:border-box;font-size:17px;border-radius:10px;border:1px solid #444;background:#222;color:#eee;padding:12px;margin:6px 0}
button{background:#c22;border:none;font-weight:600}
.row{display:flex;gap:8px}.row>*{flex:1}
#st{min-height:1.4em;color:#9c9;margin-top:6px}
small{color:#888}.ghost{background:#333}
</style></head><body>
<h1>&#128125; Kat's Alien</h1>
<textarea id="t" rows="3" maxlength="140" placeholder="Type a message..."></textarea>
<div class="row">
<select id="m"><option value="">Reaction: none</option><option value="hearts">Hearts</option>
<option value="wink">Wink</option><option value="surprise">Surprise</option>
<option value="look">Look around</option><option value="squint">Squint</option>
<option value="angry">Angry</option></select>
<select id="r"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option></select>
</div>
<button onclick="send()">Send</button>
<div id="st"></div>
<p><small>Brightness</small><input type="range" min="0" max="15" id="b" onchange="fetch('/bright?v='+this.value)"></p>
<p><small>Letters, numbers and symbols only (no emoji). &lt;3 works.</small></p>
<p><small>Address: <span id="cur">...</span></small></p>
<div class="row"><input id="ip" placeholder="Fixed IP, e.g. 192.168.1.200"><button class="ghost" onclick="setIp()">Fix IP</button></div>
<button class="ghost" onclick="if(confirm('Go back to an automatic address?'))fetch('/netcfg',{method:'POST',body:new URLSearchParams({ip:''})}).then(()=>st.textContent='Restarting...')">Use automatic address</button>
<button class="ghost" onclick="if(confirm('Forget Wi-Fi and restart in setup mode?'))fetch('/resetwifi',{method:'POST'})">Change Wi-Fi network</button>
<script>
fetch('/bright').then(r=>r.text()).then(v=>b.value=v);
fetch('/net').then(r=>r.json()).then(n=>{cur.textContent=n.ip+(n.fixed?' (fixed)':' (automatic)');ip.value=n.fixed?n.ip:n.suggest});
function setIp(){if(!confirm('Restart at '+ip.value+'? Update your shortcut to http://'+ip.value))return;
fetch('/netcfg',{method:'POST',body:new URLSearchParams({ip:ip.value})}).then(r=>r.text()).then(x=>st.textContent=x)}
function send(){const f=new FormData();f.append('text',t.value);f.append('mood',m.value);f.append('rep',r.value);
st.textContent='Sending...';fetch('/msg',{method:'POST',body:f}).then(r=>r.text()).then(x=>{st.textContent=x;if(x.startsWith('Sent'))t.value=''}).catch(()=>st.textContent='Could not reach the alien')}
</script></body></html>)HTML";

String clean(String s) {
  String out;
  for (unsigned int i = 0; i < s.length() && out.length() < 140; i++) {
    char ch = s[i];
    if (ch >= 32 && ch <= 126) out += ch;
    else if (ch == '\n') out += ' ';
  }
  out.trim();
  return out;
}

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/msg", HTTP_POST, [] {
    String text = clean(server.arg("text"));
    String mood = server.arg("mood");
    if (!text.length() && !mood.length()) { server.send(400, "text/plain", "Nothing to send"); return; }
    if (enqueue(text, mood, server.arg("rep").toInt()))
      server.send(200, "text/plain", qCount > 1 ? "Sent (queued behind " + String(qCount - 1) + ")" : "Sent!");
    else server.send(503, "text/plain", "Queue full, try again shortly");
  });
  server.on("/bright", HTTP_GET, [] {
    if (server.hasArg("v")) {
      brightness = constrain(server.arg("v").toInt(), 0, 15);
      mx->control(MD_MAX72XX::INTENSITY, brightness);
    }
    server.send(200, "text/plain", String(brightness));
  });
  server.on("/net", HTTP_GET, [] {
    IPAddress me = WiFi.localIP();
    String suggest = String(me[0]) + "." + me[1] + "." + me[2] + ".200";
    bool fixed = prefs.getString("ip", "").length() > 0;
    server.send(200, "application/json",
      "{\"ip\":\"" + me.toString() + "\",\"fixed\":" + (fixed ? "true" : "false") +
      ",\"suggest\":\"" + suggest + "\"}");
  });
  server.on("/netcfg", HTTP_POST, [] {
    String ip = server.arg("ip");
    ip.trim();
    if (ip.length()) {
      IPAddress test;
      if (!test.fromString(ip)) { server.send(400, "text/plain", "That isn't a valid IP address"); return; }
      // remember the current network details so the fixed address uses the same router
      prefs.putString("ip", ip);
      prefs.putString("gw", WiFi.gatewayIP().toString());
      prefs.putString("sn", WiFi.subnetMask().toString());
      server.send(200, "text/plain", "Saved. Restarting at http://" + ip);
    } else {
      prefs.remove("ip"); prefs.remove("gw"); prefs.remove("sn");
      server.send(200, "text/plain", "Automatic address restored. Restarting...");
    }
    delay(800);
    ESP.restart();
  });
  server.on("/resetwifi", HTTP_POST, [] {
    server.send(200, "text/plain", "Restarting in setup mode");
    delay(500);
    wm.resetSettings();
    ESP.restart();
  });
  server.begin();
}

// ---------- startup ----------
void showStatic(const char *txt) {
  display.displayClear();
  display.displayText(txt, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();
}

void setup() {
  Serial.begin(115200);
  display.begin();
  mx = display.getGraphicObject();
  display.setIntensity(brightness);
  randomSeed(esp_random());

  // Wi-Fi: reconnects automatically; first time (or after a reset) opens the setup network
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // helps many C3 SuperMini boards connect reliably
  showStatic("WiFi?");
  // fixed IP, if one was set from the web page
  prefs.begin("alien", false);
  String fip = prefs.getString("ip", "");
  if (fip.length()) {
    IPAddress ip, gw, sn;
    ip.fromString(fip);
    gw.fromString(prefs.getString("gw", "192.168.1.1"));
    sn.fromString(prefs.getString("sn", "255.255.255.0"));
    wm.setSTAStaticIPConfig(ip, gw, sn, gw);   // router doubles as DNS
    Serial.println("Using fixed IP " + fip);
  }

  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager *) { showStatic("SETUP"); });
  bool online = wm.autoConnect(SETUP_AP);

  // safety net: if a fixed IP stops working, forget it and come back automatic
  if (!online && fip.length()) {
    prefs.remove("ip"); prefs.remove("gw"); prefs.remove("sn");
    Serial.println("Fixed IP failed; reverting to automatic and restarting");
    ESP.restart();
  }

  if (online) {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([] { showStatic("UPDATE"); });
    ArduinoOTA.begin();
    setupWeb();
    String ip = WiFi.localIP().toString();
    Serial.println("Online: http://" + String(HOSTNAME) + ".local  (" + ip + ")");
    scrollText(ip, 1);
  } else {
    Serial.println("No Wi-Fi; running eyes only");
  }

  wakeUp();
}

// ---------- main loop ----------
unsigned long nextAction = 0;

void loop() {
  serviceNet();

  if (qCount > 0) {
    Msg m = queue[qHead];
    qHead = (qHead + 1) % QMAX;
    qCount--;
    showMessage(m);
    nextAction = millis() + 2000;
    return;
  }

  if (millis() < nextAction) return;

  int r = random(100);
  if      (r < 45) blink();
  else if (r < 68) glance();
  else if (r < 76) { blink(); wait(150); blink(); }
  else if (r < 83) lookAround();
  else if (r < 89) squint();
  else if (r < 93) surprise();
  else if (r < 96) hearts();
  else if (r < 98) wink();
  else             angry();

  nextAction = millis() + random(1500, 5000);
}
