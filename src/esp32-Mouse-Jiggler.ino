/*
  ESP32-S3 “Mouse Jiggler” — HID over native USB (TinyUSB via Arduino-ESP32 3.x)

  What this sketch does:
  • Enumerates as a USB mouse. While the timer is running, every ~30–90 seconds
    it eases 10–20 px along one axis (+x, −x, +y, or −y), pauses briefly, then
    eases back so the pointer ends where it started. This runs for up to
    `kHoursToRun`. The Jiggle button runs one of those moves immediately.
  • Landscape touch UI on the ST7796: Jiggle, Nudge (a 40 px square back to
    the start), and USB. Status shows the next jiggle in 5 s steps, reports
    sent with jiggles finished, and the nudge leg in progress.
  • The touch key in the red circle under the glass toggles the backlight.
    Jiggling continues while the panel is dark.

  Board: LilyGO T-Display-S3 Pro (PlatformIO, pioarduino / Arduino-ESP32 3.x)
    • env:jiggler        USB-OTG HID mouse, CDC off.
                         Reflash: hold BOOT, tap RESET, release BOOT.
    • env:jiggler-debug  USB-OTG HID plus CDC serial at 115200.
                         USB disconnect also drops that serial port.

  Touch: CST226SE at 0x5A, with CST816 at 0x15 as a fallback. Reset is GPIO13.
  SY6970 at 0x6A shares that I2C bus. Startup turns off its STAT pin (the red
  LED by RESET) and the charger watchdog that would turn the blink back on.

  based on https://github.com/rithwikmakesthings/esp32s3-Mouse-Jiggler/

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
static constexpr int   kJiggleMinPx   = 10;    // one-axis out-and-back distance
static constexpr int   kJiggleMaxPx   = 20;
static constexpr int   kJiggleMinSteps = 6;    // eased micro-steps per leg
static constexpr int   kJiggleMaxSteps = 12;
static constexpr int   kJiggleReverseDelayMs = 30; // pause at the far point before returning
static constexpr int   kMinPauseMs    = 30000; // idle window between bursts
static constexpr int   kMaxPauseMs    = 90000;
static constexpr int   kStepInterval  = 10;     // ms between micro-steps
static constexpr float kHoursToRun    = 8.0f;  // after this, unattended jiggling stops
static constexpr int   kMaxLegs       = 8;     // jiggle uses 2, nudge uses 7

// ---------- Panel and touch ----------
// Pro pins. Do not use the non-Pro variant macros (LCD_BL, SDA, GPIO15).
static constexpr int kBacklightPin = 48;
static constexpr int kSdCsPin      = 14;
static constexpr int kTouchResPin  = 13;
static constexpr int kTouchIrqPin  = 21;
static constexpr int kTouchSdaPin  = 5;
static constexpr int kTouchSclPin  = 6;
static constexpr int kBacklightMax = 16; // V1.1 constant-current steps
static constexpr uint8_t kPmuAddr  = 0x6A;
static constexpr uint8_t kPmuReg07 = 0x07;
static constexpr uint8_t kPmuStatDis = 1 << 6;      // REG07 bit 6, STAT pin off
static constexpr uint8_t kPmuWatchdogMask = 0x30;   // REG07 bits 5:4

static constexpr int kScreenW   = 480; // rotation 1 of the 222x480 panel
static constexpr int kScreenH   = 222;
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
enum class MoveKind : uint8_t { JIGGLE, NUDGE };

struct MovementSession {
  int legDx[kMaxLegs] = {};
  int legDy[kMaxLegs] = {};
  int legSteps[kMaxLegs] = {};
  int legCount = 0;
  int legIndex = 0;
  int gapMs = 0; // pause after a leg, before the next one
  MoveKind kind = MoveKind::JIGGLE;

  int  totalDx = 0;
  int  totalDy = 0;
  int  steps   = 0;

  int  prevX   = 0;
  int  prevY   = 0;
  int  i       = 0;

  unsigned long lastStepMs = 0;
  bool waitingForGap = false;
  unsigned long legDoneMs = 0;
};

static JiggleState     gState        = JiggleState::IDLE;
static MovementSession gSession;
static unsigned long   gStartMs      = 0;
static unsigned long   gNextActionMs = 0;
static bool            gEnabled      = true;
static bool            gTimeUp       = false;
static bool            gUsbWanted    = true;
static bool            gTouchOk      = false;
static bool            gScreenOn     = true;
static bool            gHomeSeen     = false;
static bool            gHomeDown     = false;
static unsigned long   gHomeBlockMs  = 0;

static uint32_t gHidTx = 0;
static uint32_t gJiggleCount = 0;

// Last text actually on the glass. Refresh rewrites only the characters that
// changed, so a ticking counter does not black out the other status lines.
struct TextField {
  char text[64];
  uint16_t color;
  int x;
  int y;
  int width;
  bool valid;
};

static TextField gFieldMode;
static TextField gFieldBadge;
static TextField gFieldUsb;
static TextField gFieldNudge;
static TextField gFieldNudgePx;
static TextField gFieldTouch;

static int gMarkX = 0;
static int gMarkY = 0;
static int gMarkDx = 0;
static int gMarkDy = 0;
static bool gMarkOn = false;

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

static void loadLeg(int index) {
  gSession.totalDx = gSession.legDx[index];
  gSession.totalDy = gSession.legDy[index];
  gSession.steps   = gSession.legSteps[index];

  gSession.prevX = 0;
  gSession.prevY = 0;
  gSession.i     = 0;
  gSession.lastStepMs = 0;
  gSession.waitingForGap = false;
}

static void beginLegs(const int *dx, const int *dy, const int *steps, int count, int gapMs, MoveKind kind) {
  if (count > kMaxLegs) {
    count = kMaxLegs;
  }
  gSession.legCount = count;
  gSession.legIndex = 0;
  gSession.gapMs = gapMs;
  gSession.kind = kind;
  for (int i = 0; i < count; i++) {
    gSession.legDx[i] = dx[i];
    gSession.legDy[i] = dy[i];
    gSession.legSteps[i] = steps[i];
  }
  loadLeg(0);
  gState = JiggleState::MOVING;
}

static void beginRandomSession() {
  const int distance = random(kJiggleMinPx, kJiggleMaxPx + 1);
  static constexpr int kAxis[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  const int axis = random(4);
  const int dx = kAxis[axis][0] * distance;
  const int dy = kAxis[axis][1] * distance;
  const int span = kJiggleMaxPx - kJiggleMinPx;
  const int steps = kJiggleMinSteps
      + (distance - kJiggleMinPx) * (kJiggleMaxSteps - kJiggleMinSteps) / span;

  const int legDx[2] = {dx, -dx};
  const int legDy[2] = {dy, -dy};
  const int legSteps[2] = {steps, steps};
  beginLegs(legDx, legDy, legSteps, 2, kJiggleReverseDelayMs, MoveKind::JIGGLE);
}

// Host axes: +x right, +y down. 40 px square centered on the start.
// The sixth leg is left. Right would finish 40 px off the start.
static constexpr int kNudgeDx[] = {0, -20, 0, 40, 0, -20, 0};
static constexpr int kNudgeDy[] = {-20, 0, 40, 0, -40, 0, 20};

static void beginNudge() {
  int steps[7];
  for (int i = 0; i < 7; i++) {
    steps[i] = (abs(kNudgeDx[i]) + abs(kNudgeDy[i])) / 2;
  }
  beginLegs(kNudgeDx, kNudgeDy, steps, 7, 0, MoveKind::NUDGE);
}

static bool stepSessionIfDue() {
  const unsigned long now = millis();

  if (gSession.waitingForGap) {
    if ((now - gSession.legDoneMs) < (unsigned long)gSession.gapMs) {
      return true;
    }
    gSession.legIndex++;
    loadLeg(gSession.legIndex);
  }

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
    if (gSession.legIndex + 1 < gSession.legCount) {
      if (gSession.gapMs > 0) {
        gSession.waitingForGap = true;
        gSession.legDoneMs = now;
        return true;
      }
      gSession.legIndex++;
      loadLeg(gSession.legIndex);
      return true;
    }
    if (gSession.kind == MoveKind::JIGGLE) {
      gJiggleCount++;
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

// CST226 reports the red-circle key as its own packet. getPoint invokes this
// once per read while that packet is present, the same hook the Cellphone
// example uses.
static void onHomeButton(void *) {
  gHomeSeen = true;
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
  touch.setHomeButtonCallback(onHomeButton, nullptr);
}

// The red LED by RESET is the charger STAT pin. It blinks when USB is
// present and no battery is connected. Bit 6 of REG07 turns the pin off.
// The watchdog, bits 5:4, reloads the power-on defaults after 40 s, which
// would light the LED again, so those bits are cleared in the same write.
static void muteChargeLed() {
  Wire.beginTransmission(kPmuAddr);
  Wire.write(kPmuReg07);
  if (Wire.endTransmission(false) != 0) {
    return;
  }
  if (Wire.requestFrom(kPmuAddr, static_cast<uint8_t>(1)) != 1) {
    return;
  }
  const uint8_t value = (Wire.read() | kPmuStatDis) & ~kPmuWatchdogMask;

  Wire.beginTransmission(kPmuAddr);
  Wire.write(kPmuReg07);
  Wire.write(value);
  Wire.endTransmission();
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

static void invalidateStatus() {
  gFieldMode.valid = false;
  gFieldBadge.valid = false;
  gFieldUsb.valid = false;
  gFieldNudge.valid = false;
  gFieldNudgePx.valid = false;
  gFieldTouch.valid = false;
  gMarkOn = false;
}

static void setScreen(bool on) {
  if (on == gScreenOn) {
    return;
  }
  gScreenOn = on;
  if (!on) {
    setBacklight(0);
    tft.fillScreen(TFT_BLACK);
    return;
  }
  // The panel was cleared while it was off, so every line has to be drawn again.
  setBacklight(kBacklightMax);
  invalidateStatus();
  drawButtons();
}

static bool jiggleMoving() {
  return gState == JiggleState::MOVING && gSession.kind == MoveKind::JIGGLE;
}

static bool nudgeMoving() {
  return gState == JiggleState::MOVING && gSession.kind == MoveKind::NUDGE;
}

// Remaining time until the next jiggle, rounded up to a 5 s step.
// 22 s left shows "next 25s"; the label changes when that step changes.
static int nextJiggleBucketSec() {
  const unsigned long now = millis();
  if (now >= gNextActionMs) {
    return 0;
  }
  const unsigned long remainSec = (gNextActionMs - now + 999UL) / 1000UL;
  return static_cast<int>(((remainSec + 4UL) / 5UL) * 5UL);
}

static int statusCountdownSec() {
  if (!gEnabled || jiggleMoving()) {
    return -1;
  }
  const int bucket = nextJiggleBucketSec();
  return bucket > 0 ? bucket : -1;
}

// Font2 has no triangle glyphs. The mark stands in for ▴ ▾ ◂ ▸.
static int drawDirectionMark(int x, int top, int dx, int dy, uint16_t color) {
  const int size = 6;
  const int cx = x + 8;
  const int cy = top + 8;
  if (dy < 0) {
    tft.fillTriangle(cx, cy - size, cx - size, cy + size, cx + size, cy + size, color);
  } else if (dy > 0) {
    tft.fillTriangle(cx, cy + size, cx - size, cy - size, cx + size, cy - size, color);
  } else if (dx < 0) {
    tft.fillTriangle(cx - size, cy, cx + size, cy - size, cx + size, cy + size, color);
  } else if (dx > 0) {
    tft.fillTriangle(cx + size, cy, cx - size, cy - size, cx - size, cy + size, color);
  }
  return 16;
}

static void eraseField(const TextField &field) {
  if (!field.valid || field.width <= 0) {
    return;
  }
  tft.setFont(&fonts::Font2);
  tft.setTextSize(1);
  tft.fillRect(field.x, field.y, field.width, tft.fontHeight(), TFT_BLACK);
}

static void storeField(TextField &field, const char *text, uint16_t color, int x, int y, int width) {
  strncpy(field.text, text, sizeof(field.text) - 1);
  field.text[sizeof(field.text) - 1] = '\0';
  field.color = color;
  field.x = x;
  field.y = y;
  field.width = width;
  field.valid = true;
}

static int sharedPrefix(const char *prev, const char *next) {
  int i = 0;
  while (prev[i] != '\0' && prev[i] == next[i]) {
    i++;
  }
  return i;
}

// Draw over the previous string. Glyphs carry their own background, so the
// line is never cleared first. A shorter replacement erases only its leftover tail.
static void paintLeft(TextField &field, const char *text, uint16_t color, int x, int y) {
  if (field.valid && field.x == x && field.y == y && field.color == color &&
      strcmp(field.text, text) == 0) {
    return;
  }

  tft.setFont(&fonts::Font2);
  tft.setTextSize(1);
  tft.setTextDatum(lgfx::top_left);
  const int lineH = tft.fontHeight();

  if (field.valid && (field.x != x || field.y != y)) {
    eraseField(field);
    field.valid = false;
    field.width = 0;
    field.text[0] = '\0';
  }

  if (text[0] == '\0') {
    if (field.valid && field.width > 0) {
      tft.fillRect(field.x, field.y, field.width, lineH, TFT_BLACK);
    }
    storeField(field, "", color, x, y, 0);
    return;
  }

  tft.setTextColor(color, TFT_BLACK);
  const bool canPatch = field.valid && field.color == color;
  const int skip = canPatch ? sharedPrefix(field.text, text) : 0;

  int headW = 0;
  if (skip > 0) {
    char head[64];
    memcpy(head, text, static_cast<size_t>(skip));
    head[skip] = '\0';
    headW = tft.textWidth(head);
  }

  int drawn = 0;
  if (text[skip] != '\0') {
    drawn = tft.drawString(text + skip, x + headW, y);
  }
  const int totalW = headW + drawn;
  if (field.valid && field.width > totalW) {
    tft.fillRect(x + totalW, y, field.width - totalW, lineH, TFT_BLACK);
  }
  storeField(field, text, color, x, y, totalW);
}

static void paintRight(TextField &field, const char *text, uint16_t color, int right, int y) {
  if (field.valid && field.y == y && field.color == color && strcmp(field.text, text) == 0) {
    return;
  }

  tft.setFont(&fonts::Font2);
  tft.setTextSize(1);
  tft.setTextDatum(lgfx::top_left);
  const int lineH = tft.fontHeight();
  const int width = text[0] != '\0' ? tft.textWidth(text) : 0;
  const int x = right - width;

  if (field.valid && field.y == y) {
    const int oldL = field.x;
    const int oldR = field.x + field.width;
    if (oldL < x) {
      tft.fillRect(oldL, y, x - oldL, lineH, TFT_BLACK);
    }
    if (oldR > x + width) {
      tft.fillRect(x + width, y, oldR - (x + width), lineH, TFT_BLACK);
    }
  } else if (field.valid) {
    eraseField(field);
  }

  if (width > 0) {
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(text, x, y);
  }
  storeField(field, text, color, x, y, width);
}

static void hideDirection() {
  if (!gMarkOn) {
    return;
  }
  tft.fillRect(gMarkX, gMarkY, 16, 16, TFT_BLACK);
  gMarkOn = false;
}

static void paintDirection(int x, int y, int dx, int dy) {
  if (gMarkOn && gMarkX == x && gMarkY == y && gMarkDx == dx && gMarkDy == dy) {
    return;
  }
  if (gMarkOn) {
    tft.fillRect(gMarkX, gMarkY, 16, 16, TFT_BLACK);
  }
  drawDirectionMark(x, y, dx, dy, TFT_WHITE);
  gMarkX = x;
  gMarkY = y;
  gMarkDx = dx;
  gMarkDy = dy;
  gMarkOn = true;
}

static void paintActivity() {
  const int y = 72;

  // Move the touch warning off the left edge before the nudge label lands there.
  if (!gTouchOk && nudgeMoving()) {
    paintRight(gFieldTouch, "No touch", TFT_YELLOW, kScreenW - 8, y);
  }

  if (nudgeMoving()) {
    paintLeft(gFieldNudge, "nudge:", TFT_WHITE, 8, y);
    const int markX = 8 + gFieldNudge.width + 6;
    paintDirection(markX, y, gSession.totalDx, gSession.totalDy);
    char detail[24];
    const int distance = abs(gSession.totalDx) + abs(gSession.totalDy);
    snprintf(detail, sizeof(detail), "%dpx (%d/%d)",
             distance, gSession.legIndex + 1, gSession.legCount);
    paintLeft(gFieldNudgePx, detail, TFT_WHITE, markX + 16 + 4, y);
  } else {
    paintLeft(gFieldNudge, "", TFT_WHITE, 8, y);
    hideDirection();
    if (gFieldNudgePx.valid) {
      paintLeft(gFieldNudgePx, "", TFT_WHITE, gFieldNudgePx.x, gFieldNudgePx.y);
    }
  }

  if (!gTouchOk && !nudgeMoving()) {
    paintLeft(gFieldTouch, "No touch", TFT_YELLOW, 8, y);
  } else if (gTouchOk && gFieldTouch.valid && gFieldTouch.width > 0) {
    paintLeft(gFieldTouch, "", gFieldTouch.color, gFieldTouch.x, gFieldTouch.y);
  }
}

static void refreshStatus() {
  if (!gScreenOn) {
    return;
  }

  const bool mounted = usbMounted();
  const int countdown = statusCountdownSec();

  tft.startWrite();

  if (gEnabled) {
    paintLeft(gFieldMode, "Jiggler: RUN", TFT_GREEN, 8, 16);
  } else {
    paintLeft(gFieldMode, "Jiggler: STOP", TFT_RED, 8, 16);
  }

  if (jiggleMoving()) {
    paintRight(gFieldBadge, "MOVE", TFT_YELLOW, kScreenW - 8, 16);
  } else if (countdown > 0) {
    char countdownText[16];
    snprintf(countdownText, sizeof(countdownText), "next %ds", countdown);
    paintRight(gFieldBadge, countdownText, TFT_WHITE, kScreenW - 8, 16);
  } else {
    paintRight(gFieldBadge, "", TFT_WHITE, kScreenW - 8, 16);
  }

  char usbLine[48];
  const char *usbState = !gUsbWanted ? "OFF" : (mounted ? "ON" : "WAIT");
  snprintf(usbLine, sizeof(usbLine), "USB: %s  sent %lu (%lu)",
           usbState,
           static_cast<unsigned long>(gHidTx),
           static_cast<unsigned long>(gJiggleCount));
  uint16_t usbColor = TFT_YELLOW;
  if (!gUsbWanted) {
    usbColor = TFT_RED;
  } else if (mounted) {
    usbColor = TFT_GREEN;
  }
  paintLeft(gFieldUsb, usbLine, usbColor, 8, 44);

  paintActivity();
  tft.endWrite();
}

// One out-and-back move, right now. The idle timer is left alone.
static void onJiggle() {
  beginRandomSession();
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

// One toggle per press. The controller repeats the home packet while the
// key is down, and a 200 ms lockout covers a burst that drops out briefly.
static void noteHomeButton() {
  if (!gHomeSeen) {
    gHomeDown = false;
    return;
  }
  if (gHomeDown) {
    return;
  }
  gHomeDown = true;
  const unsigned long now = millis();
  if (now < gHomeBlockMs) {
    return;
  }
  gHomeBlockMs = now + 200;
  setScreen(!gScreenOn);
}

static void pollTouch() {
  if (!gTouchOk) {
    return;
  }

  gHomeSeen = false;
  int16_t x = -1;
  int16_t y = -1;
  const bool down = touch.getPoint(&x, &y, 1) > 0;
  noteHomeButton();

  if (!gScreenOn) {
    gBtnJiggle.press(false);
    gBtnNudge.press(false);
    gBtnUsb.press(false);
    return;
  }

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
  muteChargeLed();

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
