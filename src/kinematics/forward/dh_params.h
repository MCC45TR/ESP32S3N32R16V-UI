#pragma once

#include <stddef.h>

struct DhParam {
  float a;
  float alpha;
  float d;
  float theta_offset;
};

size_t dh_params_count();
const DhParam *dh_params_get();
