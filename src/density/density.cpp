/*
 * Copyright (c) 2019, Los Alamos National Laboratory
 * All rights reserved.
 *
 * Author: Hoby Rakotoarivelo
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
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

#include "density/density.h"
/* -------------------------------------------------------------------------- */

Density::Density(const char* in_path, int in_rank, int in_nb_ranks, MPI_Comm in_comm)
  : json_path(in_path),
    my_rank(in_rank),
    nb_ranks(in_nb_ranks),
    comm(in_comm) {
  assert(nb_ranks > 0);
  nlohmann::json json;
  std::string buffer;

  std::ifstream file(json_path);
  assert(file.is_open());
  assert(file.good());

  // parse params and do basic checks
  file >> json;

  assert(json.count("hacc"));
  assert(json["hacc"].count("input"));
  assert(json["hacc"].count("output"));

  assert(json.count("density"));
  assert(json["density"].count("inputs"));
  assert(json["density"].count("extents"));
  assert(json["density"]["extents"].count("min"));
  assert(json["density"]["extents"].count("max"));

  assert(json.count("bins"));
  assert(json["bins"].count("count"));
  assert(json["bins"].count("adaptive"));
  assert(json["bins"].count("min_bits"));
  assert(json["bins"].count("max_bits"));

  assert(json.count("plots"));
  assert(json["plots"].count("density"));
  assert(json["plots"].count("buckets"));

  // retrieve number of cells per axis
  int const c_min = json["density"]["extents"]["min"];
  int const c_max = json["density"]["extents"]["max"];
  cells_per_axis = 1 + c_max - c_min;
  assert(cells_per_axis > 0);

  // dispatch files to MPI ranks
  int partition_size = json["density"]["inputs"].size();
  bool rank_mismatch = (partition_size < nb_ranks) or (partition_size % nb_ranks != 0);

  if (nb_ranks == 1 or not rank_mismatch) {
    int offset = static_cast<int>(partition_size / nb_ranks);
    assert(offset);

    local_rho_count = 0;
    for (int i = 0; i < offset; ++i) {
      int file_index = i + my_rank * offset;
      auto&& current = json["density"]["inputs"][file_index];
      inputs.emplace_back(current["data"], current["count"]);
      std::cout << "rank["<< my_rank <<"]: \""<< inputs.back().first << "\""<< std::endl;
      local_rho_count += inputs.back().second;
    }

    // resize buffer
    density_field.resize(local_rho_count);
    // retrieve the total number of elems
    MPI_Allreduce(&local_rho_count, &total_rho_count, 1, MPI_LONG, MPI_SUM, comm);

  } else
    throw std::runtime_error("mismatch on number of ranks and data partition");

  // data binning
  nb_bins = json["bins"]["count"];
  assert(nb_bins > 0);
  histogram.resize(nb_bins);
  buckets.resize(nb_bins);
  bits.resize(nb_bins);

  use_adaptive_binning = json["bins"]["adaptive"];
  min_bits = json["bins"]["min_bits"];
  max_bits = json["bins"]["max_bits"];
  assert(min_bits > 0 and max_bits > min_bits);

  // plots
  output_plot = json["plots"]["density"];
  output_bucket = json["plots"]["buckets"];

  // set the HACC IO manager
  ioMgr = std::make_unique<HACCDataLoader>();
  input_hacc  = json["hacc"]["input"];
  output_hacc = json["hacc"]["output"];

}

