#pragma once
#include <Arduino.h>
#include <SD.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "ff.h"
#include "sd_diskio.h"
#include "Background.h"
#include "Icons.h"

// Gestor de la microSD desde el portal web: listar, subir, descargar, borrar,
// renombrar, crear carpetas y formatear (FAT32).
//
// Todo corre en el loop principal (WebServer es síncrono), igual que la carga
// de fondos, así que nunca hay dos accesos a la SD a la vez.
class SdManager {
public:
    static void setupRoutes(WebServer &srv) {
        _srv() = &srv;

        srv.on("/api/sd/info", HTTP_GET, [] {
            JsonDocument d;
            bool ok = Background::sdOk();
            d["mounted"] = ok;
            if (ok) {
                d["total"] = (double)SD.totalBytes();
                d["used"]  = (double)SD.usedBytes();
                d["card"]  = (double)SD.cardSize();
                const char *types[] = {"-", "MMC", "SD", "SDHC", "?"};
                uint8_t t = SD.cardType();
                d["type"] = types[t < 5 ? t : 4];
            }
            _json(200, d);
        });

        srv.on("/api/sd/list", HTTP_GET, [] {
            String path = _arg("path", "/");
            if (!_ready() || !_validPath(path)) return;
            File dir = SD.open(path);
            if (!dir || !dir.isDirectory()) { _err("No es una carpeta"); return; }
            JsonDocument d;
            JsonArray arr = d.to<JsonArray>();
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                JsonObject o = arr.add<JsonObject>();
                o["name"] = String(f.name());
                o["dir"]  = f.isDirectory();
                o["size"] = (double)f.size();
                f.close();
            }
            dir.close();
            _json(200, d);
        });

        srv.on("/api/sd/file", HTTP_GET, [] {
            String path = _arg("path", "");
            if (!_ready() || !_validPath(path)) return;
            File f = SD.open(path, FILE_READ);
            if (!f || f.isDirectory()) { _err("No existe"); return; }
            String name = path.substring(path.lastIndexOf('/') + 1);
            if (_srv()->hasArg("dl")) _srv()->sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
            _srv()->streamFile(f, "application/octet-stream");
            f.close();
        });

        // Miniatura 120x80 de un fondo .565 (uno de cada 4 px): 19 KB en vez de 300 KB
        srv.on("/api/sd/thumb", HTTP_GET, [] {
            String path = _arg("path", "");
            if (!_ready() || !_validPath(path)) return;
            File f = SD.open(path, FILE_READ);
            if (!f || f.isDirectory() || f.size() != Background::BYTES) {
                if (f) f.close();
                _err("No es un fondo 480x320");
                return;
            }
            static constexpr int TW = 120, TH = 80;
            uint16_t *out = (uint16_t *)ps_malloc(TW * TH * 2);
            if (!out) { f.close(); _err("Sin memoria"); return; }
            static uint16_t row[480];
            for (int ty = 0; ty < TH; ty++) {
                f.seek((uint32_t)ty * 4 * 480 * 2);
                f.read((uint8_t *)row, sizeof(row));
                for (int tx = 0; tx < TW; tx++) out[ty * TW + tx] = row[tx * 4];
            }
            f.close();
            _srv()->sendHeader("Cache-Control", "no-store");
            _srv()->send_P(200, "application/octet-stream", (const char *)out, TW * TH * 2);
            free(out);
        });

        // Subida multipart: ?path=/carpeta ; el nombre lo pone el navegador
        srv.on("/api/sd/upload", HTTP_POST, [] {
            if (_up().failed) { _err(_up().error.c_str()); return; }
            JsonDocument d;
            d["ok"] = true;
            d["path"] = _up().path;
            d["size"] = (double)_up().size;
            _json(200, d);
        }, _handleUpload);

        srv.on("/api/sd/mkdir", HTTP_POST, [] {
            String path = _arg("path", "");
            if (!_ready() || !_validPath(path)) return;
            if (!SD.mkdir(path)) { _err("No se pudo crear la carpeta"); return; }
            _ok();
        });

        srv.on("/api/sd/delete", HTTP_POST, [] {
            String path = _arg("path", "");
            if (!_ready() || !_validPath(path)) return;
            if (path == "/") { _err("No se puede borrar la raíz (usa Formatear)"); return; }
            if (!_removeRecursive(path)) { _err("No se pudo borrar"); return; }
            Background::forget(path.c_str());
            Icons::forgetAll();
            _ok();
        });

        srv.on("/api/sd/rename", HTTP_POST, [] {
            String from = _arg("from", ""), to = _arg("to", "");
            if (!_ready() || !_validPath(from) || !_validPath(to)) return;
            if (SD.exists(to)) { _err("Ya existe un archivo con ese nombre"); return; }
            if (!SD.rename(from, to)) { _err("No se pudo renombrar"); return; }
            Background::forget(from.c_str());
            Icons::forgetAll();
            Background::forget(to.c_str());
            _ok();
        });

        srv.on("/api/sd/format", HTTP_POST, [] {
            if (_arg("confirm", "") != "FORMATEAR") { _err("Falta la confirmación"); return; }
            String msg;
            if (!format(msg)) { _err(msg.c_str()); return; }
            _ok();
        });

