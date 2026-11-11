/*
  ESP32-S3 “Mouse Jiggler” — HID over native USB (TinyUSB via Arduino-ESP32 3.x)

  What this sketch does:
  • Enumerates as a USB mouse and performs a short eased mouse movement every
    ~22–42 seconds for up to `kHoursToRun`, until the on-screen Jiggle button
    stops it.
  • Landscape touch UI on the ST7796: jiggler and USB status, plus Jiggle,
    Nudge (100 px out and back on each axis), and USB disconnect/reconnect.
  • Backlight stays on. The screen is the activity indicator.

  Board: LilyGO T-Display-S3 Pro (PlatformIO, pioarduino / Arduino-ESP32 3.x)
    • env:jiggler        USB-OTG HID mouse, CDC off.
                         Reflash: hold BOOT, tap RESET, release BOOT.
    • env:jiggler-debug  USB-OTG HID plus CDC serial at 115200.
                         USB disconnect also drops that serial port.

  Touch: CST226SE at 0x5A, with CST816 at 0x15 as a fallback. Reset is GPIO13.

  VID/PID note:
  • usbdev.VID()/PID() may not override on all core versions at runtime.
    If you need an exact VID/PID, you might have to hard-code it in the core’s USB.cpp.
*/

#include <Arduino.h>
#include "USB.h"
#include "USBHIDMouse.h"
#include "tusb.h"
#include <LovyanGFX.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

// SPI ST7796, 222x480, from the Pro examples (Setup214 / Arduino_ST7796).
// V1.1 backlight is a 16-step pulse pin, not PWM, so the light driver stays off.
class LGFX : public lgfx::LGFX_Device {
 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 17;
      cfg.pin_miso = 8;
      cfg.pin_dc = 9;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 39;
      cfg.pin_rst = 47;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.panel_width = 222;
      cfg.panel_height = 480;
      cfg.offset_x = 49;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }

 private:
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST7796 _panel;
};

// ---------- USB device ----------
ESPUSB usbdev;
USBHIDMouse Mouse;

// ---------- Tunables ----------
static constexpr int   kMoveRangePx   = 100;   // |dx|, |dy| upper bound (pixels)
static constexpr int   kMinPauseMs    = 22000; // idle window between bursts
static constexpr int   kMaxPauseMs    = 42000;
static constexpr int   kMinSteps      = 20;    // eased micro-steps per burst
static constexpr int   kMaxSteps      = 30;
static constexpr int   kStepInterval  = 5;     // ms between micro-steps
static constexpr float kHoursToRun    = 8.0f;  // after this, unattended jiggling stops
static constexpr int   kNudgePx       = 200;   // previous one-way diagonal, each axis
static constexpr int   kNudgeLegPx    = kNudgePx / 2; // out, then back, so the pointer returns
static constexpr int   kNudgeSteps    = 20;    // steps per leg; keeps each HID report inside int8

// ---------- Panel and touch ----------
// Pro pins. Do not use the non-Pro variant macros (LCD_BL, SDA, GPIO15).
static constexpr int kBacklightPin = 48;
static constexpr int kSdCsPin      = 14;
static constexpr int kTouchResPin  = 13;
static constexpr int kTouchIrqPin  = 21;
static constexpr int kTouchSdaPin  = 5;
static constexpr int kTouchSclPin  = 6;
static constexpr int kBacklightMax = 16; // V1.1 constant-current steps

static constexpr int kScreenW   = 480; // rotation 1 of the 222x480 panel
static constexpr int kScreenH   = 222;
static constexpr int kStatusH   = 110;
static constexpr int kButtonW   = 140;
static constexpr int kButtonH   = 72;
static constexpr int kButtonGap = (kScreenW - 3 * kButtonW) / 4;
static constexpr int kButtonY   = kScreenH - kButtonGap - (kButtonH / 2);

struct UiButton {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint16_t fill = 0;
  const char *label = "";
  bool curr = false;
  bool last = false;

  bool contains(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
  void press(bool down) {
    last = curr;
    curr = down;
  }
  bool isPressed() const { return curr; }
  bool justPressed() const { return curr && !last; }
  bool justReleased() const { return !curr && last; }
};

LGFX tft;
TouchDrvCSTXXX touch;

UiButton gBtnJiggle;
UiButton gBtnNudge;
UiButton gBtnUsb;

// ---------- State machine ----------
enum class JiggleState : uint8_t { IDLE, MOVING };

struct MovementSession {
  int  totalDx = 0;
  int  totalDy = 0;
  int  steps   = 0;

