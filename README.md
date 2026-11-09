# Desktop Companion — ESP32-S3 3.5" (JC3248W535)

Port del [ESP32-C3 Desktop Companion](https://github.com/kbo-maker-works/esp32-c3-desktop-companion) a la Guition JC3248W535: ESP32-S3, 16MB flash, 8MB PSRAM, pantalla IPS 3.5" 480x320 y táctil capacitivo (AXS15231B).

## Qué hay

Pantalla de inicio con 8 apps en rejilla 4x2 sobre el fondo de la SD, y una barra de estado con hora, fecha, clima, enlace con el PC y Wi-Fi. Las apps sin página aún salen atenuadas y muestran "Próximamente".

| App | Qué hace | Pulsación larga |
|---|---|---|
| Ajustes | Wi-Fi, IP, portal, microSD, memoria, uptime | Recargar SD |
| Página | Qué hace | Pulsación larga |
|---|---|---|
| Reloj | 4 esferas: digital, gigante bicolor, analógico y retro, con clima | Cambiar esfera |
| El Tiempo | Open-Meteo (sin API key), icono día/noche, humedad | Refrescar |
| Claude Code | Estado, tokens, cuota de sesión y semanal (vía pc-agent) | — |

Pendiente de portar: mascota, Matrix / Star Wars / Pac-Man / AFK.

**Gestos** (todos en `onGesture()` de `src/main.cpp`, cámbialos ahí):

| | Home | Dentro de una app |
|---|---|---|
| Tap | Abrir app | `onTap()` de la app |
| Long press | libre | Acción de la app |
| Swipe abajo | libre | Volver a la home |
| Swipe izq / der | libre | App siguiente / anterior |
| Swipe arriba | libre | libre |

## Fondos en la microSD

- Formato `.565`: RGB565 crudo de 480x320 (307200 bytes). Se carga una vez, se cachea en PSRAM y pintarlo cuesta ~1 ms.
- Convierte tus PNG/JPG con `python tools/convert_bg.py src/assets sd/bg --map background=home` (necesita `pip install pillow`).
- Copia el contenido de `sd/` a la raíz de la tarjeta (FAT32), de modo que quede `/bg/home.565`, etc.
- Sin SD, o si falta un fichero, se usa el fondo compilado (`src/assets/background_480x320.h`).
- Las rutas se configuran en `config.h` (`BG_HOME`, `BG_SETTINGS`).

## Iconos

Los iconos de la pantalla principal son máscaras monocromas que la placa tiñe con el color de acento (cian o ámbar). Gris si la app aún no está disponible, invertidos al pulsar.

**Especificación para diseñarlos:**

- Nombre: `icon_<id>.svg` o `.png`, en minúsculas, con el id de la app: `clk` reloj, `env` entorno/tiempo, `ai` IA/cuotas, `sys` sistema, `net` red, `rdr` radar/vuelos, `aud` audio, `cfg` ajustes.
- Lienzo de 48x48 px (o un múltiplo: 96, 192..., se reduce a 48).
- Monocromo: blanco sobre transparente. Sin color ni degradados (el color lo pone la placa). En un PNG sin transparencia cuenta la luminancia: blanco = icono, negro = fondo.
- Margen de seguridad de unos 4 px y trazo de unos 2 px a 48 px (4 px a 96). A 48 px, con coordenadas enteras y `stroke-linecap="square"` queda nítido, sin píxeles a medias.
- No hace falta marco: el módulo del panel ya pinta el suyo.

**Dónde van y cómo se cargan:**

1. Fuentes en `src/assets/icons/`. Para compilarlos en el firmware (respaldo si no hay SD):
   ```
   %USERPROFILE%\.platformio\penv\Scripts\python.exe tools\make_icons.py
   ```
   Genera `src/assets/icons_builtin.h` y `build_icons/icon_<id>.a4`. Usa el SVG si está instalado `cairosvg` o `resvg_py`; si no, el PNG.
2. Para cambiarlos sin reflashear, sube los `.a4` a `/icons/` de la microSD:
   ```
   ...python.exe tools\make_icons.py --upload 192.168.1.225
   ```
   O desde el portal: tarjeta microSD → **Subir icono** (convierte el PNG/SVG en el navegador). Los de la SD mandan sobre los compilados y se recargan solos al subirlos.

### Iconos de la barra superior

Un archivo por indicador, con las mismas reglas que los de la home (`icon_<nombre>`, 48x48, blanco sobre transparente). La barra los pinta a **24 px** (la mitad exacta), así que conviene que el trazo sea de **2 px o más** para que no se pierda. El **estado va por color**, no hay que dibujar variantes:

| Archivo | Indicador | Color en la placa |
|---|---|---|
| `icon_wifi` | Wi-Fi (punto + 3 arcos) | cian; los arcos exteriores se apagan según la señal; **rojo** sin conexión |
| `icon_usb` | PC conectado por USB (host USB) | ámbar; **oculto** si no hay PC |
| `icon_cloud` | servidor del Mac | violeta si responde, **rojo** si está caído; **oculto** mientras no esté configurado |
| `icon_bat` | batería | verde; **oculto** mientras no haya medición (la JC3248W535 no tiene una conocida) |

Para el nivel de Wi-Fi, la placa apaga los arcos midiendo la distancia al punto (centro 24,38; radios 9 / 17 / 25 como en `icon_wifi.svg`). Si cambias la geometría del icono, avisa para ajustarlo. Si falta un icono, la barra pinta en su lugar texto con un LED (`WIFI ■`, `USB ■`...).

Formato `.a4`: `'A' '4' ancho alto` y luego 4 bits de alpha por píxel (nibble alto primero), el mismo empaquetado que las fuentes de TextAA.

## Fuente (Nunito)

Va compilada en flash, no en la SD. Son ~110 KB con antialiasing de 4 bits y acentos (Latin-1).

- Fuentes disponibles: `Nunito14`, `Nunito18`, `Nunito24`, `Nunito36` y `Nunito72Num` (solo números, para reloj y temperatura).
- Uso: `TextAA::draw(Nunito24, x, yBase, "Configuración", color)`. La y es la línea base. También hay `drawCentered`, `drawRight` y `drawShadow`.
- Para añadir tamaños o pesos, edita la lista `FONTS` en `tools/make_aafont.py` y ejecuta `python tools/make_aafont.py "assets/fonts/Nunito[wght].ttf"`.
- Nunito tiene licencia SIL OFL (`assets/fonts/OFL.txt`).

## Cambios respecto a la C3

- La pantalla pinta en un framebuffer completo en PSRAM y lo vuelca entero. Sin parpadeo y sin borrado diferencial.
- Táctil del panel en vez del TTP223. Mantiene tap y long press, y añade swipe.
- El clima se pide una sola vez (`WeatherService`) y lo comparten el reloj y la página del tiempo.
- Zona horaria POSIX (`TZ_INFO`). El horario de invierno y verano cambia solo; antes el +1h de DST estaba fijo.
- El portal Wi-Fi (`Companion-Setup`) y el pc-agent por USB son los mismos que en la C3.

## Flashear

1. `cp src/config.h.example src/config.h` y rellénalo. El Wi-Fi es opcional: si no lo pones, se configura desde el portal.
2. Abre la carpeta en VS Code con PlatformIO y dale a Upload. La primera vez tarda porque baja la plataforma pioarduino (core 3.x).
3. Si no sube: mantén BOOT, pulsa RST y suelta BOOT.

## pc-agent (cuotas de Claude Code)

```
cd pc-agent
pip install -r requirements.txt
python companion_agent.py COM5      # Windows
python3 companion_agent.py /dev/cu.usbmodemXXXX   # macOS
```

Para las cuotas reales, configura `claude_statusline.py` como statusLine en `~/.claude/settings.json` (instrucciones dentro del script).

## Si algo falla

- **Pantalla negra:** revisa que `memory_type = qio_opi` esté en el ini y los pines de `src/core/Pins.h` contra la doc del vendedor.
- **Nada en el monitor serie:** tu placa puede llevar chip USB-UART. Pon `-DARDUINO_USB_CDC_ON_BOOT=0`.
- **El toque sale en otro sitio o en espejo:** pon `TOUCH_DEBUG 1` en `config.h`, que pinta un punto rojo donde lee el dedo, y activa `TOUCH_FLIP_X` o `TOUCH_FLIP_Y` según haga falta.
- **No carga los fondos:** mira el monitor serie (`[SD]` / `[BG]`). La tarjeta tiene que estar en FAT32 y los ficheros medir exactamente 307200 bytes.
