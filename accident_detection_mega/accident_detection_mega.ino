/*
 * ================================================================
 * Vehicle Accident Detection System
 * Board   : Arduino Mega 2560
 * Sensors : MPU6050 (I2C) + NEO-6M GPS + SIM800L GSM
 * Output  : 16x2 LCD (I2C), Green LED, Red LED, SMS + HTTP POST
 * Server  : https://sangecarsems-1.onrender.com
 * ================================================================
 */

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>
#include <LiquidCrystal_I2C.h>

// ─── USER CONFIG ─────────────────────────────────────────────────
#define ALERT_PHONE_NUMBER    "+233597848398"
#define IMPACT_THRESHOLD_G     4.0f
#define COOLDOWN_MS            15000UL
#define LCD_I2C_ADDRESS        0x27
#define CAR_NAME               "Sange Cars"
#define CAR_NUMBER             "Sange 1"

// ─── FLASK / RENDER SERVER ───────────────────────────────────────
#define FLASK_HOST   "sangecarsems-1.onrender.com"
#define FLASK_PORT   80
#define FLASK_PATH   "/api/accident"

// ─── APN (change to match your SIM network) ──────────────────────
// MTN Ghana:      "internet"
// Vodafone Ghana: "internet"
// AirtelTigo:     "internet"
#define GSM_APN   "internet"

// ─── PIN DEFINITIONS ─────────────────────────────────────────────
#define GPS_SERIAL    Serial1    // RX1=19, TX1=18
#define GSM_SERIAL    Serial2    // RX2=17, TX2=16
#define GREEN_LED     6
#define RED_LED       7

// ─── OBJECTS ─────────────────────────────────────────────────────
Adafruit_MPU6050  mpu;
TinyGPSPlus       gps;
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);

// ─── GLOBALS ─────────────────────────────────────────────────────
unsigned long lastAlertTime = 0;
bool systemArmed = false;
double currentLat = 0.0;
double currentLng = 0.0;
bool   hasGPSFix  = false;

// ─── HELPERS ─────────────────────────────────────────────────────
void lcdPrint(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}

void sendGSMCommand(const char* cmd, unsigned long waitMs = 1000) {
  GSM_SERIAL.println(cmd);
  delay(waitMs);
  while (GSM_SERIAL.available()) Serial.write(GSM_SERIAL.read());
}

bool waitForResponse(const char* expected, unsigned long timeout = 5000) {
  String buf = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (GSM_SERIAL.available()) {
      char c = GSM_SERIAL.read();
      buf += c;
      Serial.write(c);
      if (buf.indexOf(expected) >= 0) return true;
    }
  }
  return false;
}

// ─── GPS FEED ────────────────────────────────────────────────────
void feedGPS(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    while (GPS_SERIAL.available()) {
      char c = GPS_SERIAL.read();
      gps.encode(c);
    }
  }
  if (gps.location.isValid() && gps.location.isUpdated()) {
    currentLat = gps.location.lat();
    currentLng = gps.location.lng();
    hasGPSFix  = true;
  }
}

// ─── READ G-FORCE ─────────────────────────────────────────────────
float readGForce() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float mag = sqrt(
    a.acceleration.x * a.acceleration.x +
    a.acceleration.y * a.acceleration.y +
    a.acceleration.z * a.acceleration.z
  );
  return mag / 9.81f;
}

// ─── GET DATE / TIME FROM GPS ────────────────────────────────────
String getGPSDate() {
  if (gps.date.isValid()) {
    char buf[12];
    sprintf(buf, "%02d/%02d/%04d", gps.date.day(), gps.date.month(), gps.date.year());
    return String(buf);
  }
  return "Date Unavail.";
}

