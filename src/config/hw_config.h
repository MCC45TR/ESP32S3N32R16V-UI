#pragma once

// --- ESP32-S3 / MROS DEUSCARA Pin Configuration ---

// On-board addressable RGB LED.
// ESP32-S3-DevKitC-1 v1.1 uses GPIO38; initial/v1.0 boards often used GPIO48.
#ifndef PIN_ONBOARD_RGB_LED
#define PIN_ONBOARD_RGB_LED 38
#endif

// 1. Classic SPI slave link (Master target: Teensy 4.1).
// This is not the active Teensy FlexIO1 4-bit QuadSPI endpoint yet.
// 10(CS), 11(MOSI), 12(SCK), 13(MISO), then handshake lines.
#define PIN_SPI_CS   10
#define PIN_SPI_MOSI 11
#define PIN_SPI_SCK  12
#define PIN_SPI_MISO 13
// Optional 4-bit lane pins for Quad-capable endpoint bring-up.
// Keep as -1 unless board routing explicitly provides IO2/IO3 lanes.
#define PIN_SPI_WP   -1
#define PIN_SPI_HD   -1

// 2. Synchronization with Teensy4.1
// ESP_READY: S3 -> Teensy (peer-ready/transaction-ready indicator)
#define PIN_DATA_READY 14
// T41_READY: Teensy -> S3 (Teensy transport-ready indicator input)
#define PIN_T41_READY 15
// Optional IRQ/RESET handshake lines (enabled for T41 MQ transport).
#define PIN_TEENSY_IRQ   39
#define PIN_TEENSY_RESET 40
// Legacy alias kept for compatibility with existing diagnostics commands.
#define PIN_ALIVE_LED PIN_T41_READY

// 3. UART1 (Teensy status/control link, target 5 Mbps)
// Adjacent block for cleaner routing and optional HW flow control.
#define PIN_UART1_CTS 16 // Teensy4.1 RTS(Serial5 pin19) -> ESP32-S3 CTS
#define PIN_UART1_TX  17 // ESP32-S3 -> Teensy4.1 RX(Serial5 pin21)
#define PIN_UART1_RX  18 // Teensy4.1 TX(Serial5 pin20) -> ESP32-S3
#define PIN_UART1_RTS -1 // Reserved (Teensy CTS line currently passive)

// 4. Optional C3 SPI link (disabled in DEUSCARA-Teensy focused topology)
// Keep these as -1 to avoid accidental pin conflicts with Teensy wiring.
#define PIN_C3_SPI_SCK  -1
#define PIN_C3_SPI_MOSI -1
#define PIN_C3_SPI_MISO -1
#define PIN_C3_SPI_CS   -1
#define PIN_C3_ALIVE    -1

// 5. I2C (PCA9685 Servo Driver, 7-bit addr: 0x40)
#define PIN_I2C_SDA 48 // S3-48 <-> PCA-SDA
#define PIN_I2C_SCL 47 // S3-47 <-> PCA-SCL

// 6. PCA9685 Output Enable (Active LOW = outputs ON, HIGH = outputs OFF)
#define PIN_PCA_OE 21 // S3-21 <-> PCA-OE

// Canonical aliases for thesis documentation / cross-board references.
#define PIN_TEENSY_QSPI_CS    PIN_SPI_CS
#define PIN_TEENSY_QSPI_MOSI  PIN_SPI_MOSI
#define PIN_TEENSY_QSPI_SCK   PIN_SPI_SCK
#define PIN_TEENSY_QSPI_MISO  PIN_SPI_MISO
#define PIN_TEENSY_QSPI_IO0   PIN_SPI_MOSI
#define PIN_TEENSY_QSPI_IO1   PIN_SPI_MISO
#define PIN_TEENSY_QSPI_IO2   PIN_SPI_WP
#define PIN_TEENSY_QSPI_IO3   PIN_SPI_HD
#define PIN_TEENSY_SYNC_READY PIN_DATA_READY
#define PIN_TEENSY_SYNC_ALIVE PIN_T41_READY
#define PIN_TEENSY_SYNC_IRQ   PIN_TEENSY_IRQ
#define PIN_TEENSY_SYNC_RESET PIN_TEENSY_RESET
#define PIN_TEENSY_UART_TX    PIN_UART1_TX
#define PIN_TEENSY_UART_RX    PIN_UART1_RX
#define PIN_TEENSY_UART_CTS   PIN_UART1_CTS
#define PIN_TEENSY_UART_RTS   PIN_UART1_RTS
