# Sistema de Geolocalización, Detección de Caídas y Alerta Redundante (ESP32)

Este proyecto consiste en el firmware y la documentación de un prototipo embebido basado en el microcontrolador **ESP32**, diseñado para el monitoreo de pacientes o adultos mayores en tiempo real. El sistema integra telemetría satelital (GPS), posicionamiento en interiores mediante triangulación de redes Wi-Fi (WPS) y detección automática de impactos bruscos a través de un acelerómetro industrial de 3 ejes. 

Como factor crítico de éxito e innovación, el dispositivo implementa un **mecanismo de comunicación híbrido y redundante** con tolerancia a fallas, priorizando el despacho local de alertas (SMS por canal celular nativo) y conmutando dinámicamente a la nube en caso de degradación del servicio de red.

---

## 🚀 Arquitectura y Funcionalidades Clave

### 1. Detección Inteligente de Caídas
* **Hardware:** Acelerómetro digital **ADXL345** comunicado por el bus $I^2C$ a alta velocidad (400 kHz).
* **Algoritmo:** Monitoreo constante del vector de aceleración total mediante el cálculo de la magnitud espacio-vectorial:
  $$|M| = \sqrt{x^2 + y^2 + z^2}$$
* **Lógica:** El sistema detecta anomalías cuando el módulo del vector rompe los umbrales de caída libre (`FREEFALL_THRESHOLD_G = 0.30g`) seguidos inmediatamente por un impacto de desaceleración brusca (`FALL_THRESHOLD_G = 2.5g`). Cuenta con un control de rebote (*debounce*) asíncrono de 3 segundos para evitar falsos positivos consecutivos.

### 2. Sistema de Posicionamiento Híbrido (Exterior / Interior)
* **Modo Principal (GNSS):** En exteriores, un módulo GPS decodifica las sentencias NMEA estándar por hardware (`Serial2`) asistido por la biblioteca `TinyGPS++`.
* **Modo Contingencia / WPS:** En interiores, donde la señal satelital se atenúa por completo, el ESP32 activa un escaneo de entorno capturando los mapas de celdas inalámbricas de las redes Wi-Fi vecinas (BSSID y RSSI). Estos datos se envían mediante una petición segura HTTPS POST a la API de **Unwired Labs**, resolviendo la ubicación exacta por triangulación en pocos segundos.

### 3. Orquestador de Mensajería con Redundancia Crítica
* **Canal Principal (SIM800L):** Las alertas de caídas e informes de estado periódicos se despachan localmente mediante comandos AT (`AT+CMGS`) usando el módem celular GSM de baja potencia.
* **Canal de Respaldo (Twilio Gateway):** Si el módulo SIM800L experimenta fallas sostenidas (pérdida de señal, falta de saldo, o desconexión física) y acumula **10 errores consecutivos**, el firmware deshabilita temporalmente el canal celular y conmuta de manera transparente a la red de datos Wi-Fi, despachando las alertas mediante la API REST de **Twilio Cloud Gateway**.

---

## 🛠️ Configuración del Entorno (platformio.ini)

El proyecto está desarrollado sobre **PlatformIO**, lo que asegura un entorno de compilación consistente y modular. A continuación, se presenta la estructura requerida del archivo `platformio.ini` para resolver las dependencias de hardware y las directivas de velocidad de la CPU y monitor de depuración:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

; Velocidad del reloj del procesador y baudrate del monitor serie
board_build.f_cpu = 240000000L
monitor_speed = 115200

; Gestión automática de dependencias de bibliotecas
lib_deps =
    mikalhart/TinyGPSPlus @ ^1.0.3

; Opciones de compilación para optimizar el manejo de floats
build_flags =
    -D CORE_DEBUG_LEVEL=0
