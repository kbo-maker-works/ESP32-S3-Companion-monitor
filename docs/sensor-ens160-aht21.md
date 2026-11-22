# Sensor ambiental: ENS160 + AHT21

Placa combo (sustituto del CCS811) montada en el case del desktop companion (Guition JC3248W535, ESP32-S3).

## Medidas y montaje
- PCB: **9,6 × 24 mm** (medido a mano)
- Un solo agujero de anclaje
- Montaje: **cinta doble cara 3M VHB** por la cara sin componentes, dentro de un bolsillo del case (ya probado en el prototipo impreso: encaja bien)
- La zona del ENS160 y del AHT21 debe quedar libre y orientada a las rejillas de ventilación
- Ubicación: cámara de sensores en la parte baja del case, separada de la placa principal y la retroiluminación

## Sensores
| Chip | Mide | Dirección I2C |
|---|---|---|
| ENS160 (ScioSense, MOX) | eCO2 (ppm, **estimado**, no NDIR), TVOC (ppb), AQI UBA 1–5 | 0x53 (0x52 si ADD a GND) |
| AHT21 | Temperatura (°C), humedad relativa (%) | 0x38 |

## Pines de la placa (8)
VIN, 3V3, GND, SCL, SDA, ADD, CS, INT. **Verificar el orden con la serigrafía.**
- Se usan solo **VIN, GND, SDA y SCL**
- 3V3 es salida del regulador: no conectar
- ADD, CS e INT se quedan sin conectar (I2C, dirección por defecto, polling)
- VIN acepta 3,3 V o 5 V porque la placa lleva regulador

## Conexión a la Guition JC3248W535
- Va al conector **JST 1.25 4P** de la parte inferior trasera (duplicado del HC1.0 4P: comparten pines)
- Ese conector lleva 3,3 V, GND y **GPIO17 / GPIO18**. Asignar SDA/SCL y verificar cuál es cuál con un scan I2C
- El táctil tiene su propio bus, así que no choca. Aun así, hacer un scan I2C al arrancar por primera vez

## Notas de firmware
- Librerías: SparkFun ENS160 (o DFRobot_ENS160) + Adafruit AHTX0
- **Compensación:** pasar la temperatura y la humedad del AHT21 al ENS160 en cada lectura
- **Calentamiento:** en cada arranque, ~3 min con datos no válidos. La primera vez tarda ~1 h en estabilizarse y ~24 h hasta ir fino. Leer el flag de validez del ENS160 y mostrar "calentando" mientras tanto
- **Autocalentamiento:** el calentador del ENS160 sube la lectura del AHT21 unos grados. Aplicar un offset de temperatura configurable (calibrar contra un termómetro de referencia) y recalcular la humedad relativa si hace falta
- Frecuencia de lectura: cada 1–2 s internamente, refresco de UI cada 5–10 s
- Página de UI prevista: CO2, TVOC, AQI, temperatura y humedad, integrada en Page/PageManager
