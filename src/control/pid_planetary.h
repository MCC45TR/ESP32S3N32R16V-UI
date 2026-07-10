#ifndef PLANETARY_PID_ADVANCED_H
#define PLANETARY_PID_ADVANCED_H

#include <stdint.h>

/**
 * @brief SCARA Planetary Drive Control Workspace
 * Dedicated to the 7-DOF robotic platform.
 * Mathematically derived based on Willis Equation:
 * Omega_Sun (Encoder) = 5 * Omega_Carrier (Output)
 * Omega_Carrier = 0.375 * Omega_Motor_Relative
 */
typedef struct {
    // Kinematic Parameters
    float GearRatio_EncToCarrier;
    float GearRatio_MotToCarrier;
    
    // PID Constants
    float Kp;
    float Ki;
    float Kd;
    float Ts; // Sampling period in seconds
    
    // Controller State (Incremental Form)
    float last_error;
    float prev_error;
    float prev_derivative;
    float integrator;
    float last_output;
    float last_dt;
    uint32_t saturation_count;
    uint32_t integral_clamp_count;
    uint32_t finite_fault_count;
    float max_abs_error;
    float derivative_alpha;
    float slew_rate_per_s;
    
    // Limits
    float out_min;
    float out_max;
    float i_limit;
} PlanetaryPID_t;

/**
 * @brief Initializes the PID controller parameters.
 */
void planetary_pid_init(PlanetaryPID_t* pid, float kp, float ki, float kd, float ts);

/**
 * @brief Computes the required motor effort based on target and encoder feedback.
 * @param target_carrier_deg Desired position of the robot limb.
 * @param sun_encoder_deg Raw position from the sun gear encoder.
 * @return float Motor effort/speed command [-limit, +limit].
 */
float planetary_pid_compute(PlanetaryPID_t* pid, float target_carrier_deg, float sun_encoder_deg);

typedef struct {
    float last_error;
    float last_output;
    float integrator;
    float last_dt;
    float max_abs_error;
    uint32_t saturation_count;
    uint32_t integral_clamp_count;
    uint32_t finite_fault_count;
} PlanetaryPIDDiag_t;

void planetary_pid_get_diag(const PlanetaryPID_t* pid, PlanetaryPIDDiag_t* diag);

#endif // PLANETARY_PID_ADVANCED_H
