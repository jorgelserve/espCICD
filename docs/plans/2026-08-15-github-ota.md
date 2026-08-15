# GitHub Pull OTA Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** El ESP32-C3 se actualiza solo desde GitHub Releases: solo necesita WiFi y el repo. Cero PC, cero curl, cero cliente Python.

**Architecture:**
- Firmware consulta `api.github.com/repos/jorgelserve/espCICD/releases/latest`, compara semver, descarga el `.bin` y flashea por OTA.
- GitHub Actions compila con PlatformIO y publica un Release automáticamente al pushear un tag `vX.Y.Z`.
- El cliente Python local (`src/esp32oled_ci/`) se elimina; el único artefacto de release es `firmware.bin`.

**Tech Stack:**
- Librería: `gibz104/SafeGithubOTA` (PlatformIO Registry, Arduino, cero dependencias externas)
- Repo: `jorgelserve/espCICD` (rama `main`)
- CI: GitHub Actions + PlatformIO core action
- OTA particiones: `ota_0`/`ota_1` ya presentes en `partitions.csv`

---







## Fase 0 — Research & Validación Previa

### Task 0.1: Confirmar que SafeGithubOTA compila en este proyecto

**Objective:** Verificar que `gibz104/SafeGithubOTA` se puede agregar a `platformio.ini` y compila con Arduino + ESP32-C3.

**Files:**
- Modify: `firmware/platformio.ini:7-9`

**Step 1: Agregar la librería a platformio.ini**

```ini
lib_deps =
  olikraus/U8g2
  gibz104/SafeGithubOTA
```

**Step 2: Compilar**

Run: `pio run`
Expected: `SUCCESS` sin errores de librería.

**Step 3: Revertir platformio.ini** (todavía no integramos, solo validamos)

```bash
git checkout -- firmware/platformio.ini
```

**Commit:** ninguno (solo validación)

---

## Fase 1 — Firmware: Auto-Update por GitHub Releases

### Task 1.1: Agregar SafeGithubOTA y stack TLS

**Objective:** Incluir la librería, aumentar el stack del loop task para TLS y sincronizar hora por NTP.

**Files:**
- Modify: `firmware/platformio.ini:7-9`
- Modify: `firmware/src/main.cpp:1-10` (includes)
- Modify: `firmware/src/main.cpp:24-30` (globals)

**Step 1: Agregar include y constante de versión**

```cpp
#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPTelnetStream.h>
#include <SafeGithubOTA.h>
#include <stdarg.h>

const char *FW_VERSION = "0.1.0";
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
```

**Step 2: Agregar instancia global**

```cpp
SafeGithubOTA ghOta;
```

**Step 3: Actualizar platformio.ini**

```ini
lib_deps =
  olikraus/U8g2
  gibz104/SafeGithubOTA
```

**Step 4: Compilar**

Run: `pio run`
Expected: `SUCCESS`

**Commit:**
```bash
git add firmware/platformio.ini firmware/src/main.cpp
git commit -m "feat: add SafeGithubOTA dependency and TLS stack"
```

### Task 1.2: Inicializar SafeGithubOTA en setup()

**Objective:** Configurar la librería con versión, repo y callback de validación; iniciar el timer de auto-check.

**Files:**
- Modify: `firmware/src/main.cpp:244-258` (función `setup()`)

**Step 1: Inicializar después de conectar WiFi**

```cpp
void setup() {
  Serial.begin(115200);
  delay(100);
  log("\nesp32oled-ci starting\n");

  display.begin();
  show("esp32oled-ci", "boot");

  prefs.begin("esp32oled", false);

  if (!tryStaProfiles()) startProvisioningAp();
  setupTelnet();
  setupWebServer();
  renderStatus();

  // SafeGithubOTA: repo público, sin PAT, auto-check cada 6h.
  ghOta.setPatRequired(false);
  ghOta.setVersion(FW_VERSION);
  ghOta.setRepo("jorgelserve", "espCICD");
  ghOta.setFilenameMatch("firmware.bin");
  ghOta.setUpdateInterval(360);  // minutos
  ghOta.onProgress([](size_t current, size_t total) {
    int pct = total ? (int)(current * 100 / total) : 0;
    log("ota %d%%\n", pct);
  });
  ghOta.onValidation([]() {
    // Si llegamos acá, el firmware nuevo arrancó y el OLED está vivo.
    log("ota validation: ok\n");
    return true;
  });
  ghOta.begin();
}
```

