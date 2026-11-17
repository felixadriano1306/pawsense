#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "PawSense"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ESP32Servo.h>

char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

// --- Sensor Pins ---
const int TURBIDITY_PIN    = 34; // Analog ADC (via 5V->3.3V divider)
const int FLOAT_SWITCH_PIN = 27; // Digital reservoir float switch
const int PET_TRIG_PIN     = 5;  // Ultrasonic 1 (Pet detection)
const int PET_ECHO_PIN     = 18; // Via 5V->3.3V divider
const int BOWL_IR_PIN      = 19; // Digital IR obstacle sensor
const int HOPPER_TRIG_PIN  = 21; // Ultrasonic 2 (Hopper food level)
const int HOPPER_ECHO_PIN  = 22; // Via 5V->3.3V divider

// --- Actuator & Camera Trigger Pins ---
const int PUMP_PWM_PIN     = 25; // To IRF520 MOSFET driver
const int UVC_RELAY_PIN    = 26; // To 5V relay module
const int VALVE_SERVO_PIN  = 32; // Standard 180-deg servo (3-way valve)
const int FEED_SERVO_PIN   = 33; // 360-deg continuous-rotation servo
const int CAM_TRIGGER_PIN  = 14; // Hardware snapshot trigger to ESP32-CAM

Servo valveServo;
Servo feedServo;
BlynkTimer timer;

int turbidityLimit = 120;      // NTU threshold for automated flush
int portionRunMs   = 2500;     // Calibrated 360-deg servo run time
int visitCount     = 0;
unsigned long drinkSeconds = 0;
bool petPresent    = false;

long readUltrasonicCm(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 25000);
  return (duration == 0) ? 999 : (duration * 0.034 / 2);
}

void triggerCamSnapshot() {
  digitalWrite(CAM_TRIGGER_PIN, HIGH);
  delay(150);
  digitalWrite(CAM_TRIGGER_PIN, LOW);
}

void executeFlushCycle() {
  analogWrite(PUMP_PWM_PIN, 0);
  delay(300);
  valveServo.write(90); // Divert to waste reservoir
  delay(600);
  analogWrite(PUMP_PWM_PIN, 255); // Pump dirty water out
  delay(6000);
  analogWrite(PUMP_PWM_PIN, 0);
  delay(300);
  valveServo.write(0);  // Return to drinking mode
  delay(600);
  Blynk.logEvent("high_turbidity_detected", "HIGH TURBIDITY DETECTED: Auto-flush complete.");
  Blynk.logEvent("refill_clean_water", "REFILL CLEAN WATER: Reservoir drained after flush.");
}

void dispensePortion() {
  bool foodInBowl = (digitalRead(BOWL_IR_PIN) == LOW);
  if (foodInBowl) {
    return; // Skip feeding if bowl still has food
  }
  feedServo.write(180); // Rotate 360-deg dispenser
  delay(portionRunMs);
  feedServo.write(90);  // Stop continuous servo
  delay(400);

  triggerCamSnapshot();
  bool dispensedOk = (digitalRead(BOWL_IR_PIN) == LOW);
  if (!dispensedOk) {
    Blynk.logEvent("food_not_dispensed", "FOOD NOT DISPENSED: Check hopper for jam.");
  }
}

// Blynk App Manual Controls
BLYNK_WRITE(V5) { if (param.asInt() == 1) executeFlushCycle(); }
BLYNK_WRITE(V9) { if (param.asInt() == 1) dispensePortion(); }
BLYNK_WRITE(V10) { portionRunMs = param.asInt(); }

void monitorSystem() {
  // 1. Eye-Safety Optical Interlock (< 30 cm)
  long petDist = readUltrasonicCm(PET_TRIG_PIN, PET_ECHO_PIN);
  bool nearNow = (petDist < 30);

  if (nearNow) {
    digitalWrite(UVC_RELAY_PIN, LOW);  // Cut UV-C immediately
    analogWrite(PUMP_PWM_PIN, 240);    // Ramp up drinking flow
    drinkSeconds++;
    if (!petPresent) {
      visitCount++;
      triggerCamSnapshot();
    }
  } else {
    digitalWrite(UVC_RELAY_PIN, HIGH); // Safe to sterilize
    analogWrite(PUMP_PWM_PIN, 130);    // Gentle circulation
  }
  petPresent = nearNow;

  // 2. Water Quality & Level
  int rawTurb = analogRead(TURBIDITY_PIN);
  int ntu = map(rawTurb, 0, 4095, 300, 0);
  int purityPct = constrain(map(ntu, 0, 200, 100, 0), 0, 100);
  bool waterOk = (digitalRead(FLOAT_SWITCH_PIN) == HIGH);

  if (ntu > turbidityLimit && waterOk) {
    executeFlushCycle();
  }

  // 3. Hopper Level & Bowl IR
  long hopperDist = readUltrasonicCm(HOPPER_TRIG_PIN, HOPPER_ECHO_PIN);
  int hopperPct = constrain(map(hopperDist, 20, 4, 0, 100), 0, 100);
  bool bowlHasFood = (digitalRead(BOWL_IR_PIN) == LOW);

  if (hopperPct <= 20) {
    Blynk.logEvent("refill_food", "REFILL FOOD: Feeder hopper is running low.");
  }

  // 4. Sync Virtual Pins V0–V13 to Blynk Web & Mobile App
  Blynk.virtualWrite(V0, purityPct);
  Blynk.virtualWrite(V1, ntu);
  Blynk.virtualWrite(V2, waterOk ? 1 : 0);
  Blynk.virtualWrite(V3, petPresent ? 1 : 0);
  Blynk.virtualWrite(V7, hopperPct);
  Blynk.virtualWrite(V8, bowlHasFood ? 1 : 0);
  Blynk.virtualWrite(V12, visitCount);
  Blynk.virtualWrite(V13, drinkSeconds);
}

void setup() {
  Serial.begin(115200);
  pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(PET_TRIG_PIN, OUTPUT);
  pinMode(PET_ECHO_PIN, INPUT);
  pinMode(BOWL_IR_PIN, INPUT);
  pinMode(HOPPER_TRIG_PIN, OUTPUT);
  pinMode(HOPPER_ECHO_PIN, INPUT);
  pinMode(PUMP_PWM_PIN, OUTPUT);
  pinMode(UVC_RELAY_PIN, OUTPUT);
  pinMode(CAM_TRIGGER_PIN, OUTPUT);

  valveServo.attach(VALVE_SERVO_PIN);
  valveServo.write(0);
  feedServo.attach(FEED_SERVO_PIN);
  feedServo.write(90);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  timer.setInterval(1000L, monitorSystem);
}

void loop() {
  Blynk.run();
  timer.run();
}