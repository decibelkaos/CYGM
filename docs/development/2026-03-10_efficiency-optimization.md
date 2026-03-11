# CYGM Efficiency Optimization Checklist

**Created:** 2026-03-10
**Device:** JC2432W328 (ESP32-D0WD, 4MB Flash, 520KB SRAM)
**Firmware:** 1.6.0 Beta
**Binary Size (before):** 1,644,448 bytes (1.57 MB) — 51% of 3MB factory partition

---

## Checklist

| # | Item | Priority | Status | Est. Savings |
|---|------|----------|--------|-------------|
| A | Compiler optimization: DEBUG → SIZE | Critical | [x] APPLIED | ~250-400 KB flash, faster execution |
| B | FreeRTOS tick rate: 1000Hz → 100Hz | Critical | [x] APPLIED | CPU cycles (10x fewer scheduler interrupts) |
| C | Reclaim SPIFFS → OTA dual partition | Critical | [x] APPLIED | OTA updates enabled (2x 1.94MB slots) |
| D | mbedTLS SSL_IN_CONTENT_LEN: 8192 → 4096 | Critical | [~] BLOCKED | 4 KB peak RAM (see notes) |
| E | Disable mbedTLS TLS Server | Medium | [x] APPLIED | ~10-20 KB flash |
| F | Strip unused mbedTLS features | Medium | [x] APPLIED | ~15-25 KB flash |
| G | Reduce ECP curves: 12 → 2 | Medium | [x] APPLIED | ~20-40 KB flash |
| H | Certificate bundle: FULL → CMN | Medium | [x] APPLIED | ~25 KB flash |
| I | Disable IPv6 | **Critical** | [x] APPLIED | ~7 KB DRAM + ~39 KB flash |
| J | Disable DHCP server + SoftAP | **Critical** | [x] APPLIED | ~3-5 KB DRAM + flash |
| K | Reduce network buffer counts | **Critical** | [x] APPLIED | ~6.4 KB DRAM (WiFi static RX) |
| L | Enable NEWLIB_NANO_FORMAT | Medium | [x] APPLIED | ~50 KB flash |
| M | Log level: INFO → WARN (production) | Minor | [ ] TODO | Flash (fewer strings) |
| N | CPU frequency: 160MHz → 240MHz | Minor | [x] APPLIED | ~33% faster SSL/UI |

---

## A. Compiler Optimization (DEBUG → SIZE)

**What:** `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` uses `-Og` (debug-friendly, no size optimization). Switching to `CONFIG_COMPILER_OPTIMIZATION_SIZE` uses `-Os` which aggressively optimizes for code size.

**Why it matters:** On ESP32, smaller code = fewer instruction cache misses = faster execution. Debug optimization is 15-25% larger with no benefit for a production device.

**How:** `idf.py menuconfig` → Compiler options → Optimization Level → Optimize for size (-Os)

**Before:**
```
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y  (-Og)
Binary: 1,644,448 bytes (1.57 MB)
```

**After:**
```
CONFIG_COMPILER_OPTIMIZATION_SIZE=y  (-Os)
Binary: 1,518,256 bytes (1.45 MB) — SAVED 126,192 bytes (123 KB, 7.7%)
```

**Notes:**
- Backtraces and panic handler output still work perfectly
- Actually FASTER on ESP32 because smaller code = fewer instruction cache misses
- Safe to revert anytime by changing back to DEBUG
- Also fixed UTF-8 BOM in sdkconfig.defaults (caused warning on every build)

---

## B. FreeRTOS Tick Rate (1000Hz → 100Hz)

**What:** `CONFIG_FREERTOS_HZ=1000` fires the scheduler tick interrupt every 1ms. Default ESP-IDF is 100Hz (every 10ms).

**Why it matters:** At 1000Hz, the CPU handles 10x more timer interrupts than needed. This device doesn't need sub-10ms `vTaskDelay` precision. 100Hz is plenty for UI updates, network polling, and alarms.

