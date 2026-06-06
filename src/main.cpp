#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TinyGPSPlus.h>
#include <ctype.h>

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define SDA_PIN 21
#define SCL_PIN 22
#define STATUS_LED 2

// Pines para el módulo SIM800L (UART Secundaria remapeada)
#define SIM800L_RX_PIN 27  // Conectar al TX del SIM800L
#define SIM800L_TX_PIN 26  // Conectar al RX del SIM800L (usar divisor de tensión de ser necesario)

const uint8_t ADXL345_ADDR = 0x53;
const uint8_t ADXL345_DEVID = 0x00;
const uint8_t ADXL345_POWER_CTL = 0x2D;
const uint8_t ADXL345_DATA_FORMAT = 0x31;
const uint8_t ADXL345_DATAX0 = 0x32;

// Ajustados para modo de resolución estándar ±4g (128 LSB/g)
const float FALL_THRESHOLD_G = 2.5f;
const float FREEFALL_THRESHOLD_G = 0.30f; 
const uint32_t FALL_DEBOUNCE_MS = 3000U;
const unsigned long LOCATION_SMS_INTERVAL_MS = 120000UL;
const unsigned long FALL_SMS_INTERVAL_MS = 30000UL;
const unsigned long GPS_FALLBACK_TIMEOUT_MS = 300000UL;
const unsigned long INTERNET_LOCATION_RETRY_MS = 120000UL;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;

const char* WIFI_SSID = "indega";
const char* WIFI_PASSWORD = "lcdawireless";

// Credenciales de Twilio
const char* TWILIO_ACCOUNT_SID = "AC8ce3757770a7476ff512445d24158a92";
const char* TWILIO_AUTH_TOKEN = "2111027ac55c74dfc906f51c324cc42f";
const char* TWILIO_FROM_NUMBER = "+18312434648";
const char* TWILIO_TO_NUMBER = "+595985805505"; // Destinatario (Formato internacional)

// Constantes de token de Unwired Labs
const char* UNWIRED_LABS_API_KEY = "pk.e797b33bae51eee7d9ed1bad47e90e67";

TinyGPSPlus gps;
#define gpsSerial Serial2

// Instancia de puerto serie por software/hardware para el SIM800L
HardwareSerial simSerial(1); 

WiFiClientSecure secureClient;

bool adxlDetected = false;
bool internetLocationValid = false;
float internetLat = 0.0f;
float internetLng = 0.0f;
unsigned long startTime = 0UL;
unsigned long lastInitMsg = 0UL;
unsigned long lastFallTime = 0UL;
unsigned long lastAccelRead = 0UL;
unsigned long lastStatusPrint = 0UL;
unsigned long lastLocationSms = 0UL;
unsigned long lastFallSms = 0UL;
unsigned long lastInternetAttempt = 0UL;

uint32_t totalCharsReceived = 0U;
uint32_t totalSentencesReceived = 0U;
uint32_t loopCount = 0U;
uint32_t unknownSentences = 0U;
uint32_t ggaSentences = 0U;
uint32_t rmcSentences = 0U;
uint32_t gllSentences = 0U;
uint32_t gsaSentences = 0U;
uint32_t gsvSentences = 0U;
uint32_t gssSentences = 0U;
uint32_t vtgSentences = 0U;
bool lastFixValid = false;
unsigned long lastFixTime = 0UL;
String nmeaLine = "";

// --- CONTROL DE REDUNDANCIA SIM800L / TWILIO ---
uint8_t sim800lFailCount = 0;
const uint8_t MAX_SIM_FAILURES = 10;

// Prototipos de funciones
void sendFallSms();
void sendLocationSms2Min();
void procesarAcelerometro();
bool sendSms(const String &body);
bool sendSmsSIM800L(const String &body);
bool sendSmsTwilio(const String &body);
bool fetchInternetLocation();
bool ensureWiFiConnected();
void initSIM800L();

