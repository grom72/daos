"""
  (C) Copyright 2018-2024 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import os
import shutil
import site

from ClusterShell.NodeSet import NodeSet
# pylint: disable=import-error,no-name-in-module
from util.host_utils import get_local_host
from util.network_utils import (PROVIDER_ALIAS, SUPPORTED_PROVIDERS, NetworkException,
                                get_common_provider, get_fastest_interface)
from util.run_utils import run_remote


class TestEnvironmentException(Exception):
    """Exception for launch.py execution."""


def _update_path(daos_prefix):
    """Update the PATH environment variable for functional testing.

    Args:
        daos_prefix (str): daos install prefix

    """
    parts = os.environ.get("PATH").split(":")

    # Insert bin and sbin at the beginning of PATH if prefix is not /usr
    if daos_prefix != os.path.join(os.sep, "usr"):
        bin_dir = os.path.join(daos_prefix, "bin")
        sbin_dir = os.path.join(daos_prefix, "sbin")
        parts.insert(0, bin_dir)
        parts.insert(0, sbin_dir)

    # /usr/sbin is not setup on non-root user for CI nodes.
    # SCM formatting tool mkfs.ext4 is located under /usr/sbin directory.
    usr_sbin = os.path.join(os.sep, "usr", "sbin")
    if usr_sbin not in parts:
        parts.append(usr_sbin)

    os.environ["PATH"] = ":".join(parts)


def set_python_environment(logger):
    """Set up the test python environment.

    Args:
        logger (Logger): logger for the messages produced by this method
    """
    logger.debug("-" * 80)
    required_python_paths = [
        os.path.abspath("util/apricot"),
        os.path.abspath("util"),
        os.path.abspath("cart"),
        os.path.abspath("."),
    ]

    # Include the cart directory paths when running from sources
    cart_utils_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cart", "util")
    required_python_paths.append(cart_utils_dir)

    required_python_paths.extend(site.getsitepackages())

    # Check the PYTHONPATH env definition
    python_path = os.environ.get("PYTHONPATH")
    if python_path is None or python_path == "":
        # Use the required paths to define the PYTHONPATH env if it is not set
        os.environ["PYTHONPATH"] = ":".join(required_python_paths)
    else:
        # Append any missing required paths to the existing PYTHONPATH env
        defined_python_paths = [
            os.path.abspath(os.path.expanduser(path))
            for path in python_path.split(":")]
        for required_path in required_python_paths:
            if required_path not in defined_python_paths:
                python_path += ":" + required_path
        os.environ["PYTHONPATH"] = python_path
    logger.debug("Testing with PYTHONPATH=%s", os.environ["PYTHONPATH"])


def log_environment(logger):
    """Log the current environment variable assignments.

    Args:
        logger (Logger): logger for the messages produced by this method
    """
    logger.debug("ENVIRONMENT VARIABLES")
    for key in sorted(os.environ):
        if not key.startswith("BASH_FUNC_"):
            logger.debug("  %s: %s", key, os.environ[key])


class TestEnvironment():
    """Collection of test environment variables."""

    __ENV_VAR_MAP = {
        'app_dir': 'DAOS_TEST_APP_DIR',
        'app_src': 'DAOS_TEST_APP_SRC',
        'log_dir': 'DAOS_TEST_LOG_DIR',
        'shared_dir': 'DAOS_TEST_SHARED_DIR',
        'user_dir': 'DAOS_TEST_USER_DIR',
        'interface': 'DAOS_TEST_FABRIC_IFACE',
        'provider': 'D_PROVIDER',
        'insecure_mode': 'DAOS_TEST_INSECURE_MODE',
        'bullseye_src': 'DAOS_TEST_BULLSEYE_SRC',
        'bullseye_file': 'COVFILE',
        'daos_prefix': 'DAOS_TEST_PREFIX',
        'agent_user': 'DAOS_TEST_AGENT_USER',
        'systemd_library_path': 'DAOS_TEST_SYSTEMD_LIBRARY_PATH',
        'control_config': 'DAOS_TEST_CONTROL_CONFIG',
        'agent_config': 'DAOS_TEST_AGENT_CONFIG',
        'server_config': 'DAOS_TEST_SERVER_CONFIG',
    }

    def __init__(self):
        """Initialize a TestEnvironment object with existing or default test environment values."""
        self.set_defaults(None)

    def set_defaults(self, logger, servers=None, clients=None, provider=None, insecure_mode=None,
                     agent_user=None, log_dir=None, systemd_lib_path=None):
        """Set the default test environment variable values with optional inputs.

        Args:
            logger (Logger): logger for the messages produced by this method
            servers (NodeSet, optional): hosts designated for the server role in testing. Defaults
                to None.
            clients (NodeSet, optional): hosts designated for the client role in testing. Defaults
                to None.
            provider (str, optional): provider to use in testing. Defaults to None.
            insecure_mode (bool, optional): whether or not to run tests in insecure mode. Defaults
                to None.
            agent_user (str, optional): user account to use when running the daos_agent. Defaults
                to None.
            log_dir (str, optional): test log directory base path. Defaults to None.
            systemd_lib_path (str, optional): systemd library path. Defaults to None.

        Raises:
            TestEnvironmentException: if there are any issues setting environment variable default
                values
        """
        all_hosts = NodeSet()
        all_hosts.update(servers)
        all_hosts.update(clients)

        # Override values if explicitly specified
        if log_dir is not None:
            self.log_dir = log_dir
        if provider is not None:
            self.provider = provider
        if insecure_mode is not None:
            self.insecure_mode = insecure_mode
        if agent_user is not None:
            self.agent_user = agent_user
        if systemd_lib_path is not None:
            self.systemd_library_path = systemd_lib_path

        # Set defaults for any unset values
        if self.log_dir is None:
            self.log_dir = os.path.join(os.sep, "var", "tmp", "daos_testing")
        if self.shared_dir is None:
            self.shared_dir = os.path.expanduser(os.path.join("~", "daos_test"))
        if self.app_dir is None:
            self.app_dir = os.path.join(self.shared_dir, "daos_test", "apps")
        if self.user_dir is None:
            self.user_dir = os.path.join(self.log_dir, "user")
        if self.interface is None:
            self.interface = self._default_interface(logger, all_hosts)
        if self.provider is None:
            self.provider = self._default_provider(logger, servers)
        if self.insecure_mode is None:
            self.insecure_mode = "True"
        if self.bullseye_src is None:
            self.bullseye_src = os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "..", "test.cov")
        if self.bullseye_file is None:
            self.bullseye_file = os.path.join(os.sep, "tmp", "test.cov")
        if self.daos_prefix is None:
            self.daos_prefix = self._default_daos_prefix(logger)
        if self.agent_user is None:
            self.agent_user = 'root'
        if self.control_config is None:
            self.control_config = os.path.join(self.log_dir, "configs", "daos_control.yml")
        if self.agent_config is None:
            self.agent_config = os.path.join(self.log_dir, "configs", "daos_agent.yml")
        if self.server_config is None:
            self.server_config = os.path.join(self.log_dir, "configs", "daos_server.yml")

    def __set_value(self, key, value):
        """Set the test environment variable.

        Args:
            key (str): test environment variable name
            value (str): value to set; None to clear
        """
        if value is not None:
            os.environ[self.__ENV_VAR_MAP[key]] = str(value)
        elif os.environ.get(self.__ENV_VAR_MAP[key]):
            os.environ.pop(self.__ENV_VAR_MAP[key])

    @property
    def app_dir(self):
        """Get the application directory path.

        Returns:
            str: the application directory path
        """
        return os.environ.get(self.__ENV_VAR_MAP['app_dir'])

    @app_dir.setter
    def app_dir(self, value):
        """Set the application directory path.

        Args:
            value (str): the application directory path
        """
        self.__set_value('app_dir', value)

    @property
    def app_src(self):
        """Get the location from which to copy test applications.

        Returns:
            str: the location from which to copy test applications
        """
        return os.environ.get(self.__ENV_VAR_MAP['app_src'])

    @app_src.setter
    def app_src(self, value):
        """Set the location from which to copy test applications.

        Args:
            value (str): the location from which to copy test applications
        """
        self.__set_value('app_src', value)

    @property
    def log_dir(self):
        """Get the local log directory path.

        Returns:
            str: the local log directory path
        """
        return os.environ.get(self.__ENV_VAR_MAP['log_dir'])

    @log_dir.setter
    def log_dir(self, value):
        """Set the local log directory path.

        Args:
            value (str): the local log directory path
        """
        self.__set_value('log_dir', value)

    @property
    def shared_dir(self):
        """Get the shared log directory path.

        Returns:
            str: the shared log directory path
        """
        return os.environ.get(self.__ENV_VAR_MAP['shared_dir'])

    @shared_dir.setter
    def shared_dir(self, value):
        """Set the shared log directory path.

        Args:
            value (str): the shared log directory path
        """
        self.__set_value('shared_dir', value)

    @property
    def user_dir(self):
        """Get the user directory path.

        Returns:
            str: the user directory path
        """
        return os.environ.get(self.__ENV_VAR_MAP['user_dir'])

    @user_dir.setter
    def user_dir(self, value):
        """Set the user directory path.

        Args:
            value (str): the user directory path
        """
        self.__set_value('user_dir', value)

    @property
    def interface(self):
        """Get the interface device.

        Returns:
            str: the interface device
        """
        return os.environ.get(self.__ENV_VAR_MAP['interface'])

    @interface.setter
    def interface(self, value):
        """Set the interface device.

        Args:
            value (str): the interface device
        """
        self.__set_value('interface', value)

    def _default_interface(self, logger, hosts):
        """Get the default interface.

        Args:
            logger (Logger): logger for the messages produced by this method
            hosts (NodeSet): hosts on which to find a homogeneous interface

        Raises:
            TestEnvironmentException: if there is a problem finding active network interfaces when
                hosts are provided

        Returns:
            str: the default interface; can be None
        """
        interface = os.environ.get("D_INTERFACE")
        if interface is None and hosts:
            # Find all the /sys/class/net interfaces on the launch node (excluding lo)
            logger.debug("Detecting network devices - D_INTERFACE not set")
            try:
                interface = get_fastest_interface(logger, hosts | get_local_host())
            except NetworkException as error:
                raise TestEnvironmentException("Error obtaining a default interface!") from error
        return interface

    @property
    def provider(self):
        """Get the provider.

        Returns:
            str: the provider
        """
        return os.environ.get(self.__ENV_VAR_MAP['provider'])

    @provider.setter
    def provider(self, value):
        """Set the provider.

        Args:
            value (str): the provider or provider alias
        """
        alias = PROVIDER_ALIAS.get(value, None)
        if alias is not None:
            self.__set_value('provider', alias)
        else:
            self.__set_value('provider', value)

    def _default_provider(self, logger, hosts):
        """Get the default provider.

        Args:
            logger (Logger): logger for the messages produced by this method
            hosts (NodeSet): hosts on which to find a homogeneous provider

        Raises:
            TestEnvironmentException: if there is an error obtaining the default provider when hosts
                are provided

        Returns:
            str: the default provider; can be None
        """
        if not hosts:
            return None

        logger.debug(
            "Detecting provider for %s - %s not set",
            self.interface, self.__ENV_VAR_MAP['provider'])
        provider = None
        supported = list(SUPPORTED_PROVIDERS)

        # Check for a Omni-Path interface
        logger.debug("Checking for Omni-Path devices")
        command = "which opainfo && sudo -n opainfo"
        result = run_remote(logger, hosts, command)
        if result.passed:
            # Omni-Path adapter found; remove verbs as it will not work with OPA devices.
            logger.debug("  Excluding verbs provider for Omni-Path adapters")
            supported = list(filter(lambda x: 'verbs' not in x, supported))

        # Detect all supported providers for this interface that are common to all of the hosts
        common_providers = get_common_provider(logger, hosts, self.interface, supported)
        if common_providers:
            # Select the preferred found provider based upon SUPPORTED_PROVIDERS order
            logger.debug("Supported providers detected: %s", common_providers)
            for key in supported:
                if key in common_providers:
                    provider = key
                    break

        # Report an error if a provider cannot be found
        if not provider:
            raise TestEnvironmentException(
                f"Error obtaining a supported provider for {self.interface} from: {supported}")

        logger.debug("  Found %s provider for %s", provider, self.interface)
        return provider

    @property
    def insecure_mode(self):
        """Get the insecure_mode.

        Returns:
            str: the insecure_mode
        """
        return os.environ.get(self.__ENV_VAR_MAP['interface'])

    @insecure_mode.setter
    def insecure_mode(self, value):
        """Set the insecure_mode.

        Args:
            value (str, bool): the insecure mode
        """
        self.__set_value('insecure_mode', value)

    @property
    def bullseye_src(self):
        """Get the bullseye source file.

        Returns:
            str: the bullseye source file
        """
        return os.environ.get(self.__ENV_VAR_MAP['bullseye_src'])

    @bullseye_src.setter
    def bullseye_src(self, value):
        """Set the bullseye source file.

        Args:
            value (str, bool): the bullseye source file
        """
        self.__set_value('bullseye_src', value)

    @property
    def bullseye_file(self):
        """Get the bullseye file.

        Returns:
            str: the bullseye file
        """
        return os.environ.get(self.__ENV_VAR_MAP['bullseye_file'])

    @bullseye_file.setter
    def bullseye_file(self, value):
        """Set the bullseye file.

        Args:
            value (str, bool): the bullseye file
        """
        self.__set_value('bullseye_file', value)

    @property
    def daos_prefix(self):
        """Get the daos_prefix.

        Returns:
            str: the daos_prefix
        """
        return os.environ.get(self.__ENV_VAR_MAP['daos_prefix'])

    @daos_prefix.setter
    def daos_prefix(self, value):
        """Set the daos_prefix.

        Args:
            value (str, bool): the daos_prefix
        """
        self.__set_value('daos_prefix', value)

    def _default_daos_prefix(self, logger):
        """Get the default daos_prefix.

        Args:
            logger (Logger): logger for the messages produced by this method

        Raises:
            TestEnvironmentException: if there is an error obtaining the default daos_prefix

        Returns:
            str: the default daos_prefix
        """
        if logger is None:
            return None

        logger.debug(
            "Detecting daos_prefix for %s - %s not set",
            self.daos_prefix, self.__ENV_VAR_MAP['daos_prefix'])

        daos_bin_path = shutil.which('daos')
        if not daos_bin_path:
            raise TestEnvironmentException("Failed to find installed daos!")

        # E.g. /usr/bin/daos -> /usr
        return os.path.dirname(os.path.dirname(daos_bin_path))

    @property
    def agent_user(self):
        """Get the daos_agent user.

        Returns:
            str: the daos_agent user
        """
        return os.environ.get(self.__ENV_VAR_MAP['agent_user'])

    @agent_user.setter
    def agent_user(self, value):
        """Set the daos_agent user.

        Args:
            value (str): the daos_agent user
        """
        self.__set_value('agent_user', value)

    @property
    def systemd_library_path(self):
        """Get the systemd LD_LIBRARY_PATH.

        Returns:
            str: the systemd LD_LIBRARY_PATH
        """
        return os.environ.get(self.__ENV_VAR_MAP['systemd_library_path'])

    @systemd_library_path.setter
    def systemd_library_path(self, value):
        """Set the systemd LD_LIBRARY_PATH.

        Args:
            value (str): the systemd LD_LIBRARY_PATH
        """
        self.__set_value('systemd_library_path', value)

    @property
    def control_config(self):
        """Get the control config file used in testing.

        Returns:
            str: the control config file
        """
        return os.environ.get(self.__ENV_VAR_MAP['control_config'])

    @control_config.setter
    def control_config(self, value):
        """Set the control config file used in testing.

        Args:
            value (str): the control config file
        """
        self.__set_value('control_config', value)

    @property
    def agent_config(self):
        """Get the agent config file used in testing.

        Returns:
            str: the agent config file
        """
        return os.environ.get(self.__ENV_VAR_MAP['agent_config'])

    @agent_config.setter
    def agent_config(self, value):
        """Set the agent config file used in testing.

        Args:
            value (str): the agent config file
        """
        self.__set_value('agent_config', value)

    @property
    def server_config(self):
        """Get the server config file used in testing.

        Returns:
            str: the server config file
        """
        return os.environ.get(self.__ENV_VAR_MAP['server_config'])

    @server_config.setter
    def server_config(self, value):
        """Set the server config file used in testing.

        Args:
            value (str): the server config file
        """
        self.__set_value('server_config', value)

    def config_file_directories(self):
        """Get the unique list of directories for the client, control, and server config files.

        Returns:
            list: a list of directories for the client, control, and server config files
        """
        directories = set()
        directories.add(os.path.dirname(self.agent_config))
        directories.add(os.path.dirname(self.control_config))
        directories.add(os.path.dirname(self.server_config))
        return list(directories)


def set_test_environment(logger, test_env=None, servers=None, clients=None, provider=None,
                         insecure_mode=False, details=None, agent_user=None, log_dir=None,
                         systemd_lib_path=None):
    """Set up the test environment.

    Args:
        logger (Logger): logger for the messages produced by this method
        test_env (TestEnvironment, optional): the current test environment. Defaults to None.
        servers (NodeSet, optional): hosts designated for the server role in testing. Defaults to
            None.
        clients (NodeSet, optional): hosts designated for the client role in testing. Defaults to
            None.
        provider (str, optional): provider to use in testing. Defaults to None.
        insecure_mode (bool, optional): whether or not to run tests in insecure mode. Defaults to
            False.
        details (dict, optional): dictionary to update with interface and provider settings if
            provided. Defaults to None.
        agent_user (str, optional): user account to use when running the daos_agent. Defaults to
            None.
        log_dir (str, optional): test log directory base path. Defaults to None.
        systemd_lib_path (str, optional): systemd library path. Defaults to None.

    Raises:
        TestEnvironmentException: if there is a problem setting up the test environment

    """
    logger.debug("-" * 80)
    logger.debug("Setting up the test environment variables")

    if test_env:
        # Get the default fabric interface, provider, and daos prefix
        test_env.set_defaults(
            logger, servers, clients, provider, insecure_mode, agent_user, log_dir,
            systemd_lib_path)
        logger.info("Testing with interface:   %s", test_env.interface)
        logger.info("Testing with provider:    %s", test_env.provider)
        logger.info("Testing with daos_prefix: %s", test_env.daos_prefix)
        logger.info("Testing with agent_user:  %s", test_env.agent_user)

        # Update the PATH environment variable
        _update_path(test_env.daos_prefix)

        if details:
            details["interface"] = test_env.interface
            details["provider"] = test_env.provider

        # Assign additional DAOS environment variables used in functional testing
        os.environ["D_LOG_FILE"] = os.path.join(test_env.log_dir, "daos.log")
        os.environ["D_LOG_FILE_APPEND_PID"] = "1"
        os.environ["D_LOG_FILE_APPEND_RANK"] = "1"

        # Default agent socket dir to be accessible by agent user
        if os.environ.get("DAOS_AGENT_DRPC_DIR") is None and test_env.agent_user != 'root':
            os.environ["DAOS_AGENT_DRPC_DIR"] = os.path.join(test_env.log_dir, "daos_agent")

    # Python paths required for functional testing
    set_python_environment(logger)

    # Log the environment variable assignments
    log_environment(logger)
