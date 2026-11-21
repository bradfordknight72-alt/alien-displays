// Grey Alien display - animated eyes
// Board: ESP32-C3 SuperMini + MAX7219 FC-16 4-in-1 (32 x 8 dots)
// Arduino IDE: Board "ESP32C3 Dev Module", USB CDC On Boot: Enabled
// Library: MD_MAX72XX (installed along with MD_Parola)
//
// Eyes are drawn as tilted ellipses (outer corners up, like a grey alien),
// so their size and tilt are just numbers below - tweak and re-upload.

#include <MD_MAX72xx.h>

// ---- Same pins as the test sketch ----
#define DIN_PIN  6
#define CLK_PIN  4
#define CS_PIN   7

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4

// ---- If the picture comes out mirrored or upside down, flip these ----
#define FLIP_X false
#define FLIP_Y false

// ---- Eye shape ----
const float EYE_A    = 7.8;    // half-length of the eye, in dots
const float EYE_B    = 3.3;    // half-height of the eye, in dots
const float EYE_TILT = 18.0;   // degrees; outer corners up
const float LEFT_CX  = 7.5;    // eye centres across the 32-dot width
const float RIGHT_CX = 23.5;
const float EYE_CY   = 3.5;

const uint8_t BRIGHTNESS = 3;  // 0-15

MD_MAX72XX mx(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// Draw modes
enum Mode { NORMAL, ANGRY, HEARTS };

// ---------- low-level drawing ----------
void px(int x, int y, bool on) {
  if (x < 0 || x > 31 || y < 0 || y > 7) return;
  int col = FLIP_X ? x : 31 - x;       // FC-16 column 0 is at the input end
  int row = FLIP_Y ? 7 - y : y;
  mx.setPoint(row, col, on);
}

const char *HEART[6] = {
  ".##.##.",
  "#######",
  "#######",
  ".#####.",
  "..###..",
  "...#...",
};

// Draw one eye into its 16-column half.
// open: 1.0 normal, 0 closed, >1 wide.  pdx/pdy: pupil offset in dots.
void drawEye(bool rightEye, float open, float pdx, float pdy, Mode mode) {
  float cx = rightEye ? RIGHT_CX : LEFT_CX;
  int x0 = rightEye ? 16 : 0;
  float t = EYE_TILT * PI / 180.0;
  float c = cos(t), s = sin(t);
  float b = EYE_B * open;

  for (int x = x0; x < x0 + 16; x++) {
    for (int y = 0; y < 8; y++) {
      bool on = false;

      if (mode == HEARTS) {
        int hx = x - (int)(cx - 3);          // 7 wide, centred
        int hy = y - 1;                      // 6 tall, rows 1-6
        on = (hx >= 0 && hx < 7 && hy >= 0 && hy < 6 && HEART[hy][hx] == '#');
      } else if (b > 0.35) {
        // mirror the right eye so both outer corners tilt up
        float dx = (x - cx) * (rightEye ? -1 : 1);
        float dy = y - EYE_CY;
        float u =  dx * c + dy * s;
        float v = -dx * s + dy * c;
        on = (u * u) / (EYE_A * EYE_A) + (v * v) / (b * b) <= 1.0;

        // pupil: a dark 3x3 spot that moves (same screen direction for both eyes)
        if (on && open > 0.6) {
          float qx = x - (cx + pdx), qy = y - (EYE_CY + pdy);
          float r = (open > 1.1) ? 0.6 : 1.1;   // tiny pupil when surprised
          if (fabs(qx) <= r && fabs(qy) <= r) on = false;
        }

        // angry brow: clear above a line that slopes down toward the nose
        if (on && mode == ANGRY) {
          float inner = rightEye ? (x0 + 15 - x) : (x - x0);   // 0 outer .. 15 inner
          float brow = 0.5 + inner * 0.22;
          if (y < brow) on = false;
        }
      } else if (open <= 0.35) {
        // closed: a thin slanted line
        float dx = (x - cx) * (rightEye ? -1 : 1);
        int yl = (int)round(EYE_CY + dx * tan(t) * 0.6);
        on = (y == yl && fabs(dx) < EYE_A - 1);
      }
      px(x, y, on);
    }
  }
}

void drawEyes(float open, float pdx = 0, float pdy = 0, Mode mode = NORMAL) {
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);   // draw off-screen
  drawEye(false, open, pdx, pdy, mode);
  drawEye(true,  open, pdx, pdy, mode);
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);    // show it all at once
}

// ---------- animations ----------
float pupilX = 0, pupilY = 0;   // where the eyes are currently looking

void blink() {
  const float steps[] = {0.7, 0.35, 0.0, 0.0, 0.35, 0.7, 1.0};
  for (float o : steps) { drawEyes(o, pupilX, pupilY); delay(28); }
}

void lookTo(float tx, float ty, int stepMs = 45) {
  while (pupilX != tx || pupilY != ty) {
    if (pupilX < tx) pupilX++; else if (pupilX > tx) pupilX--;
    if (pupilY < ty) pupilY++; else if (pupilY > ty) pupilY--;
    drawEyes(1.0, pupilX, pupilY);
    delay(stepMs);
  }
}

void glance() {
  float dir = random(2) ? 3 : -3;
  lookTo(dir, random(-1, 2));
  delay(random(500, 1400));
  if (random(3) == 0) blink();
  lookTo(0, 0);
}

void lookAround() {
  lookTo(-3, 0);  delay(600);
  lookTo(3, 0, 60); delay(600);
  lookTo(0, -1);  delay(400);
  lookTo(0, 0);
}

void squint() {
  for (float o = 1.0; o >= 0.55; o -= 0.15) { drawEyes(o, pupilX, pupilY); delay(50); }
  delay(random(1200, 2200));
  for (float o = 0.55; o <= 1.0; o += 0.15) { drawEyes(o, pupilX, pupilY); delay(50); }
  drawEyes(1.0, pupilX, pupilY);
}

void surprise() {
  drawEyes(1.25, 0, 0); delay(1200);
  blink(); blink();
}

void angry() {
  drawEyes(1.0, 0, 1, ANGRY); delay(1800);
  drawEyes(1.0, pupilX, pupilY);
}

void hearts() {
  drawEyes(1.0, 0, 0, HEARTS);
  for (int i = 0; i < 4; i++) {            // gentle pulse
    for (int b = BRIGHTNESS; b <= BRIGHTNESS + 5; b++) { mx.control(MD_MAX72XX::INTENSITY, b); delay(40); }
    for (int b = BRIGHTNESS + 5; b >= BRIGHTNESS; b--) { mx.control(MD_MAX72XX::INTENSITY, b); delay(40); }
  }
  blink();
}

// ---------- main ----------
unsigned long nextAction = 0;

void setup() {
  Serial.begin(115200);
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, BRIGHTNESS);
  mx.clear();
  randomSeed(esp_random());

  // wake-up: eyes open slowly
  for (float o = 0; o <= 1.0; o += 0.1) { drawEyes(o); delay(60); }
  blink();
  Serial.println("Alien eyes running");
}

void loop() {
  if (millis() < nextAction) return;

  int r = random(100);
  if      (r < 45) blink();
  else if (r < 70) glance();
  else if (r < 78) { blink(); delay(150); blink(); }   // double blink
  else if (r < 85) lookAround();
  else if (r < 91) squint();
  else if (r < 95) surprise();
  else if (r < 98) hearts();
  else             angry();

  nextAction = millis() + random(1500, 5000);
}