**Step 2: Compilar**

Run: `pio run`
Expected: `SUCCESS`

**Commit:**
```bash
git add firmware/src/main.cpp
git commit -m "feat: init SafeGithubOTA with public repo and 6h auto-check"
```

### Task 1.3: Loop de telnet con comando `check`

**Objective:** Permitir forzar un check de updates por telnet con el comando `check`.

**Files:**
- Modify: `firmware/src/main.cpp:260-289` (función `loop()`)

**Step 1: Procesar input de telnet**

```cpp
void loop() {
  telnetStream.loop();
  if (g_mode == "prov") dns.processNextRequest();
  server.handleClient();

  // Telnet commands.
  if (telnetStream.isConnected()) {
    while (telnetStream.available()) {
      char c = (char)telnetStream.read();
      static String line;
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line.equalsIgnoreCase("check")) {
          log("checking for updates...\n");
          ghOta.checkForUpdate();
        } else if (line.equalsIgnoreCase("status")) {
          log("mode=%s ssid=%s ip=%s version=%s\n",
              g_mode.c_str(), g_ssid.c_str(), g_ip.c_str(), FW_VERSION);
        } else {
          log("commands: check | status\n");
        }
        line = "";
      } else {
        line += c;
      }
    }
  }

  // ... resto del loop igual ...
}
```

**Step 2: Compilar**

Run: `pio run`
Expected: `SUCCESS`

**Commit:**
```bash
git add firmware/src/main.cpp
git commit -m "feat: add telnet commands check/status for OTA"
```

### Task 1.4: OLED muestra versión

**Objective:** Mostrar la versión de firmware en la OLED para debugging.

**Files:**
- Modify: `firmware/src/main.cpp:250-251` (boot screen)

**Step 1: Mostrar versión al boot**

```cpp
display.begin();
show("esp32oled-ci", FW_VERSION);
```

**Step 2: Compilar y verificar**

Run: `pio run`
Expected: `SUCCESS`

**Commit:**
```bash
git add firmware/src/main.cpp
git commit -m "feat: show FW_VERSION on OLED at boot"
```

---







## Fase 2 — GitHub Actions: Release Automático

### Task 2.1: Workflow de build + release al taggear

**Objective:** Al pushear un tag `vX.Y.Z`, GitHub Actions compila con PlatformIO y publica un Release con `firmware.bin`.

**Files:**
- Create: `.github/workflows/release.yml`

**Step 1: Crear workflow**

```yaml
name: Firmware Release

on:
  push:
    tags:
      - 'v*'

permissions:
  contents: write

jobs:
  build-and-release:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install PlatformIO
        run: pip install platformio

      - name: Build firmware
        run: pio run
        working-directory: firmware

      - name: Upload firmware.bin to Release
        uses: softprops/action-gh-release@v2
        with:
          files: firmware/.pio/build/esp32oled-ci/firmware.bin
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

**Step 2: Verificar workflow existe**

Run: `ls .github/workflows/release.yml`
Expected: archivo presente

**Step 3: Tag de prueba y verificar release**

```bash
git tag v0.1.0
git push origin v0.1.0
```

Luego verificar en `https://github.com/jorgelserve/espCICD/releases` que aparezca `v0.1.0` con `firmware.bin`.

**Commit:**
```bash
git add .github/workflows/release.yml
git commit -m "ci: add GitHub Actions release workflow on tag push"
```

---

## Fase 3 — Limpieza del Cliente Python

### Task 3.1: Eliminar código del cliente Python

