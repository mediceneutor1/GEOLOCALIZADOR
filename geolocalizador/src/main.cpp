#include <Wire.h>
#include <TinyGPS++.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

/* --- CONFIGURACIÓN DE PINES --- */
#define PIN_SOS 4
#define PIN_LED_R 13
#define PIN_LED_G 12
#define PIN_LED_B 14

// El ESP32 tiene múltiples UARTs. Usaremos Serial1 y Serial2.
#define RX_GPS 16
#define TX_GPS 17
#define RX_SIM 26
#define TX_SIM 27

/* --- INSTANCIAS --- */
TinyGPSPlus gps;
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
HardwareSerial SerialGPS(2);
HardwareSerial SerialSIM(1);

// Configuración del destinatario (Paraguay)
const String NUMERO_CUIDADOR = "+5959XXXXXXXXX"; 

/* --- FUNCIONES AUXILIARES --- */

void enviarComandoAT(String comando, int espera = 500) {
  SerialSIM.println(comando);
  delay(espera);
  while (SerialSIM.available()) {
    Serial.print((char)SerialSIM.read());
  }
}

void enviarSMS(String texto) {
  Serial.println("Preparando envío de SMS...");
  enviarComandoAT("AT+CMGF=1"); // Modo texto
  String aux = "AT+CMGS=\"" + NUMERO_CUIDADOR + "\"";
  enviarComandoAT(aux);
  SerialSIM.print(texto);
  SerialSIM.write(26); // Ctrl+Z para enviar
  delay(3000);
  Serial.println("\nSMS procesado.");
}

void enviarAlerta(String motivo) {
  String mensaje = "ALERTA " + motivo + ": ";
  if (gps.location.isValid()) {
    mensaje += "Ubicacion actual: https://www.google.com/maps?q=";
    mensaje += String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
  } else {
    mensaje += "Ubicacion desconocida (Sin señal GPS).";
  }
  enviarSMS(mensaje);
}

/* --- SETUP --- */

void setup() {
  Serial.begin(115200);
  SerialGPS.begin(9600, SERIAL_8N1, RX_GPS, TX_GPS);
  SerialSIM.begin(115200, SERIAL_8N1, RX_SIM, TX_SIM);

  pinMode(PIN_SOS, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);

  if(!accel.begin()) {
    Serial.println("Error: No se encontro el ADXL345");
    digitalWrite(PIN_LED_R, HIGH);
  }

  // Inicialización básica del SIM7000G
  Serial.println("Inicializando SIM7000G...");
  enviarComandoAT("AT");         // Verificar conexión
  enviarComandoAT("AT+CPIN?");   // Verificar estado de SIM
  enviarComandoAT("AT+CGNSPWR=1"); // Encender GPS interno si el módulo lo soporta
}

/* --- LOOP PRINCIPAL --- */

void loop() {
  // 1. Leer tramas del GPS
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  // 2. Revisar botón SOS (Lógica negativa por INPUT_PULLUP)
  if (digitalRead(PIN_SOS) == LOW) {
    digitalWrite(PIN_LED_B, HIGH); // Azul para SOS
    enviarAlerta("BOTON SOS PRESIONADO");
    digitalWrite(PIN_LED_B, LOW);
  }

  // 3. Revisar Acelerómetro (Detección de caída simple)
  sensors_event_t event;
  accel.getEvent(&event);
  
  // Si la fuerza total en Z supera un umbral (impacto brusco)
  // Nota: Esto es un bosquejo, requiere calibración para evitar falsos positivos
  if (abs(event.acceleration.z) > 25.0) { 
    digitalWrite(PIN_LED_R, HIGH);
    enviarAlerta("CAIDA DETECTADA");
    digitalWrite(PIN_LED_R, LOW);
  }

  // Indicador de funcionamiento (parpadeo verde cada 5 seg)
  if (millis() % 5000 < 100) {
    digitalWrite(PIN_LED_G, HIGH);
    delay(50);
    digitalWrite(PIN_LED_G, LOW);
  }
}