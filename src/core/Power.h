#pragma once
#include <Arduino.h>

// Alimentación y regla de pantalla.
//
// REGLA DEL DISPOSITIVO: si hay un PC conectado por USB, la pantalla NUNCA se
// apaga ni se atenúa. Cualquier atenuado/apagado automático (inactividad, modo
// noche...) tiene que preguntar antes a Power::mayDimScreen().
//
// usbHost(): la placa va en modo USB-Serial/JTAG (ARDUINO_USB_MODE=1) y
// HWCDC::isPlugged() mira si llegan tramas SOF del host. Con un PC (aunque nadie
// tenga el puerto serie abierto) da true; con un cargador de pared o un
// powerbank no hay host USB, así que da false. Pendiente de confirmar en la placa.
namespace Power {

inline bool usbHost() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    return HWCDC::isPlugged();
#else
    return false;
#endif
}

// ¿Se permite atenuar/apagar la pantalla automáticamente ahora?
inline bool mayDimScreen() { return !usbHost(); }

}  // namespace Power
