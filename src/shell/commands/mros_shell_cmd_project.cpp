#include "src/shell/mros_shell_internal.h"

#include "src/shell/mros_shell.h"
#include "src/web/web_server.h"

#include <string>

namespace mros::shell {
namespace {

std::string lower_copy(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
    else out.push_back(ch);
  }
  return out;
}

void write_blank(ShellState& state) {
  shell_write_line(state, "");
}

void write_title(ShellState& state, const char* title) {
  shell_write_line(state, title);
  shell_write_line(state, "--------------------------------------------------------------------------------");
}

void write_usage(ShellState& state) {
  shell_write_line(state, "Usage: project status|info|web-page|goals|todo|license|version");
  shell_write_line(state, "");
  shell_write_line(state, "Aliases:");
  shell_write_line(state, "  project licence");
  shell_write_line(state, "  project licanse");
}

void write_status(ShellState& state) {
  write_title(state, "MROS Project Status");
  shell_write_line(state,
                   "MROS DEUSCARA is an active robotic-control ecosystem for a DIY 7-DOF arm. "
                   "The current S3 firmware acts as the bridge runtime: it hosts the web UI, "
                   "streams telemetry, drives low-level actuator paths, exposes mshell/SSH, "
                   "and coordinates the t41/C3 robot-control links.");
  write_blank(state);
  shell_write_line(state, "Current state:");
  shell_write_line(state, "  - Platform focus: ESP32-S3 N32R16 bridge with DDR/PSRAM enabled.");
  shell_write_line(state, "  - Control surface: Web UI, WebSocket telemetry, HTTP APIs, mshell, SSH shell, and robot command framework.");
  shell_write_line(state, "  - Math layer: FK/IK, Jacobian options, DLS/QP/SVD-style solver selection, null-space policy, and trajectory preview controls.");
  shell_write_line(state, "  - Hardware links: t41 SPI primary path, C3 encoder side path, ESP-NOW fail-safe concept, and PCA9685 servo output layer.");
  shell_write_line(state, "  - Project documentation source: parent README.md, README-TR.md, MATLAB toolset notes, and firmware docs.");
}

void write_info(ShellState& state) {
  write_title(state, "MROS Project Information");
  shell_write_line(state,
                   "MROS DEUSCARA is a high-performance educational robot-control project "
                   "built around a bridge-centric architecture. Teensy 4.1 is the primary "
                   "robot-brain for motion/control logic, the ESP32-S3 bridge provides web "
                   "control and real-time coordination, and the ESP32-C3 encoder hub contributes "
                   "feedback and redundant receiver behavior.");
  write_blank(state);
  shell_write_line(state, "Core capabilities:");
  shell_write_line(state, "  - 7-DOF robot arm control with turret, joints, gripper, and trajectory concepts.");
  shell_write_line(state, "  - Real-time web dashboard with FK visualization, 3D robot view, route preview, telemetry, PID controls, and calibration panels.");
  shell_write_line(state, "  - Planetary/Willis-inspired PID architecture for turret and mechanical feedback experiments.");
  shell_write_line(state, "  - MATLAB reference models for DH/PoE kinematics, IK, Jacobians, trajectory generation, workspace analysis, and PID simulation.");
  shell_write_line(state, "  - A shell-first API direction where web actions, SSH commands, AI commands, and robot intents can converge into one command vocabulary.");
}

void write_web_page(ShellState& state) {
  write_title(state, "MROS Web Page");
  shell_write_line(state,
                   "The web page is the browser-side operator console for MROS. It is served "
                   "from the ESP32-S3 LittleFS package and communicates with the firmware "
                   "through authenticated WebSocket and HTTP endpoints. The page is not only "
                   "a dashboard; it is also a live robot workbench.");
  write_blank(state);
  shell_write_line(state, "Web UI responsibilities:");
  shell_write_line(state, "  - Display live telemetry from the S3, t41, C3, PCA9685, Wi-Fi, PID loop, and trajectory services.");
  shell_write_line(state, "  - Render FK/IK previews and 3D robot motion, including dashed route visualization for point and multi-point paths.");
  shell_write_line(state, "  - Provide coordinate, joint, turret, gripper, PID, calibration, diagnostic, and shell-terminal controls.");
  shell_write_line(state, "  - Act as the WEB math backend when selected, using browser-side JavaScript solvers for IK and trajectory preview.");
  shell_write_line(state, "  - Receive shell-generated robot intents so commands like robot math, robot path, and robot cartesian can update the UI.");
}

void write_goals(ShellState& state) {
  write_title(state, "MROS Project Goals");
  shell_write_line(state,
                   "The project goal is to make a transparent, hackable, research-friendly "
                   "robotics stack that can be studied from mechanics to math to firmware to UI. "
                   "The system is intentionally built as a full ecosystem instead of a single board demo.");
  write_blank(state);
  shell_write_line(state, "Primary goals:");
  shell_write_line(state, "  - Build a robust 7-DOF arm platform with understandable mechanical, electrical, and firmware layers.");
  shell_write_line(state, "  - Keep robot motion explainable through FK, IK, Jacobian, singularity, null-space, and trajectory tools.");
  shell_write_line(state, "  - Make every important web control available from shell commands for SSH and future AI integration.");
  shell_write_line(state, "  - Support redundant communication paths so the robot can degrade safely when a primary link is weak or offline.");
  shell_write_line(state, "  - Preserve MATLAB as the reference math lab while porting stable behavior into firmware and browser-side solvers.");
}

void write_todo(ShellState& state) {
  write_title(state, "MROS Project TODO");
  shell_write_line(state,
                   "The immediate roadmap is to keep closing the gap between the MATLAB models, "
                   "the web solver, and the embedded robot API. The shell command framework is now "
                   "the backbone for that work.");
  write_blank(state);
  shell_write_line(state, "Near-term work:");
  shell_write_line(state, "  - Align the MATLAB robot model, web DH model, and firmware FK constants so all solvers share one source of truth.");
  shell_write_line(state, "  - Promote browser-side preview planners into a shared planner service with ground constraints and shortest-turret branch selection.");
  shell_write_line(state, "  - Add a real multi-point executor so path preview, path run, and physical trajectory playback follow the same compiled plan.");
  shell_write_line(state, "  - Expand robot math JSON contracts for AI/SSH clients, including solver diagnostics, warnings, singularity metrics, and validation results.");
  shell_write_line(state, "  - Continue porting MATLAB features: workspace analysis, singularity maps, spline/circular paths, PID tuning, and serial/live command experiments.");
}

void write_license(ShellState& state) {
  write_title(state, "MROS License");
  shell_write_line(state,
                   "This project is released for academic, educational, and non-commercial "
                   "research use under the Creative Commons Attribution-NonCommercial 4.0 "
                   "International license, also known as CC BY-NC 4.0.");
  write_blank(state);
  shell_write_line(state, "Practical summary:");
  shell_write_line(state, "  - You may share and adapt the material.");
  shell_write_line(state, "  - You must give appropriate credit and indicate changes when required.");
  shell_write_line(state, "  - You may not use the project material for commercial purposes.");
  shell_write_line(state, "  - No warranty is provided; hardware, firmware, and motion experiments remain the operator's responsibility.");
  shell_write_line(state, "  - Copyright: 2026 MROS Engineering - MCC45TR.");
}

void write_version(ShellState& state) {
  write_title(state, "MROS Version");
  shell_write_line(state, "Project name       : MROS DEUSCARA");
  shell_write_line(state, "Project version    : v1.5");
  shell_write_line(state, "Firmware target    : ESP32-S3 Bridge / Hub");
  shell_printf(state, "Runtime version    : %s\n", web_server_system_version());
  shell_printf(state, "Shell              : %s %s\n", kShellName, kShellVersion);
  shell_write_line(state, "Primary build env  : s3_mros_hub_ddr");
  shell_write_line(state, "License            : CC BY-NC 4.0");
  shell_write_line(state, "Repository family  : MROS-DEUSCARA / MCC45TR");
}

}  // namespace

