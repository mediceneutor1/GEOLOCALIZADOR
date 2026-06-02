#include <Arduino.h>
#include <Wire.h>
#include <TinyGPSPlus.h>

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define SDA_PIN 21
#define SCL_PIN 22
#define STATUS_LED 2

const uint8_t ADXL345_ADDR = 0x53;
const uint8_t ADXL345_DEVID = 0x00;
const uint8_t ADXL345_POWER_CTL = 0x2D;
const uint8_t ADXL345_DATA_FORMAT = 0x31;
const uint8_t ADXL345_DATAX0 = 0x32;

const float FALL_THRESHOLD_G = 2.5f;
const float FREEFALL_THRESHOLD_G = 0.60f;
const uint32_t FALL_DEBOUNCE_MS = 3000;

TinyGPSPlus gps;
#define gpsSerial Serial2

bool adxlDetected = false;
bool gpsReady = false;
unsigned long startTime = 0;
unsigned long lastInitMsg = 0;
unsigned long lastFallTime = 0;
unsigned long lastAccelRead = 0;
unsigned long lastStatusPrint = 0;

uint32_t totalCharsReceived = 0;
uint32_t totalSentencesReceived = 0;
uint32_t loopCount = 0;
uint32_t unknownSentences = 0;
uint32_t ggaSentences = 0;
uint32_t rmcSentences = 0;
uint32_t gsaSentences = 0;
uint32_t gssSentences = 0;
uint32_t gllSentences = 0;
uint32_t gsvSentences = 0;
uint32_t vtgSentences = 0;
bool lastFixValid = false;
unsigned long lastFixTime = 0;
String nmeaLine = "";

void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL345_ADDR, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0;
}

int16_t readAxis(uint8_t regLow) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(regLow);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL345_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return 0;
  uint8_t low = Wire.read();
  uint8_t high = Wire.read();
  return (int16_t)((high << 8) | low);
}

bool detectADXL345() {
  Wire.beginTransmission(ADXL345_ADDR);
  if (Wire.endTransmission() != 0) return false;
  uint8_t id = readRegister(ADXL345_DEVID);
  return (id != 0x00);
}

void initADXL345() {
  writeRegister(ADXL345_POWER_CTL, 0x08);
  writeRegister(ADXL345_DATA_FORMAT, 0x01);
  delay(50);
}

float accelMagnitude(float xg, float yg, float zg) {
  return sqrt(xg * xg + yg * yg + zg * zg);
}

bool detectFall(float xg, float yg, float zg) {
  float magnitude = accelMagnitude(xg, yg, zg);
  return (magnitude >= FALL_THRESHOLD_G || magnitude <= FREEFALL_THRESHOLD_G);
}

void printADXLData() {
  if (!adxlDetected) {
    Serial.println("[ADXL345] No detectado, verifica I2C/power.");
    return;
  }

  int16_t xRaw = readAxis(ADXL345_DATAX0);
  int16_t yRaw = readAxis(ADXL345_DATAX0 + 2);
  int16_t zRaw = readAxis(ADXL345_DATAX0 + 4);

  float xg = xRaw / 256.0f;
  float yg = yRaw / 256.0f;
  float zg = zRaw / 256.0f;
  float mag = accelMagnitude(xg, yg, zg);

  Serial.print("[ADXL345] X: "); Serial.print(xg, 3); Serial.print(" g");
  Serial.print("  Y: "); Serial.print(yg, 3); Serial.print(" g");
  Serial.print("  Z: "); Serial.print(zg, 3); Serial.print(" g");
  Serial.print("  |M| = "); Serial.print(mag, 3); Serial.println(" g");

  if (detectFall(xg, yg, zg) && millis() - lastFallTime > FALL_DEBOUNCE_MS) {
    lastFallTime = millis();
    Serial.println("*** ALERTA: POSIBLE CAÍDA DETECTADA ***");
  }
}

void trackNMEASentence(const String& sentence) {
  String line = sentence;
  line.trim();
  if (line.length() == 0 || line.charAt(0) != '$') return;

  if (line.startsWith("$GPGGA") || line.startsWith("$GNGGA") || line.startsWith("$BDGGA")) ggaSentences++;
  else if (line.startsWith("$GPRMC") || line.startsWith("$GNRMC")) rmcSentences++;
  else if (line.startsWith("$GPGLL") || line.startsWith("$GNGLL")) gllSentences++;
  else if (line.startsWith("$GPGSA") || line.startsWith("$GNGSA")) gsaSentences++;
  else if (line.startsWith("$GPGSV") || line.startsWith("$GNGSV")) gsvSentences++;
  else if (line.startsWith("$GPGSS") || line.startsWith("$GNGSS")) gssSentences++;
  else if (line.startsWith("$GPVTG") || line.startsWith("$GNVTG")) vtgSentences++;
  else unknownSentences++;
}

