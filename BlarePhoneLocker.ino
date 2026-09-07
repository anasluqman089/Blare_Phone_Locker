#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ESP32Servo.h>


#define PIN_MODE    D0
#define PIN_ACT_A   D1
#define PIN_ACT_B   D2

#define PIN_SERVO   D6
#define PIN_BUZZER  D10

#define TFT_CS      D7
#define TFT_DC      D8
#define TFT_RST     D9

#define SERVO_OPEN      90
#define SERVO_CLOSED    0
#define ALARM_INTERVAL_SEC 1800UL    
#define MAX_LOCK_MINUTES 240UL       
#define BUZZ_REPEATS 5


Adafruit_ST7789 tft =
  Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

Servo lockerServo;


enum Mode {
  CLOCK_MODE,
  LOCKER_MODE
};

Mode currentMode = CLOCK_MODE;


int hours = 7;
int minutes = 0;
int seconds = 0;

unsigned long lastSecTick = 0;


bool alarmEnabled = false;

// Alarm target in seconds since midnight
unsigned long alarmTargetSec = 0;

bool lockerOpen = true;

unsigned long lockDurationSec = 0;

unsigned long lockEndMillis = 0;

bool lockTimerRunning = false;
bool lockFinished = false;


bool buzzing = false;
int buzzCount = 0;
unsigned long buzzToggleMillis = 0;
bool buzzState = false;

struct Btn {
  uint8_t pin;
  bool lastReading;
  bool state;
  unsigned long lastDebounce;
};

Btn btnMode = { PIN_MODE, HIGH, HIGH, 0 };
Btn btnA = { PIN_ACT_A, HIGH, HIGH, 0 };
Btn btnB = { PIN_ACT_B, HIGH, HIGH, 0 };


bool pressed(Btn &b) {
  bool reading = digitalRead(b.pin);
  bool fired = false;

  if (reading != b.lastReading) {
    b.lastDebounce = millis();
  }

  if ((millis() - b.lastDebounce) > 50UL) {
    if (reading != b.state) {
      b.state = reading;
      
      if (b.state == LOW) {
        fired = true;
      }
    }
  }

  b.lastReading = reading;
  return fired;
}

void startBuzz(int count) {
  if (count <= 0) return;
  buzzing = true;
  buzzCount = count;
  buzzState = true;
  digitalWrite(PIN_BUZZER, HIGH);
  buzzToggleMillis = millis();
}

void updateBuzzer() {
  if (!buzzing) return;

  unsigned long interval;

  if (buzzState) {
    interval = 120UL;
  }
  else {
    interval = 80UL;
  }

  if (millis() - buzzToggleMillis >= interval) {
    buzzToggleMillis = millis();
    buzzState = !buzzState;
    digitalWrite(PIN_BUZZER, buzzState ? HIGH : LOW);

    if (!buzzState) {
      buzzCount--;
      if (buzzCount <= 0) {
        buzzing = false;
        buzzState = false;
        digitalWrite(PIN_BUZZER, LOW);
      }
    }
  }
}


void moveServo(bool open) {
  if (open) {
    lockerServo.write(SERVO_OPEN);
    lockerOpen = true;
  } else {
    lockerServo.write(SERVO_CLOSED);
    lockerOpen = false;
  }
}


unsigned long nowSeconds() {
  return
    ((unsigned long)hours * 3600UL) +
    ((unsigned long)minutes * 60UL) +
    (unsigned long)seconds;
}


void setNextThirtyMinAlarm() {
  unsigned long current = nowSeconds();
  unsigned long next =
    ((current / ALARM_INTERVAL_SEC) + 1UL)
    * ALARM_INTERVAL_SEC;

  if (next >= 86400UL) {
    next -= 86400UL;
  }

  alarmTargetSec = next;
  alarmEnabled = true;
}


void drawClockMode() {

  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("BLARE CLOCK  "); 

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(5);
  tft.setCursor(15, 60);

  char timeBuffer[16];
  sprintf(timeBuffer, "%02d:%02d", hours, minutes);
  tft.print(timeBuffer);

  tft.setTextSize(3);
  tft.print(":");
  sprintf(timeBuffer, "%02d", seconds);
  tft.print(timeBuffer);

  tft.setTextSize(2);
  tft.setCursor(10, 140);

  if (alarmEnabled) {
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    unsigned long t = alarmTargetSec;
    unsigned long alarmHour = (t / 3600UL) % 24UL;
    unsigned long alarmMinute = (t / 60UL) % 60UL;
    
    char alarmBuffer[24];
    sprintf(alarmBuffer, "ALARM %02lu:%02lu  ", alarmHour, alarmMinute);
    tft.print(alarmBuffer);
  } else {
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.print("ALARM OFF      "); // Padded spaces
  }

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(10, 180);
  tft.print("A: Alarm ON/OFF");
  tft.setCursor(10, 205);
  tft.print("B: Next 30m alarm");
}