**Caveat:** Any `vTaskDelay(1)` calls currently delay 1ms. At 100Hz they'd delay 10ms. This is fine for debouncing and polling but verify there are no timing-critical 1ms delays.

**How:** `idf.py menuconfig` → Component config → FreeRTOS → Kernel → configTICK_RATE_HZ → 100

**Before:**
```
CONFIG_FREERTOS_HZ=1000
```

**After:**
```
CONFIG_FREERTOS_HZ=100
```

**Code fixes required (sub-10ms delays → 0 ticks at 100Hz):**
```
battery.c:78  — vTaskDelay(pdMS_TO_TICKS(2))  → vTaskDelay(1)  // ADC inter-sample yield
battery.c:111 — vTaskDelay(pdMS_TO_TICKS(1))  → vTaskDelay(1)  // Quick ADC yield
led.c:94      — vTaskDelay(pdMS_TO_TICKS(6))  → vTaskDelay(1)  // LED fade step (was 480ms, now 800ms fade — still looks great)
```

**Also updated:** sdkconfig.defaults (prevents revert on reconfigure)

---

## C. Reclaim SPIFFS → OTA Dual Partition

**What:** Replaced unused SPIFFS (960KB) + single factory app with dual OTA partition layout. Enables wireless firmware updates.

**Before:**
```csv
nvs,      data, nvs,     0x9000,   0x6000,    # 24KB
phy_init, data, phy,     0xf000,   0x1000,    # 4KB
factory,  app,  factory, 0x10000,  0x300000,  # 3MB (single slot)
spiffs,   data, spiffs,  0x310000, 0xF0000,   # 960KB (UNUSED)
```

**After:**
```csv
nvs,      data, nvs,     0x9000,   0x4000,    # 16KB
otadata,  data, ota,     0xd000,   0x2000,    # 8KB (tracks active slot)
phy_init, data, phy,     0xf000,   0x1000,    # 4KB
ota_0,    app,  ota_0,   0x10000,  0x1F0000,  # 1.94MB
ota_1,    app,  ota_1,   0x200000, 0x1F0000,  # 1.94MB
```

**Changes made:**
- `partitions.csv` — new OTA layout
- `sdkconfig.defaults` — `CONFIG_PARTITION_TABLE_CUSTOM=y`, rollback enabled
- `update_checker.c` — full OTA download with progress overlay, rollback confirmation
- `update_checker.h` — added `firmware_url`, `update_start_ota()`, `update_confirm_boot()`
- `main.c` — calls `update_confirm_boot()` on successful init
- `version.json` — added `firmware_url` field for OTA download URL
- `CMakeLists.txt` — added `esp_https_ota` dependency

**Notes:**
- NVS reduced 24KB→16KB (device uses ~3KB, plenty of headroom)
- Each OTA slot: 1.94MB vs ~1.52MB binary = ~420KB growth headroom
- First flash after partition change MUST be via USB
- Subsequent updates can be wireless (OTA)
- Rollback: if new firmware crashes before `update_confirm_boot()`, bootloader auto-reverts
- OTA download: closes Dexcom SSL first, uses ~20-25KB DRAM, streams in 1KB chunks

---

## D. mbedTLS SSL_IN_CONTENT_LEN (8192 → 4096)

**What:** Maximum TLS input record buffer. With `VARIABLE_BUFFER_LENGTH=y`, this is the ceiling, not the allocation. Currently 8192 (8KB max), but the device's TLS connections work fine with 4096.

**Why it matters:** During SSL handshakes on a fragmented heap, a 4KB cap prevents mbedTLS from trying to allocate an 8KB contiguous block. This directly helps the "min_ever" decline issue.

**BLOCKED:** `sdkconfig.defaults` explicitly sets this to 8192 with the comment:
"mbedTLS - Larger input buffer for certificate chains (digitalkaos.tech needs ~7KB)"