  int  prevX   = 0;
  int  prevY   = 0;
  int  i       = 0;

  unsigned long lastStepMs = 0;
  bool outAndBack = false; // nudge: second leg reverses the first
  bool returning  = false;
};

static JiggleState     gState        = JiggleState::IDLE;
static MovementSession gSession;
static unsigned long   gStartMs      = 0;
static unsigned long   gNextActionMs = 0;
static bool            gEnabled      = true;
static bool            gTimeUp       = false;
static bool            gUsbWanted    = true;
static bool            gTouchOk      = false;

static uint32_t gHidTx = 0;

static bool gPainted       = false;
static bool gPaintEnabled  = false;
static bool gPaintMoving   = false;
static bool gPaintWanted   = false;
static bool gPaintMounted  = false;
static bool gPaintTouch    = false;
static uint32_t gPaintTx   = 0;

// ---------- Helpers ----------
static float easeCosine01(float t) {
  // 0.5 * (1 - cos(pi * t))
  return 0.5f * (1.0f - cosf(PI * t));
}

static int8_t satInt8(int value) {
  if (value > 127) return 127;
  if (value < -127) return -127;
  return static_cast<int8_t>(value);
}

static bool usbMounted() {
  return tud_mounted();
}

// Queue one relative report. False means "try this same delta again":
// the host has not accepted it yet, so the step must not be discarded.
static bool sendDelta(int dx, int dy) {
  if (dx == 0 && dy == 0) {
    return true;
  }
  if (!gUsbWanted) {
    return true;
  }
  if (!tud_mounted()) {
    return false;
  }
  if (tud_suspended()) {
    tud_remote_wakeup();
    return false;
  }
  if (!tud_hid_n_ready(0)) {
    return false;
  }
  if (!tud_hid_mouse_report(HID_REPORT_ID_MOUSE, 0, satInt8(dx), satInt8(dy), 0, 0)) {
    return false;
  }
  gHidTx++;
  return true;
}

static void seedRng() {
#if defined(ESP_PLATFORM)
  randomSeed(esp_random());
#else
  randomSeed(analogRead(A0));
#endif
}

static void scheduleNextIdleWindow() {
  gNextActionMs = millis() + (unsigned long)random(kMinPauseMs, kMaxPauseMs);
}

static void beginSession(int totalDx, int totalDy, int steps) {
  gSession.totalDx = totalDx;
  gSession.totalDy = totalDy;
  gSession.steps   = steps;

  gSession.prevX = 0;
  gSession.prevY = 0;
  gSession.i     = 0;
  gSession.lastStepMs = 0;
  gSession.outAndBack = false;
  gSession.returning  = false;

  gState = JiggleState::MOVING;
}

static void beginRandomSession() {
  beginSession(
      random(-kMoveRangePx, kMoveRangePx + 1),
      random(-kMoveRangePx, kMoveRangePx + 1),
      random(kMinSteps, kMaxSteps + 1));
}

static void beginNudge() {
  beginSession(kNudgeLegPx, kNudgeLegPx, kNudgeSteps);
  gSession.outAndBack = true;
}

static bool stepSessionIfDue() {
  const unsigned long now = millis();

  // The first step must not stamp lastStepMs. Doing that while i is still 0
  // resets the timer on every loop, so the interval never elapses and no
  // HID report is ever queued.
  if (gSession.i != 0 && (now - gSession.lastStepMs) < (unsigned long)kStepInterval) {
    return true;
  }

  const int nextIndex = gSession.i + 1;
  const float t       = float(nextIndex) / float(gSession.steps);
  const float e       = easeCosine01(t);

  const int xPos = lroundf(gSession.totalDx * e);
  const int yPos = lroundf(gSession.totalDy * e);

  const int dx = xPos - gSession.prevX;
  const int dy = yPos - gSession.prevY;

  if (!sendDelta(dx, dy)) {
    return true;
  }

  gSession.lastStepMs = now;
  gSession.prevX = xPos;
  gSession.prevY = yPos;
  gSession.i     = nextIndex;

  if (gSession.i >= gSession.steps) {
    if (gSession.outAndBack && !gSession.returning) {
      const int backDx = -gSession.totalDx;
      const int backDy = -gSession.totalDy;
      const int steps  = gSession.steps;
      beginSession(backDx, backDy, steps);
      gSession.outAndBack = true;
      gSession.returning  = true;
      return true;
    }
    gState = JiggleState::IDLE;
    scheduleNextIdleWindow();
    return false;
  }

  return true;
}

// V1.1 steps the backlight by pulsing GPIO48. A held high level is also full
// on for a V1.0 PWM backlight.
static void setBacklight(uint8_t value) {
  static uint8_t level = 0;
  if (value > kBacklightMax) {
    value = kBacklightMax;
  }
  if (value == 0) {
    digitalWrite(kBacklightPin, LOW);
    delay(3);
    level = 0;
    return;
  }
  if (level == 0) {
    digitalWrite(kBacklightPin, HIGH);
    level = kBacklightMax;
    delayMicroseconds(30);
  }
  const int from = kBacklightMax - level;
  const int to = kBacklightMax - value;
  const int pulses = (kBacklightMax + to - from) % kBacklightMax;
  for (int i = 0; i < pulses; i++) {
    digitalWrite(kBacklightPin, LOW);
    digitalWrite(kBacklightPin, HIGH);
  }
  level = value;
}

static void initPanel() {
  pinMode(kSdCsPin, OUTPUT);
  digitalWrite(kSdCsPin, HIGH);

  pinMode(kBacklightPin, OUTPUT);
  setBacklight(kBacklightMax);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  setBacklight(kBacklightMax);
}

static void initTouch() {
  pinMode(kTouchResPin, OUTPUT);
  digitalWrite(kTouchResPin, LOW);
  delay(10);
  digitalWrite(kTouchResPin, HIGH);
  delay(50);
  pinMode(kTouchIrqPin, INPUT);

  Wire.begin(kTouchSdaPin, kTouchSclPin);
  gTouchOk = touch.begin(Wire, CST226SE_SLAVE_ADDRESS, kTouchSdaPin, kTouchSclPin);
  if (!gTouchOk) {
    gTouchOk = touch.begin(Wire, CST816_SLAVE_ADDRESS, kTouchSdaPin, kTouchSclPin);
  }
  if (!gTouchOk) {
    return;
  }
  touch.setMaxCoordinates(kScreenW, kScreenH);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
}

static void initButton(UiButton &btn, int16_t centerX, const char *label, uint16_t fill) {
  btn.x = centerX - (kButtonW / 2);
  btn.y = kButtonY - (kButtonH / 2);
  btn.w = kButtonW;
  btn.h = kButtonH;
  btn.fill = fill;
  btn.label = label;
}

static void drawButton(const UiButton &btn, bool inverted) {
  const uint16_t fill = inverted ? TFT_WHITE : btn.fill;
  const uint16_t text = inverted ? btn.fill : TFT_WHITE;
  const int radius = min(btn.w, btn.h) / 4;
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, radius, fill);
  tft.drawRoundRect(btn.x, btn.y, btn.w, btn.h, radius, TFT_WHITE);
  tft.setFont(&fonts::Font2);
  tft.setTextSize(1);
  tft.setTextColor(text, fill);
  tft.setTextDatum(lgfx::middle_center);
  tft.drawString(btn.label, btn.x + (btn.w / 2), btn.y + (btn.h / 2));
}

