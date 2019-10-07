/*
 * Copyright (c) 2019, Los Alamos National Laboratory
 * All rights reserved.
 *
 * Authors: Pascal Grosset, Chris Biwer, Jesus Pulido and Hoby Rakotoarivelo.
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

#include <iostream>
#include <sstream>
#include <vector>
#include <time.h>
#include <stdlib.h>
#include <mpi.h>
/* -------------------------------------------------------------------------- */
#include "io/interface.h"
#include "io/hacc.h"
/* -------------------------------------------------------------------------- */
#include "compressors/kernels/interface.h"
#include "compressors/kernels/factory.h"
#include "compressors/metrics/interface.h"
#include "compressors/metrics/factory.h"
/* -------------------------------------------------------------------------- */
#include "utils/json.h"
#include "utils/timer.h"
#include "utils/log.h"
#include "utils/memory.h"
#include "utils/utils.h"

/* -------------------------------------------------------------------------- */
bool valid(int argc, char **argv, int rank, int nb_ranks) {
  // check args
  if (argc < 2) {
    if (rank == 0)
      std::cerr << "Usage: mpirun [nranks] input.json" << std::endl;
    return false;
  }

  // Check if input file provided exists
  if (not fileExists(argv[1])) {
    if (rank == 0) {
      std::cerr << "Error: could not find input file: " << argv[1] << std::endl;
    }
    return false;
  }

  // validate JSON file
  nlohmann::json json;

  try {
    std::ifstream file(argv[1]);
    file >> json;
  }
  catch (nlohmann::json::parse_error& e) {
    if (rank == 0) {
      std::cerr << "Error: invalid input file " << argv[1] << std::endl;
      std::cerr << e.what() << std::endl;
      std::cerr << "Please verify your JSON file using e.g. ";
      std::cerr << "https://jsonformatter.curiousconcept.com" << std::endl;
    }
    return false;
  }

  // check if powers of 2 number of ranks
  if (json["compress"]["output"].count("dump")) {
    if (not isPowerOfTwo(nb_ranks)) {
      if (rank == 0) {
        std::cerr << "Please run with powers of two ranks when dumping HACC files.";
        std::cerr << std::endl;
      }
      return false;
    }
  }
  return true;
}

