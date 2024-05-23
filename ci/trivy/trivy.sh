#!/bin/bash

set -eux

trivy fs -c ci/trivy/trivy.yaml -f $1 --exit-code 1 .