/* -------------------------------------------------------------------------- */
void Density::cacheData() {

  bool const master_rank = (my_rank == 0);

  assert(not input_hacc.empty());
  assert(not inputs.empty());

  // step 1: load particle file
  ioMgr->init(input_hacc, comm);
  ioMgr->saveParams();
  ioMgr->setSave(true);

  if (master_rank)
    std::cout << "Caching particle data ... " << std::flush;

  std::string const columns[] = {"x", "y", "z", "vx", "vy", "vz", "id"};

  for (int i = 0; i < dim; ++i) {
    if (ioMgr->load(columns[i])) {
      if (master_rank) {
        std::cout << ioMgr->getDataInfo();
        std::cout << ioMgr->getLog();
      }
      if (i == 0)
        local_particles = ioMgr->getNumElements();

      auto const n = local_particles;
      auto const data = static_cast<float*>(ioMgr->data);
      coords[i].resize(n);
      std::copy(data, data + n, coords[i].data());
      ioMgr->close();
    }
    MPI_Barrier(comm);
  }

  // update particle count and coordinates data extents
  MPI_Allreduce(&local_particles, &total_particles, 1, MPI_LONG, MPI_SUM, comm);

  for (int i = 0; i < dim; ++i) {
    coords_min[i] = static_cast<float>(ioMgr->data_extents[i].first);
    coords_max[i] = static_cast<float>(ioMgr->data_extents[i].second);
  }

  for (int i = 0; i < dim; ++i) {
    if (ioMgr->load(columns[i + dim])) {
      auto const n = ioMgr->getNumElements();
      auto const data = static_cast<float*>(ioMgr->data);
      velocs[i].resize(n);
      std::copy(data, data + n, velocs[i].data());
      ioMgr->close();
    }
    MPI_Barrier(comm);
  }

  if (ioMgr->load(columns[dim * 2])) {
    auto const n = ioMgr->getNumElements();
    auto const data = static_cast<long*>(ioMgr->data);
    index.resize(n);
    std::copy(data, data + n, index.data());
    ioMgr->close();
  }

  MPI_Barrier(comm);

  if (master_rank) {
    std::cout << "done." << std::endl;
    std::cout << "Caching density data ... " << std::flush;
  }

  // step 2: load density file
  long offset = 0;
  long count = 0;
  std::string path;

  for (auto&& current : inputs) {
    std::tie(path, count) = current;

    std::ifstream file(path, std::ios::binary);
    assert(file.good());

    auto buffer = reinterpret_cast<char *>(density_field.data() + offset);
    auto size = count * sizeof(float);

    file.seekg(0, std::ios::beg);
    file.read(buffer, size);
    file.close();

    // update offset
    offset += count;
  }

  MPI_Barrier(comm);
  if (master_rank)
    std::cout << "done." << std::endl;

}


