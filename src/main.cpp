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
// const char* UNWIRED_LABS_API_KEY = "pk.e797b33bae51eee7d9ed1bad47e90e67"; // Reemplaza con tu clave real de Unwired Labs
const char* WIFI_SSID = "indega";
const char* WIFI_PASSWORD = "lcdawireless";
// Reemplaza con tus credenciales de Twilio (SID de cuenta, token de autenticación, número de teléfono de origen y destino)
const char* TWILIO_ACCOUNT_SID = "AC8ce3757770a7476ff512445d24158a92";
const char* TWILIO_AUTH_TOKEN = "2111027ac55c74dfc906f51c324cc42f";
// Reemplaza con tu número de teléfono de Twilio (debe incluir el código de país, por ejemplo, +1 para EE.UU.)
const char* TWILIO_FROM_NUMBER = "+18312434648";
const char* TWILIO_TO_NUMBER = "+595985805505";
//  constantes de token de la pagina de triangulacion gps
const char* UNWIRED_LABS_API_KEY = "pk.e797b33bae51eee7d9ed1bad47e90e67";
TinyGPSPlus gps;
#define gpsSerial Serial2

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

void sendFallSms();
void sendLocationSms();
void procesarAcelerometro();

/**
 * @brief Codifica una cadena de texto en formato URL (Percent-encoding).
 * Transforma caracteres especiales y espacios en secuencias válidas para
 * peticiones HTTP POST con contenido 'application/x-www-form-urlencoded'.
 */
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

/**
 * @brief Validación estática inicial de credenciales.
 * Comprueba que las longitudes de los vectores de caracteres destinados a la
 * conectividad y plataforma Twilio contengan datos mínimos obligatorios.
 */
bool isSmsConfigured() {
  return strlen(WIFI_SSID) > 0 && strlen(WIFI_PASSWORD) > 0 &&
         strlen(TWILIO_ACCOUNT_SID) > 2 && strlen(TWILIO_AUTH_TOKEN) > 2 &&
         strlen(TWILIO_FROM_NUMBER) > 5 && strlen(TWILIO_TO_NUMBER) > 5;
}

/**
 * @brief Inicializa y gestiona la conexión a la red de área local inalámbrica (WLAN).
 * Llama a la API de Wi-Fi del ESP32 de forma asíncrona implementando un bucle de guarda
 * basado en 'millis()' para evitar bloqueos indefinidos del procesador si el AP no responde.
 */
void connectWiFi() {
  Serial.printf("Conectando a WiFi %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
    yield(); // Alimenta y evita el disparo del perro guardián (Watchdog Timer) del ESP32
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No se pudo conectar a WiFi.");
  }
}

/**
 * @brief Hilo supervisor de enlace (Keep-Alive) para la capa de red.
 * Evalúa de forma no bloqueante el estado del stack de red inalámbrico. En caso
 * de desconexión física o lógica, fuerza un intento guiado de reconexión.
 */
bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  connectWiFi();
  return WiFi.status() == WL_CONNECTED;
}

/**
 * @brief Parseador manual robusto y eficiente para tipos de datos flotantes en JSON.
 * Diseñado específicamente para omitir espacios en blanco o tabulaciones tras los delimitadores
 * clave (como ": "), localizando el número mediante subcadenas y convirtiéndolo a primitivo 'float'.
 */
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

