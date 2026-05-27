#include <Arduino.h>

// DEFINICIÓN DE PINES UART PARA EL SIM800L (Ajusta según tu PCB)
// Recuerda: RX del ESP32 va al TX del SIM800L, y TX del ESP32 va al RX del SIM800L
#define SIM_RX_PIN 4
#define SIM_TX_PIN 5

// El SIM800L suele iniciar en modo Auto-Baudios, 9600 es lo más estable para arrancar
#define SIM_BAUDRATE 9600 

void setup() {
  // Inicializa el monitor serie de la computadora
  Serial.begin(115200);
  delay(2000); // Espera para USB nativo en ESP32-S3

  Serial.println("=========================================");
  Serial.println("--- Monitor Puente AT para SIM800L ---");
  Serial.println("Escribe tus comandos AT en la terminal.");
  Serial.println("Asegúrate de configurar 'Both NL & CR' o enviar con Enter.");
  Serial.println("=========================================");

  // Inicializa el puerto serial por hardware (Serial1) para el SIM800L
  // Configuración: Baudios, Modo (SERIAL_8N1), Pin RX, Pin TX
  Serial1.begin(SIM_BAUDRATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
}

void loop() {
  // Lo que se recibe del SIM800L se muestra en la computadora
  if (Serial1.available()) {
    while (Serial1.available()) {
      Serial.write(Serial1.read());
    }
  }

  // Lo que escribes en la terminal de la computadora se envía al SIM800L
  if (Serial.available()) {
    while (Serial.available()) {
      Serial1.write(Serial.read());
    }
  }
}