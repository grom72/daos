"""
  (C) Copyright 2020-2024 Intel Corporation.
  (C) Copyright 2025 Hewlett Packard Enterprise Development LP

  SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import queue
import re
import threading
import time

from apricot import TestWithServers
from daos_utils import DaosCommand
from exception_utils import CommandFailure
from general_utils import DaosTestError
from mdtest_test_base import MdtestBase
from nvme_utils import ServerFillUp
from pydaos.raw import DaosApiError


def get_data_parity_number(log, oclass):
    """Return EC Object Data and Parity count.

    Args:
        log (logger): Log object for reporting error
        oclass (string): EC Object type.

    Returns:
        list: Data and Parity numbers from object type

    """
    if 'EC' not in oclass:
        log.error("Provide EC Object type only and not %s", str(oclass))
        return 0

    tmp = re.findall(r'\d+', oclass)
    return {'data': tmp[0], 'parity': tmp[1]}


def check_aggregation_status(log, pool, quick_check=True, attempt=20):
    """EC Aggregation triggered status.

    Args:
        log (logger): Log object for reporting information
        pool (TestPool): pool object to get the query.
        quick_check (bool): Return immediately when Aggregation starts for any storage type.
        attempt (int): Number of attempts to do pool query at interval of 5 seconds.
                      default is 20 attempts.

    Returns:
        dict: Storage Aggregation stats SCM/NVMe True/False.

    """
    agg_status = {'scm': False, 'nvme': False}
    pool.connect()
    initial_usage = pool.pool_percentage_used()

    for _tmp in range(attempt):
        current_usage = pool.pool_percentage_used()
        log.debug("pool_percentage during Aggregation = %s", current_usage)
        for storage_type in ['scm', 'nvme']:
            if current_usage[storage_type] > initial_usage[storage_type]:
                log.debug("Aggregation Started for {}.....".format(storage_type))
                agg_status[storage_type] = True
                # Return immediately once aggregation starts for quick check
                if quick_check:
                    return agg_status
            else:
                initial_usage[storage_type] = current_usage[storage_type]
        time.sleep(5)
    return agg_status


class ErasureCodeIor(ServerFillUp):
    """Class to used for EC testing.

    It will get the object types from yaml file write the IOR data set with IOR.
    """

    def __init__(self, *args, **kwargs):
        """Initialize a ServerFillUp object."""
        super().__init__(*args, **kwargs)
        self.server_count = None
        self.ec_container = None
        self.cont_uuid = []
        self.cont_number = 0
        self.read_set_from_beginning = True
        self.nvme_local_cont = None

    def setUp(self):
        """Set up each test case."""
        # Start the servers and agents
        super().setUp()

        # Fail IOR test in case of Warnings
        self.fail_on_warning = True
        engine_count = self.server_managers[0].get_config_value("engines_per_host")
        self.server_count = len(self.hostlist_servers) * engine_count
        # Create the Pool
        kwargs = {
            "scm": self.params.get("scm", "/run/create_pool_max_size/*", False),
            "nvme": self.params.get("nvme", "/run/create_pool_max_size/*", False),
            "percentage": self.params.get("percentage", "/run/create_pool_max_size/*", 96)
        }
        self.create_pool_max_size(**kwargs)
        self.update_ior_cmd_with_pool()
        self.obj_class = self.params.get("dfs_oclass_list", '/run/ior/objectclass/*')
        self.ior_chu_trs_blk_size = self.params.get("chunk_block_transfer_sizes", '/run/ior/*')

    def ec_container_create(self, oclass):
        """Create the container for EC object."""
        # Get container params
        self.ec_container = self.get_container(
            self.pool, create=False, daos=self.get_daos_command(), oclass=oclass)

        # update object class for container create, if supplied explicitly.
        ec_object = get_data_parity_number(self.log, oclass)
        rd_fac = "rd_fac:{}".format(ec_object['parity'])
        if self.ec_container.properties.value is None:
            self.ec_container.properties.update(rd_fac)
        else:
            self.ec_container.properties.update("{},{}"
                                                .format(self.ec_container.properties.value, rd_fac))
        # create container
        self.ec_container.create()
        self.nvme_local_cont = self.ec_container

    def ior_param_update(self, oclass, sizes):
        """Update the IOR command parameters.

        Args:
            oclass (list): list of the obj class to use with IOR
            sizes (list): Update Transfer, Chunk and Block sizes
        """
        self.ior_local_cmd.dfs_chunk.update(sizes[0])
        self.ior_local_cmd.block_size.update(sizes[1])
        self.ior_local_cmd.transfer_size.update(sizes[2])
        self.ior_local_cmd.dfs_oclass.update(oclass[0])
        self.ior_local_cmd.dfs_dir_oclass.update(oclass[0])

    def ior_write_single_dataset(self, oclass, sizes, storage='NVMe', operation="WriteRead",
                                 percent=1):
        """Write IOR single data set with EC object.

        Args:
            oclass (list): list of the obj class to use with IOR
            sizes (list): Update Transfer, Chunk and Block sizes
            storage (str): Data to be written on storage,default to NVMe.
            operation (str): Data to be Written only or Write and Read both. default to WriteRead
                            both
            percent (int): %of storage to be filled. Default it will use the given parameters in
                            yaml file.
        """
        try:
            self.log.info(self.pool.pool_percentage_used())
        except ZeroDivisionError:
            self.log.info("Either SCM or NVMe is used so ignore the error")

        self.ior_param_update(oclass, sizes)

        # Create the new container with correct redundancy factor for EC
        self.ec_container_create(oclass[0])
        self.update_ior_cmd_with_pool(create_cont=False)

        # Start IOR Write
        self.start_ior_load(storage, operation, percent, create_cont=False)

        # Store the container UUID for future reading
        self.cont_uuid.append(self.ior_local_cmd.dfs_cont.value)

    def ior_write_dataset(self, storage='NVMe', operation="WriteRead", percent=1):
        """Write IOR data set with different EC object and different sizes.

        Args:
            storage (str): Data to be written on storage, default to NVMe
            operation (str): Data to be Written only or Write and Read both. default to WriteRead
                            both.
            percent (int): %of storage to be filled. Default it's 1%.
        """
        for oclass in self.obj_class:
            for sizes in self.ior_chu_trs_blk_size:
                # Skip the object type if server count does not meet the minimum EC object server
                # count
                if oclass[1] > self.server_count:
                    continue
                self.ior_write_single_dataset(oclass, sizes, storage, operation, percent)

    def ior_read_single_dataset(self, oclass, sizes, storage='NVMe', operation="Read", percent=1):
        # pylint: disable=too-many-arguments
        """Read IOR single data set with EC object.

        Args:
            oclass (list): list of the obj class to use with IOR
            sizes (list): Update Transfer, Chunk and Block sizes
            storage (str): Data to be written on which storage
            operation (str): Data to be Read only or Auto_Read which select IOR blocksize during
                            runtime.
            percent (int): %of storage to be filled. Default it's 1%.
        """
        self.ior_param_update(oclass, sizes)

        # retrieve the container UUID to read the existing data
        self.nvme_local_cont.uuid = self.cont_uuid[self.cont_number]

        # Start IOR Read
        self.start_ior_load(storage, operation, percent, create_cont=False)

    def ior_read_dataset(self, storage='NVMe', operation="Read", percent=1, parity=1):
        """Read IOR data and verify for different EC object and different sizes.

        Args:
            storage (str): Data to be written on storage, default to NVMe
            percent (int): %of storage to be filled. Default it's 1%
            operation (str): Data to be Read only or Auto_Read which select IOR blocksize during
                            runtime.
            parity (int): object parity type for reading data, default is 1.
        """
        # By default read the data set from beginning, or start from the specific container UUID
        if self.read_set_from_beginning:
            self.cont_number = 0

        for oclass in self.obj_class:
            for sizes in self.ior_chu_trs_blk_size:
                # Skip the object type if server count does not meet the minimum EC object server
                # count.
                if oclass[1] > self.server_count:
                    continue
                parity_set = "P{}".format(parity)
                # Read the requested data+parity data set only
                if parity != 1 and parity_set not in oclass[0]:
                    self.log.debug("Skipping Read as object type is %s", oclass[0])
                    self.cont_number += 1
                    continue

                self.ior_read_single_dataset(oclass, sizes, storage, operation, percent)
                self.cont_number += 1

    def check_aggregation_status(self, quick_check=True, attempt=20):
        """EC Aggregation triggered status.

        Args:
            quick_check (bool): Return immediately when Aggregation starts for any storage type.
            attempt (int): Number of attempts to do pool query at interval of 5 seconds.
                        default is 20 attempts.

        Returns:
            dict: Storage Aggregation stats SCM/NVMe True/False.
        """
        return check_aggregation_status(self.log, self.pool, quick_check, attempt)


class ErasureCodeSingle(TestWithServers):
    """Class to used for EC testing for single type data."""

    def __init__(self, *args, **kwargs):
        """Initialize a TestWithServers object."""
        super().__init__(*args, **kwargs)
        self.server_count = None
        self.set_online_rebuild = False
        self.rank_to_kill = None
        self.daos_cmd = None
        self.container = []

    def setUp(self):
        """Set up each test case."""
        # Start the servers and agents
        super().setUp()
        engine_count = self.server_managers[0].get_config_value(
            "engines_per_host")
        self.server_count = len(self.hostlist_servers) * engine_count
        self.obj_class = self.params.get("dfs_oclass_list", '/run/objectclass/*')
        self.singledata_set = self.params.get("single_data_set", '/run/container/*')
        self.add_pool()
        self.out_queue = queue.Queue()

    def ec_container_create(self, oclass):
        """Create the container for EC object.

        Args:
            oclass (str): object class for creating the container.
        """
        self.container.append(self.get_container(self.pool, create=False, oclass=oclass))
        if self.container[-1].control_method.value == \
                self.container[-1].USE_DAOS and self.container[-1].oclass.value:
            self.container[-1].oclass.update(self.container[-1].oclass.value.replace("OC_", ""),
                                             "container.oclass")

        # Get the Parity count for setting the container RF property.
        ec_object = get_data_parity_number(self.log, oclass)
        self.container[-1].properties.update("rd_fac:{}".format(ec_object['parity']))

        # create container
        self.container[-1].create()

    @staticmethod
    def single_type_param_update(container, data):
        """Update the data set content provided from yaml file.

        Args:
            container (TestContainer): container object
            data (list): dataset content from test yaml file.
        """
        container.object_qty.update(data[0])
        container.record_qty.update(data[1])
        container.dkey_size.update(data[2])
        container.akey_size.update(data[3])
        container.data_size.update(data[4])

    def write_single_type_dataset(self, results=None):
        """Write single type data set with different EC object and different sizes.

        Args:
            results (queue): queue for returning thread results
        """
        for oclass in self.obj_class:
            for sizes in self.singledata_set:
                # Skip the object type if server count does not meet the minimum EC object server
                # count
                if oclass[1] > self.server_count:
                    continue
                # Create the new container with correct redundancy factor for EC object type
                try:
                    self.ec_container_create(oclass[0])
                    self.single_type_param_update(self.container[-1], sizes)
                    # Write the data
                    self.container[-1].write_objects(obj_class=oclass[0])
                    if results is not None:
                        results.put("PASS")
                except (CommandFailure, DaosApiError, DaosTestError):
                    if results is not None:
                        results.put("FAIL")
                    raise

    def read_single_type_dataset(self, results=None, parity=1):
        """Read single type data and verify for different EC object and different sizes.

        Args:
            results (queue): queue for returning thread results
            parity (int): object parity number for reading, default All.
        """
        cont_count = 0
        self.daos_cmd = DaosCommand(self.bin)
        for oclass in self.obj_class:
            for _sizes in self.singledata_set:
                # Skip the object type if server count does not meet the minimum EC object server
                # count
                if oclass[1] > self.server_count:
                    continue
                parity_set = "P{}".format(parity)
                # Read the requested data+parity data set only
                if parity != 1 and parity_set not in oclass[0]:
                    self.log.debug("Skipping Read as object type is %s", oclass[0])
                    cont_count += 1
                    continue

                self.daos_cmd.container_set_prop(pool=self.pool.uuid,
                                                 cont=self.container[cont_count].uuid,
                                                 prop="status",
                                                 value="healthy")

                # Read data and verified the content
                try:
                    if not self.container[cont_count].read_objects():
                        if results is not None:
                            results.put("FAIL")
                        self.fail("Data verification Error")
                    cont_count += 1
                    if results is not None:
                        results.put("PASS")
                except (CommandFailure, DaosApiError, DaosTestError):
                    if results is not None:
                        results.put("FAIL")
                    raise

    def start_online_single_operation(self, operation, parity=1):
        """Do Write/Read operation with single data type.

        Raises:
            ValueError: if operation is invalid

        Args:
            operation (str): WRITE or READ operation
        """
        # Create the single data Write/Read threads
        if operation == 'WRITE':
            job = threading.Thread(target=self.write_single_type_dataset,
                                   kwargs={"results": self.out_queue})
        elif operation == 'READ':
            job = threading.Thread(target=self.read_single_type_dataset,
                                   kwargs={"results": self.out_queue,
                                           "parity": parity})
        else:
            raise ValueError(f'Invalid operation: {operation}')

        # Launch the single data write/read thread
        job.start()

        # Kill the server rank while IO operation in progress
        if self.set_online_rebuild:
            time.sleep(10)
            # Kill the server rank
            if self.rank_to_kill is not None:
                self.server_managers[0].stop_ranks([self.rank_to_kill], force=True)

        # Wait to finish the thread
        job.join()

        # Verify the queue and make sure no FAIL for any run
        while not self.out_queue.empty():
            if self.out_queue.get() == "FAIL":
                self.fail("FAIL")


class ErasureCodeMdtest(MdtestBase):
    """Class to used for EC testing for MDtest Benchmark."""

    def setUp(self):
        """Set up each test case."""
        super().setUp()
        # Create Pool
        self.add_pool()
        self.container = None
        self.out_queue = queue.Queue()

    def _start_execute_mdtest(self, mdtest_result_queue):
        """Run the execute_mdtest method

        Args:
            mdtest_result_queue(Queue) : Queue for passing errors.
        Returns:
            result(object) : mdtest run result
        """
        try:
            result = self.execute_mdtest(mdtest_result_queue)
        except Exception:  # pylint: disable=broad-except
            mdtest_result_queue.put('Mdtest Failed')
        return result

    def start_online_mdtest(self, ranks_to_stop):
        """Run mdtest and stop ranks while mdtest is running.

        Args:
            ranks_to_stop (list): ranks to stop while mdtest is running
        """
        # Create the container and check the status
        self.container = self.get_mdtest_container(self.pool)
        # Create the MDtest run thread
        job = threading.Thread(
            target=self._start_execute_mdtest,
            kwargs={"mdtest_result_queue": self.out_queue})

        # Launch the MDtest thread
        job.start()

        # Stop the server ranks while IO operation in progress
        time.sleep(self.mdtest_cmd.stonewall_timer.value / 2)
        self.server_managers[0].stop_ranks(ranks_to_stop, force=True)

        # Wait to finish the thread
        job.join()

        # Verify the queue result and make sure test has no failure
        while not self.out_queue.empty():
            result = self.out_queue.get()
            if result == "Mdtest Failed":
                self.fail(result)
