#pragma once

#include <stdint.h>

constexpr uint32_t BIT_NET_ACTIVE = 1UL << 0;
constexpr uint32_t BIT_FK_ACTIVE = 1UL << 1;
constexpr uint32_t BIT_SHELL_ACTIVE = 1UL << 2;
constexpr uint32_t BIT_TRAJ_ACTIVE = 1UL << 3;
constexpr uint32_t BIT_STORAGE_DIRTY = 1UL << 4;

void event_bus_init();
void event_bus_publish(uint32_t event_mask);
void event_bus_clear(uint32_t event_mask);
uint32_t event_bus_wait(uint32_t event_mask, uint32_t timeout_ms);
