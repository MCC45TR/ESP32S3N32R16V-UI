#include "matrix_dsp.h"

void matrix_mul_3x3(const float a[9], const float b[9], float out[9]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float acc = 0.0f;
      for (int k = 0; k < 3; ++k) acc += a[r * 3 + k] * b[k * 3 + c];
      out[r * 3 + c] = acc;
    }
  }
}
