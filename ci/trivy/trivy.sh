#!/bin/bash

set -eux

trivy fs -c ci/trivy/trivy.yaml --dependency-tree -f table --show-suppressed \
	--exit-code 1 .