/* -------------------------------------------------------------------------- */
void Density::computeFrequencies() {
#if !DEBUG_DENSITY
  if (my_rank == 0)
    std::cout << "Computing frequencies ... " << std::flush;

  assert(local_rho_count);
  assert(total_rho_count);

  // determine data values extents
  total_rho_max = 0.0;
  total_rho_min = 0.0;
  local_rho_min = *std::min_element(density_field.data(), density_field.data() + local_rho_count);
  local_rho_max = *std::max_element(density_field.data(), density_field.data() + local_rho_count);
  MPI_Allreduce(&local_rho_max, &total_rho_max, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(&local_rho_min, &total_rho_min, 1, MPI_DOUBLE, MPI_MIN, comm);

  // compute histogram of values
  long local_histo[nb_bins];
  auto total_histo = histogram.data();

  std::fill(local_histo, local_histo + nb_bins, 0);
  std::fill(total_histo, total_histo + nb_bins, 0);

  if (not use_adaptive_binning) {
    double const range = total_rho_max - total_rho_min;
    double const capacity = range / nb_bins;

    for (auto k = 0; k < local_rho_count; ++k) {
      double relative_value = (density_field[k] - total_rho_min) / range;
      int bin_index = static_cast<int>((range * relative_value) / capacity);

      if (bin_index >= nb_bins)
        bin_index--;

      local_histo[bin_index]++;
    }
  } else {
    auto const capacity = static_cast<long>(local_rho_count / double(nb_bins));
    for (int bin_index = 0; bin_index < nb_bins; ++bin_index) {
      local_histo[bin_index] = capacity;
    }
  }

  MPI_Allreduce(local_histo, total_histo, nb_bins, MPI_LONG, MPI_SUM, comm);

  if (my_rank == 0) {
    dumpHistogram();
    std::cout << "done." << std::endl;
    std::cout << "= number of particles: " << total_rho_count << std::endl;
    std::cout << "= number of bins: " << nb_bins << std::endl;
    std::cout << "= density range: [" << total_rho_min << ", " << total_rho_max << "]" << std::endl;
    std::cout << "= histogram file: '" << output_plot << ".dat'" << std::endl;
  }

  MPI_Barrier(comm);
#endif
}

/* -------------------------------------------------------------------------- */
void Density::dumpHistogram() {

  std::ofstream file(output_plot + ".dat", std::ios::trunc);
  assert(file.is_open());
  assert(file.good());

  file << "# bins: " << std::to_string(nb_bins) << std::endl;
  file << "# col 1: density range" << std::endl;
  file << "# col 2: particle count" << std::endl;

  if (not use_adaptive_binning) {
    double const width = (total_rho_max - total_rho_min) / static_cast<double>(nb_bins);
    for (int k =0; k < nb_bins; ++k) {
      file << total_rho_min + (k * width) << "\t" << histogram[k] << std::endl;
    }
  } else {
    for (int k =0; k < nb_bins; ++k) {
      file << total_rho_min + bin_ranges[k] << "\t" << histogram[k] << std::endl;
    }
  }

  file.close();
}

/* -------------------------------------------------------------------------- */
void Density::computeDensityBins() {
#if !DEBUG_DENSITY
  if (use_adaptive_binning) {
    // adjust number of bins
    // for equiprobable bins: Prins et al. "Chi-square goodness-of-fit test".
    constexpr double const exponent = 2.0/5;
    nb_bins = static_cast<int>(2 * std::pow(local_rho_count, exponent));
    bin_capacity = static_cast<long>(local_rho_count / double(nb_bins));
    bin_ranges.resize(nb_bins);

    if (my_rank == 0)
      std::cout << "nb_bins: " << nb_bins << ", capacity: " << bin_capacity << std::endl;

    // now compute quantiles on 'density_field'
    if (my_rank == 0)
      std::cout << "Sorting density field ... " << std::flush;

    std::vector<float> sorted_densities(density_field);
    std::sort(sorted_densities.begin(), sorted_densities.end());

    if (my_rank == 0)
      std::cout << "done." << std::endl;

    for (int i = 0; i < nb_bins; ++i) {
      bin_ranges[i] = sorted_densities[i * bin_capacity];
      if (my_rank == 0)
        std::cout << "bin_ranges["<< i <<"] = " << bin_ranges[i] << std::endl;
    }

    sorted_densities.clear();
    sorted_densities.shrink_to_fit();
  }

  // assign number of bits for each bin
  assignBits();
  MPI_Barrier(comm);
#endif
}

/* -------------------------------------------------------------------------- */
void Density::assignBits() {

  bool const mode = 2;

  if (not use_adaptive_binning) {
    // just assign bits heuristically for now.
    // quick ugly hack, to be fixed after.
    if (mode == 1) {
      bits[0] = min_bits;
      for (int i =   1; i <    2; ++i) bits[i] = 20;
      for (int i =   2; i <    5; ++i) bits[i] = 21;
      for (int i =   5; i <   25; ++i) bits[i] = 22;
      for (int i =  25; i <  100; ++i) bits[i] = 23;
      for (int i = 100; i <  200; ++i) bits[i] = 24;
      for (int i = 200; i <  500; ++i) bits[i] = 25;
      for (int i = 500; i < 1200; ++i) bits[i] = 26;
      for (int i = 1200; i < nb_bins; ++i) bits[i] = max_bits;
    } else {
      bits[0] = min_bits;
      for (int i =   1; i <    2; ++i) bits[i] = 22;
      for (int i =   2; i <    5; ++i) bits[i] = 22;
      for (int i =   5; i <   25; ++i) bits[i] = 23;
      for (int i =  25; i <  100; ++i) bits[i] = 24;
      for (int i = 100; i <  200; ++i) bits[i] = 25;
      for (int i = 200; i <  500; ++i) bits[i] = 26;
      for (int i = 500; i < 1200; ++i) bits[i] = 26;
      for (int i = 1200; i < nb_bins; ++i) bits[i] = max_bits;
    }
  } else {
    // assign number of bits evenly on bins
    // use an uniform distribution for now.
    auto const values_width = 1 + max_bits - min_bits;
    auto const nb_values_per_bit = static_cast<int>(nb_bins / values_width);
    for (int i = 0; i < values_width; ++i) {
      for (int j = 0; j < nb_values_per_bit; ++j) {
        if (i < 2)
          bits[i * nb_values_per_bit + j] = min_bits + i;
        else
          bits[i * nb_values_per_bit + j] = max_bits;
      }
    }
  }

}

/* -------------------------------------------------------------------------- */
long Density::deduceDensityIndex(const float* particle) const {

  float range[3];
  float shifted[3];
  auto const n_cells_axis = static_cast<float>(cells_per_axis);

  // step 1: shift particle coordinates and compute related range
  for (int i = 0; i < 3; ++i) {
    shifted[i] = particle[i] - coords_min[i];
    range[i] = coords_max[i] - coords_min[i];
  }

  // step 2: physical coordinates to logical coordinates
  auto i = static_cast<int>(std::floor(shifted[0] * n_cells_axis / range[0]));
  auto j = static_cast<int>(std::floor(shifted[1] * n_cells_axis / range[1]));
  auto k = static_cast<int>(std::floor(shifted[2] * n_cells_axis / range[2]));

  // step 3: logical coordinates to flat array index
  return i + j * cells_per_axis + k * cells_per_axis * cells_per_axis;
}

/* -------------------------------------------------------------------------- */
int Density::deduceBucketIndex(float const& rho) const {

  assert(rho < local_rho_max);

  if (not use_adaptive_binning) {
    auto const coef = rho / (local_rho_max - local_rho_min);
    auto const bucket_index = std::min(static_cast<int>(std::floor(coef * float(nb_bins))), nb_bins - 1);
    assert(bucket_index < nb_bins);
    return bucket_index;
  } else {
    if (rho < bin_ranges[0])
      return 0;

    for (int i = 1; i < nb_bins; ++i) {
      if (bin_ranges[i - 1] <= rho and rho <= bin_ranges[i]) {
        return i;
      }
    }
    return nb_bins - 1;
  }
}

/* -------------------------------------------------------------------------- */
void Density::bucketParticles() {

  if (my_rank == 0)
    std::cout << "Bucketing particles ... " << std::flush;

#if !DEBUG_DENSITY
  for (int i = 0; i < local_particles; ++i) {
    float particle[] = { coords[0][i], coords[1][i], coords[2][i] };
    auto const density_index = deduceDensityIndex(particle);
    assert(density_index < local_rho_count);
    auto const bucket_index  = deduceBucketIndex(density_field[density_index]);
    assert(bucket_index < nb_bins);
    // copy data in correct bucket
    buckets[bucket_index].emplace_back(i);
  }

  MPI_Barrier(comm);
  dumpBucketDistrib();
#else
  nb_bins = 1;
  bits[0] = min_bits;
  buckets.resize(1);
  for (int i = 0; i < local_particles; ++i)
    buckets[0].emplace_back(i);

  MPI_Barrier(comm);
#endif

  if (my_rank == 0)
    std::cout << "done" << std::endl;
}

/* -------------------------------------------------------------------------- */
void Density::dumpBucketDistrib() {

  long local_count[nb_bins];
  long total_count[nb_bins];
  for (int i = 0; i < nb_bins; ++i)
    local_count[i] = buckets[i].size();

  MPI_Reduce(local_count, total_count, nb_bins, MPI_LONG, MPI_SUM, 0, comm);

  if (my_rank == 0) {
    std::ofstream file(output_bucket + ".dat", std::ios::trunc);
    assert(file.is_open());
    assert(file.good());

    file << "# bins: " << nb_bins << std::endl;
    file << "# col 1: bin " << std::endl;
    file << "# col 2: particle count" << std::endl;
    for (int i = 0; i < nb_bins; ++i)
      file << i << "\t" << total_count[i] << std::endl;
    file.close();
  }

  MPI_Barrier(comm);
}

/* -------------------------------------------------------------------------- */
void Density::dumpBitsDistrib() {

  if (my_rank == 0) {
    std::ofstream file("bits_distrib.dat", std::ios::trunc);
    assert(file.is_open());
    assert(file.good());

    file << "# bins: " << nb_bins << std::endl;
    file << "# col 1: density" << std::endl;
    file << "# col 2: bits" << std::endl;

    if (not use_adaptive_binning) {
      double const width = (total_rho_max - total_rho_min) / static_cast<double>(nb_bins);
      for (int k =0; k < nb_bins; ++k) {
        file << total_rho_min + (k * width) << "\t" << bits[k] << std::endl;
      }
    } else {
      for (int k =0; k < nb_bins; ++k) {
        file << total_rho_min + bin_ranges[k] << "\t" << bits[k] << std::endl;
      }
    }
    file.close();
  }

  MPI_Barrier(comm);
}

/* -------------------------------------------------------------------------- */
void Density::process(int step) {

  assert(step < 6);
  assert(not buckets.empty());

  auto data = coords[step].data();

  if (my_rank == 0)
    std::cout << "Inflate and deflate data ... " << std::flush;

#if ENABLE_LOSSLESS
  size_t local_bytes_fpzip[] = {0, 0};
  size_t local_bytes_blosc[] = {0, 0};
  size_t total_bytes_fpzip[] = {0, total_particles * sizeof(float)};
  size_t total_bytes_blosc[] = {0, total_particles * sizeof(float)};
  size_t nb_elems[] = {0, 0, 0, 0, 0};

  std::vector<float> dataset;
  decompressed[step].reserve(local_particles);

  for (int j = 0; j < nb_bins; ++j) {

    if (buckets[j].empty())
      continue;

    // retrieve number of particles for this bucket
    nb_elems[0] = buckets[j].size();

    // step 1: create dataset according to computed bin.
    dataset.reserve(nb_elems[0]);
    for (auto&& particle_index : buckets[j])
      dataset.emplace_back(data[particle_index]);

    // step 2: inflate agregated dataset and release memory
    void* raw_data = static_cast<void*>(dataset.data());
    void* raw_inflate = nullptr;
    void* raw_deflate = nullptr;
    void* raw_inflate_blosc = nullptr;
    void* raw_deflate_blosc = nullptr;

    auto kernel_fpzip = CompressorFactory::create("fpzip");
    kernel_fpzip->init();
    kernel_fpzip->parameters["bits"] = std::to_string(bits[j]);
    kernel_fpzip->compress(raw_data, raw_inflate, "float", sizeof(float), nb_elems);

    // early memory release
    raw_data = nullptr;
    dataset.clear();
    dataset.shrink_to_fit();

    auto kernel_blosc = CompressorFactory::create("blosc");
    kernel_blosc->init();

    auto type_size = kernel_fpzip->getBytes() / nb_elems[0];
    std::cout << "chunk_size: " << kernel_fpzip->getBytes() << ", type_size: " << type_size << std::endl;
    kernel_blosc->compress(raw_inflate, raw_inflate_blosc, "float", type_size, nb_elems);

    // update compression metrics
    local_bytes_fpzip[0] += kernel_fpzip->getBytes();
    local_bytes_blosc[0] += kernel_blosc->getBytes();
    kernel_blosc->close();

    // step 3: deflate data and store it
    kernel_fpzip->decompress(raw_inflate, raw_deflate, "float", sizeof(float), nb_elems);
    for (int k = 0; k < nb_elems[0]; ++k)
      decompressed[step].emplace_back(static_cast<float*>(raw_deflate)[k]);
  }

  MPI_Barrier(comm);
  MPI_Reduce(local_bytes_fpzip, total_bytes_fpzip, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, comm);
  MPI_Reduce(local_bytes_blosc, total_bytes_blosc, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, comm);

  if (my_rank == 0) {
    std::cout << "done" << std::endl;

    // print stats
    auto const& bytes_lossy = total_bytes_fpzip;
    auto const& bytes_final = total_bytes_blosc;
    double const ratios[] = { bytes_lossy[1] / double(bytes_lossy[0]),
                              bytes_final[1] / double(bytes_final[0]) };

    std::printf("\tdeflate size: [lossy: %lu, final: %lu]\n", bytes_lossy[0], bytes_final[0]);
    std::printf("\tinflate size: [lossy: %lu, final: %lu]\n", bytes_lossy[1], bytes_final[1]);
    std::printf("\tcompression : [lossy: %.3f, final: %.3f]\n", ratios[0], ratios[1]);
    std::fflush(stdout);
  }
#else
  size_t local_bytes[] = {0, 0};
  size_t total_bytes[] = {0, total_particles * sizeof(float)};
  size_t nb_elems[] = {0, 0, 0, 0, 0};

  std::vector<float> dataset;
  decompressed[step].reserve(local_particles);

  for (int j = 0; j < nb_bins; ++j) {
    if (buckets[j].empty())
      continue;

    // retrieve number of particles for this bucket
    nb_elems[0] = buckets[j].size();

    // step 1: create dataset according to computed bin.
    dataset.reserve(nb_elems[0]);
    for (auto&& particle_index : buckets[j])
      dataset.emplace_back(data[particle_index]);

    // step 2: inflate agregated dataset and release memory
    void* raw_data = static_cast<void*>(dataset.data());
    void* raw_inflate = nullptr;
    void* raw_deflate = nullptr;

    auto kernel_lossy = CompressorFactory::create("fpzip");
    kernel_lossy->init();
    kernel_lossy->parameters["bits"] = std::to_string(bits[j]);
    kernel_lossy->compress(raw_data, raw_inflate, "float", sizeof(float), nb_elems);
    dataset.clear();

    // update compression metrics
    local_bytes[0] += kernel_lossy->getBytes();

    // step 3: deflate data and store it
    kernel_lossy->decompress(raw_inflate, raw_deflate, "float", sizeof(float), nb_elems);
    for (int k = 0; k < nb_elems[0]; ++k)
      decompressed[step].emplace_back(static_cast<float*>(raw_deflate)[k]);

    std::free(raw_inflate);
    std::free(raw_deflate);
  }

  MPI_Barrier(comm);
  MPI_Reduce(local_bytes, total_bytes, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, comm);

  if (my_rank == 0) {
    std::cout << "done" << std::endl;
    // print stats
    std::printf(" \u2022 raw: %lu, zip: %lu\n", total_bytes[1], total_bytes[0]);
    std::printf(" \u2022 rate: %.3f\n", total_bytes[1] / double(total_bytes[0]));
    std::fflush(stdout);
  }
#endif

  coords[step].clear();
  coords[step].shrink_to_fit();
  MPI_Barrier(comm);
}

/* -------------------------------------------------------------------------- */
void Density::dump() {

  // step 0: ease memory pressure by releasing unused data
  density_field.clear();
  density_field.shrink_to_fit();
  histogram.clear();
  histogram.shrink_to_fit();
  bits.clear();
  bits.shrink_to_fit();

  // step 1: sort all uncompressed data
  std::vector<long> uid;
  uid.reserve(local_particles);
  for (auto&& bucket : buckets) {
    for (auto&& i : bucket) {
      uid.emplace_back(index[i]);
    }
  }
  index.clear();
  index.shrink_to_fit();

  std::vector<float> v[dim];
  for (int i = 0; i < dim; ++i) {
    v[i].reserve(local_particles);
    for (auto&& bucket : buckets) {
      for (auto&& k : bucket) {
        v[i].emplace_back(velocs[i][k]);
      }
    }
    velocs[i].clear();
    velocs[i].shrink_to_fit();
  }

  buckets.clear();
  buckets.shrink_to_fit();
  MPI_Barrier(comm);

  // step 2: prepare dataset partition and header
  int periods[dim] = {0, 0, 0};
  auto dim_size = ioMgr->mpi_partition;
  MPI_Cart_create(comm, 3, dim_size, periods, 0, &comm);

  // init writer and open file
  gio::GenericIO gioWriter(comm, output_hacc);
  gioWriter.setNumElems(local_particles);

  // init physical params
  for (int d = 0; d < dim; ++d) {
    gioWriter.setPhysOrigin(ioMgr->phys_orig[d], d);
    gioWriter.setPhysScale(ioMgr->phys_scale[d], d);
  }

  MPI_Barrier(comm);

  unsigned const flags[] = {
    gio::GenericIO::VarHasExtraSpace,
    gio::GenericIO::VarHasExtraSpace|gio::GenericIO::VarIsPhysCoordX,
    gio::GenericIO::VarHasExtraSpace|gio::GenericIO::VarIsPhysCoordY,
    gio::GenericIO::VarHasExtraSpace|gio::GenericIO::VarIsPhysCoordZ
  };

  gioWriter.addVariable( "x", decompressed[0].data(), flags[1]);
  gioWriter.addVariable( "y", decompressed[1].data(), flags[2]);
  gioWriter.addVariable( "z", decompressed[2].data(), flags[3]);
  gioWriter.addVariable("vx", v[0].data(), flags[0]);
  gioWriter.addVariable("vy", v[1].data(), flags[0]);
  gioWriter.addVariable("vz", v[2].data(), flags[0]);
  gioWriter.addVariable("id", uid.data(), flags[0]);
  gioWriter.write();

  // release memory
  for (auto& dataset : decompressed) {
    dataset.clear();
    dataset.shrink_to_fit();
  }

  MPI_Barrier(comm);
}

/* -------------------------------------------------------------------------- */
void Density::run() {

  // step 1: load current rank dataset in memory
  cacheData();

  // step 2: compute bins and assign bits for each of them
  computeDensityBins();

  // step 3: compute frequencies and histogram
  computeFrequencies();

  // dump it for plot purposes
  dumpBitsDistrib();

  // step 4: bucket particles
  bucketParticles();

  // step 5: inflate and deflate bucketed data
  for (int component = 0; component < dim; ++component)
    process(component);

  // step 6: dump them
  dump();
}

/* -------------------------------------------------------------------------- */
