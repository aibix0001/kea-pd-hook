#!/usr/bin/env python3
"""
Test script to connect to VyOS router and check for route6 configuration
"""

from netmiko import ConnectHandler
import logging
import time

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def test_router_connection():
    """Test connection to VyOS router and check route6 configuration"""

    router_ip = "10.20.30.14"
    username = "vyos"

    # Create Netmiko connection (using SSH keys)
    vyos_device = {
        "device_type": "vyos",
        "host": router_ip,
        "username": username,
        "use_keys": True,
        "key_file": "~/.ssh/id_ed25519",
        "timeout": 10,
    }

    try:
        # Connect to router
        logger.info(f"Connecting to {router_ip}...")
        net_connect = ConnectHandler(**vyos_device)

        # Test basic connectivity
        logger.info("Testing basic connectivity...")
        version_output = net_connect.send_command("show version | head -1")
        logger.info(f"Version: {version_output}")

        # Check for route6 configuration
        logger.info("Checking for route6 configuration...")
        config_output = net_connect.send_command("show configuration commands | match 'route6'")
        logger.info("Route6 configuration check results:")
        print("Output:")
        print(config_output)

        # Check for specific route
        prefix = "2001:470:731b:5900::/56"
        logger.info(f"Checking for specific route: {prefix}")
        route_output = net_connect.send_command(f"show configuration commands | match 'route6 {prefix}'")
        logger.info(f"Specific route check for {prefix}:")
        print("Output:")
        print(route_output)

        net_connect.disconnect()

    except Exception as e:
        logger.error(f"Connection failed: {str(e)}")

if __name__ == "__main__":
    test_router_connection()