/**
 * @brief Mecanismo de Contingencia WPS (Sistema de Posicionamiento Wi-Fi) en Interiores.
 * Al perderse el enlace satelital GPS, esta función detiene el enlace IP, realiza un escaneo
 * pasivo del espectro de 2.4GHz capturando las direcciones físicas de hardware (BSSID/MAC) 
 * y potencias (RSSI) de los routers circundantes. Reestablece internet y envía un JSON a la 
 * API de Unwired Labs, retornando coordenadas terrestres de alta precisión basadas en mapas de celdas.
 */
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
  
  secureClient.setInsecure(); // Salta la validación criptográfica de la cadena de certificados para ahorrar memoria RAM estática (SRAM)
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  Serial.println("[WPS] Enviando mapa de celdas WiFi a la API...");
  int code = http.POST(jsonPayload);
  
  if (code != HTTP_CODE_OK) {
    Serial.printf("[WPS] Error en la petición HTTPS: %d\n", code);
    Serial.println(http.getString());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  bool okLat = false;
  bool okLon = false;
  float lat = parseJsonFloat(payload, "\"lat\":", okLat);
  float lon = parseJsonFloat(payload, "\"lon\":", okLon); 
  
  bool okAcc = false;
  float accuracy = parseJsonFloat(payload, "\"accuracy\":", okAcc);

  if (okLat && okLon) {
    internetLat = lat;
    internetLng = lon;
    internetLocationValid = true;
    
    Serial.println("\n==================================================");
    Serial.println("[WPS] ¡TRIANGULACIÓN WIFI EXITOSA!");
    Serial.printf("Latitud calculada:  %.6f\n", internetLat);
    Serial.printf("Longitud calculada: %.6f\n", internetLng);
    if (okAcc) {
      Serial.printf("Margen de error de la API: %.1f metros\n", accuracy);
    }
    Serial.println("==================================================\n");
    return true;
  }

  Serial.println("[WPS] Error: El servidor respondió pero no se pudieron parsear las coordenadas.");
  return false;
}

/**
 * @brief Pasarela de red hacia la API de mensajería comercial (Twilio Cloud Gateway).
 * Configura sockets TLS mediante WiFiClientSecure. Inicia una petición de red del tipo
 * HTTP POST inyectando credenciales codificadas en Base64 (Basic Authentication) y
 * despacha los buffers empaquetados hacia la red celular internacional.
 */
bool sendSms(const String &body) {
  if (!isSmsConfigured()) {
    Serial.println("SMS no configurado: define credenciales Twilio y WiFi.");
    return false;
  }
  if (!ensureWiFiConnected()) {
    Serial.println("SMS cancelado: WiFi no conectado.");
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
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("SMS enviado correctamente.");
    http.end();
    return true;
  }

  Serial.printf("Error SMS: %d -> %s\n", httpCode, http.errorToString(httpCode).c_str());
  Serial.println(http.getString());
  http.end();
  return false;
}

/**
 * @brief Primitiva de bajo nivel para escritura en bus I2C.
 * Abre transmisiones directas hacia la dirección esclava del ADXL345, indexa
 * el registro de memoria de hardware deseado e inyecta la configuración en bytes.
 */
void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

/**
 * @brief Primitiva de bajo nivel para lectura en bus I2C.
 * Apunta al registro destino del transductor y ejecuta una petición 'requestFrom' 
 * secuencial forzando la lectura síncrona del dato disponible en el buffer I2C.
 */
uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL345_ADDR, (uint8_t)1);
  if (Wire.available()) {
    return Wire.read();
  }
  return 0;
}

/**
 * @brief Lee palabras de 16 bits del espacio de memoria del transductor.
 * Los datos binarios de aceleración del ADXL345 se almacenan fragmentados en dos
 * registros contiguos de 8 bits (Bajo/Alto). Esta función realiza una combinación lógica
 * mediante operaciones de desplazamiento a nivel de bits (Bitwise shift y OR) para unificar la lectura.
 */
int16_t readAxis(uint8_t regLow) {
  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(regLow);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL345_ADDR, (uint8_t)2);
  if (Wire.available() < 2) {
    return 0;
  }
  uint8_t low = Wire.read();
  uint8_t high = Wire.read();
  return (int16_t)((high << 8) | low);
}

/**
 * @brief Comprobación física de hardware del transductor en bus I2C.
 * Realiza un ping eléctrico a la dirección esclava y solicita el registro de identidad
 * de dispositivo (DEVID). Retorna 'true' si detecta la firma invariable real del chip (0xE5).
 */