/* -------------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
  // init MPI
  int rank;
  int nb_ranks;
  int threading = 1;
  MPI_Comm comm = MPI_COMM_WORLD;

  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &threading);
  MPI_Comm_size(comm, &nb_ranks);
  MPI_Comm_rank(comm, &rank);

  // check input params
  if (not valid(argc, argv, rank, nb_ranks)) {
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  // Pass JSON file to json parser and
  nlohmann::json json;
  std::ifstream file(argv[1]);
  file >> json;

  std::vector<std::string> scalars;
  std::vector<std::string> compressors;
  std::vector<std::string> metrics;

  bool dump = false;
  std::string output_file;
  std::string output_path = ".";

  // Load in the parameters
  std::string input = json["compress"]["input"];
  std::string temp  = json["compress"]["output"]["log"];
  std::string logs  = "logs/" + temp + "_rank_" + std::to_string(rank);
  std::string stats = json["compress"]["output"]["stats"];

  for (auto&& name : json["input"]["scalars"])
    scalars.push_back(name);

  for (auto&& field : json["compress"]["kernels"])
    compressors.push_back(field["name"]);

  for (auto&& field : json["compress"]["metrics"])
    metrics.push_back(field["name"]);

  if (json["compress"]["output"].count("dump")) {
    dump = true;
    output_file = extractFileName(input);
    output_path = json["compress"]["output"]["dump"];
    if (rank == 0)
      createFolder(output_path);
  }

  // For humans; all seems valid, let's start ...
  if (rank == 0) {
    std::cout << "Running compression ... " << std::endl;
    std::cout << "Look at the log for progress update ... " << std::endl;
  }

  //
  // Create log and metrics files
  Timer clock_overall;
  std::stringstream debug_log;
  std::stringstream metrics_info;
  std::stringstream output_csv;

  writeLog(logs, debug_log.str());

  output_csv << "Compressor_field" << "__" << "params" << ", " << "name, ";
  for (auto&& metric : metrics)
    output_csv << metric << ", ";

  output_csv << "Compression Throughput(MB/s), DeCompression Throughput(MB/s)";
  output_csv << ", Compression Ratio" << std::endl;
  metrics_info << "Input file: " << input << std::endl;

  clock_overall.start();

  //
  // managers
  DataLoaderInterface* io_manager = new HACCDataLoader();
  CompressorInterface* compress_manager = nullptr;
  MetricInterface* metrics_manager = nullptr;

  // Check if the data info exist for a dataset
  if (json["input"].count("data-info")) {
    auto const& info = json["input"]["data-info"];
    // insert datainfo into io parameter list
    for (auto it = info.begin(); it != info.end(); ++it) {
      std::string value = it.value();
      io_manager->loaderParams[it.key()] = value;
    }
  }

  // init and save parameters of input file to facilitate rewrite
  io_manager->init(input, comm);
  io_manager->setSave(dump);

  if (dump)
    io_manager->saveInputFileParameters();

  // Cycle through compressors and parameters
  for (int c = 0; c < compressors.size(); ++c) {
    // initialize compressor
    compress_manager = CompressorFactory::create(compressors[c]);
    if (compress_manager == nullptr) {
      if (rank == 0) {
        std::cout << "Unsupported compressor: " << compressors[c] << " ... ";
        std::cout << "Skipping!" << std::endl;
      }
      continue;
    }

    // initialize compressor
    compress_manager->init();

    // Apply parameter if same for all scalars, else delay for later
    bool sameCompressorParams = true;
    if (json["compress"]["kernels"][c].count("params"))
      sameCompressorParams = false;
    else {
      auto const& current = json["kernels"][c];
      for (auto it = current.begin(); it != current.end(); ++it) {
        if (it.key() != "name" and it.key() != "prefix") {
          std::string value = it.value();
          compress_manager->parameters[it.key()] = value;
        }
      }
    }

    // log
    metrics_info << std::endl;
    metrics_info << "---------------------------------------" << std::endl;
    metrics_info << "Compressor: " << compress_manager->getName() << std::endl;

    debug_log << "---------------------------------------" << std::endl;
    debug_log << "Compressor: " << compress_manager->getName() << std::endl;

    // Cycle through scalars
    for (auto& scalar : scalars) {
      Timer clock_zip;
      Timer clock_unzip;
      Memory memory_manager;

      memory_manager.start();

      // Check if parameter is valid before proceding
      if (not io_manager->loadData(scalar)) {
        memory_manager.stop();
        continue;
      }

      // Read in compressor parameter for this field
      if (not sameCompressorParams) {
        // reset param for each field
        compress_manager->parameters.clear();
        auto const& param = json["compress"]["kernels"][c]["params"];
        int const nb_params = param.size();

        for (int i = 0; i < nb_params; i++) {
          for (auto&& current : param[i]["scalar"]) {
            std::string name = current;
            if (name != scalar)
              continue;

            //auto& param = json["compress"]["kernels"][c]["params"][i];
            for (auto it = param[i].begin(); it != param[i].end(); ++it) {
              if (it.key() != "scalar") {
                double value = it.value();
                compress_manager->parameters[it.key()] = std::to_string(value);
              }
            }
          }
        }
      }

      // log stuff
      debug_log << io_manager->getDataInfo();
      debug_log << io_manager->getLog();
      appendLog(logs, debug_log);

      metrics_info << compress_manager->getInfos() << std::endl;
      output_csv << compress_manager->getName() << "_" << scalar;
      output_csv << "__" << compress_manager->getInfos();
      output_csv << ", " << json["compress"]["kernels"][c]["prefix"] << ", ";

      MPI_Barrier(comm);

      // compress
      void* raw_comp = nullptr;

      clock_zip.start();
      compress_manager->compress(
        io_manager->data, raw_comp,
        io_manager->getType(),
        io_manager->getTypeSize(),
        io_manager->getSizePerDim()
      );
      clock_zip.stop();

      // decompress
      void* raw_decomp = nullptr;

      clock_unzip.start();
      compress_manager->decompress(
        raw_comp, raw_decomp,
        io_manager->getType(),
        io_manager->getTypeSize(),
        io_manager->getSizePerDim()
      );
      clock_unzip.stop();

      unsigned long local_size[2];
      local_size[0] = compress_manager->getBytes();
      local_size[1] = io_manager->getTypeSize() * io_manager->getNumElements();

      unsigned long total_size[2];
      MPI_Allreduce(local_size  , total_size  , 1, MPI_UNSIGNED_LONG, MPI_SUM, comm);
      MPI_Allreduce(local_size+1, total_size+1, 1, MPI_UNSIGNED_LONG, MPI_SUM, comm);

      // get compression ratio
      float const compression_ratio = total_size[1] / (float) total_size[0];

      debug_log << std::endl << std::endl;
      debug_log << "local compressed size: "   << local_size[0] << ", ";
      debug_log << "total compressed size: "   << total_size[0] << std::endl;
      debug_log << "local uncompressed size: " << local_size[1] << ", ";
      debug_log << "total uncompressed size: " << total_size[1] << std::endl;
      debug_log << "Compression ratio: " << compression_ratio << std::endl;

      appendLog(logs, compress_manager->getLog());
      compress_manager->clearLog();

      //
      // metrics
      debug_log << std::endl;
      debug_log << "----- " << scalar << " error metrics ----- " << std::endl;
      metrics_info << std::endl;
      metrics_info << "Field: " << scalar << std::endl;

      for (int m = 0; m < metrics.size(); ++m) {
        metrics_manager = MetricsFactory::create(metrics[m]);
        if (metrics_manager == nullptr) {
          if (rank == 0) {
            std::cout << "Unsupported metric: " << metrics[m] << " ... ";
            std::cout << "Skipping!" << std::endl;
          }
          continue;
        }

        // Read in additional params for metrics
        auto& current = json["compress"]["metrics"][m];
        for (auto it = current.begin(); it != current.end(); it++) {
          std::string key = it.key();
          if (key == "name")
            continue;

          for (auto&& metric : json["compress"]["metrics"][m][key]) {
            if (metric == scalar) {
              metrics_manager->parameters[key] = scalar;
              break;
            }
          }
        }

        // Launch
        metrics_manager->init(comm);
        metrics_manager->execute(
          io_manager->data, raw_decomp,
          io_manager->getNumElements()
        );

        debug_log << metrics_manager->getLog();
        metrics_info << metrics_manager->getLog();
        output_csv << metrics_manager->getGlobalValue() << ", ";

        if (rank == 0) {
          if (not metrics_manager->additionalOutput.empty()) {
            createFolder("logs");
            std::string outputHistogramName = "logs/";
            outputHistogramName += extractFileName(input) + "_" + compressors[c];
            outputHistogramName += "_" + scalar + "_" + metrics[m] + "_";
            outputHistogramName += compress_manager->getInfos() + "_hist.py";
            writeFile(outputHistogramName, metrics_manager->additionalOutput);
          }
        }
        metrics_manager->close();
      }

      debug_log << "-----------------------------" << std::endl;
      debug_log << std::endl;
      debug_log << "Memory in use: " << memory_manager.getMemoryInUseInMB();
      debug_log << " MB" << std::endl;

      //
      // Metrics Computation
      double compress_time = clock_zip.getDuration();
      double decompress_time = clock_unzip.getDuration();

      size_t bytes = io_manager->getNumElements() * io_manager->getTypeSize();
      double megabytes = static_cast<double>(bytes) / (1024. * 1024.);
      double compress_throughput = megabytes / compress_time;
      double decompress_throughput = megabytes / decompress_time;

      double min_throughput[2] {0, 0};
      double max_throughput[2] {0, 0};
      double max_compress_time = 0;

      MPI_Reduce(      &compress_time, &max_compress_time, 1, MPI_DOUBLE, MPI_MAX, 0, comm);
      MPI_Reduce(  &compress_throughput, max_throughput  , 1, MPI_DOUBLE, MPI_MAX, 0, comm);
      MPI_Reduce(  &compress_throughput, min_throughput  , 1, MPI_DOUBLE, MPI_MIN, 0, comm);
      MPI_Reduce(&decompress_throughput, max_throughput+1, 1, MPI_DOUBLE, MPI_MAX, 0, comm);
      MPI_Reduce(&decompress_throughput, min_throughput+1, 1, MPI_DOUBLE, MPI_MIN, 0, comm);

      if (dump) {
        debug_log << "writing: " << scalar << std::endl;

        io_manager->saveCompData(scalar, raw_decomp);
        debug_log << io_manager->getLog();
      }

      // deallocate
      std::free(raw_decomp);

      io_manager->close();
      memory_manager.stop();
      //
      // log stuff
      auto const memory_leaked = memory_manager.getMemorySizeInMB();

      debug_log << std::endl;
      debug_log << "Compress time: " << compress_time << std::endl;
      debug_log << "Decompress time: " << decompress_time << std::endl;
      debug_log << std::endl;
      debug_log << "Memory leaked: " << memory_leaked << " MB" << std::endl;
      debug_log << ".........................................";
      debug_log << std::endl << std::endl;
      appendLog(logs, debug_log);

      if (rank == 0) {

        float const ratio = total_size[1] / (float) total_size[0];

        metrics_info << "Max Compression Throughput: "   << max_throughput[0];
        metrics_info << " MB/s" << std::endl;
        metrics_info << "Max DeCompression Throughput: " << max_throughput[1];
        metrics_info << " MB/s" << std::endl;
        metrics_info << "Max Compress Time: " << max_compress_time;
        metrics_info << " s" << std::endl;
        metrics_info << "Min Compression Throughput: "   << min_throughput[0];
        metrics_info << " MB/s" << std::endl;
        metrics_info << "Min DeCompression Throughput: " << min_throughput[1];
        metrics_info << " MB/s" << std::endl;
        metrics_info << "Compression ratio: " << ratio << std::endl;

        output_csv << min_throughput[0] << ", ";
        output_csv << min_throughput[1] << ", ";
        output_csv << ratio << std::endl;

        writeFile(stats + ".txt", metrics_info.str());
        writeFile(stats + ".csv", output_csv.str());
      }

      MPI_Barrier(comm);
    }

    if (dump) {
      Timer clock_dump;
      clock_dump.start();

      debug_log << "Dumping data ... " << std::endl;

      // Pass data that was not compressed
      for (auto&& param : io_manager->inOutData) {
        if (not param.doWrite) {
          io_manager->loadData(param.name);
          io_manager->saveCompData(param.name, io_manager->data);
          io_manager->close();
        }
      }

      auto const& current_compressor = json["compress"]["kernels"][c];
      temp = current_compressor["prefix"];
      auto output_decompressed = (output_path != "."
        ? std::string(output_path + "/" + temp + "__" + output_path)
        : std::string(temp + "__" + output_path)
      );

      io_manager->writeData(output_decompressed);
      clock_dump.stop();

      debug_log << io_manager->getLog();
      debug_log << "Dumping data took: " << clock_dump.getDuration() << " s.";
      debug_log << std::endl;
      appendLog(logs, debug_log);
    }
    compress_manager->close();
  }

  clock_overall.stop();
  debug_log << std::endl;
  debug_log << "Total run time: " << clock_overall.getDuration() << " s.";
  debug_log << std::endl;
  appendLog(logs, debug_log);

  // For humans
  if (rank == 0) {
    std::cout << std::endl << "That's all folks!" << std::endl;
  }

  MPI_Finalize();
  return EXIT_SUCCESS;
}
/* -------------------------------------------------------------------------- */