void shell_help_project(ShellState& state) {
  write_usage(state);
  shell_write_line(state, "");
  shell_write_line(state, "Show embedded project documentation summaries derived from the MROS README,");
  shell_write_line(state, "MATLAB notes, firmware docs, and current S3 bridge architecture.");
}

int shell_cmd_project(ShellContext& ctx) {
  if (ctx.args.size() < 2U) {
    write_info(ctx.state);
    return 0;
  }

  const std::string sub = lower_copy(ctx.args[1]);
  if (sub == "--help" || sub == "-h" || sub == "help") {
    shell_help_project(ctx.state);
    return 0;
  }

  if (ctx.args.size() > 2U) {
    shell_write_line(ctx.state, "project: too many arguments");
    write_usage(ctx.state);
    return 1;
  }

  if (sub == "status") {
    write_status(ctx.state);
    return 0;
  }
  if (sub == "info") {
    write_info(ctx.state);
    return 0;
  }
  if (sub == "web-page" || sub == "webpage" || sub == "web") {
    write_web_page(ctx.state);
    return 0;
  }
  if (sub == "goals" || sub == "goal") {
    write_goals(ctx.state);
    return 0;
  }
  if (sub == "todo" || sub == "todos") {
    write_todo(ctx.state);
    return 0;
  }
  if (sub == "license" || sub == "licence" || sub == "licanse") {
    write_license(ctx.state);
    return 0;
  }
  if (sub == "version" || sub == "ver") {
    write_version(ctx.state);
    return 0;
  }

  shell_printf(ctx.state, "project: unknown topic '%s'\n", sub.c_str());
  write_usage(ctx.state);
  return 1;
}

}  // namespace mros::shell
