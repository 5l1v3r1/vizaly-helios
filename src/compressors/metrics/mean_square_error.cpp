/*
 * Copyright (c) 2019, Los Alamos National Laboratory
 * All rights reserved.
 *
 * Authors: Pascal Grosset, Jesus Pulido and Hoby Rakotoarivelo.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cmath>
#include <string>
#include <sstream>
#include "compressors/metrics/mean_square_error.h"
/* -------------------------------------------------------------------------- */
void meanSquareError::execute(void *original, void *approx, size_t n) {

  double mean_square_error = 0;
  for (std::size_t i = 0; i < n; ++i) {
    auto diff = static_cast<float*>(original)[i] - static_cast<float*>(approx)[i];
    mean_square_error += std::pow(diff, 2.0);
  }

  double local_mse = mean_square_error / n;
  local_val = local_mse;

  double total_mse = 0;
  size_t total_n = 0;

  MPI_Allreduce(&mean_square_error, &total_mse, 1, MPI_DOUBLE, MPI_SUM, comm);
  MPI_Allreduce(&n, &total_n, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, comm);
  total_val = total_mse / (double) total_n;

  log << "- mean_square_error: " << total_val << std::endl;

  MPI_Barrier(comm);
}
/* -------------------------------------------------------------------------- */