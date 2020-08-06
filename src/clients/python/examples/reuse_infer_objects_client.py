#!/usr/bin/env python
# Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import argparse
import numpy as np
import sys
from builtins import range
import tritonhttpclient
import tritongrpcclient
import tritonshmutils.shared_memory as shm
import tritonclientutils as utils

FLAGS = None


def infer_and_validata(use_shared_memory, orig_input0_data, orig_input1_data):
    if use_shared_memory:
        input0_data = orig_input0_data
        input1_data = orig_input1_data
        byte_size = input0_data.size * input0_data.itemsize
        inputs[0].set_shared_memory("input0_data", byte_size)
        inputs[1].set_shared_memory("input1_data", byte_size)
        outputs[0].set_shared_memory("output0_data", byte_size)
        outputs[1].set_shared_memory("output1_data", byte_size)
    else:
        input0_data = orig_input0_data
        input1_data = orig_input1_data * 2
        inputs[0].set_data_from_numpy(np.expand_dims(input0_data, axis=0))
        inputs[1].set_data_from_numpy(np.expand_dims(input1_data, axis=0))
        outputs[0].unset_shared_memory()
        outputs[1].unset_shared_memory()

    results = triton_client.infer(model_name=model_name,
                                  inputs=inputs,
                                  outputs=outputs)

    # Read results from the shared memory.
    output0 = results.get_output("OUTPUT0")
    if output0 is not None:
        if use_shared_memory:
            if protocol == "grpc":
                output0_data = shm.get_contents_as_numpy(
                    shm_op0_handle, utils.triton_to_np_dtype(output0.datatype),
                    output0.shape)
            else:
                output0_data = shm.get_contents_as_numpy(
                    shm_op0_handle,
                    utils.triton_to_np_dtype(output0['datatype']),
                    output0['shape'])
        else:
            output0_data = results.as_numpy('OUTPUT0')
    else:
        print("OUTPUT0 is missing in the response.")
        sys.exit(1)

    output1 = results.get_output("OUTPUT1")
    if output1 is not None:
        if use_shared_memory:
            if protocol == "grpc":
                output1_data = shm.get_contents_as_numpy(
                    shm_op1_handle, utils.triton_to_np_dtype(output1.datatype),
                    output1.shape)
            else:
                output1_data = shm.get_contents_as_numpy(
                    shm_op1_handle,
                    utils.triton_to_np_dtype(output1['datatype']),
                    output1['shape'])
        else:
            output1_data = results.as_numpy('OUTPUT1')
    else:
        print("OUTPUT1 is missing in the response.")
        sys.exit(1)

    if use_shared_memory:
        print("\n\n======== SHARED_MEMORY ========\n")
    else:
        print("\n\n======== NO_SHARED_MEMORY ========\n")
    for i in range(16):
        print(str(input0_data[i]) + " + " + str(input1_data[i]) + " = " +
              str(output0_data[0][i]))
        print(str(input0_data[i]) + " - " + str(input1_data[i]) + " = " +
              str(output1_data[0][i]))
        if (input0_data[i] + input1_data[i]) != output0_data[0][i]:
            print("shm infer error: incorrect sum")
            sys.exit(1)
        if (input0_data[i] - input1_data[i]) != output1_data[0][i]:
            print("shm infer error: incorrect difference")
            sys.exit(1)
    print("\n======== END ========\n\n")