void drawLockerMode() {

  tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("PHONE LOCKER");

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(10, 60);

  char durationBuffer[32];
  sprintf(durationBuffer, "Duration: %lu min  ", lockDurationSec / 60UL);
  tft.print(durationBuffer);

  tft.setCursor(10, 90);

  if (lockerOpen) {
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.print("Locker: OPEN   ");
  } else {
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.print("Locker: LOCKED ");
  }

  tft.setCursor(15, 130);

  if (lockTimerRunning) {
    unsigned long remainingMs = 0;
    
    if ((long)(lockEndMillis - millis()) > 0) {
      remainingMs = lockEndMillis - millis();
    }

    unsigned long remainingSec = remainingMs / 1000UL;
    unsigned long h = remainingSec / 3600UL;
    unsigned long m = (remainingSec / 60UL) % 60UL;
    unsigned long s = remainingSec % 60UL;

    char countdownBuffer[20];
    sprintf(countdownBuffer, "%02lu:%02lu:%02lu    ", h, m, s);

    tft.setTextSize(3);
    tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
    tft.print(countdownBuffer);
  }
  else if (lockFinished) {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.print("TIME'S UP!         ");
  }
  else {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.print("Ready to lock      ");
  }

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(10, 180);
  tft.print("A: +30 min       ");
  tft.setCursor(10, 205);
  tft.print("B: Lock/Open     ");
}

void refreshScreen(bool clearScreen = false) {
  if (clearScreen) {
    tft.fillScreen(ST77XX_BLACK);
  }

  if (currentMode == CLOCK_MODE) {
    drawClockMode();
  } else {
    drawLockerMode();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(PIN_ACT_A, INPUT_PULLUP);
  pinMode(PIN_ACT_B, INPUT_PULLUP);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  lockerServo.setPeriodHertz(50);
  lockerServo.attach(PIN_SERVO, 500, 2400);

  moveServo(true);

  tft.init(240, 320);
  tft.setRotation(1); 

  refreshScreen(true);

  startBuzz(1);
}

void loop() {

  if (millis() - lastSecTick >= 1000UL) {
    lastSecTick += 1000UL;
    seconds++;

    if (seconds >= 60) {
      seconds = 0;
      minutes++;
    }

    if (minutes >= 60) {
      minutes = 0;
      hours++;
    }

    if (hours >= 24) {
      hours = 0;
    }

    if (alarmEnabled && nowSeconds() == alarmTargetSec) {
      startBuzz(BUZZ_REPEATS);
      setNextThirtyMinAlarm();
      refreshScreen();
    }

    if (!buzzing) {
      if (currentMode == CLOCK_MODE) {
        drawClockMode();
      } else if (currentMode == LOCKER_MODE && lockTimerRunning) {
        drawLockerMode();
      }
    }
  }

  if (lockTimerRunning && (long)(millis() - lockEndMillis) >= 0) {
    lockTimerRunning = false;
    lockFinished = true;
    startBuzz(BUZZ_REPEATS);
    refreshScreen();
  }


  updateBuzzer();

  if (pressed(btnMode)) {
    if (currentMode == CLOCK_MODE) {
      currentMode = LOCKER_MODE;
    } else {
      currentMode = CLOCK_MODE;
    }

    startBuzz(1);
    refreshScreen(true); 
  }


  if (currentMode == CLOCK_MODE) {

    if (pressed(btnA)) {
      alarmEnabled = !alarmEnabled;
      if (alarmEnabled) {
        setNextThirtyMinAlarm();
      }
      startBuzz(1);
      refreshScreen();
    }

    if (pressed(btnB)) {
      setNextThirtyMinAlarm();
      startBuzz(1);
      refreshScreen();
    }
  }

  else {

    if (pressed(btnA)) {
      if (lockDurationSec < MAX_LOCK_MINUTES * 60UL) {
        lockDurationSec += 1800UL;
      }
      lockFinished = false;
      startBuzz(1);
      refreshScreen();
    }


    if (pressed(btnB)) {

      if (lockerOpen) {
        if (lockDurationSec > 0) {
          moveServo(false);
          
          lockEndMillis = millis() + (lockDurationSec * 1000UL);
          lockTimerRunning = true;
          lockFinished = false;

          startBuzz(2);
          refreshScreen();
        }
      }

      else {

        if (!lockTimerRunning) {
          moveServo(true);
          lockTimerRunning = false;
          lockFinished = false;
          startBuzz(1);
          refreshScreen();
        } else {

          startBuzz(3); 
        }
      }
    }
  }
}