String getGPSTime() {
  if (gps.time.isValid()) {
    char buf[12];
    sprintf(buf, "%02d:%02d:%02d UTC", gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
  }
  return "00:00:00 UTC";
}

// ─── SEND SMS ────────────────────────────────────────────────────
void sendSMSAlert(float gForce) {
  Serial.println("[SMS] Preparing alert...");
  lcdPrint("Sending SMS...", "Please wait");

  char locBuf[64];
  if (hasGPSFix) {
    sprintf(locBuf, "https://maps.google.com/?q=%.6f,%.6f", currentLat, currentLng);
  } else {
    strcpy(locBuf, "GPS unavailable.");
  }

  sendGSMCommand("AT+CMGF=1", 1000);
  GSM_SERIAL.print("AT+CMGS=\"");
  GSM_SERIAL.print(ALERT_PHONE_NUMBER);
  GSM_SERIAL.println("\"");
  delay(1000);

  GSM_SERIAL.println("*** ACCIDENT DETECTED ***");
  GSM_SERIAL.println();
  GSM_SERIAL.print("Vehicle : "); GSM_SERIAL.println(CAR_NAME);
  GSM_SERIAL.print("Plate   : "); GSM_SERIAL.println(CAR_NUMBER);
  GSM_SERIAL.print("Date    : "); GSM_SERIAL.println(getGPSDate());
  GSM_SERIAL.print("Time    : "); GSM_SERIAL.println(getGPSTime());
  GSM_SERIAL.print("G-Force : "); GSM_SERIAL.print(gForce, 2); GSM_SERIAL.println("G");
  GSM_SERIAL.println();
  GSM_SERIAL.println("Location:");
  GSM_SERIAL.println(locBuf);
  GSM_SERIAL.println();
  GSM_SERIAL.println("-- Auto Alert System --");

  GSM_SERIAL.write(26); // Ctrl+Z to send
  delay(5000);
  Serial.println("[SMS] Sent.");
}

// ─── GPRS: Connect to internet ───────────────────────────────────
bool gprsConnect() {
  Serial.println("[GPRS] Connecting...");
  sendGSMCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", 1000);
  GSM_SERIAL.print("AT+SAPBR=3,1,\"APN\",\"");
  GSM_SERIAL.print(GSM_APN);
  GSM_SERIAL.println("\"");
  delay(1000);
  sendGSMCommand("AT+SAPBR=1,1", 5000);
  sendGSMCommand("AT+SAPBR=2,1", 2000);
  Serial.println("[GPRS] Connected.");
  return true;
}

void gprsDisconnect() {
  sendGSMCommand("AT+HTTPTERM", 1000);
  sendGSMCommand("AT+SAPBR=0,1", 2000);
  Serial.println("[GPRS] Disconnected.");
}

// ─── HTTP POST to Flask/Render ────────────────────────────────────
void sendToFlaskAPI(float gForce) {
  Serial.println("[API] Posting to Render server...");
  lcdPrint("Posting to API", "Please wait...");

  // Build JSON body
  char locLatBuf[16] = "0.000000";
  char locLngBuf[16] = "0.000000";
  if (hasGPSFix) {
    dtostrf(currentLat, 1, 6, locLatBuf);
    dtostrf(currentLng, 1, 6, locLngBuf);
  }

  char gforceBuf[8];
  dtostrf(gForce, 1, 2, gforceBuf);

  char jsonBody[320];
  sprintf(jsonBody,
    "{\"vehicle\":\"%s\",\"plate\":\"%s\","
    "\"lat\":%s,\"lng\":%s,"
    "\"gforce\":%s,"
    "\"time\":\"%s\","
    "\"date\":\"%s\"}",
    CAR_NAME, CAR_NUMBER,
    locLatBuf, locLngBuf,
    gforceBuf,
    getGPSTime().c_str(),
    getGPSDate().c_str()
  );

  int bodyLen = strlen(jsonBody);
  Serial.print("[API] JSON: "); Serial.println(jsonBody);

  // GPRS connect
  if (!gprsConnect()) {
    Serial.println("[API] GPRS failed — skipping HTTP POST");
    return;
  }

  // HTTP POST
  sendGSMCommand("AT+HTTPINIT", 1000);
  sendGSMCommand("AT+HTTPPARA=\"CID\",1", 500);

  GSM_SERIAL.print("AT+HTTPPARA=\"URL\",\"http://");
  GSM_SERIAL.print(FLASK_HOST);
  GSM_SERIAL.print(FLASK_PATH);
  GSM_SERIAL.println("\"");
  delay(1000);

  sendGSMCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 500);

  GSM_SERIAL.print("AT+HTTPDATA=");
  GSM_SERIAL.print(bodyLen);
  GSM_SERIAL.println(",10000");
  if (!waitForResponse("DOWNLOAD", 5000)) {
    Serial.println("[API] DOWNLOAD prompt not received");
    gprsDisconnect();
    return;
  }

  GSM_SERIAL.print(jsonBody);
  delay(2000);

  sendGSMCommand("AT+HTTPACTION=1", 8000); // 1 = POST
  delay(5000);

  String response = "";
  unsigned long t = millis();
  while (millis() - t < 5000) {
    while (GSM_SERIAL.available()) {
      char c = GSM_SERIAL.read();
      response += c;
      Serial.write(c);
    }
  }

  if (response.indexOf("200") >= 0) {
    Serial.println("[API] Server responded 200 OK");
    lcdPrint("Dashboard", "Updated! :)");
  } else {
    Serial.println("[API] Server response error");
    lcdPrint("API Error", "Check server");
  }

  sendGSMCommand("AT+HTTPREAD", 2000);
  gprsDisconnect();
}