bool detectADXL345() {
  Wire.beginTransmission(ADXL345_ADDR);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  uint8_t id = readRegister(ADXL345_DEVID);
  return (id == 0xE5); 
}

/**
 * @brief Configura el mapa de registros operativos del acelerómetro digital.
 * Despierta al sensor del modo standby pasándolo a "Measurement Mode" e inicializa
 * el registro DATA_FORMAT en resolución estándar balanceada de ±4g.
 */
void initADXL345() {
  writeRegister(ADXL345_POWER_CTL, 0x08);    
  writeRegister(ADXL345_DATA_FORMAT, 0x01);   
  delay(50);
}

/**
 * @brief Calcula el módulo del vector de aceleración espacial (Magnitud R).
 * Ejecuta algebraicamente la raíz cuadrada de la sumatoria de las componentes
 * ortogonales al cuadrado ($|M| = \sqrt{x^2 + y^2 + z^2}$), obteniendo la fuerza G total.
 */
float accelMagnitude(float xg, float yg, float zg) {
  return sqrt(xg * xg + yg * yg + zg * zg);
}

/**
 * @brief Filtro de umbrales cinemáticos para la detección física de caídas e impactos.
 * Analiza el comportamiento inercial mediante dos flancos críticos:
 * 1. Caída Libre (Free-fall): Magnitud por debajo de 0.30g debido a la ingravidez del descenso.
 * 2. Impacto Severo (Shock): Picos de desaceleración abrupta que superen o igualen las 2.5g.
 */
bool detectFall(float xg, float yg, float zg) {
  float magnitude = accelMagnitude(xg, yg, zg);
  return (magnitude >= FALL_THRESHOLD_G || magnitude <= FREEFALL_THRESHOLD_G);
}

/**
 * @brief Subsistema de Procesamiento Analógico-Digital del Acelerómetro.
 * Extrae los bytes en bruto del bus I2C, aplica el factor de conversión de escala
 * (128.0 LSB/g) para convertirlos a valores físicos de aceleración gravitatoria, despacha
 * la telemetría depurada a consola y monitoriza picos inerciales aplicando un antirebote por software.
 */
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
    Serial.print("[ADXL345] X: "); Serial.print(xg, 3); Serial.print(" g");
    Serial.print("  Y: "); Serial.print(yg, 3); Serial.print(" g");
    Serial.print("  Z: "); Serial.print(zg, 3); Serial.print(" g");
    Serial.print("  |M| = "); Serial.print(mag, 3); Serial.println(" g");
    lastAccelRead = millis();
  }

  if (detectFall(xg, yg, zg) && (millis() - lastFallTime > FALL_DEBOUNCE_MS)) {
    lastFallTime = millis();
    Serial.printf("\n*** ALERTA: POSIBLE CAIDA DETECTADA (|M| = %.3fg) ***\n", mag);
    sendFallSms();
  }
}

/**
 * @brief Analizador de cabeceras NMEA (National Marine Electronics Association) para Tesis.
 * Recibe cadenas de texto estructuradas del puerto serie y realiza un desglose lógico por software
 * discriminando los tipos de tramas procesadas (GGA, RMC, GLL, GSV) para auditar la calidad del canal.
 */
void trackNMEASentence(const String &sentence) {
  String line = sentence;
  line.trim();
  if (line.length() == 0 || line.charAt(0) != '$') {
    return;
  }
  if (line.startsWith("$GPGGA") || line.startsWith("$GNGGA") || line.startsWith("$BDGGA")) {
    ggaSentences++;
  } else if (line.startsWith("$GPRMC") || line.startsWith("$GNRMC")) {
    rmcSentences++;
  } else if (line.startsWith("$GPGLL") || line.startsWith("$GNGLL")) {
    gllSentences++;
  } else if (line.startsWith("$GPGSA") || line.startsWith("$GNGSA")) {
    gsaSentences++;
  } else if (line.startsWith("$GPGSV") || line.startsWith("$GNGSV")) {
    gsvSentences++;
  } else if (line.startsWith("$GPGSS") || line.startsWith("$GNGSS")) {
    gssSentences++;
  } else if (line.startsWith("$GPVTG") || line.startsWith("$GNVTG")) {
    vtgSentences++;
  } else {
    unknownSentences++;
  }
}

