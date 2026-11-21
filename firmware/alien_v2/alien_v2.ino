// Alien display - v2 (cloud)
// One firmware for every display. Each one registers itself with Supabase,
// shows a claim code until you add it in the app, then picks up messages,
// settings and firmware updates by checking in every 15 seconds.
//
// Board: ESP32C3 Dev Module, USB CDC On Boot: Enabled,
//        Partition Scheme: "Default 4MB with spiffs"
// Libraries: MD_Parola (+ MD_MAX72XX), WiFiManager (tzapu), ArduinoJson (Benoit Blanchon, v7)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
enum Mode { NORMAL, ANGRY, HEARTS };

// ---------- small built-in SHA-256 (verifies downloaded firmware) ----------
struct Sha256 {
  uint32_t h[8]; uint8_t buf[64]; uint32_t len = 0; uint64_t bits = 0;
  static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void begin() {
    const uint32_t init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(h, init, sizeof(h)); len = 0; bits = 0;
  }
  void block(const uint8_t *p) {
    static const uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
      uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (ror(e,6) ^ ror(e,11) ^ ror(e,25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }
  void update(const uint8_t *p, size_t n) {
    bits += (uint64_t)n * 8;
    while (n--) { buf[len++] = *p++; if (len == 64) { block(buf); len = 0; } }
  }
  String hex() {
    uint64_t total = bits;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t zero = 0; while (len != 56) update(&zero, 1);
    uint8_t lenb[8]; for (int i = 0; i < 8; i++) lenb[i] = total >> (56 - 8 * i);
    update(lenb, 8);
    char out[65];
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", (unsigned)h[i]);
    return String(out);
  }
};

// ---------- build settings ----------
#define FW_VERSION "2.1.0"

const char *SUPABASE_URL = "https://vvnrjlsqtfieftvjenhf.supabase.co";
const char *SUPABASE_KEY = "sb_publishable_bkutITeoaX9-TnZGXf9Uuw_f1Xgtu5C";  // public by design
// Wi-Fi upload password for the Arduino IDE. Kept out of git: copy
// secrets.example.h to secrets.h (ignored) and set your own. CI writes it from a repo secret.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef OTA_PASSWORD_VALUE
  #define OTA_PASSWORD_VALUE "changeme"
#endif
const char *OTA_PASSWORD = OTA_PASSWORD_VALUE;

#define DIN_PIN  6
#define CLK_PIN  4
#define CS_PIN   7
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4
#define FLIP_X false
#define FLIP_Y false

const unsigned long POLL_MS = 15000;
const float EYE_A = 7.8, EYE_B = 3.3, EYE_TILT = 18.0;
const float LEFT_CX = 7.5, RIGHT_CX = 23.5, EYE_CY = 3.5;
const uint8_t SCROLL_SPEED = 45;

// ---------- globals ----------
MD_Parola display(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx;
WebServer server(80);
WiFiManager wm;
Preferences prefs;
WiFiClientSecure tls;

String chipId, token, hostName, claimCode;
bool online = false, claimed = false;
uint8_t brightness = 3;
unsigned long nextPoll = 0;

struct Msg { String text; String mood; uint8_t repeats; };
const int QMAX = 6;
Msg queue[QMAX];
int qHead = 0, qCount = 0;
String scrollBuf;

// ---------- display cycle (all adjustable per display in the app) ----------
uint16_t eyesSeconds  = 45;      // eyes run this long between clock/replay
bool     clockOn      = true;
bool     clock24      = false;
uint8_t  replayHours  = 6;       // 0 = don't replay old messages
String   tzPosix      = "CST6CDT,M3.2.0,M11.1.0";   // US Central
unsigned long cycleStart = 0;

// recent messages, replayed during the cycle so nobody misses one
struct Recent { String text; time_t at; };
const int RMAX = 5;
Recent recent[RMAX];
int rCount = 0;

void rememberMessage(const String &text) {
  if (!text.length()) return;
  time_t now = time(nullptr);
  if (now < 1700000000) return;                // clock not set yet
  if (rCount == RMAX) { for (int i = 1; i < RMAX; i++) recent[i - 1] = recent[i]; rCount--; }
  recent[rCount++] = { text, now };
}

bool enqueue(const String &text, const String &mood, int reps) {
  if (qCount >= QMAX) return false;
  queue[(qHead + qCount) % QMAX] = { text, mood, (uint8_t)constrain(reps, 1, 5) };
  qCount++;
  return true;
}

// ---------- background work during every pause ----------
void serviceNet() {
  if (!online) return;
  server.handleClient();
  ArduinoOTA.handle();
}
void wait(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) { serviceNet(); delay(2); }
}

// ======================================================================
// Eyes (same as v1)
// ======================================================================
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
void drawEyes(float open, float pdx = 0, float pdy = 0, Mode mode = NORMAL) { drawEyes2(open, open, pdx, pdy, mode); }

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
void lookAround() { lookTo(-3, 0); wait(600); lookTo(3, 0, 60); wait(600); lookTo(0, -1); wait(400); lookTo(0, 0); }
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

// ======================================================================
// Text
// ======================================================================
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

void showStatic(const char *txt) {
  display.displayClear();
  display.displayText(txt, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();
}

void showMessage(const Msg &m) {
  Serial.printf("Message: \"%s\" mood=%s x%d\n", m.text.c_str(), m.mood.c_str(), m.repeats);
  blink();
  playMood(m.mood);
  if (m.text.length()) scrollText(m.text, m.repeats);
  rememberMessage(m.text);
  pupilX = pupilY = 0;
  wakeUp();
  playMood(m.mood);
}

// ---------- clock and replay ----------
bool clockValid() { return time(nullptr) > 1700000000; }

void showClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  static const char *DAYS[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char *MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char buf[40];
  if (clock24) snprintf(buf, sizeof(buf), "%d:%02d   %s %s %d", t.tm_hour, t.tm_min, DAYS[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday);
  else {
    int h = t.tm_hour % 12; if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s   %s %s %d", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM",
             DAYS[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday);
  }
  scrollText(buf, 1);
}

void replayRecent() {
  if (!replayHours || !clockValid()) return;
  time_t cutoff = time(nullptr) - (time_t)replayHours * 3600;
  for (int i = 0; i < rCount; i++) {
    if (recent[i].at >= cutoff) scrollText(recent[i].text, 1);
    if (qCount) return;                        // a new message arrived: show that instead
  }
}

void runCycle() {
  bool showed = false;
  if (clockOn && clockValid()) { showClock(); showed = true; }
  if (!qCount && replayHours) {
    time_t cutoff = time(nullptr) - (time_t)replayHours * 3600;
    for (int i = 0; i < rCount; i++) if (recent[i].at >= cutoff) { showed = true; break; }
    replayRecent();
  }
  if (showed) { pupilX = pupilY = 0; wakeUp(); }
}

String clean(const String &s) {
  String out;
  for (unsigned int i = 0; i < s.length() && out.length() < 140; i++) {
    char ch = s[i];
    if (ch >= 32 && ch <= 126) out += ch;
    else if (ch == '\n') out += ' ';
  }
  out.trim();
  return out;
}

// ======================================================================
// Cloud
// ======================================================================
// Calls a database function. Returns the HTTP status; body goes into `out`.
int rpc(const char *fn, const String &jsonBody, String &out) {
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(8000);
  if (!http.begin(tls, String(SUPABASE_URL) + "/rest/v1/rpc/" + fn)) return -1;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonBody);
  out = http.getString();
  http.end();
  return code;
}

bool registerDevice() {
  JsonDocument req; req["p_chip_id"] = chipId;
  String body, resp; serializeJson(req, body);
  int code = rpc("device_register", body, resp);
  Serial.printf("register -> %d %s\n", code, resp.c_str());
  if (code != 200) {
    if (resp.indexOf("already claimed") >= 0) scrollText("Remove me in the app, then restart", 2);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  token = doc["token"].as<String>();
  claimCode = doc["claim_code"].as<String>();
  prefs.putString("token", token);
  return true;
}

// Downloads firmware, checks its SHA-256 while writing, installs and restarts.
void cloudUpdate(const String &version, const String &path, const String &sha256hex, int size) {
  Serial.printf("Firmware %s available (%d bytes)\n", version.c_str(), size);
  showStatic("UPDATE");
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(tls, String(SUPABASE_URL) + "/storage/v1/object/public/firmware/" + path);
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0 || !Update.begin(len)) {
    Serial.printf("Update download failed: %d\n", code);
    http.end(); display.displayClear(); return;
  }

  Sha256 sha;
  sha.begin();
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  unsigned long lastData = millis();
  while (remaining > 0 && millis() - lastData < 15000) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min((int)sizeof(buf), min(avail, remaining)));
      sha.update(buf, n);
      Update.write(buf, n);
      remaining -= n;
      lastData = millis();
    } else delay(5);
  }
  http.end();