The firmware update checker connects to digitalkaos.tech which sends a certificate chain
that exceeds 4096 bytes. Reducing this would break OTA update checking with
`MBEDTLS_ERR_SSL_ALLOC_FAILED` or truncated certificate chain errors.

**Decision: Keep at 8192.** The 4KB potential savings is not worth breaking the update checker.

**Before:**
```
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192  (required by digitalkaos.tech cert chain)
```

**After:**
```
N/A — keeping at 8192
```

---

## E. Disable mbedTLS TLS Server

**What:** `CONFIG_MBEDTLS_TLS_SERVER=y` includes server-side TLS code. This device is a pure HTTPS client (Dexcom API, Open-Meteo, GitHub).

**Why it matters:** TLS server code paths, server-side cipher suites, and session ticket server logic all waste flash and code cache.

**How:** `idf.py menuconfig` → Component config → mbedTLS → TLS → Change to "TLS client only"

**Before:**
```
CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=y
CONFIG_MBEDTLS_TLS_SERVER=y
```

**After:**
```
CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y
```

---

## F. Strip Unused mbedTLS Features

**What:** Several mbedTLS features are enabled that this device never uses:
- `MBEDTLS_SSL_RENEGOTIATION` — Server-initiated renegotiation (not needed for client)
- `MBEDTLS_PEM_WRITE_C` — Generate PEM certificates (never done)
- `MBEDTLS_X509_CRL_PARSE_C` — Certificate Revocation Lists (not checked)
- `MBEDTLS_X509_CSR_PARSE_C` — Certificate Signing Requests (not generated)
- `MBEDTLS_PKCS7_C` — PKCS#7 format (not used)
- ~~`MBEDTLS_SHA512_C`~~ — **MUST KEEP**: SHA-384 is required for Dexcom cert validation (ECDSA SHA-384)

**How:** `idf.py menuconfig` → Component config → mbedTLS → disable each feature

**Before:**
```
CONFIG_MBEDTLS_SSL_RENEGOTIATION=y
CONFIG_MBEDTLS_PEM_WRITE_C=y
CONFIG_MBEDTLS_X509_CRL_PARSE_C=y
CONFIG_MBEDTLS_X509_CSR_PARSE_C=y
CONFIG_MBEDTLS_PKCS7_C=y
CONFIG_MBEDTLS_SHA512_C=y
```

**After:**
```
CONFIG_MBEDTLS_SSL_RENEGOTIATION=n
CONFIG_MBEDTLS_PEM_WRITE_C=n
CONFIG_MBEDTLS_X509_CRL_PARSE_C=n
CONFIG_MBEDTLS_X509_CSR_PARSE_C=n
CONFIG_MBEDTLS_PKCS7_C=n
# CONFIG_MBEDTLS_SHA512_C=y  ← KEPT (SHA-384 needed for Dexcom ECDSA certs)
```

---

## G. Reduce ECP Curves (12 → 3)

**What:** 12 elliptic curves are enabled for key exchange. Reduced to 3 essential curves.

**Curves to KEEP:**
- `SECP256R1` (P-256) — most common
- `SECP384R1` (P-384) — used by Dexcom certs (ECDSA SHA-384)
- `CURVE25519` — modern alternative

**Curves to DISABLE:**
- SECP192R1, SECP224R1, SECP521R1
- SECP192K1, SECP224K1, SECP256K1
- BP256R1, BP384R1, BP512R1

**Why it matters:** Each curve adds 2-5KB flash for its implementation. 10 unused curves = ~20-40KB wasted.

**How:** `idf.py menuconfig` → Component config → mbedTLS → Elliptic Curve → disable unused curves

**Before:**
```
CONFIG_MBEDTLS_ECP_DP_SECP192R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP224R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y   # KEEP
CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP521R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP192K1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP224K1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP256K1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_BP256R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_BP384R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_BP512R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED=y   # KEEP
```