/**
 * @brief Impresor de la Capa de Aplicación para datos geográficos.
 * Despliega en consola el origen de los datos de ubicación activos (Satélite Directo o Fallback WPS),
 * aplicando formateo matemático de precisión con 6 decimales junto a marcas temporales coordinadas (UTC).
 */
void displayLocationInfo() {
  Serial.println("\n==== INFORMACIÓN DE UBICACIÓN ====");
  if (gps.location.isValid()) {
    Serial.print("Latitud:  "); Serial.print(gps.location.lat(), 6); Serial.print(" "); Serial.println(gps.location.rawLat().negative ? "S" : "N");
    Serial.print("Longitud: "); Serial.print(gps.location.lng(), 6); Serial.print(" "); Serial.println(gps.location.rawLng().negative ? "W" : "E");
  } else if (internetLocationValid) {
    Serial.print("Latitud internet:  "); Serial.print(internetLat, 6); Serial.println(" (WiFi Fallback)");
    Serial.print("Longitud internet: "); Serial.print(internetLng, 6); Serial.println(" (WiFi Fallback)");
  } else {
    Serial.println("Ubicación inválida / Buscando satélites...");
  }

  if (gps.date.isValid()) {
    Serial.printf("Fecha UTC: %02d/%02d/%04d\n", gps.date.day(), gps.date.month(), gps.date.year());
  }
  if (gps.time.isValid()) {
    Serial.printf("Hora UTC:  %02d:%02d:%02d\n", gps.time.hour(), gps.time.minute(), gps.time.second());
  }
}

/**
 * @brief Monitor de diagnóstico lógico para telemetría del canal GPS.
 * Inspecciona los contadores acumulados de bytes leídos, errores en sumas de verificación (Checksum),
 * cantidad de satélites en rango óptimo y estado operativo de fijación bidimensional (Fix).
 */
void displayGPSStatus() {
  Serial.println("\n---- ESTADO GPS ----");
  Serial.print("Chars RX: "); Serial.println(totalCharsReceived);
  Serial.print("Sentencias exitosas: "); Serial.println(totalSentencesReceived);
  Serial.print("Fix válido: "); Serial.println(gps.location.isValid() ? "SI" : "NO");
  Serial.print("Satélites: "); Serial.println(gps.satellites.value());
  Serial.print("Checksum fallidos: "); Serial.println(gps.failedChecksum());
  Serial.printf("Sentencias -> GGA:%d, RMC:%d, GLL:%d, Otros:%d\n", ggaSentences, rmcSentences, gllSentences, unknownSentences);
}

/**
 * @brief Conmutador físico de estado de salida digital (LED Heartbeat).
 * Invierte de forma lógica la salida eléctrica del pin GPIO2 para indicar visualmente
 * que el lazo principal de ejecución asíncrono se está procesando activamente.
 */
void toggleStatusLED() {
  digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
}

/**
 * @brief Rutina de despacho crítico ante eventos de colisión o caída del usuario.
 * Compone de inmediato la cadena de caracteres de la alerta crítica de impacto, discrimina
 * e inyecta la mejor coordenada geográfica disponible y dispara el envío del SMS prioritario.
 */
