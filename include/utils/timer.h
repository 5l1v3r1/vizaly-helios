/*
 * Copyright (c) 2019, Los Alamos National Laboratory
 * All rights reserved.
 *
 * Authors: Pascal Grosset and Hoby Rakotoarivelo
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

#pragma once
/* -------------------------------------------------------------------------- */
#include <chrono>
#include <ctime>
#include <sstream>
/* -------------------------------------------------------------------------- */
class Timer {

	std::chrono::high_resolution_clock::time_point tic {}, toc {};
	std::chrono::duration<double> elapsed_seconds {};

  public:
	 Timer() = default;
	~Timer() = default;

	void start() {
    tic = std::chrono::high_resolution_clock::now();
	}

	void stop() {
    toc = std::chrono::high_resolution_clock::now();
    elapsed_seconds = toc - tic;
	}

	double getDuration() {
    return elapsed_seconds.count();
	}

	static std::string getCurrentTime() {
    time_t now = time(0);
    tm *ltm = localtime(&now);

    std::stringstream buffer;
    buffer << "_" << 1 + ltm->tm_mon << "_" << ltm->tm_mday << "__";
    buffer << ltm->tm_hour << "_" << ltm->tm_min << "_" << ltm->tm_sec;
    buffer << "_" << std::endl;
    return buffer.str();
	}
};
/* -------------------------------------------------------------------------- */