static void drawButtons() {
  drawButton(gBtnJiggle, false);
  drawButton(gBtnNudge, false);
  drawButton(gBtnUsb, false);
}

static void drawStatus(bool mounted, bool moving) {
  tft.fillRect(0, 0, kScreenW, kStatusH, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.setTextSize(1);
  tft.setTextDatum(lgfx::top_left);

  if (gEnabled) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Jiggler: RUN", 8, 16);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Jiggler: STOP", 8, 16);
  }

  if (moving) {
    tft.setTextDatum(lgfx::top_right);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("MOVE", kScreenW - 8, 16);
    tft.setTextDatum(lgfx::top_left);
  }

  char usbLine[40];
  const char *usbState = !gUsbWanted ? "OFF" : (mounted ? "ON" : "WAIT");
  snprintf(usbLine, sizeof(usbLine), "USB: %s  sent %lu", usbState, static_cast<unsigned long>(gHidTx));
  if (!gUsbWanted) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
  } else if (mounted) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
  } else {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  }
  tft.drawString(usbLine, 8, 44);

  if (!gTouchOk) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("No touch", 8, 72);
  }
}

static void refreshStatus() {
  const bool mounted = usbMounted();
  const bool moving  = gState == JiggleState::MOVING;
  if (gPainted && gPaintEnabled == gEnabled && gPaintMoving == moving &&
      gPaintWanted == gUsbWanted && gPaintMounted == mounted && gPaintTouch == gTouchOk &&
      gPaintTx == gHidTx) {
    return;
  }

  drawStatus(mounted, moving);
  gPainted      = true;
  gPaintEnabled = gEnabled;
  gPaintMoving  = moving;
  gPaintWanted  = gUsbWanted;
  gPaintMounted = mounted;
  gPaintTouch   = gTouchOk;
  gPaintTx      = gHidTx;
}