        // Volver a montar (si se metió la tarjeta con la placa encendida)
        srv.on("/api/sd/mount", HTTP_POST, [] {
            SD.end();
            Background::forgetAll();
            Icons::forgetAll();
            if (!Background::beginSD()) { _err("No se detecta la tarjeta"); return; }
            _ok();
        });
    }

    // Formatea la tarjeta en FAT32 (clúster 32 KB) y crea /bg.
    // Bloquea unos segundos (escribe las tablas FAT de toda la tarjeta).
    static bool format(String &msg) {
        if (!Background::sdOk()) { msg = "No hay tarjeta montada"; return false; }
        int pdrv = _findDrive();
        if (pdrv < 0) { msg = "No encuentro la unidad FatFs de la SD"; return false; }

        Background::forgetAll();
        Icons::forgetAll();
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        const size_t WORK = 32 * 1024;   // buffer grande = formateo más rápido
        void *work = ps_malloc(WORK);
        if (!work) { msg = "Sin memoria para formatear"; return false; }

        Serial.printf("[SD] Formateando unidad %s en FAT32...\n", drv);
        uint32_t t0 = millis();
        const MKFS_PARM opt = {(BYTE)FM_FAT32, 0, 0, 0, 32768};
        FRESULT res = f_mkfs(drv, &opt, work, WORK);
        free(work);
        if (res != FR_OK) {
            msg = "f_mkfs falló (código " + String((int)res) + ")";
            Serial.printf("[SD] %s\n", msg.c_str());
            return false;
        }
        Serial.printf("[SD] Formateada en %lu ms\n", (unsigned long)(millis() - t0));

        // Remontar limpio y dejar la estructura básica
        SD.end();
        if (!Background::beginSD()) { msg = "Formateada, pero no se pudo volver a montar"; return false; }
        SD.mkdir("/bg");
        return true;
    }

private:
    struct Upload {
        File file;
        String path, error;
        size_t size = 0;
        bool failed = false;
        uint8_t *buf = nullptr;   // escribir en bloques grandes va mucho más rápido en la SD
        size_t used = 0;
    };
    static constexpr size_t UP_BUF = 16 * 1024;

    static WebServer *&_srv() { static WebServer *s = nullptr; return s; }
    static Upload &_up() { static Upload u; return u; }

    static String _arg(const char *k, const char *def) {
        return _srv()->hasArg(k) ? _srv()->arg(k) : String(def);
    }

    static void _json(int code, JsonDocument &d) {
        String out;
        serializeJson(d, out);
        _srv()->send(code, "application/json", out);
    }
    static void _ok() { JsonDocument d; d["ok"] = true; _json(200, d); }
    static void _err(const char *m) { JsonDocument d; d["ok"] = false; d["error"] = m; _json(400, d); }

    static bool _ready() {
        if (Background::sdOk()) return true;
        _err("No hay tarjeta microSD");
        return false;
    }

    static bool _validPath(const String &p) {
        if (p.startsWith("/") && p.indexOf("..") < 0 && p.length() < 200) return true;
        _err("Ruta no válida");
        return false;
    }

    static bool _removeRecursive(const String &path) {
        File f = SD.open(path);
        if (!f) return false;
        if (!f.isDirectory()) { f.close(); return SD.remove(path); }
        for (File c = f.openNextFile(); c; c = f.openNextFile()) {
            String child = path + "/" + c.name();
            c.close();
            if (!_removeRecursive(child)) { f.close(); return false; }
        }
        f.close();
        return SD.rmdir(path);
    }

    static void _flush(Upload &u) {
        if (!u.used || !u.file) return;
        if (u.file.write(u.buf, u.used) != u.used) { u.failed = true; u.error = "Error escribiendo (¿tarjeta llena?)"; }
        u.size += u.used;
        u.used = 0;
    }

    static void _handleUpload() {
        HTTPUpload &h = _srv()->upload();
        Upload &u = _up();
        switch (h.status) {
            case UPLOAD_FILE_START: {
                u.failed = false;
                u.error = "";
                u.size = 0;
                u.used = 0;
                String dir = _arg("path", "/");
                if (!dir.endsWith("/")) dir += "/";
                String name = h.filename.substring(h.filename.lastIndexOf('/') + 1);
                u.path = dir + name;
                if (!Background::sdOk() || !dir.startsWith("/") || dir.indexOf("..") >= 0 || name.isEmpty()) {
                    u.failed = true; u.error = "Ruta o tarjeta no válida"; return;
                }
                if (!u.buf) u.buf = (uint8_t *)ps_malloc(UP_BUF);
                if (!u.buf) { u.failed = true; u.error = "Sin memoria"; return; }
                if (SD.exists(u.path)) SD.remove(u.path);
                u.file = SD.open(u.path, FILE_WRITE);
                if (!u.file) { u.failed = true; u.error = "No se pudo crear " + u.path; return; }
                Serial.printf("[SD] Subiendo %s\n", u.path.c_str());
                break;
            }
            case UPLOAD_FILE_WRITE:
                if (u.failed || !u.file) return;
                for (size_t off = 0; off < h.currentSize && !u.failed;) {
                    size_t n = min(h.currentSize - off, UP_BUF - u.used);
                    memcpy(u.buf + u.used, h.buf + off, n);
                    u.used += n;
                    off += n;
                    if (u.used == UP_BUF) _flush(u);
                }
                break;
            case UPLOAD_FILE_END:
                if (u.file) {
                    _flush(u);
                    u.file.close();
                    Background::forget(u.path.c_str());   // si era un fondo en uso, se recarga
                    if (u.path.startsWith("/icons/")) Icons::forgetAll();
                    if (!u.failed) Serial.printf("[SD] %s: %u bytes\n", u.path.c_str(), (unsigned)u.size);
                }
                if (u.failed) SD.remove(u.path);
                break;
            case UPLOAD_FILE_ABORTED:
                if (u.file) u.file.close();
                SD.remove(u.path);
                u.failed = true;
                u.error = "Subida cancelada";
                break;
        }
    }

    // La unidad FatFs que usa la SD (la librería no expone su número)
    static int _findDrive() {
        for (int i = 0; i < FF_VOLUMES; i++)
            if (sdcard_num_sectors(i) > 0) return i;
        return -1;
    }
};
