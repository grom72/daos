#!/bin/bash

set -eux

trivy fs -c ci/trivy/trivy.yaml --dependency-tree -f table --show-suppressed .
#\
#	--skip-dirs ./src/client/java/hadoop-daos --exit-code 1 .