static void onJiggle() {
  gEnabled = !gEnabled;
  if (gEnabled) {
    if (gTimeUp) {
      gTimeUp  = false;
      gStartMs = millis();
    }
    scheduleNextIdleWindow();
  } else if (gState == JiggleState::MOVING) {
    gState = JiggleState::IDLE;
  }
}

static void onNudge() {
  beginNudge();
}

static void onUsb() {
  const bool wanted = !gUsbWanted;
  const bool ok     = wanted ? tud_connect() : tud_disconnect();
  if (!ok) {
    return;
  }
  gUsbWanted = wanted;
}

static void serviceButton(UiButton &btn, bool down, int16_t x, int16_t y, void (*action)()) {
  btn.press(down && btn.contains(x, y));
  if (btn.justReleased()) {
    action();
  }
  if (btn.justPressed() || btn.justReleased()) {
    drawButton(btn, btn.isPressed());
  }
}

static void pollTouch() {
  if (!gTouchOk) {
    return;
  }

  int16_t x = -1;
  int16_t y = -1;
  const bool down = touch.getPoint(&x, &y, 1) > 0;

  serviceButton(gBtnJiggle, down, x, y, onJiggle);
  serviceButton(gBtnNudge, down, x, y, onNudge);
  serviceButton(gBtnUsb, down, x, y, onUsb);
}

static void enforceRuntimeLimit() {
  if (!gEnabled || gTimeUp) {
    return;
  }

  const unsigned long elapsedMs = millis() - gStartMs;
  const unsigned long limitMs   = (unsigned long)(kHoursToRun * 60.0f * 60.0f * 1000.0f);
  if (elapsedMs < limitMs) {
    return;
  }

  gTimeUp  = true;
  gEnabled = false;
  gState   = JiggleState::IDLE;
}

// ---------- Arduino lifecycle ----------
void setup() {
  initPanel();
  initTouch();

  const int buttonX0 = kButtonGap + (kButtonW / 2);
  initButton(gBtnJiggle, buttonX0, "Jiggle", TFT_DARKGREEN);
  initButton(gBtnNudge, buttonX0 + kButtonW + kButtonGap, "Nudge", TFT_NAVY);
  initButton(gBtnUsb, buttonX0 + 2 * (kButtonW + kButtonGap), "USB", TFT_MAROON);
  drawButtons();
  refreshStatus();

  // (Optional) Spoof as a Dell Laser Mouse MS3220
  usbdev.VID(0x413C); // May require core edit to truly take effect
  usbdev.PID(0x250E);
  usbdev.productName("Dell Laser Mouse MS3220");
  usbdev.manufacturerName("Dell");
  usbdev.usbAttributes(static_cast<uint8_t>(TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP));

  Mouse.begin();    // register HID before the device stack starts
  usbdev.begin();   // bring up TinyUSB device stack

  seedRng();

  gStartMs = millis();
  scheduleNextIdleWindow();
}

void loop() {
  pollTouch();
  enforceRuntimeLimit();

  switch (gState) {
    case JiggleState::IDLE:
      if (gEnabled && millis() >= gNextActionMs) {
        beginRandomSession();
      }
      break;

    case JiggleState::MOVING:
      stepSessionIfDue();
      break;
  }

  refreshStatus();
  delay(gState == JiggleState::MOVING ? 1 : 5);
}
