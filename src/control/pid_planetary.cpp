#include "pid_planetary.h"
#include <cmath>
#include <math.h>

namespace {
constexpr float kMinDtS = 0.0005f;
constexpr float kMaxDtS = 0.1f;

float clampf(float value, float lo, float hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

uint32_t sat_inc(uint32_t value) {
    return value == 0xFFFFFFFFUL ? value : value + 1U;
}
}

void planetary_pid_init(PlanetaryPID_t* pid, float kp, float ki, float kd, float ts) {
    pid->Kp = kp;
    pid->Ki = ki;
    pid->Kd = kd;
    pid->Ts = (ts > 0.0f) ? ts : 0.001f; // Default 1kHz if invalid
    
    pid->GearRatio_EncToCarrier = 0.2f;   // Willis Factor
    // Sign note:
    // Channel direction inversion (kTurretDirCh*) is already applied in the
    // turret PWM layer. Keeping this ratio positive avoids double inversion.
    pid->GearRatio_MotToCarrier = 0.375f; // Gearbox Trans
    
    pid->last_error = 0.0f;
    pid->prev_error = 0.0f;
    pid->prev_derivative = 0.0f;
    pid->integrator = 0.0f;
    pid->last_output = 0.0f;
    pid->last_dt = pid->Ts;
    pid->saturation_count = 0;
    pid->integral_clamp_count = 0;
    pid->finite_fault_count = 0;
    pid->max_abs_error = 0.0f;
    pid->derivative_alpha = 0.25f;
    pid->slew_rate_per_s = 0.0f;

    // Default safe limits
    pid->out_min = -100.0f;
    pid->out_max = 100.0f;
    pid->i_limit = 50.0f;
}

float planetary_pid_compute(PlanetaryPID_t* pid, float target_carrier_deg, float sun_encoder_deg) {
    if (pid == nullptr) return 0.0f;
    if (!std::isfinite(target_carrier_deg) || !std::isfinite(sun_encoder_deg)) {
        pid->finite_fault_count = sat_inc(pid->finite_fault_count);
        return pid->last_output;
    }

    // 1. Final Position Feedback (Resolution Advantage Factor: 5)
    float actual_carrier_deg = sun_encoder_deg * pid->GearRatio_EncToCarrier;
    
    // 2. Error Calculation
    float error = target_carrier_deg - actual_carrier_deg;

    float dt = pid->Ts;
    if (!std::isfinite(dt) || dt < kMinDtS) dt = kMinDtS;
    if (dt > kMaxDtS) dt = kMaxDtS;
    pid->last_dt = dt;
    const float abs_error = fabsf(error);
    if (abs_error > pid->max_abs_error) pid->max_abs_error = abs_error;

    // 3. Positional PID with explicit integrator state.
    float proportional = pid->Kp * error;
    pid->integrator += pid->Ki * dt * error;
    if (pid->integrator > pid->i_limit) {
        pid->integrator = pid->i_limit;
        pid->integral_clamp_count = sat_inc(pid->integral_clamp_count);
    } else if (pid->integrator < -pid->i_limit) {
        pid->integrator = -pid->i_limit;
        pid->integral_clamp_count = sat_inc(pid->integral_clamp_count);
    }

    const float raw_derivative = (error - pid->last_error) / dt;
    const float alpha = clampf(pid->derivative_alpha, 0.0f, 1.0f);
    pid->prev_derivative = (pid->prev_derivative * (1.0f - alpha)) + (raw_derivative * alpha);
    float derivative = pid->Kd * pid->prev_derivative;
    float carrier_output = proportional + pid->integrator + derivative;
    if (!std::isfinite(carrier_output)) {
        pid->finite_fault_count = sat_inc(pid->finite_fault_count);
        return pid->last_output;
    }

    // 4. Actuation Normalization
    // Since the output is Carrier speed, and Omega_c = -0.375 * Omega_m,
    // the motor command must be scaled by the inverse of the actuation ratio.
    float output = carrier_output;
    if (fabsf(pid->GearRatio_MotToCarrier) > 0.001f) {
        output = carrier_output / pid->GearRatio_MotToCarrier;
    }

    // 5. Saturation and Anti-Windup (back out the last integral step if saturated)
    if (pid->slew_rate_per_s > 0.0f) {
        const float max_step = pid->slew_rate_per_s * dt;
        output = clampf(output, pid->last_output - max_step, pid->last_output + max_step);
    }

    if (output > pid->out_max) {
        output = pid->out_max;
        pid->integrator -= pid->Ki * dt * error;
        pid->saturation_count = sat_inc(pid->saturation_count);
    } else if (output < pid->out_min) {
        output = pid->out_min;
        pid->integrator -= pid->Ki * dt * error;
        pid->saturation_count = sat_inc(pid->saturation_count);
    }

    if (!std::isfinite(output)) {
        pid->finite_fault_count = sat_inc(pid->finite_fault_count);
        output = pid->last_output;
    }

    // Update State
    pid->prev_error = pid->last_error;
    pid->last_error = error;
    pid->last_output = output;

    return output;
}

void planetary_pid_get_diag(const PlanetaryPID_t* pid, PlanetaryPIDDiag_t* diag) {
    if (diag == nullptr) return;
    *diag = PlanetaryPIDDiag_t {};
    if (pid == nullptr) return;
    diag->last_error = pid->last_error;
    diag->last_output = pid->last_output;
    diag->integrator = pid->integrator;
    diag->last_dt = pid->last_dt;
    diag->max_abs_error = pid->max_abs_error;
    diag->saturation_count = pid->saturation_count;
    diag->integral_clamp_count = pid->integral_clamp_count;
    diag->finite_fault_count = pid->finite_fault_count;
}