void sendFallSms() {
  if (millis() - lastFallSms < FALL_SMS_INTERVAL_MS) {
    return;
  }
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

/**
 * @brief Rutina secuencial de despacho periódico para rastreo pasivo.
 * Se ejecuta de forma cíclica e independiente en segundo plano, enviando un mensajeSMS estructurado 
 * con el estado general de salud del sistema de telemetría y el enlace cartográfico de Google Maps.
 */
void sendLocationSms() {
  if (millis() - lastLocationSms < LOCATION_SMS_INTERVAL_MS) {
    return;
  }
  lastLocationSms = millis();

  String body = "Estado ESP32 de Tesis. Ubicacion: ";
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
 * @brief Función de configuración inicial y asignación de periféricos de hardware.
 * Establece los canales UART serie, inicializa la interfaz maestra I2C fijando el reloj en alta velocidad
 * (400kHz), arranca las comunicaciones inalámbricas de red e invoca el arranque lógico del módulo GPS y acelerómetro.
 */
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  Serial.println("\n======================================");
  Serial.println("   ALERTA DE UBICACIÓN Y CAÍDA ESP32  ");
  Serial.println("======================================\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); // Configura bus I2C en modo rápido (Fast Mode)

  connectWiFi();

  Serial.println("Iniciando ADXL345...");
  adxlDetected = detectADXL345();
  if (adxlDetected) {
    Serial.println("ADXL345 en línea y verificado.");
    initADXL345();
  } else {
    Serial.println("ADXL345 NO responde. Revisa bus I2C y pull-ups.");
  }

  Serial.println("Iniciando GPS en Serial2 a 9600...");
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  startTime = millis();
}

/**
 * @brief Núcleo ejecutivo principal del firmware (Lazo infinito asíncrono no bloqueante).
 * Realiza un barrido continuo para leer los buffers del puerto UART secundario alimentando al motor decodificador 
 * de TinyGPS++. Ejecuta de forma paralela el procesamiento del acelerómetro en tiempo real, gestiona de forma automatizada 
 * la activación temporal de las funciones de contingencia (Fallback WPS) y despacha la telemetría periódica.
 */
void loop() {
  loopCount++;
  bool isColdStart = (millis() - startTime) < GPS_FALLBACK_TIMEOUT_MS;

  // Lector de secuencia del canal UART de comunicación del módulo GPS
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    totalCharsReceived++;
    
    // Alimenta el decodificador TinyGPS++ para la validación interna de tramas lógicas NMEA
    if (gps.encode(c)) {
      totalSentencesReceived++;
      toggleStatusLED();
    }

    // Almacenamiento e inspección de subcadenas paralelas NMEA de auditoría para la tesis
    if (c == '\r') continue;
    if (c == '\n') {
      if (nmeaLine.length() > 0) {
        trackNMEASentence(nmeaLine);
        nmeaLine = "";
      }
      continue;
    }
    if (nmeaLine.length() < 128) {
      nmeaLine += c;
    }
  }

  // Activación del Sistema de Contingencia por Red en Interiores tras el fin del Cold Start (5 minutos)
  if (!gps.location.isValid() && !internetLocationValid && !isColdStart && (millis() - lastInternetAttempt >= INTERNET_LOCATION_RETRY_MS)) {
    fetchInternetLocation();
  }

  procesarAcelerometro();

  // Orquestador periódico de impresión de telemetría por consola general cada 5 segundos
  if (millis() - lastStatusPrint >= 5000UL) {
    displayLocationInfo();
    displayGPSStatus();
    Serial.print("Última alerta caída: ");
    if (lastFallTime == 0UL) {
      Serial.println("Ninguna");
    } else {
      Serial.print((millis() - lastFallTime) / 1000UL);
      Serial.println(" segundos atrás");
    }
    lastStatusPrint = millis();
  }

  sendLocationSms();

  // Supervisor de integridad del canal físico UART (Alertas por falla de cableado lógicas posteriores al Cold Start)
  if (!isColdStart && totalCharsReceived < 10) {
    Serial.println("\n⚠ ALERTA: No llegan datos lógicos del GPS. Revisa RX/TX.");
    delay(1000); 
  }

  delay(10); // Retraso de guarda mínimo de estabilidad estructural del hilo principal
}