**After:**
```
CONFIG_MBEDTLS_ECP_DP_SECP192R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP224R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED=y   # MUST KEEP (Dexcom ECDSA certs)
CONFIG_MBEDTLS_ECP_DP_SECP521R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP192K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP224K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP256K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP256R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP384R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP512R1_ENABLED=n
# KEPT: SECP256R1 + SECP384R1 + CURVE25519
```

---

## H. Certificate Bundle (FULL → CMN)

**What:** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` embeds 200+ root CA certificates. `DEFAULT_CMN` includes only ~40 commonly-used CAs.

**Why it matters:** This device connects to 3 servers (Dexcom Share API, Open-Meteo weather, GitHub releases). All use certificates signed by major CAs (DigiCert, Let's Encrypt, etc.) that are included in the common bundle.

**Risk:** If any server uses an uncommon CA, TLS handshake would fail. Low risk for these well-known services.

**How:** `idf.py menuconfig` → Component config → mbedTLS → Certificate Bundle → Use common certificate bundle

**Before:**
```
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS=200
```

**After:**
```
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y
# ~40 common CAs vs 200+ full bundle
```

---

## I. Disable IPv6

**What:** `CONFIG_LWIP_IPV6=y` enables full IPv6 stack (neighbor discovery, routing, address management). This device connects to WiFi as a station using IPv4 only.

**Why it matters:** IPv6 adds ~20KB flash for the protocol stack plus RAM for data structures (neighbor tables, prefixes, routers, destinations — currently configured for 5+5+3+10 entries).

**How:** `idf.py menuconfig` → Component config → LWIP → Enable IPv6 → disable

**Before:**
```
CONFIG_LWIP_IPV6=y
CONFIG_LWIP_IPV6_NUM_ADDRESSES=3
CONFIG_LWIP_IPV6_ND6_NUM_NEIGHBORS=5
CONFIG_LWIP_IPV6_ND6_NUM_PREFIXES=5
CONFIG_LWIP_IPV6_ND6_NUM_ROUTERS=3
CONFIG_LWIP_IPV6_ND6_NUM_DESTINATIONS=10
```

**After:**
```
# CONFIG_LWIP_IPV6 is not set
# All IPv6 ND6 tables removed — saves ~7KB DRAM + ~39KB flash
```

**Notes:**
- Applied together with J and K to fix critical SSL handshake failure
- Root cause: only ~24KB DRAM available (not 51KB — MALLOC_CAP_INTERNAL was counting ~27KB of unusable IRAM)
- SSL handshake needs ~50KB peak DRAM → guaranteed failure at 24KB
- These three items combined free ~16-20KB DRAM, bringing free DRAM to ~40-44KB

---

## J. Disable DHCP Server + SoftAP

**What:** DHCP server (`CONFIG_LWIP_DHCPS=y`) and SoftAP support (`CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y`) are enabled. This device is a WiFi station — it never hosts a network or serves DHCP.

**How:**
- `idf.py menuconfig` → Component config → LWIP → Enable DHCP server → disable
- `idf.py menuconfig` → Component config → Wi-Fi → SoftAP → disable

**Before:**
```
CONFIG_LWIP_DHCPS=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
```

**After:**
```
# CONFIG_LWIP_DHCPS is not set
# CONFIG_ESP_WIFI_SOFTAP_SUPPORT is not set
```

---

## K. Reduce Network Buffer Counts

**What:** Network PCB/socket counts are set for a device running multiple servers and clients simultaneously. This device has 1-2 concurrent TCP connections max.

**How:** `idf.py menuconfig` → Component config → LWIP → various settings

**Before:**
```
CONFIG_LWIP_MAX_SOCKETS=10
CONFIG_LWIP_MAX_ACTIVE_TCP=16
CONFIG_LWIP_MAX_LISTENING_TCP=16
CONFIG_LWIP_MAX_UDP_PCBS=16
CONFIG_LWIP_MAX_RAW_PCBS=16
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32
```

**After:**
```
CONFIG_LWIP_MAX_SOCKETS=4
CONFIG_LWIP_MAX_ACTIVE_TCP=4
CONFIG_LWIP_MAX_LISTENING_TCP=2
CONFIG_LWIP_MAX_UDP_PCBS=4
CONFIG_LWIP_MAX_RAW_PCBS=4
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6    (matches RX_BA_WIN=6, saves ~6.4KB)
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16
```

**Notes:**
- WiFi static RX buffers: each ~1.6KB, saving 4 buffers = ~6.4KB permanent DRAM
- Static RX = 6 matches RX_BA_WIN = 6 (minimum for AMPDU)
- Dynamic buffers only cap peak allocation, not baseline
- Listening TCP reduced to 2 (device never listens, but 0 may cause issues)

---

## L. Enable NEWLIB_NANO_FORMAT

**What:** Full newlib printf includes floating-point formatting, wide character support, and other features. Nano printf is much smaller. The device uses `%f` for battery voltage display but nano printf supports this with a linker flag.

**How:** `idf.py menuconfig` → Component config → Newlib → Enable nano formatting → yes

**Before:**
```
# CONFIG_NEWLIB_NANO_FORMAT is not set
```

**After:**
```
CONFIG_NEWLIB_NANO_FORMAT=y
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
```

---

## M. Log Level (INFO → WARN)

**What:** `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` compiles all `ESP_LOGI()`, `ESP_LOGW()`, and `ESP_LOGE()` calls with their string constants. Switching to WARN removes INFO strings from the binary.

**Why it matters:** The codebase has hundreds of `ESP_LOGI()` calls. Each one embeds a format string in flash. Removing them saves flash and eliminates serial output overhead during normal operation.

**Caveat:** You lose diagnostic visibility. Consider keeping INFO during development and switching to WARN for production builds.

**How:** `idf.py menuconfig` → Component config → Log → Default log verbosity → Warning

**Before:**
```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL=3
```

**After:**
```
(pending)
```

---

## N. CPU Frequency (160MHz → 240MHz)

**What:** ESP32-D0WD supports up to 240MHz. Currently running at 160MHz.

**Why it matters:** 240MHz gives ~33% more CPU throughput for SSL handshakes (the biggest CPU consumer), JSON parsing, and LVGL rendering. The tradeoff is slightly higher power consumption (~10-20mA more).

**For a battery device:** You could use 240MHz during active operations and dynamic frequency scaling to 80MHz during idle. Or just run 240MHz — the battery impact is modest compared to WiFi radio power.

**How:** `idf.py menuconfig` → Component config → ESP System Settings → CPU frequency → 240 MHz

**Before:**
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
```