  String hex = sha.hex();

  if (remaining != 0 || !sha256hex.equalsIgnoreCase(hex)) {
    Serial.printf("Update rejected: %s (got %s)\n", remaining ? "incomplete" : "checksum mismatch", hex.c_str());
    Update.abort();
    display.displayClear();
    return;
  }
  if (Update.end(true)) {
    Serial.println("Update installed, restarting");
    showStatic("OK!");
    delay(800);
    ESP.restart();
  }
  display.displayClear();
}

String slugify(const String &name) {
  String s;
  for (char ch : name) {
    if (isalnum(ch)) s += (char)tolower(ch);
    else if ((ch == ' ' || ch == '-' || ch == '_') && s.length() && !s.endsWith("-")) s += '-';
  }
  while (s.endsWith("-")) s.remove(s.length() - 1);
  return s.length() ? s.substring(0, 30) : String("alien");
}

void poll() {
  if (!online) return;
  if (!token.length() && !registerDevice()) return;

  JsonDocument req; req["p_token"] = token; req["p_fw_version"] = FW_VERSION;
  String body, resp; serializeJson(req, body);
  int code = rpc("device_poll", body, resp);
  if (code != 200) {
    Serial.printf("poll -> %d %s\n", code, resp.c_str());
    if (resp.indexOf("unknown device") >= 0) {     // removed in the app: start over
      token = ""; prefs.remove("token"); claimed = false;
    }
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;

  claimed = doc["claimed"] | false;
  if (!claimed) { claimCode = doc["claim_code"].as<String>(); return; }

  // settings
  if (doc["settings"]["brightness"].is<int>()) {
    uint8_t b = constrain(doc["settings"]["brightness"].as<int>(), 0, 15);
    if (b != brightness) { brightness = b; mx->control(MD_MAX72XX::INTENSITY, brightness); }
  }
  JsonObject st = doc["settings"];
  eyesSeconds = constrain((int)(st["eyes_seconds"] | 45), 10, 600);
  clockOn     = st["clock"] | true;
  clock24     = st["clock24"] | false;
  replayHours = constrain((int)(st["replay_hours"] | 6), 0, 24);
  String tz   = st["tz"] | "CST6CDT,M3.2.0,M11.1.0";
  if (tz != tzPosix) {
    tzPosix = tz;
    configTzTime(tzPosix.c_str(), "pool.ntp.org", "time.nist.gov");
  }

  // local network name follows the display name (takes effect after a restart)
  String slug = slugify(doc["name"] | "alien");
  if (slug != hostName) prefs.putString("host", slug);

  for (JsonObject m : doc["messages"].as<JsonArray>()) {
    enqueue(clean(m["text"] | ""), m["mood"] | "", m["repeats"] | 3);
  }

  if (!doc["firmware"].isNull()) {
    cloudUpdate(doc["firmware"]["version"] | "", doc["firmware"]["path"] | "",
                doc["firmware"]["sha256"] | "", doc["firmware"]["size"] | 0);
  }
}

// ======================================================================
// Local web page (works on your home Wi-Fi even if the internet is down)
// ======================================================================
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Alien</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:420px;margin:auto;padding:16px}
h1{font-size:22px;color:#f55}textarea,select,button{width:100%;box-sizing:border-box;font-size:17px;border-radius:10px;border:1px solid #444;background:#222;color:#eee;padding:12px;margin:6px 0}
button{background:#c22;border:none;font-weight:600}.ghost{background:#333}small{color:#888}#st{color:#9c9}
</style></head><body><h1>&#128125; <span id="n">Alien</span></h1>
<textarea id="t" rows="3" maxlength="140" placeholder="Type a message..."></textarea>
<select id="m"><option value="">Reaction: none</option><option value="hearts">Hearts</option><option value="wink">Wink</option>
<option value="surprise">Surprise</option><option value="look">Look around</option><option value="squint">Squint</option><option value="angry">Angry</option></select>
<button onclick="send()">Send</button><div id="st"></div>
<p><small id="info"></small></p>
<button class="ghost" onclick="if(confirm('Forget Wi-Fi and restart in setup mode?'))fetch('/resetwifi',{method:'POST'})">Change Wi-Fi network</button>
<script>
fetch('/info').then(r=>r.json()).then(i=>{n.textContent=i.name;info.textContent='Firmware '+i.fw+(i.claimed?'':' - claim code '+i.code)});
function send(){const f=new FormData();f.append('text',t.value);f.append('mood',m.value);
fetch('/msg',{method:'POST',body:f}).then(r=>r.text()).then(x=>{st.textContent=x;t.value=''})}
</script></body></html>)HTML";

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/info", HTTP_GET, [] {
    JsonDocument d;
    d["name"] = hostName; d["fw"] = FW_VERSION; d["claimed"] = claimed; d["code"] = claimCode; d["chip"] = chipId;
    String s; serializeJson(d, s);
    server.send(200, "application/json", s);
  });
  server.on("/msg", HTTP_POST, [] {
    String text = clean(server.arg("text")), mood = server.arg("mood");
    if (!text.length() && !mood.length()) { server.send(400, "text/plain", "Nothing to send"); return; }
    server.send(enqueue(text, mood, 2) ? 200 : 503, "text/plain", "Sent!");
  });
  server.on("/resetwifi", HTTP_POST, [] {
    server.send(200, "text/plain", "Restarting in setup mode");
    delay(500); wm.resetSettings(); ESP.restart();
  });
  server.begin();
}

