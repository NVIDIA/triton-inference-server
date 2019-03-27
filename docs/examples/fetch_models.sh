#!/bin/bash
# Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
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

set -ex

# Caffe2 resnet50
mkdir -p model_repository/resnet50_netdef/1
wget -O model_repository/resnet50_netdef/1/model.netdef \
     http://download.caffe2.ai.s3.amazonaws.com/models/resnet50/predict_net.pb
wget -O model_repository/resnet50_netdef/1/init_model.netdef \
     http://download.caffe2.ai.s3.amazonaws.com/models/resnet50/init_net.pb

# TensorFlow inception
mkdir -p model_repository/inception_graphdef/1
wget -O /tmp/inception_v3_2016_08_28_frozen.pb.tar.gz \
     https://storage.googleapis.com/download.tensorflow.org/models/inception_v3_2016_08_28_frozen.pb.tar.gz
(cd /tmp && tar xzf inception_v3_2016_08_28_frozen.pb.tar.gz)
mv /tmp/inception_v3_2016_08_28_frozen.pb model_repository/inception_graphdef/1/model.graphdef

# Custom models
mkdir -p model_repository/image_preprocess_nchw_3x224x224_inception/1
mkdir -p model_repository/preprocess_resnet50_ensemble/1
wget -O /tmp/custom_models.tar.gz `curl https://api.github.com/repos/NVIDIA/tensorrt-inference-server/releases/latest | grep -oP '(?<=browser_download_url\":\ \")https.*custom_models\.tar\.gz(?=\")'`
(cd /tmp && tar xzf custom_models.tar.gz)
mv /tmp/custom_models/libimagepreprocess.so model_repository/image_preprocess_nchw_3x224x224_inception/1/.