# Tests whether the same InferInput and InferRequestedOutput objects can be
# successfully used repeatedly for different inferences using/not-using
# shared memory.
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-v',
                        '--verbose',
                        action="store_true",
                        required=False,
                        default=False,
                        help='Enable verbose output')
    parser.add_argument('-i',
                        '--protocol',
                        type=str,
                        required=False,
                        default='HTTP',
                        help='Protocol (HTTP/gRPC) used to communicate with ' +
                        'the inference service. Default is HTTP.')
    parser.add_argument('-u',
                        '--url',
                        type=str,
                        required=False,
                        default='localhost:8000',
                        help='Inference server URL. Default is localhost:8000.')

    FLAGS = parser.parse_args()

    protocol = FLAGS.protocol.lower()

    try:
        if protocol == "grpc":
            # Create gRPC client for communicating with the server
            triton_client = tritongrpcclient.InferenceServerClient(
                url=FLAGS.url, verbose=FLAGS.verbose)
        else:
            # Create HTTP client for communicating with the server
            triton_client = tritonhttpclient.InferenceServerClient(
                url=FLAGS.url, verbose=FLAGS.verbose)
    except Exception as e:
        print("client creation failed: " + str(e))
        sys.exit(1)

    # To make sure no shared memory regions are registered with the
    # server.
    triton_client.unregister_system_shared_memory()
    triton_client.unregister_cuda_shared_memory()

    # We use a simple model that takes 2 input tensors of 16 integers
    # each and returns 2 output tensors of 16 integers each. One
    # output tensor is the element-wise sum of the inputs and one
    # output is the element-wise difference.
    model_name = "simple"
    model_version = ""

    # Create the data for the two input tensors. Initialize the first
    # to unique integers and the second to all ones.
    input0_data = np.arange(start=0, stop=16, dtype=np.int32)
    input1_data = np.ones(shape=16, dtype=np.int32)

    input_byte_size = input0_data.size * input0_data.itemsize
    output_byte_size = input_byte_size

    # Create Output0 and Output1 in Shared Memory and store shared memory handles
    shm_op0_handle = shm.create_shared_memory_region("output0_data",
                                                     "/output0_simple",
                                                     output_byte_size)
    shm_op1_handle = shm.create_shared_memory_region("output1_data",
                                                     "/output1_simple",
                                                     output_byte_size)

    # Register Output0 and Output1 shared memory with Triton Server
    triton_client.register_system_shared_memory("output0_data",
                                                "/output0_simple",
                                                output_byte_size)
    triton_client.register_system_shared_memory("output1_data",
                                                "/output1_simple",
                                                output_byte_size)

    # Create Input0 and Input1 in Shared Memory and store shared memory handles
    shm_ip0_handle = shm.create_shared_memory_region("input0_data",
                                                     "/input0_simple",
                                                     input_byte_size)
    shm_ip1_handle = shm.create_shared_memory_region("input1_data",
                                                     "/input1_simple",
                                                     input_byte_size)

    # Put input data values into shared memory
    shm.set_shared_memory_region(shm_ip0_handle, [input0_data])
    shm.set_shared_memory_region(shm_ip1_handle, [input1_data])

    # Register Input0 and Input1 shared memory with Triton Server
    triton_client.register_system_shared_memory("input0_data", "/input0_simple",
                                                input_byte_size)
    triton_client.register_system_shared_memory("input1_data", "/input1_simple",
                                                input_byte_size)

    # Set the parameters to use data from shared memory
    inputs = []
    if protocol == "grpc":
        inputs.append(tritongrpcclient.InferInput('INPUT0', [1, 16], "INT32"))

        inputs.append(tritongrpcclient.InferInput('INPUT1', [1, 16], "INT32"))
    else:
        inputs.append(tritonhttpclient.InferInput('INPUT0', [1, 16], "INT32"))

        inputs.append(tritonhttpclient.InferInput('INPUT1', [1, 16], "INT32"))

    outputs = []
    if protocol == "grpc":
        outputs.append(tritongrpcclient.InferRequestedOutput('OUTPUT0'))
        outputs.append(tritongrpcclient.InferRequestedOutput('OUTPUT1'))
    else:
        outputs.append(tritonhttpclient.InferRequestedOutput('OUTPUT0'))
        outputs.append(tritonhttpclient.InferRequestedOutput('OUTPUT1'))

    # Use shared memory
    infer_and_validata(True, input0_data, input1_data)

    # Don't use shared memory
    infer_and_validata(False, input0_data, input1_data)

    # Use shared memory
    infer_and_validata(True, input0_data, input1_data)

    triton_client.unregister_system_shared_memory()
    shm.destroy_shared_memory_region(shm_ip0_handle)
    shm.destroy_shared_memory_region(shm_ip1_handle)
    shm.destroy_shared_memory_region(shm_op0_handle)
    shm.destroy_shared_memory_region(shm_op1_handle)