// ======================================================================
// Startup and main loop
// ======================================================================
void setup() {
  Serial.begin(115200);
  display.begin();
  mx = display.getGraphicObject();
  display.setIntensity(brightness);
  randomSeed(esp_random());

  char id[13];
  snprintf(id, sizeof(id), "%012llX", ESP.getEfuseMac());
  chipId = id;

  prefs.begin("alien", false);
  token    = prefs.getString("token", "");
  hostName = prefs.getString("host", "alien-" + chipId.substring(8));
  Serial.printf("Alien v%s  chip %s  host %s\n", FW_VERSION, chipId.c_str(), hostName.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  showStatic("WiFi?");
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager *) { showStatic("SETUP"); });
  online = wm.autoConnect(("Alien-Setup-" + chipId.substring(8)).c_str());

  if (online) {
    tls.setInsecure();   // alpha: encrypted but not certificate-checked; firmware is still SHA-256 verified
    MDNS.begin(hostName.c_str());
    MDNS.addService("http", "tcp", 80);
    ArduinoOTA.setHostname(hostName.c_str());
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([] { showStatic("UPDATE"); });
    ArduinoOTA.begin();
    setupWeb();
    configTzTime(tzPosix.c_str(), "pool.ntp.org", "time.nist.gov");   // internet time
    poll();
  }
  cycleStart = millis();
  display.displayClear();
  wakeUp();
}

unsigned long nextAction = 0;

void loop() {
  serviceNet();

  if (online && millis() >= nextPoll) {
    poll();
    nextPoll = millis() + POLL_MS;
  }

  if (qCount > 0) {
    Msg m = queue[qHead];
    qHead = (qHead + 1) % QMAX;
    qCount--;
    showMessage(m);
    nextAction = millis() + 2000;
    cycleStart = millis();
    return;
  }

  if (millis() < nextAction) return;

  // every eyesSeconds: clock, then recent messages, then back to the eyes
  if (claimed && millis() - cycleStart >= (unsigned long)eyesSeconds * 1000) {
    runCycle();
    cycleStart = millis();
    nextAction = millis() + 1500;
    return;
  }

  // not yet added in the app: show the claim code now and then
  if (online && !claimed && claimCode.length()) {
    scrollText("Code " + claimCode, 1);
    wakeUp();
    nextAction = millis() + 4000;
    return;
  }

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
