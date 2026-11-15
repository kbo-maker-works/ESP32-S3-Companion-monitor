---
name: git-publish
description: Hace commit de los cambios del desktop companion y los sube a GitHub (kbo-maker-works/ESP32-S3-Companion-monitor). Solo cuando el usuario lo ejecuta con /git-publish.
disable-model-invocation: true
argument-hint: "[mensaje del commit (opcional)]"
---

# Subir el proyecto a GitHub

El usuario ha pedido subir **solo cuando él ejecuta este skill**. Fuera de aquí no se hacen commits ni push, aunque haya cambios terminados.

Repo: `https://github.com/kbo-maker-works/ESP32-S3-Companion-monitor`, rama `main`, remoto `origin`.
Trabaja en `C:\git-proyects\desktop-companion`. Responde en español.

## 1. Comprobar el estado

- `git status --short` y `git diff --stat`. Si no hay nada que subir (ni cambios ni commits por delante de `origin/main`), dilo y termina.
- Identidad del repo: `git config user.name` debe ser `Alejandrokbo` y `git config user.email` `41395415+Alejandrokbo@users.noreply.github.com`. Si no, configúralas **solo en el repo** (sin `--global`). Nunca uses el Gmail del usuario en los commits.
- Si otra sesión (p.ej. `support`) está a mitad de un cambio o flasheando, avisa al usuario antes de seguir: podrías subir archivos a medias.

## 2. Revisar lo que se va a publicar (el repo es público)

- `git add -A`, y después `git diff --cached --name-only`.
- `src/config.h` **nunca** puede estar preparado. Si lo está: `git rm --cached src/config.h`, confirma que sigue en `.gitignore` y avisa.
- Busca secretos en lo preparado (`git diff --cached`): contraseñas reales, tokens (`ghp_`, `glpat-`, `sk-ant`, `xox`), claves de API, cadenas de conexión, emails personales, SSIDs/contraseñas Wi-Fi reales. Si aparece algo, **para**, quítalo del commit y pregunta al usuario. Una IP privada de ejemplo (192.168.x.x) no es un secreto.
- Avisa si algún archivo nuevo pesa más de 10 MB, o si aparecen binarios de compilación (`.pio/`, `.bin`, `.elf`).

## 3. Commit

- Mensaje: usa `$ARGUMENTS` si el usuario lo dio. Si no, escribe uno en español: primera línea corta (qué cambia) y, si hace falta, una lista breve debajo.
- Termina el mensaje con las líneas de atribución que indique el system-reminder vigente de la sesión.
- En PowerShell 5.1 no pases un mensaje multilínea con un here-string a `git commit -F -` (se rompe en argumentos). Escribe el mensaje en un archivo del directorio scratchpad y usa `git commit -F <archivo>`.

## 4. Subir

- `git fetch origin`. Si `origin/main` tiene commits que no están en local, haz `git pull --rebase origin main`. Si hay conflictos, para y explícaselos al usuario. No los resuelvas por tu cuenta.
- `git push origin main`. **Nunca** `--force` ni `--force-with-lease`, ni push a otras ramas, salvo que el usuario lo pida de forma explícita en esta ejecución.
- Si el push pide credenciales y la sesión no puede mostrar el login, dile que ejecute `! git push origin main`.

## 5. Informar

En pocas líneas: hash corto del commit, cuántos archivos han cambiado, el enlace al commit (`https://github.com/kbo-maker-works/ESP32-S3-Companion-monitor/commit/<hash>`) y cualquier cosa que hayas excluido o que deba revisar.
