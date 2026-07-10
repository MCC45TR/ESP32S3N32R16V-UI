#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdarg>

#include "WString.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

typedef struct {
    uint32_t queue_depth;
    uint32_t queue_capacity;
    uint32_t queue_high_watermark;
    uint32_t enqueued_bytes;
    uint32_t dropped_bytes;
    uint32_t processed_bytes;
    uint32_t emitted_lines;
    uint32_t truncated_lines;
} MrosConsoleDiagSnapshot;

typedef size_t (*MrosConsoleWriteFn)(const uint8_t *data, size_t size, void *user_data);
typedef int (*MrosConsoleWritableFn)(void *user_data);

struct MrosConsoleBackend {
    MrosConsoleWriteFn write = nullptr;
    MrosConsoleWritableFn writable = nullptr;
    void *user_data = nullptr;
};

class MROS_Console {
public:
    MROS_Console();
    void begin();
    
    // Process queued characters and append into the UART log ring buffer.
    // MUST ONLY BE CALLED FROM THE MAIN LOOP / TASK!
    void process();

    size_t write(uint8_t c);
    size_t write(const uint8_t *buffer, size_t size);
    size_t print(const char *text);
    size_t print(const String &text);
    size_t println(const char *text = "");
    size_t println(const String &text);
    int printf(const char *format, ...);

    void setMirrorBackend(const MrosConsoleBackend *backend);
    void clearMirrorBackend();
    void setSerialMirrorSuppressed(bool suppressed);
    bool serialMirrorSuppressed() const;
    void getDiagSnapshot(MrosConsoleDiagSnapshot* snapshot) const;

private:
    void mirrorImmediate(const uint8_t *buffer, size_t size);
    QueueHandle_t log_queue;
    volatile bool serial_mirror_suppressed;
    MrosConsoleBackend mirror_backend;
};

extern MROS_Console mros_console;

void mros_console_set_serial_mirror_suppressed(bool suppressed);
bool mros_console_is_serial_mirror_suppressed();
void mros_console_get_diag_snapshot(MrosConsoleDiagSnapshot* snapshot);
