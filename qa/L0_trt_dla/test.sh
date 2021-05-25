#!/bin/bash
# Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

REPO_VERSION=${NVIDIA_TRITON_SERVER_VERSION}
if [ "$#" -ge 1 ]; then
    REPO_VERSION=$1
fi
if [ -z "$REPO_VERSION" ]; then
    echo -e "Repository version must be specified"
    echo -e "\n***\n*** Test Failed\n***"
    exit 1
fi

export CUDA_VISIBLE_DEVICES=0

# Need to run on only one device since only creating a single
# PLAN. Without this test will fail on a heterogeneous system.
export CUDA_VISIBLE_DEVICES=0

IMAGE_CLIENT=../clients/image_client
IMAGE=../images/vulture.jpeg

CAFFE2PLAN=../common/caffe2plan

DATADIR=${DATADIR:="/data/inferenceserver/${REPO_VERSION}"}
OPTDIR=${OPTDIR:="/opt"}
SERVER=${OPTDIR}/tritonserver/bin/tritonserver
BACKEND_DIR=${OPTDIR}/tritonserver/backends

SERVER_ARGS="--model-repository=`pwd`/models --exit-timeout-secs=120 --backend-directory=${BACKEND_DIR}"
SERVER_LOG="./inference_server.log"
source ../common/util.sh

rm -fr models && mkdir models 
cp -r $DATADIR/caffe_models/trt_model_store/resnet50_plan models/.
rm -f *.log

set +e

# Create the PLAN file
mkdir -p models/resnet50_plan/1 && rm -f models/resnet50_plan/1/model.plan && \
    $CAFFE2PLAN -b32 -n prob -o models/resnet50_plan/1/model.plan \
                $DATADIR/caffe_models/resnet50_dla.prototxt $DATADIR/caffe_models/resnet50.caffemodel
if [ $? -ne 0 ]; then
    echo -e "\n***\n*** Failed to generate resnet50 DLA compatible PLAN\n***"
    exit 1
fi

set -e

# Enable NVDLA by specifying secondary devices in instance group
echo "instance_group [{" >> models/resnet50_plan/config.pbtxt
echo "    kind: KIND_GPU" >> models/resnet50_plan/config.pbtxt
echo "    secondary_devices [{" >> models/resnet50_plan/config.pbtxt
echo "          kind: KIND_NVDLA " >> models/resnet50_plan/config.pbtxt
echo "          device_id: 0" >> models/resnet50_plan/config.pbtxt
echo "    }]" >> models/resnet50_plan/config.pbtxt
echo "}]" >> models/resnet50_plan/config.pbtxt

run_server
if [ "$SERVER_PID" == "0" ]; then
    echo -e "\n***\n*** Failed to start $SERVER\n***"
    cat $SERVER_LOG
    exit 1
fi

RET=0

set +e

CLIENT_LOG=${IMAGE_CLIENT##*/}.log

echo "Model: resnet50_plan" >> $CLIENT_LOG
$IMAGE_CLIENT $EXTRA_ARGS -m resnet50_plan -s VGG -c 1 -b 1 $IMAGE >> $CLIENT_LOG 2>&1
if [ $? -ne 0 ]; then
    cat $CLIENT_LOG
    RET=1
fi

if [ `grep -c VULTURE $CLIENT_LOG` != "1" ]; then
    echo -e "\n***\n*** Failed. Expected 1 VULTURE results\n***"
    RET=1
fi

set -e

kill $SERVER_PID
wait $SERVER_PID

rm -rf models

if [ $RET -eq 0 ]; then
    echo -e "\n***\n*** Test Passed\n***"
else
    echo -e "\n***\n*** Test FAILED\n***"
fi

exit $RET
