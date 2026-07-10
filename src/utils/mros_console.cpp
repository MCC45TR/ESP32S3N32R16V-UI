#include "mros_console.h"

MROS_Console mros_console;

#include "src/drivers/uart/uart1_cobs_driver.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string.h>

#include "src/platform/mros_uart.h"

namespace {
constexpr UBaseType_t kConsoleQueueLength = 1024;
constexpr size_t kConsoleLineMax = 240;
constexpr int kConsoleMirrorBaud = 115200;
static volatile uint32_t g_console_queue_high_watermark = 0;
static volatile uint32_t g_console_enqueued_bytes = 0;
static volatile uint32_t g_console_dropped_bytes = 0;
static volatile uint32_t g_console_processed_bytes = 0;
static volatile uint32_t g_console_emitted_lines = 0;
static volatile uint32_t g_console_truncated_lines = 0;

inline void increment_counter(volatile uint32_t* counter) {
    if (counter == nullptr) {
        return;
    }
    *counter = *counter + 1U;
}

void update_console_queue_high_watermark(QueueHandle_t queue) {
    if (queue == NULL) {
        return;
    }
    const UBaseType_t depth = xPortInIsrContext()
                                  ? uxQueueMessagesWaitingFromISR(queue)
                                  : uxQueueMessagesWaiting(queue);
    if (depth > g_console_queue_high_watermark) {
        g_console_queue_high_watermark = depth;
    }
}

size_t serial_backend_write(const uint8_t *data, const size_t size, void *user_data) {
    const auto port = static_cast<uart_port_t>(reinterpret_cast<intptr_t>(user_data));
    if (data == nullptr || size == 0U || !mros::platform::mros_uart_is_ready(port)) {
        return 0U;
    }
    const int written = mros::platform::mros_uart_write(port, data, size);
    return written > 0 ? static_cast<size_t>(written) : 0U;
}

int serial_backend_writable(void *user_data) {
    const auto port = static_cast<uart_port_t>(reinterpret_cast<intptr_t>(user_data));
    return mros::platform::mros_uart_writable(port);
}

MrosConsoleBackend default_serial_backend() {
    mros::platform::UartConfig config = {};
    config.port = UART_NUM_0;
    config.tx_pin = UART_PIN_NO_CHANGE;
    config.rx_pin = UART_PIN_NO_CHANGE;
    config.baud_rate = kConsoleMirrorBaud;
    config.rx_buffer_size = 1024;
    config.tx_buffer_size = 1024;
    config.queue_size = 0;
    (void)mros::platform::mros_uart_init(config);

    MrosConsoleBackend backend;
    backend.write = serial_backend_write;
    backend.writable = serial_backend_writable;
    backend.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(UART_NUM_0));
    return backend;
}
}

MROS_Console::MROS_Console() {
    log_queue = NULL;
    serial_mirror_suppressed = false;
    mirror_backend = {};
}

void MROS_Console::begin() {
    if (log_queue == NULL) {
        // Create queue for 1024 characters
        log_queue = xQueueCreate(kConsoleQueueLength, sizeof(uint8_t));
    }
    if (mirror_backend.write == nullptr) {
        mirror_backend = default_serial_backend();
    }
}

size_t MROS_Console::write(uint8_t c) {
    mirrorImmediate(&c, 1U);

    // Safely buffer to queue if initialized
    if (log_queue != NULL) {
        BaseType_t sent = pdFALSE;
        if (xPortInIsrContext()) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            sent = xQueueSendFromISR(log_queue, &c, &xHigherPriorityTaskWoken);
        } else {
            // Runtime logging must never block control or web init paths.
            sent = xQueueSend(log_queue, &c, 0);
        }
        if (sent == pdTRUE) {
            increment_counter(&g_console_enqueued_bytes);
            update_console_queue_high_watermark(log_queue);
        } else {
            increment_counter(&g_console_dropped_bytes);
        }
    }
    return 1;
}

size_t MROS_Console::write(const uint8_t *buffer, size_t size) {
    if (buffer == nullptr || size == 0U) {
        return 0U;
    }

    mirrorImmediate(buffer, size);

    // Safely push each char to queue
    if (log_queue != NULL) {
        const bool in_isr = xPortInIsrContext();
        size_t enqueue_limit = size;
        if (!in_isr) {
            const UBaseType_t spaces = uxQueueSpacesAvailable(log_queue);
            if (enqueue_limit > static_cast<size_t>(spaces)) {
                g_console_dropped_bytes +=
                    static_cast<uint32_t>(enqueue_limit - static_cast<size_t>(spaces));
                enqueue_limit = static_cast<size_t>(spaces);
            }
        }
        for (size_t i = 0; i < enqueue_limit; i++) {
            BaseType_t sent = pdFALSE;
            if (in_isr) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                sent = xQueueSendFromISR(log_queue, &buffer[i], &xHigherPriorityTaskWoken);
            } else {
                sent = xQueueSend(log_queue, &buffer[i], 0);
            }
            if (sent == pdTRUE) {
                increment_counter(&g_console_enqueued_bytes);
                update_console_queue_high_watermark(log_queue);
            } else {
                increment_counter(&g_console_dropped_bytes);
            }
        }
    }
    return size;
}

