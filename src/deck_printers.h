// Guition deck: glue between deck.json printers, their secrets (NVS) and
// deck_bambu. A printer with its secrets stored is powered: it connects and
// gets its pages.
#ifndef DECK_PRINTERS_H
#define DECK_PRINTERS_H

#include <Arduino.h>
#include "deck_config.h"

namespace deck {

// Hand the configured printers, with their stored secrets, to deck_bambu.
void printersApply(const DeckConfig& cfg);
// Store a printer's serial and access code; an empty one keeps what is stored.
bool printerSecretSet(const String& id, const String& serial, const String& code);
bool printerHasSecret(const String& id);
// Power on the printers handed over since the last call; true when one was.
bool printersPowerSync();

}  // namespace deck

#endif  // DECK_PRINTERS_H