**Objective:** Remover el cliente local de despliegue; el proyecto ahora es solo PlatformIO + auto-update.

**Files:**
- Delete: `src/esp32oled_ci/` (directorio completo)
- Delete: `tests/` (directorio completo)
- Delete: `pyproject.toml`
- Delete: `uv.lock`
- Modify: `.gitignore` (si hay entradas de Python)
- Modify: `README.md` (si existe y menciona el cliente)

**Step 1: Eliminar directorios**

```bash
rm -rf src/esp32oled_ci tests pyproject.toml uv.lock
```

**Step 2: Limpiar .gitignore**

Quitar entradas de Python si las hay (`__pycache__`, `.venv`, etc.).

**Step 3: Verificar que solo queda firmware/**

Run: `ls -la`
Expected: `firmware/`, `.github/`, `.gitignore`, `README.md`, `docs/`

**Commit:**
```bash
git add -A
git commit -m "refactor: remove Python client, project is PlatformIO-only"
```

---







## Fase 4 — Verificación Completa End-to-End

### Task 4.1: Simular update desde GitHub

**Objective:** Probar el flujo completo de auto-update sin necesidad de pushear un tag real al repo del usuario.

**Files:**
- Ninguno (verificación)

**Step 1: Desde el repo actual, crear un tag y pushear**

```bash
git tag v0.2.0
git push origin v0.2.0
```

**Step 2: Monitorear GitHub Actions**

```bash
gh run watch --exit-status
```

Expected: Workflow `Firmware Release` completa con éxito y el Release `v0.2.0` tiene `firmware.bin`.

**Step 3: Forzar check desde el ESP32**

```bash
telnet 192.168.78.102
# dentro de telnet:
check
```

Expected en telnet:
```
checking for updates...
ota 0%
ota 50%
ota 100%
ota validation: ok
```

**Step 4: Verificar que el dispositivo siga vivo**

```bash
curl -s http://192.168.78.102/status
```

Expected: JSON con `mode: sta`, SSID e IP.

**Step 5: Verificar rollback**

Si algo sale mal en el paso 3, el ESP32 debe volver a la versión anterior automáticamente en el próximo boot. Verificar en el OLED o telnet que la versión vuelve a `0.1.0`.

**Step 6: Limpiar tag de prueba** (opcional)

```bash
git tag -d v0.2.0
git push origin :refs/tags/v0.2.0
```

---

## Fase 5 — Documentación Mínima

### Task 5.1: Actualizar README

**Objective:** Documentar el flujo de auto-update y los comandos de telnet.

**Files:**
- Modify: `README.md` (si existe)

**Contenido mínimo:**
- Cómo actualizar: pushear un tag `vX.Y.Z` → GitHub Actions compila y publica Release → ESP32 lo descarga solo
- Comandos de telnet: `check` (forzar update), `status` (versión y estado)
- Portal web: `http://<ip>/` para provisioning WiFi
- Requisitos: WiFi + repo público en GitHub

**Commit:**
```bash
git add README.md
git commit -m "docs: document GitHub pull OTA flow"
```

---







## Resumen de Cambios

| Fase | Archivos | Acción |
|---|---|---|
| 1 | `firmware/platformio.ini`, `firmware/src/main.cpp` | Agregar SafeGithubOTA, init, comando telnet |
| 2 | `.github/workflows/release.yml` | CI: tag → build → release |
| 3 | `src/esp32oled_ci/`, `tests/`, `pyproject.toml`, `uv.lock` | Eliminar cliente Python |
| 4 | — | Verificación E2E |
| 5 | `README.md` | Documentación |

## Criterios de Aceptación

1. `pio run` compila firmware con SafeGithubOTA sin errores.
2. Al pushear tag `v*`, GitHub Actions publica un Release con `firmware.bin`.
3. Comando `check` por telnet dispara update desde GitHub.
4. Si el update falla o el firmware no valida, el ESP32 vuelve a la versión anterior.
5. No queda código del cliente Python en el repo.
6. El dispositivo funciona sin PC: solo WiFi + GitHub.
