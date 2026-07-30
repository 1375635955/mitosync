#!/bin/bash

git submodule update --init --recursive
cd vcpkg && ./bootstrap-vcpkg.sh && cd ..
vcpkg/vcpkg install