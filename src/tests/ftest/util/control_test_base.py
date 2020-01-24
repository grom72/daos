#!/usr/bin/python
"""
  (C) Copyright 2019 Intel Corporation.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
  The Government's rights to use, modify, reproduce, release, perform, display,
  or disclose this software are subject to the terms of the Apache License as
  provided in Contract No. B609815.
  Any reproduction of computer software, computer software documentation, or
  portions thereof marked with this legend must also reproduce the markings.
"""
from avocado.utils import process

from apricot import TestWithServers
from dmg_utils import DmgCommand


class ControlTestBase(TestWithServers):
    # pylint: disable=too-few-public-methods
    """Defines common methods for control tests.

    :avocado: recursive
    """

    def __init__(self, *args, **kwargs):
        """Initialize a ControlTestBase object."""
        super(ControlTestBase, self).__init__(*args, **kwargs)
        self.setup_start_agents = False

    def run_dmg_command(self):
        """Run the dmg command."""
        # Create dmg command
        dmg = DmgCommand(self.bin)
        dmg.get_params(self)
        dmg.set_hostlist(self.server_managers[0])

        try:
            dmg.run()
        except process.CmdError as details:
            self.fail("dmg command failed: {}".format(details))
