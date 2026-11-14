#include "firefox_launcher.h"   // se il tuo header si chiama apri_firefox.h, scrivi quello
#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

static const uint8_t SHORTCUT_KEY = 't';
static const uint16_t TERMINAL_DELAY_MS = 2500;

static USBHIDKeyboard keyboard;

void launcherInit() {
  keyboard.begin();
  USB.begin();
}

void launchFirefox() {
  Serial.println("[USB] apro il terminale");
  keyboard.press(KEY_LEFT_GUI);
  keyboard.press(SHORTCUT_KEY);
  delay(100);
  keyboard.releaseAll();
  delay(TERMINAL_DELAY_MS);

  Serial.println("[USB] invio: setsid firefox");
  keyboard.print("setsid firefox");
  keyboard.write(KEY_RETURN);
}