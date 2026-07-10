#pragma once
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct WebServerDiagSnapshot {
  float pid_cycle_avg_ms = 0.0f;
  uint32_t pid_cycle_last_ms = 0;
  uint32_t pid_cycle_exec_ms = 0;
  uint32_t pid_cycle_peak_ms = 0;
  float fk_last_ms = 0.0f;
  float fk_avg_ms = 0.0f;
  float fk_max_ms = 0.0f;
  uint32_t fk_samples = 0;
  uint32_t cpu_freq_core0_mhz = 0;
  uint32_t cpu_freq_core1_mhz = 0;
  uint32_t cpu_freq_target_mhz = 0;
  uint32_t cpu_freq_applied_mhz = 0;
  uint32_t ws_clients_legacy = 0;
  uint32_t ws_clients_telemetry = 0;
  uint32_t ws_clients_shell = 0;
  uint32_t ws_clients_debug = 0;
  uint32_t ws_clients_total = 0;
  uint32_t ws_clients_auth = 0;
  uint32_t ws_clients_scene = 0;
  uint32_t ws_clients_shell_auth = 0;
  uint32_t ws_clients_debug_auth = 0;
  uint32_t ws_clients_debug_subscribed = 0;
  bool storage_ready = false;
  bool pca_ready = false;
  unsigned long last_web_feedback_ms = 0;
};

struct WebSecuritySnapshot {
  bool auth_session_active = false;
  uint32_t login_fail_count = 0;
  uint32_t login_lockout_ms = 0;
  uint32_t ws_auth_count = 0;
  uint32_t ws_shell_auth_count = 0;
  uint32_t ws_debug_auth_count = 0;
  bool serial_auth_required = false;
};

struct WebRobotUiCommand {
  uint32_t revision = 0;
  char op[16] = {};
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool has_from = false;
  float from_x = 0.0f;
  float from_y = 0.0f;
  float from_z = 0.0f;
  float t_ms = 1000.0f;
  bool ee_auto = true;
  float roll_deg = 0.0f;
  float pitch_deg = 0.0f;
  float yaw_deg = 0.0f;
  float ee_pitch = 0.0f;
  bool apply = false;
  float joints[7] = {};
  uint8_t joint_count = 0;
  char calc_mode[16] = {};
  char source[16] = {};
};

struct WebRobotMathState {
  uint32_t revision = 0;
  char solver[24] = {};
  char jacobian[24] = {};
  char nullspace[24] = {};
  char trajectory[24] = {};
  char seed_policy[24] = {};
  char limits_profile[24] = {};
  char model[32] = {};
  char frame[24] = {};
  char units[8] = {};
  float pos_tol_mm = 0.5f;
  float ori_tol_deg = 0.5f;
  float singularity_threshold = 5.0f;
  float alpha_step = 0.5f;
  float null_gain = 0.1f;
  float lambda_max = 0.5f;
  float max_step_deg = 10.0f;
  uint16_t max_iter = 500;
  char path_height_mode[16] = {};
  float ground_z_mm = 0.0f;
  char turret_mode[24] = {};
  float cart_step_mm = 8.0f;
  float yaw_step_deg = 4.0f;
  float jump_revolute_deg = 18.0f;
  bool allow_negative_z_input = false;
};

void web_server_init();
void web_server_loop();
void web_server_fk_loop();
void web_server_set_runtime_task_handle(TaskHandle_t task_handle);
void web_server_notify_runtime_task();
void web_server_set_fk_task_handle(TaskHandle_t task_handle);
void web_server_notify_fk_task();
void web_server_report_pid_cycle_ms(uint32_t cycle_ms, uint32_t exec_ms);
void web_server_get_diag_snapshot(WebServerDiagSnapshot *snapshot);
void web_server_get_security_snapshot(WebSecuritySnapshot *snapshot);
void web_server_logout_all();
bool web_server_set_ik_compute_preference(const char *mode);
const char* web_server_get_ik_compute_preference();
uint32_t web_server_publish_robot_ui_target(const WebRobotUiCommand *command);
uint32_t web_server_publish_robot_math_state(const WebRobotMathState *state);
void web_server_get_robot_math_state(WebRobotMathState *state);
uint32_t web_server_total_ws_client_count();
const char* web_server_system_version();
