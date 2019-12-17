#!/usr/bin/python
"""
  (C) Copyright 2017-2019 Intel Corporation.

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
from __future__ import print_function

import os
import traceback
from avocado.utils import process
from apricot import TestWithServers, skipForTicket
import check_for_pool


# pylint: disable=broad-except
class MultiServerCreateDeleteTest(TestWithServers):
    """
    Tests DAOS pool creation, trying both valid and invalid parameters.

    :avocado: recursive
    """

    @skipForTicket("DAOS-3621")
    def test_create(self):
        """Test basic pool creation.

        :avocado: tags=all,pool,full_regression,small,multitarget
        """
        # Accumulate a list of pass/fail indicators representing what is
        # expected for each parameter then "and" them to determine the
        # expected result of the test
        expected_for_param = []

        userlist = self.params.get("user", '/run/tests/users/*')
        user = os.getlogin() if userlist[0] == 'valid' else userlist[0]
        expected_for_param.append(userlist[1])

        grouplist = self.params.get("group", '/run/tests/groups/*')
        group = os.getlogin() if grouplist[0] == 'valid' else grouplist[0]
        expected_for_param.append(grouplist[1])

        setidlist = self.params.get("setname", '/run/tests/setnames/*')
        setid = setidlist[0]
        expected_for_param.append(setidlist[1])

        tgtlistlist = self.params.get("tgt", '/run/tests/tgtlist/*')
        tgtlist = tgtlistlist[0]
        expected_for_param.append(tgtlistlist[1])

        # if any parameter is FAIL then the test should FAIL
        expected_result = 'PASS'
        for result in expected_for_param:
            if result == 'FAIL':
                expected_result = 'FAIL'
                break

        host1 = self.hostlist_servers[0]
        host2 = self.hostlist_servers[1]

        dmg = os.path.join(self.bin, "dmg")

        try:
            cmd = (
                "{} pool create "
                "--user={} "
                "--group={} "
                "--sys={} "
                "--ranks={}".format(dmg, user, group, setid, tgtlist))

            uuid_str = (
                """{0}""".format(process.system_output(cmd)).split(" ")[0])
            print("uuid is {0}\n".format(uuid_str))

            if '0' in tgtlist:
                exists = check_for_pool.check_for_pool(host1, uuid_str)
                if exists != 0:
                    self.fail("Pool {0} not found on host {1}.\n"
                              .format(uuid_str, host1))
            if '1' in tgtlist:
                exists = check_for_pool.check_for_pool(host2, uuid_str)
                if exists != 0:
                    self.fail("Pool {0} not found on host {1}.\n"
                              .format(uuid_str, host2))

            delete_cmd = (
                "{} pool destroy "
                "--pool={} "
                # "--group={2} "  TODO: should this be implemented?
                "--force".format(dmg, uuid_str))  # , setid))

            process.system(delete_cmd)

            if '0' in tgtlist:
                exists = check_for_pool.check_for_pool(host1, uuid_str)
                if exists == 0:
                    self.fail("Pool {0} found on host {1} after destroy.\n"
                              .format(uuid_str, host1))
            if '1' in tgtlist:
                exists = check_for_pool.check_for_pool(host2, uuid_str)
                if exists == 0:
                    self.fail("Pool {0} found on host {1} after destroy.\n"
                              .format(uuid_str, host2))

            if expected_result in ['FAIL']:
                self.fail("Test was expected to fail but it passed.\n")

        except Exception as exc:
            print(exc)
            print(traceback.format_exc())
            if expected_result == 'PASS':
                self.fail("Test was expected to pass but it failed.\n")