String urlEncode(const String &value) {
  String encoded = "";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

bool isSmsConfigured() {
  return strlen(WIFI_SSID) > 0 && strlen(WIFI_PASSWORD) > 0 &&
         strlen(TWILIO_ACCOUNT_SID) > 2 && strlen(TWILIO_AUTH_TOKEN) > 2 &&
         strlen(TWILIO_FROM_NUMBER) > 5 && strlen(TWILIO_TO_NUMBER) > 5;
}

void connectWiFi() {
  Serial.printf("Conectando a WiFi %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
    yield(); 
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No se pudo conectar a WiFi.");
  }
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  connectWiFi();
  return WiFi.status() == WL_CONNECTED;
}

float parseJsonFloat(const String &json, const char *key, bool &ok) {
  ok = false;
  int start = json.indexOf(key);
  if (start < 0) {
    return 0.0f;
  }
  start += strlen(key);
  while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) {
    start++;
  }
  int end = start;
  while (end < json.length() && (isDigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == '+')) {
    end++;
  }
  if (end <= start) {
    return 0.0f;
  }
  String value = json.substring(start, end);
  ok = true;
  return value.toFloat();
}

bool fetchInternetLocation() {
  if (millis() - lastInternetAttempt < INTERNET_LOCATION_RETRY_MS) {
    return internetLocationValid;
  }
  lastInternetAttempt = millis();

  Serial.println("\n[WPS] Iniciando escaneo de redes WiFi vecinas para triangulación precisa...");
  WiFi.disconnect();
  delay(100);
  
  int n = WiFi.scanNetworks();
  Serial.printf("[WPS] Se encontraron %d redes en el área.\n", n);
  
  if (n == 0) {
    Serial.println("[WPS] Error: No se detectaron redes vecinas. Cancelando triangulación.");
    ensureWiFiConnected(); 
    return false;
  }

  String jsonPayload = "{\n";
  jsonPayload += "  \"token\": \"" + String(UNWIRED_LABS_API_KEY) + "\",\n";
  jsonPayload += "  \"fallbacks\": {\"ipf\": \"false\"},\n"; 
  jsonPayload += "  \"wifi\": [\n";

  int maxRedes = (n > 6) ? 6 : n; 
  for (int i = 0; i < maxRedes; ++i) {
    jsonPayload += "    {\n";
    jsonPayload += "      \"bssid\": \"" + WiFi.BSSIDstr(i) + "\",\n";
    jsonPayload += "      \"signal\": " + String(WiFi.RSSI(i)) + "\n";
    jsonPayload += "    }";
    if (i < maxRedes - 1) jsonPayload += ",\n";
    else jsonPayload += "\n";
  }
  jsonPayload += "  ]\n}";

  if (!ensureWiFiConnected()) {
    Serial.println("[WPS] No se pudo reconectar al WiFi para enviar los datos.");
    return false;
  }

  HTTPClient http;
  const char* url = "https://us1.unwiredlabs.com/v2/process.php";
  
  secureClient.setInsecure(); 
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  Serial.println("[WPS] Enviando mapa de celdas WiFi a la API...");
  int code = http.POST(jsonPayload);
  
  if (code != HTTP_CODE_OK) {
    Serial.printf("[WPS] Error en la petición HTTPS: %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  bool okLat = false;
  bool okLon = false;
  float lat = parseJsonFloat(payload, "\"lat\":", okLat);
  float lon = parseJsonFloat(payload, "\"lon\":", okLon); 
  
  if (okLat && okLon) {
    internetLat = lat;
    internetLng = lon;
    internetLocationValid = true;
    return true;
  }
  return false;
}

/**
 * @brief Orquestador inteligente de SMS con tolerancia a fallas.
 * Intenta usar el hardware local (SIM800L). Si este falla de manera sostenida
 * acumulando 10 errores, conmuta automáticamente a la pasarela IP de Twilio.
 */
bool sendSms(const String &body) {
  if (sim800lFailCount < MAX_SIM_FAILURES) {
    Serial.printf("[SMS] Intentando envío vía SIM800L (Fallas acumuladas: %d/%d)...\n", sim800lFailCount, MAX_SIM_FAILURES);
    if (sendSmsSIM800L(body)) {
      sim800lFailCount = 0; // Resetea el contador ante un éxito
      return true;
    } else {
      sim800lFailCount++;
      Serial.printf("[SMS] Error en SIM800L. Contador de fallas incrementado a: %d\n", sim800lFailCount);
      
      // Si justo este intento fue el décimo fallo, disparamos Twilio inmediatamente como contingencia
      if (sim800lFailCount >= MAX_SIM_FAILURES) {
        Serial.println("\n⚠ CRÍTICO: Umbral de fallas del SIM800L alcanzado. Conmutando a CONTINGENCIA (Twilio)...");
        return sendSmsTwilio(body);
      }
      return false;
    }
  } else {
    Serial.println("[SMS] Canal SIM800L deshabilitado por fallas persistentes. Usando Twilio...");
    return sendSmsTwilio(body);
  }
}

/**
 * @brief Despacho de SMS nativo mediante comandos AT para el SIM800L.
 */
bool sendSmsSIM800L(const String &body) {
  // Limpia cualquier residuo en el buffer serie
  while(simSerial.available()) simSerial.read();

  simSerial.print("AT+CMGS=\"");
  simSerial.print(TWILIO_TO_NUMBER); // Reutiliza el número de destino configurado
  simSerial.println("\"");
  
  delay(200); // Pequeña ventana de tiempo esperando el caracter '>' de respuesta
  
  // Enviamos el cuerpo del mensaje
  simSerial.print(body);
  simSerial.write(26); // Envía el comando de cierre CTRL+Z (ASCII 26)
  
  unsigned long start = millis();
  String response = "";
  // Espera la respuesta del módem por un máximo de 5 segundos
  while (millis() - start < 5000UL) {
    while (simSerial.available()) {
      char c = simSerial.read();
      response += c;
    }
    if (response.indexOf("OK") != -1 && response.indexOf("+CMGS:") != -1) {
      Serial.println("[SIM800L] SMS enviado con éxito de forma local.");
      return true;
    }
    if (response.indexOf("ERROR") != -1) {
      break;
    }
    yield();
  }
  Serial.println("[SIM800L] Falla o timeout en la confirmación del comando AT.");
  return false;
}

/**
 * @brief Envío redundante vía Twilio Cloud Gateway.
 */
bool sendSmsTwilio(const String &body) {
  if (!isSmsConfigured()) {
    Serial.println("[Twilio] Error: Credenciales de red/Twilio incompletas.");
    return false;
  }
  if (!ensureWiFiConnected()) {
    Serial.println("[Twilio] Cancelado: Sin conexión WiFi.");
    return false;
  }

  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://api.twilio.com/2010-04-01/Accounts/") + TWILIO_ACCOUNT_SID + "/Messages.json";
  http.begin(secureClient, url);
  http.setAuthorization(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload = "To=" + urlEncode(TWILIO_TO_NUMBER);
  payload += "&From=" + urlEncode(TWILIO_FROM_NUMBER); 
  payload += "&Body=" + urlEncode(body);

  int httpCode = http.POST(payload);
  http.end();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("[Twilio] SMS de respaldo enviado correctamente.");
    return true;
  }
  Serial.printf("[Twilio] Error HTTP: %d\n", httpCode);
  return false;
}

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
  return (readRegister(ADXL345_DEVID) == 0xE5); 
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

void procesarAcelerometro() {
  if (!adxlDetected) return;

  int16_t xRaw = readAxis(ADXL345_DATAX0);
  int16_t yRaw = readAxis(ADXL345_DATAX0 + 2);
  int16_t zRaw = readAxis(ADXL345_DATAX0 + 4);

  float xg = xRaw / 128.0f;
  float yg = yRaw / 128.0f;
  float zg = zRaw / 128.0f;
  float mag = accelMagnitude(xg, yg, zg);

  if (millis() - lastAccelRead >= 10000UL) {
    lastAccelRead = millis();
  }

  if (detectFall(xg, yg, zg) && (millis() - lastFallTime > FALL_DEBOUNCE_MS)) {
    lastFallTime = millis();
    Serial.printf("\n*** ALERTA: CAIDA DETECTADA (|M| = %.3fg) ***\n", mag);
    sendFallSms();
  }
}

void trackNMEASentence(const String &sentence) {
  String line = sentence;
  line.trim();
  if (line.length() == 0 || line.charAt(0) != '$') return;
  if (line.startsWith("$GPGGA") || line.startsWith("$GNGGA") || line.startsWith("$BDGGA")) ggaSentences++;
  else if (line.startsWith("$GPRMC") || line.startsWith("$GNRMC")) rmcSentences++;
  else if (line.startsWith("$GPGLL") || line.startsWith("$GNGLL")) gllSentences++;
  else unknownSentences++;
}

void displayLocationInfo() {
  Serial.println("\n==== INFORMACIÓN DE UBICACIÓN ====");
  if (gps.location.isValid()) {
    Serial.printf("Latitud:  %.6f %s\n", gps.location.lat(), gps.location.rawLat().negative ? "S" : "N");
    Serial.printf("Longitud: %.6f %s\n", gps.location.lng(), gps.location.rawLng().negative ? "W" : "E");
  } else if (internetLocationValid) {
    Serial.printf("Latitud internet:  %.6f (WiFi Fallback)\n", internetLat);
    Serial.printf("Longitud internet: %.6f (WiFi Fallback)\n", internetLng);
  } else {
    Serial.println("Ubicación inválida / Buscando satélites...");
  }
}

void displayGPSStatus() {
  Serial.printf("\n---- ESTADO GPS ----\nChars RX: %d | Sentencias: %d | Satélites: %d\n", 
                totalCharsReceived, totalSentencesReceived, gps.satellites.value());
}

void toggleStatusLED() {
  digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
}

void sendFallSms() {
  if (millis() - lastFallSms < FALL_SMS_INTERVAL_MS) return;
  lastFallSms = millis();

  String body = "ALERTA CAIDA ESP32. Ubicacion: ";
  if (gps.location.isValid()) {
    body += "https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
  } else if (internetLocationValid) {
    body += "https://maps.google.com/?q=" + String(internetLat, 6) + "," + String(internetLng, 6) + " (Wifi Fallback)";
  } else {
    body += "sin ubicacion disponible";
  }
  sendSms(body);
}

void sendLocationSms2Min() {
  lastLocationSms = millis(); 

  String body = "Estado del paciente Ubicacion: ";
  if (gps.location.isValid()) {
    body += "https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
  } else if (internetLocationValid) {
    body += "https://maps.google.com/?q=" + String(internetLat, 6) + "," + String(internetLng, 6) + " (Wifi Fallback)";
  } else {
    body += "sin ubicacion disponible";
  }
  body += " - Estado de Caida: ";
  body += (lastFallTime == 0UL) ? "Ninguna registrada" : String((millis() - lastFallTime) / 1000UL) + "s desde el impacto";

  sendSms(body);
}

/**
 * @brief Configuración del módem en modo texto plano para SMS.
 */
void initSIM800L() {
  Serial.println("Inicializando SIM800L...");
  simSerial.println("AT"); // Test de baudrate
  delay(200);
  simSerial.println("AT+CMGF=1"); // Configura modo TEXTO para SMS
  delay(200);
  simSerial.println("AT+CSCS=\"GSM\""); // Set de caracteres estándar
  delay(200);
  Serial.println("SIM800L configurado en Modo Texto.");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  Serial.println("\n======================================");
  Serial.println("   ALERTA DE UBICACIÓN Y CAÍDA ESP32  ");
  Serial.println("======================================\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); 

  // Inicializa UART interna 1 para el SIM800L (remapa pines libres)
  simSerial.begin(9600, SERIAL_8N1, SIM800L_RX_PIN, SIM800L_TX_PIN);
  initSIM800L();

  connectWiFi();

  Serial.println("Iniciando ADXL345...");
  adxlDetected = detectADXL345();
  if (adxlDetected) {
    Serial.println("ADXL345 en línea y verificado.");
    initADXL345();
  } else {
    Serial.println("ADXL345 NO responde.");
  }

  Serial.println("Iniciando GPS en Serial2 a 9600...");
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  startTime = millis();
}

void loop() {
  loopCount++;
  bool isColdStart = (millis() - startTime) < GPS_FALLBACK_TIMEOUT_MS;

  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    totalCharsReceived++;
    if (gps.encode(c)) {
      totalSentencesReceived++;
      toggleStatusLED();
    }
    if (c == '\r') continue;
    if (c == '\n') {
      if (nmeaLine.length() > 0) {
        trackNMEASentence(nmeaLine);
        nmeaLine = "";
      }
      continue;
    }
    if (nmeaLine.length() < 128) nmeaLine += c;
  }

  if (!gps.location.isValid() && !internetLocationValid && !isColdStart && (millis() - lastInternetAttempt >= INTERNET_LOCATION_RETRY_MS)) {
    fetchInternetLocation();
  }

  procesarAcelerometro();

  if (millis() - lastStatusPrint >= 5000UL) {
    displayLocationInfo();
    displayGPSStatus();
    lastStatusPrint = millis();
  }

  // LAZO DE CONTROL UNIFICADO (CADA 2 MINUTOS)
  if (millis() - lastLocationSms >= LOCATION_SMS_INTERVAL_MS) {
    if (!gps.location.isValid() && !isColdStart) {
      fetchInternetLocation(); 
    }
    sendLocationSms2Min(); 
  }

  if (!isColdStart && totalCharsReceived < 10) {
    Serial.println("\n⚠ ALERTA: No llegan datos lógicos del GPS. Revisa RX/TX.");
    delay(1000); 
  }

  delay(10); 
}