**After:**
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
```

---

## Running Total

| Metric | Before | After A+B+I+J+K | After All (A-N exc. D,M) |
|--------|--------|---------|-----------------|
| Binary size | 1,644,448 bytes | ~1,480,000 bytes | Est. ~1,250,000 bytes (-24%) |
| Free flash (per OTA slot) | — | ~460 KB | Est. ~690 KB |
| DRAM freed | — | ~16-20 KB | ~20-25 KB |
| CPU efficiency | 160MHz + 1000Hz tick | 160MHz + 100Hz tick | 240MHz + 100Hz tick |
| mbedTLS flash | baseline | baseline | -70-110 KB (E+F+G+H) |
| printf flash | baseline | baseline | -50 KB (L) |

### Critical Bug Fixes Applied with I/J/K

1. **Ghost heap metric** — Memory checks used `MALLOC_CAP_INTERNAL` (DRAM + IRAM = 51KB) instead of `MALLOC_CAP_DEFAULT` (DRAM only = 24KB). SSL needs DRAM. Fixed to `MALLOC_CAP_DEFAULT`.
2. **Aggressive SSL close** — `background_tasks.c` closed SSL after initial auth fetch, forcing a new ~50KB handshake when glucose task started with only 24KB DRAM. Fixed: SSL kept alive for glucose task to reuse.
3. **Insufficient DRAM** — Items I/J/K free ~16-20KB DRAM, bringing available DRAM from ~24KB to ~40-44KB.
