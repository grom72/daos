#!/bin/bash

set -eux

# workaround to ignore hadoop files as --skip-dirs option does not work properly on GHA
rm -rf ./src/client/java/hadoop-daos

trivy fs -c ci/trivy/trivy.yaml --dependency-tree -f table --show-suppressed \
	--skip-dirs ./src/client/java/hadoop-daos --exit-code 1 .