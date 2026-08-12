#ifndef CLOUD_RELIGHT_DRAINE_HLSLI
#define CLOUD_RELIGHT_DRAINE_HLSLI

/*
 * SPDX-FileCopyrightText: Copyright (c) <2023> NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// [Jendersie and d'Eon 2023]
//   SIGGRAPH 2023 Talks
//   https://doi.org/10.1145/3587421.3595409
//   https://research.nvidia.com/labs/rtr/approximate-mie/

// EVAL for the Draine (and therefore Cornette-Shanks) phase function
//   g = HG shape parameter
//   a = "alpha" shape parameter

// Warning: this function doesn't special case isotropic scattering and can numerically fail for certain inputs

#include "Common/Math.hlsli"

// eval:
//   u = dot(prev_dir, next_dir)
float evalDraine(in float u, in float g, in float a)
{
	return ((1 - g * g) * (1 + a * u * u)) / (4. * (1 + (a * (1 + 2 * g * g)) / 3.) * Math::PI * pow(1 + g * g - 2 * g * u, 1.5));
}

#endif