size_t MROS_Console::print(const char *text) {
    if (text == nullptr) {
        return 0U;
    }
    return write(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

size_t MROS_Console::print(const String &text) {
    return write(reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
}

size_t MROS_Console::println(const char *text) {
    size_t written = print(text);
    static constexpr char kNewline[] = "\n";
    written += write(reinterpret_cast<const uint8_t *>(kNewline), 1U);
    return written;
}

size_t MROS_Console::println(const String &text) {
    size_t written = print(text);
    static constexpr char kNewline[] = "\n";
    written += write(reinterpret_cast<const uint8_t *>(kNewline), 1U);
    return written;
}

int MROS_Console::printf(const char *format, ...) {
    if (format == nullptr) {
        return 0;
    }

    char stack_buffer[256] = {};
    va_list args;
    va_start(args, format);
    va_list args_copy;
    va_copy(args_copy, args);
    const int needed = std::vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return needed;
    }
    if (static_cast<size_t>(needed) < sizeof(stack_buffer)) {
        write(reinterpret_cast<const uint8_t *>(stack_buffer), static_cast<size_t>(needed));
        va_end(args_copy);
        return needed;
    }

    std::string dynamic_buffer(static_cast<size_t>(needed) + 1U, '\0');
    std::vsnprintf(dynamic_buffer.data(), dynamic_buffer.size(), format, args_copy);
    va_end(args_copy);
    write(reinterpret_cast<const uint8_t *>(dynamic_buffer.c_str()), static_cast<size_t>(needed));
    return needed;
}

void MROS_Console::setMirrorBackend(const MrosConsoleBackend *backend) {
    if (backend == nullptr) {
        clearMirrorBackend();
        return;
    }
    mirror_backend = *backend;
}

void MROS_Console::clearMirrorBackend() {
    mirror_backend = {};
}

void MROS_Console::setSerialMirrorSuppressed(bool suppressed) {
    serial_mirror_suppressed = suppressed;
}

bool MROS_Console::serialMirrorSuppressed() const {
    return serial_mirror_suppressed;
}

void MROS_Console::mirrorImmediate(const uint8_t *buffer, size_t size) {
    if (serial_mirror_suppressed || buffer == nullptr || size == 0U ||
        mirror_backend.write == nullptr) {
        return;
    }
    size_t allowed = size;
    if (mirror_backend.writable != nullptr) {
        const int writable = mirror_backend.writable(mirror_backend.user_data);
        if (writable <= 0) {
            return;
        }
        allowed = std::min<size_t>(allowed, static_cast<size_t>(writable));
    }
    if (allowed == 0U) {
        return;
    }
    mirror_backend.write(buffer, allowed, mirror_backend.user_data);
}

void MROS_Console::process() {
    if (log_queue == NULL) return;

    static char line_buf_s3[kConsoleLineMax + 10] = {0};
    static size_t line_len_s3 = 0;
    static bool line_truncated = false;
    uint8_t c;
    // Drain the queue completely
    while (xQueueReceive(log_queue, &c, 0) == pdTRUE) {
        increment_counter(&g_console_processed_bytes);
        if (line_len_s3 < kConsoleLineMax) {
            line_buf_s3[line_len_s3++] = (char)c;
            line_buf_s3[line_len_s3] = '\0';
        } else {
            line_truncated = true;
        }
        if (c == '\n') {
            if (line_truncated) {
                const char* trunc = "[TRUNC]\n";
                const size_t room = sizeof(line_buf_s3) - line_len_s3 - 1U;
                const size_t trunc_len = strlen(trunc);
                const size_t copy_len = trunc_len < room ? trunc_len : room;
                memcpy(line_buf_s3 + line_len_s3, trunc, copy_len);
                line_len_s3 += copy_len;
                line_buf_s3[line_len_s3] = '\0';
                increment_counter(&g_console_truncated_lines);
            }
            appendSystemLog("S3", String(line_buf_s3));
            increment_counter(&g_console_emitted_lines);
            // Forward S3 runtime logs to Teensy 4.1 over shared UART.
            if (mros::platform::mros_uart_is_ready(UART_NUM_1)) {
                (void)mros::platform::mros_uart_write(UART_NUM_1, "S3:", 3U);
                (void)mros::platform::mros_uart_write(UART_NUM_1, line_buf_s3, strlen(line_buf_s3));
            }
            line_len_s3 = 0;
            line_buf_s3[0] = '\0';
            line_truncated = false;
        }
    }
}

void MROS_Console::getDiagSnapshot(MrosConsoleDiagSnapshot* snapshot) const {
    if (snapshot == nullptr) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->queue_capacity = kConsoleQueueLength;
    snapshot->queue_depth =
        (log_queue != NULL) ? uxQueueMessagesWaiting(log_queue) : 0U;
    snapshot->queue_high_watermark = g_console_queue_high_watermark;
    snapshot->enqueued_bytes = g_console_enqueued_bytes;
    snapshot->dropped_bytes = g_console_dropped_bytes;
    snapshot->processed_bytes = g_console_processed_bytes;
    snapshot->emitted_lines = g_console_emitted_lines;
    snapshot->truncated_lines = g_console_truncated_lines;
}

void mros_console_set_serial_mirror_suppressed(bool suppressed) {
    mros_console.setSerialMirrorSuppressed(suppressed);
}

bool mros_console_is_serial_mirror_suppressed() {
    return mros_console.serialMirrorSuppressed();
}

void mros_console_get_diag_snapshot(MrosConsoleDiagSnapshot* snapshot) {
    mros_console.getDiagSnapshot(snapshot);
}