// ─── ACCIDENT HANDLER ─────────────────────────────────────────────
void triggerAccidentResponse(float gForce) {
  digitalWrite(RED_LED,   HIGH);
  digitalWrite(GREEN_LED, LOW);

  Serial.print("[ACCIDENT] G="); Serial.println(gForce);

  lcdPrint("!! ACCIDENT !!", "Locating GPS...");

  // Wait up to 5s for GPS fix
  unsigned long gpsWait = millis();
  while (!hasGPSFix && millis() - gpsWait < 5000) { feedGPS(500); }

  if (hasGPSFix) {
    char coordBuf[32];
    sprintf(coordBuf, "%.4f, %.4f", currentLat, currentLng);
    lcdPrint("Location Found", coordBuf);
    delay(1500);
  } else {
    lcdPrint("No GPS Fix", "Sending anyway");
    delay(1000);
  }

  // 1. Send SMS
  sendSMSAlert(gForce);

  // 2. Send to Flask / Render dashboard
  sendToFlaskAPI(gForce);

  lcdPrint("Alert Sent!", "System rearming");
  delay(3000);

  digitalWrite(RED_LED,   LOW);
  digitalWrite(GREEN_LED, HIGH);
}

// ─── SETUP ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  GPS_SERIAL.begin(9600);
  GSM_SERIAL.begin(9600);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED,   OUTPUT);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();

  // Boot screen
  lcdPrint("Accident Detect", "System v2.0");
  delay(2000);

  lcdPrint(CAR_NAME, CAR_NUMBER);
  delay(2000);

  // MPU6050 init
  lcdPrint("MPU6050", "Initialising...");
  delay(500);
  if (!mpu.begin()) {
    lcdPrint("MPU6050 ERROR", "Check wiring!");
    while (true) { delay(1000); }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  delay(500);
  Serial.println("[MPU6050] OK");

  // GSM init
  lcdPrint("GSM Module", "Initialising...");
  delay(3000);
  sendGSMCommand("AT",        2000);
  sendGSMCommand("AT+CMGF=1", 1000);
  sendGSMCommand("AT+CNMI=1,2,0,0,0", 1000);
  Serial.println("[GSM] Ready");

  // GPS
  lcdPrint("GPS Module", "Waiting fix...");
  Serial.println("[GPS] Waiting for fix...");
  delay(1000);

  // Armed
  lcdPrint("System ARMED", "Monitoring...");
  digitalWrite(GREEN_LED, HIGH);
  systemArmed = true;
  Serial.println("[SYSTEM] Armed and monitoring.");
}

// ─── MAIN LOOP ────────────────────────────────────────────────────
void loop() {
  feedGPS(200);

  float gForce = readGForce();

  // Update LCD with live status
  if (hasGPSFix) {
    char line1[17];
    char line2[17];
    sprintf(line1, "%02d:%02d %02d/%02d",
      gps.time.hour(), gps.time.minute(),
      gps.date.day(), gps.date.month());
    sprintf(line2, "G:%.1fG %s", gForce, CAR_NUMBER);
    lcdPrint(line1, line2);
  } else {
    char line2[17];
    sprintf(line2, "G:%.1fG No Fix", gForce);
    lcdPrint("GPS Searching...", line2);
  }

  // Debug to Serial Monitor
  Serial.print("G: "); Serial.print(gForce); Serial.print("G  |  GPS: ");
  Serial.println(hasGPSFix ? "FIX" : "NO FIX");

  // Check threshold
  unsigned long now = millis();
  if (gForce >= IMPACT_THRESHOLD_G && (now - lastAlertTime) >= COOLDOWN_MS) {
    lastAlertTime = now;
    triggerAccidentResponse(gForce);
  }

  delay(300);
}
