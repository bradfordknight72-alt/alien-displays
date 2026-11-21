// Grey Alien display - first light test
// Board: ESP32-C3 SuperMini + MAX7219 FC-16 4-in-1
// Arduino IDE: Board "ESP32C3 Dev Module", USB CDC On Boot: Enabled
// Library: MD_Parola (Library Manager installs MD_MAX72XX with it)

#include <MD_Parola.h>
#include <MD_MAX72xx.h>

// ---- Change these to match the GPIO pins your jumpers go to ----
#define DIN_PIN  6   // panel DIN  -> ESP32 GPIO
#define CLK_PIN  4   // panel CLK  -> ESP32 GPIO
#define CS_PIN   7   // panel CS   -> ESP32 GPIO
// panel VCC -> 5V pin, panel GND -> G

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW   // if letters look scrambled, try GENERIC_HW
#define MAX_DEVICES   4

MD_Parola display(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

const char *messages[] = { "Hi Kat!", "Take me to your leader", "<3" };
uint8_t msgIndex = 0;

void setup() {
  display.begin();
  display.setIntensity(2);      // 0-15; keep it low on USB power
  display.displayClear();
  display.displayText(messages[msgIndex], PA_CENTER, 60, 1500,
                      PA_SCROLL_LEFT, PA_SCROLL_LEFT);
}

void loop() {
  if (display.displayAnimate()) {             // true when a message finishes
    msgIndex = (msgIndex + 1) % 3;
    display.displayText(messages[msgIndex], PA_CENTER, 60, 1500,
                        PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }
}