void displayLocationInfo() {
  Serial.println("\n==== INFORMACIÓN DE UBICACIÓN ====");
  if (gps.location.isValid()) {
    Serial.print("Latitud:  "); Serial.print(gps.location.lat(), 6); Serial.print(" "); Serial.println(gps.location.rawLat().negative ? "S" : "N");
    Serial.print("Longitud: "); Serial.print(gps.location.lng(), 6); Serial.print(" "); Serial.println(gps.location.rawLng().negative ? "W" : "E");
  } else {
    Serial.println("Ubicación inválida");
  }

  if (gps.date.isValid()) {
    Serial.printf("Fecha UTC: %02d/%02d/%04d\n", gps.date.day(), gps.date.month(), gps.date.year());
  } else {
    Serial.println("Fecha inválida");
  }
  if (gps.time.isValid()) {
    Serial.printf("Hora UTC:  %02d:%02d:%02d\n", gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    Serial.println("Hora inválida");
  }

  if (gps.speed.isValid()) {
    Serial.print("Velocidad: "); Serial.print(gps.speed.kmph(), 2); Serial.println(" km/h");
  }
  if (gps.course.isValid()) {
    Serial.print("Rumbo:     "); Serial.print(gps.course.deg(), 2); Serial.println(" °");
  }
  if (gps.altitude.isValid()) {
    Serial.print("Altitud:   "); Serial.print(gps.altitude.meters(), 2); Serial.println(" m");
  }
}

void displayGPSStatus() {
  Serial.println("\n---- ESTADO GPS ----");
  Serial.print("Ciclos: "); Serial.println(loopCount);
  Serial.print("Chars RX: "); Serial.println(totalCharsReceived);
  Serial.print("Sentencias: "); Serial.println(totalSentencesReceived);
  Serial.print("Fix válido: "); Serial.println(gps.location.isValid() ? "SI" : "NO");
  Serial.print("Satélites: "); Serial.println(gps.satellites.value());
  Serial.print("Checksum fallidos: "); Serial.println(gps.failedChecksum());
  if (gps.location.isValid()) {
    lastFixValid = true;
    lastFixTime = millis();
  }
}

void toggleStatusLED() {
  digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  Serial.println("\n======================================");
  Serial.println("  ALERTA DE UBICACIÓN Y CAÍDA ESP32  ");
  Serial.println("======================================\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  Serial.println("Iniciando ADXL345...");
  adxlDetected = detectADXL345();
  if (adxlDetected) {
    Serial.println("ADXL345 detectado.");
    initADXL345();
  } else {
    Serial.println("ADXL345 NO detectado. Verifica SDA/SCL y alimentación.");
  }

  Serial.println("Iniciando GPS en Serial2...");
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  startTime = millis();
}

void loop() {
  loopCount++;
  bool isColdStart = (millis() - startTime) < 300000UL;

  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    totalCharsReceived++;
    if (c == '\r') continue;
    if (c == '\n') {
      if (nmeaLine.length() > 0) {
        trackNMEASentence(nmeaLine);
        nmeaLine = "";
      }
      continue;
    }
    if (nmeaLine.length() < 128) nmeaLine += c;

    if (gps.encode(c)) {
      totalSentencesReceived++;
      toggleStatusLED();
      gpsReady = true;
    }
  }

  if (millis() - lastAccelRead >= 10000) {
    printADXLData();
    lastAccelRead = millis();
  }

  if (millis() - lastStatusPrint >= 5000) {
    if (gps.location.isValid()) {
      displayLocationInfo();
    } else if (isColdStart && millis() - lastInitMsg > 5000) {
      Serial.println("GPS inicializando... Espera fix válido.");
      lastInitMsg = millis();
    }
    displayGPSStatus();
    Serial.print("Última alerta caída: ");
    if (lastFallTime == 0) {
      Serial.println("ninguna");
    } else {
      Serial.print((millis() - lastFallTime) / 1000);
      Serial.println(" seg atrás");
    }
    lastStatusPrint = millis();
  }

  if (!isColdStart && !gpsReady && gps.charsProcessed() < 10) {
    Serial.println("\n⚠ ERROR: GPS no responde. Revisa conexiones, alimentación y baudrate.");
    delay(5000);
  }

  delay(100);
}
