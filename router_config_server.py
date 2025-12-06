#!/usr/bin/env python3
"""
Flask server to handle VyOS router configuration from NetBox webhook data
Receives prefix data from NetBox and configures VyOS routers via SSH
"""

from flask import Flask, request, jsonify
import logging
import time
import paramiko
import requests
import json
from datetime import datetime
from typing import Dict, Any, Optional

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# In-memory storage for configuration history
configuration_history = []

# NetBox GraphQL configuration
NETBOX_GRAPHQL_URL = "https://netbox.lab.aibix.io/graphql/"
NETBOX_TOKEN = "584149a859ea8e7a7f5e2d610c7235a3e2d2460c"  # For testing


def find_interface_by_ipv6_address(router_link_addr: str, netbox_url: str, netbox_token: str) -> dict:
    """
    Find interface name by IPv6 link address using NetBox REST API
    
    Args:
        router_link_addr: IPv6 link address to search for (from dhcpv6_router_link_addr custom field)
        netbox_url: NetBox base URL (e.g., https://netbox.example.com)
        netbox_token: NetBox API token
    
    Returns:
        {
            'success': bool,
            'interface_name': str or None,
            'device_id': str or None,
            'error': str or None
        }
    """
    # Query interfaces by IP address
    interfaces_url = f"{netbox_url.rstrip('/')}/api/ipam/ip-addresses/"
    interfaces_params = {
        "address": router_link_addr
    }
    
    headers = {
        "Authorization": f"Token {netbox_token}",
        "Content-Type": "application/json",
        "Accept": "application/json"
    }
    
    try:
        # Find IP addresses matching the router link address
        response = requests.get(interfaces_url, params=interfaces_params, headers=headers, timeout=30, verify=False)
        response.raise_for_status()
        
        ip_data = response.json()
        
        if not ip_data.get("results") or len(ip_data["results"]) == 0:
            return {
                'success': False,
                'error': f'No IP address found for router link address {router_link_addr}'
            }
        
        # Get the first matching IP address
        ip_info = ip_data["results"][0]
        interface_info = ip_info.get("assigned_object_id")
        
        if not interface_info:
            return {
                'success': False,
                'error': f'IP address {router_link_addr} is not assigned to an interface'
            }
        
        # Get interface details
        interface_id = interface_info
        interface_detail_url = f"{netbox_url.rstrip('/')}/api/dcim/interfaces/{interface_id}/"
        interface_response = requests.get(interface_detail_url, headers=headers, timeout=30, verify=False)
        interface_response.raise_for_status()
        
        interface_detail = interface_response.json()
        interface_name = interface_detail.get("name")
        device_id = interface_detail.get("device", {}).get("id")
        
        if not interface_name:
            return {
                'success': False,
                'error': f'Could not get interface name for interface ID {interface_id}'
            }
        
        logger.info(f"Found interface: {interface_name} (ID: {interface_id}) on device {device_id}")
        
        return {
            'success': True,
            'interface_name': interface_name,
            'device_id': str(device_id) if device_id else None
        }
        
    except requests.exceptions.RequestException as e:
        return {
            'success': False,
            'error': f'NetBox API request failed: {str(e)}'
        }
    except Exception as e:
        return {
            'success': False,
            'error': f'Unexpected error: {str(e)}'
        }


def find_router_management_ip(router_ipv6_address: str, netbox_url: str, netbox_token: str) -> dict:
    """
    Find router management IP using NetBox REST API
    
    Args:
        router_ipv6_address: IPv6 address to search for (from dhcpv6_router_ip custom field)
        netbox_url: NetBox base URL (e.g., https://netbox.example.com)
        netbox_token: NetBox API token
    
    Returns:
        {
            'success': bool,
            'management_ip': str or None,
            'device_id': str or None,
            'error': str or None
        }
    """
    # Step 1: Find device by IPv6 address
    devices_url = f"{netbox_url.rstrip('/')}/api/dcim/devices/"
    devices_params = {
        "interface_ip_address": router_ipv6_address
    }
    
    headers = {
        "Authorization": f"Token {netbox_token}",
        "Content-Type": "application/json",
        "Accept": "application/json"
    }
    
    try:
        # Find devices with the IPv6 address
        response = requests.get(devices_url, params=devices_params, headers=headers, timeout=30, verify=False)
        response.raise_for_status()
        
        devices_data = response.json()
        
        if not devices_data.get("results") or len(devices_data["results"]) == 0:
            return {
                'success': False,
                'error': f'No device found with IPv6 address {router_ipv6_address}'
            }
        
        # Look for device with name containing "p1r4v" (device 4)
        target_device = None
        for device in devices_data["results"]:
            if "p1r4v" in device.get("name", "").lower():
                target_device = device
                break
        
        # If not found, use first device (fallback)
        if not target_device:
            target_device = devices_data["results"][0]
        
        device_id = str(target_device["id"])
        device_name = target_device.get("name", "Unknown")
        
        logger.info(f"Found device: {device_name} (ID: {device_id})")
        logger.info(f"Target device details: {target_device}")
        
        # Step 2: Get device details to find interface name for routing
        device_detail_url = f"{netbox_url.rstrip('/')}/api/dcim/devices/{device_id}/"
        detail_response = requests.get(device_detail_url, headers=headers, timeout=30, verify=False)
        detail_response.raise_for_status()
        
        device_detail = detail_response.json()
        
        # Extract primary IP (this should be the management IP)
        primary_ip = device_detail.get("primary_ip")
        
        if not primary_ip:
            return {
                'success': False,
                'error': f'No primary IP found for device {device_name} (ID: {device_id})'
            }
        
        management_ip = primary_ip.get("address") if isinstance(primary_ip, dict) else str(primary_ip)
        
        logger.info(f"Found management IP: {management_ip} for device {device_name}")
        
        return {
            'success': True,
            'management_ip': management_ip,
            'device_id': device_id,
            'device_name': device_name
        }
        
    except requests.exceptions.RequestException as e:
        return {
            'success': False,
            'error': f'NetBox API request failed: {str(e)}'
        }
    except Exception as e:
        return {
            'success': False,
            'error': f'Unexpected error: {str(e)}'
        }
    """
    headers = {
        "Authorization": f"Token {netbox_token}",
        "Content-Type": "application/json",
        "Accept": "application/json"
    }
        }
      }
    }
    """
    
    variables = {"routerIpv6": router_ipv6_address}
    
    headers = {
        "Authorization": f"Token {netbox_token}",
        "Content-Type": "application/json"
    }
    
    payload = {
        "query": query,
        "variables": variables
    }
    
    # Debug: print the exact query being sent
    logger.info(f"Sending GraphQL query: {query.strip()}")
    
    try:
        response = requests.post(netbox_url, json=payload, headers=headers, timeout=30, verify=False)
        response.raise_for_status()
        
        data = response.json()
        
        # Debug: log the raw response
        logger.info(f"Raw GraphQL response: {response.text}")
        
        # Parse response for management IP
        devices = data.get("data", {}).get("device_list", [])
        
        if not devices:
            return {
                'success': False,
                'error': f'No device found with IPv6 address {router_ipv6_address}'
            }
        
        device = devices[0]  # Take first device
        mgmt_interfaces = device.get("interfaces", [])
        
        if not mgmt_interfaces:
            return {
                'success': False,
                'error': f'No MGMT VRF interfaces found for device {device["id"]}'
            }
        
        mgmt_interface = mgmt_interfaces[0]  # Take first MGMT interface
        ip_addresses = mgmt_interface.get("ip_addresses", [])
        
        # Filter for IPv4 addresses only
        ipv4_addresses = [ip for ip in ip_addresses if ip.get("family") == 4]
        
        if not ipv4_addresses:
            return {
                'success': False,
                'error': f'No IPv4 addresses found on MGMT interface for device {device["id"]}'
            }
        
        # Return first IPv4 address
        management_ip = ipv4_addresses[0]["address"]
        
        return {
            'success': True,
            'management_ip': management_ip,
            'device_id': device["id"]
        }
        
    except requests.exceptions.RequestException as e:
        return {
            'success': False,
            'error': f'GraphQL request failed: {str(e)}'
        }
    except Exception as e:
        return {
            'success': False,
            'error': f'Unexpected error: {str(e)}'
        }


class VyOSRouter:
    """VyOS router configuration handler"""
    
    def __init__(self, router_ip: str, username: str = "vyos", timeout: int = 10):
        self.router_ip = router_ip
        self.username = username
        self.timeout = timeout
        self.ssh = None
    
    def connect(self) -> bool:
        """Connect to VyOS router via SSH"""
        try:
            self.ssh = paramiko.SSHClient()
            self.ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            
            logger.info(f"Connecting to VyOS router at {self.router_ip}")
            self.ssh.connect(self.router_ip, username=self.username, timeout=self.timeout)
            
            # Check current mode and get basic info
            logger.info("Checking router status...")
            stdin, stdout, stderr = self.ssh.exec_command("show version")
            version_output = stdout.read().decode().strip()
            if version_output:
                logger.info(f"VyOS version: {version_output}")
            
            # Check if we're already in configuration mode
            stdin, stdout, stderr = self.ssh.exec_command("show configuration commands | head -3")
            config_sample = stdout.read().decode().strip()
            if config_sample and not config_sample.startswith("Invalid command"):
                logger.info(f"Router appears to be in configuration mode")
                logger.info(f"Sample config: {config_sample}")
            else:
                logger.info("Router is in operational mode")
            
            return True
            
        except Exception as e:
            logger.error(f"Failed to connect to {self.router_ip}: {str(e)}")
            return False
    
    def verify_route(self, prefix_cidr: str, cpe_link_local: str, interface_name: str) -> bool:
        """Verify that route was successfully configured"""
        if not self.ssh:
            logger.error("SSH connection not established for verification")
            return False
            
        try:
            # Show configuration commands to verify route exists
            verify_cmd = f"show configuration commands | match 'set protocols static route6 {prefix_cidr}'"
            logger.info(f"Verifying route with: {verify_cmd}")
            stdin, stdout, stderr = self.ssh.exec_command(verify_cmd)
            
            output = stdout.read().decode().strip()
            error_output = stderr.read().decode().strip()
            
            if error_output:
                logger.error(f"Verification command error: {error_output}")
                return False
            
            # Check if route configuration exists in output
            expected_route = f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local} interface {interface_name}"
            
            if expected_route in output:
                logger.info(f"✅ Route verification successful: {expected_route}")
                return True
            else:
                logger.error(f"❌ Route verification failed. Expected: {expected_route}")
                logger.error(f"❌ Actual output: {output}")
                return False
                
        except Exception as e:
            logger.error(f"Failed to verify route: {str(e)}")
            return False
            
            # Check if the route configuration exists in output
            expected_route = f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local} interface {interface_name}"
            
            if expected_route in output:
                logger.info(f"✅ Route verification successful: {expected_route}")
                return True
            else:
                logger.error(f"❌ Route verification failed. Expected: {expected_route}")
                logger.error(f"❌ Actual output: {output}")
                return False
                
        except Exception as e:
            logger.error(f"Failed to verify route: {str(e)}")
            return False

    def configure_route(self, prefix_cidr: str, cpe_link_local: str, interface_name: str) -> bool:
        """Configure static route on VyOS router"""
        if not self.ssh:
            logger.error("SSH connection not established")
            return False
        
        try:
            # Check current mode first
            logger.info("Checking current router mode...")
            stdin, stdout, stderr = self.ssh.exec_command("show configuration commands | head -1")
            mode_output = stdout.read().decode().strip()
            mode_error = stderr.read().decode().strip()
            
            if mode_error or "Invalid command" in mode_output:
                # We're in operational mode, need to enter configure
                logger.info("Router is in operational mode, entering configuration mode")
                stdin, stdout, stderr = self.ssh.exec_command("configure")
                time.sleep(2)
                
                # Check if we successfully entered configuration mode
                config_output = stdout.read().decode().strip()
                config_error = stderr.read().decode().strip()
                logger.info(f"Configure mode output: {config_output}")
                if config_error:
                    logger.error(f"Configure mode error: {config_error}")
                    return False
            else:
                logger.info("Router is already in configuration mode")
                logger.info(f"Current mode output: {mode_output}")
            
            # Add the route - try different syntax variations
            route_cmd = f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local} interface {interface_name}"
            logger.info(f"Executing: {route_cmd}")
            stdin, stdout, stderr = self.ssh.exec_command(route_cmd)
            time.sleep(1)
            
            # Check for route command errors
            route_output = stdout.read().decode().strip()
            route_error = stderr.read().decode().strip()
            
            logger.info(f"Route command output: {route_output}")
            if route_error:
                logger.error(f"Route command error: {route_error}")
                
                # Try alternative syntax without interface first
                logger.info("Trying alternative route syntax...")
                alt_route_cmd = f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local}"
                logger.info(f"Executing alternative: {alt_route_cmd}")
                stdin, stdout, stderr = self.ssh.exec_command(alt_route_cmd)
                time.sleep(1)
                
                alt_output = stdout.read().decode().strip()
                alt_error = stderr.read().decode().strip()
                
                if alt_error:
                    logger.error(f"Alternative route command also failed: {alt_error}")
                    return False
                else:
                    logger.info("Alternative route command succeeded")
                    route_output = alt_output
            
            # Show current configuration changes before commit
            logger.info("Showing configuration changes...")
            stdin, stdout, stderr = self.ssh.exec_command("show configuration commands")
            show_output = stdout.read().decode().strip()
            show_error = stderr.read().decode().strip()
            
            if show_error:
                logger.error(f"Show config error: {show_error}")
            else:
                logger.info(f"Current config changes:\n{show_output}")
            
            # Commit and save
            logger.info("Committing configuration")
            stdin, stdout, stderr = self.ssh.exec_command("commit")
            time.sleep(3)
            
            # Check commit output
            commit_output = stdout.read().decode().strip()
            commit_error = stderr.read().decode().strip()
            
            logger.info(f"Commit output: {commit_output}")
            if commit_error:
                logger.error(f"Commit error: {commit_error}")
                logger.error("Attempting to discard changes and exit...")
                stdin, stdout, stderr = self.ssh.exec_command("exit discard")
                return False
            
            logger.info("Saving configuration")
            stdin, stdout, stderr = self.ssh.exec_command("save")
            time.sleep(1)
            
            # Exit configuration mode
            stdin, stdout, stderr = self.ssh.exec_command("exit")
            time.sleep(1)
            
            # Verify the route was actually configured
            logger.info("Verifying route configuration...")
            if self.verify_route(prefix_cidr, cpe_link_local, interface_name):
                logger.info(f"✅ Successfully configured and verified route for {prefix_cidr} via {cpe_link_local} interface {interface_name}")
                return True
            else:
                logger.error(f"❌ Route configuration verification failed for {prefix_cidr}")
                return False
            
        except Exception as e:
            logger.error(f"Failed to configure route: {str(e)}")
            return False
    
    def close(self):
        """Close SSH connection"""
        if self.ssh:
            self.ssh.close()
            self.ssh = None


def validate_webhook_data(data: Dict[str, Any]) -> tuple[bool, str]:
    """Validate incoming webhook data"""
    required_fields = ["prefix", "router_ipv6", "cpe_link_local", "router_link_addr", "leasetime"]
    
    for field in required_fields:
        if field not in data or not data[field]:
            return False, f"Missing required field: {field}"
    
    # Check if lease is still valid
    current_time = datetime.now().timestamp()
    if current_time >= data["leasetime"]:
        return False, f"Lease expired (now: {current_time} >= leasetime: {data['leasetime']})"
    
    return True, ""


@app.route('/configure-router', methods=['POST'])
def configure_router():
    """Handle router configuration webhook from NetBox"""
    try:
        data = request.get_json()
        
        if not data:
            return jsonify({"status": "error", "message": "No JSON data received"}), 400
        
        logger.info(f"Received webhook data: {data}")
        
        # Validate webhook data
        is_valid, error_msg = validate_webhook_data(data)
        if not is_valid:
            return jsonify({
                "status": "error",
                "message": error_msg
            }), 400
        
        # Extract data
        prefix_cidr = data["prefix"]
        router_ipv6 = data["router_ipv6"]  # IPv6 address for GraphQL lookup
        cpe_link_local = data["cpe_link_local"]
        router_link_addr = data["router_link_addr"]  # IPv6 link address for interface lookup
        leasetime = data["leasetime"]
        client_duid = data.get("client_duid", "")
        iaid = data.get("iaid", "")
        netbox_prefix_id = data.get("netbox_prefix_id", "")
        
        logger.info(f"Processing configuration request:")
        logger.info(f"  Prefix: {prefix_cidr}")
        logger.info(f"  Router IPv6: {router_ipv6}")
        logger.info(f"  CPE Link-Local: {cpe_link_local}")
        logger.info(f"  Router Link Addr: {router_link_addr}")
        
        # Find interface name using router link address
        interface_result = find_interface_by_ipv6_address(
            router_link_addr,
            NETBOX_GRAPHQL_URL.replace('/graphql', ''),  # Use base URL for REST API
            NETBOX_TOKEN
        )
        
        if not interface_result['success']:
            return jsonify({
                "status": "error",
                "message": interface_result['error'],
                "router_link_addr": router_link_addr
            }), 400
        
        interface_name = interface_result['interface_name']
        device_id = interface_result['device_id']
        
        logger.info(f"Found interface: {interface_name} on device {device_id}")
        
        # Find management IP using REST API
        mgmt_result = find_router_management_ip(
            router_ipv6, 
            NETBOX_GRAPHQL_URL.replace('/graphql', ''),  # Use base URL for REST API
            NETBOX_TOKEN
        )
        
        if not mgmt_result['success']:
            return jsonify({
                "status": "error",
                "message": mgmt_result['error'],
                "router_ipv6": router_ipv6
            }), 400
        
        management_ip = mgmt_result['management_ip']
        
        logger.info(f"Found management IP: {management_ip} for device {device_id}")
        
        # Configure router using management IP
        # Extract just the IP address part if it's in CIDR notation
        if '/' in management_ip:
            management_ip = management_ip.split('/')[0]
        
        router = VyOSRouter(management_ip)
        
        if not router.connect():
            return jsonify({
                "status": "error",
                "message": f"Failed to connect to router at management IP {management_ip}",
                "router_ipv6": router_ipv6,
                "management_ip": management_ip
            }), 500
        
        try:
            success = router.configure_route(prefix_cidr, cpe_link_local, interface_name)
            
            if success:
                # Record configuration in history
                config_record = {
                    "timestamp": datetime.now().isoformat(),
                    "prefix": prefix_cidr,
                    "router_ipv6": router_ipv6,
                    "router_link_addr": router_link_addr,
                    "management_ip": management_ip,
                    "device_id": device_id,
                    "interface_name": interface_name,
                    "cpe_link_local": cpe_link_local,
                    "client_duid": client_duid,
                    "iaid": iaid,
                    "netbox_prefix_id": netbox_prefix_id,
                    "status": "success"
                }
                configuration_history.append(config_record)
                
                return jsonify({
                    "status": "success",
                    "message": f"Successfully configured route for {prefix_cidr} via {cpe_link_local} interface {interface_name}",
                    "prefix": prefix_cidr,
                    "router_ipv6": router_ipv6,
                    "router_link_addr": router_link_addr,
                    "management_ip": management_ip,
                    "device_id": device_id,
                    "interface_name": interface_name,
                    "cpe_link_local": cpe_link_local,
                    "route_added": f"{prefix_cidr} via {cpe_link_local} interface {interface_name}",
                    "timestamp": datetime.now().isoformat()
                })
            else:
                return jsonify({
                    "status": "error",
                    "message": "Failed to configure route",
                    "router_ipv6": router_ipv6,
                    "router_link_addr": router_link_addr,
                    "management_ip": management_ip,
                    "interface_name": interface_name
                }), 500
                
        finally:
            router.close()
    
    except Exception as e:
        logger.error(f"Error processing webhook: {str(e)}")
        return jsonify({
            "status": "error",
            "message": f"Internal server error: {str(e)}"
        }), 500


@app.route('/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({
        "status": "healthy",
        "timestamp": datetime.now().isoformat(),
        "configurations_count": len(configuration_history)
    })


@app.route('/history', methods=['GET'])
def get_configuration_history():
    """Get configuration history"""
    return jsonify({
        "count": len(configuration_history),
        "configurations": configuration_history
    })


@app.route('/reset', methods=['POST'])
def reset_data():
    """Reset configuration history (for testing)"""
    global configuration_history
    configuration_history.clear()
    logger.info("Reset configuration history")
    return jsonify({'status': 'reset', 'message': 'Configuration history cleared'})


@app.route('/test-connection', methods=['POST'])
def test_router_connection():
    """Test connection to a router"""
    try:
        data = request.get_json()
        
        if not data or "router_ip" not in data:
            return jsonify({"status": "error", "message": "router_ip is required"}), 400
        
        router_ip = data["router_ip"]
        username = data.get("username", "vyos")
        
        router = VyOSRouter(router_ip, username)
        
        if router.connect():
            router.close()
            return jsonify({
                "status": "success",
                "message": f"Successfully connected to {router_ip}",
                "router_ip": router_ip
            })
        else:
            return jsonify({
                "status": "error",
                "message": f"Failed to connect to {router_ip}",
                "router_ip": router_ip
            }), 500
    
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": f"Connection test failed: {str(e)}"
        }), 500


@app.route('/verify-routes', methods=['POST'])
def verify_routes():
    """Verify existing routes on a router"""
    try:
        data = request.get_json()
        
        if not data or "router_ip" not in data:
            return jsonify({"status": "error", "message": "router_ip is required"}), 400
        
        router_ip = data["router_ip"]
        username = data.get("username", "vyos")
        prefix_cidr = data.get("prefix_cidr", "")
        
        router = VyOSRouter(router_ip, username)
        
        if not router.connect():
            return jsonify({
                "status": "error",
                "message": f"Failed to connect to router at {router_ip}",
                "router_ip": router_ip
            }), 500
        
        try:
            if not router.ssh:
                return jsonify({
                    "status": "error",
                    "message": "SSH connection not established",
                    "router_ip": router_ip
                }), 500
                
            if prefix_cidr:
                # Verify specific route
                verify_cmd = f"show configuration commands | match 'set protocols static route6 {prefix_cidr}'"
                logger.info(f"Verifying specific route with: {verify_cmd}")
                stdin, stdout, stderr = router.ssh.exec_command(verify_cmd)
                
                output = stdout.read().decode().strip()
                error_output = stderr.read().decode().strip()
                
                if error_output:
                    return jsonify({
                        "status": "error",
                        "message": f"Verification command error: {error_output}",
                        "router_ip": router_ip,
                        "prefix_cidr": prefix_cidr
                    }), 500
                
                return jsonify({
                    "status": "success",
                    "message": f"Route verification completed for {prefix_cidr}",
                    "router_ip": router_ip,
                    "prefix_cidr": prefix_cidr,
                    "route_config": output,
                    "route_exists": bool(output.strip())
                })
            else:
                # Show all static routes
                routes_cmd = "show configuration commands | match 'set protocols static route6'"
                logger.info(f"Getting all static routes with: {routes_cmd}")
                stdin, stdout, stderr = router.ssh.exec_command(routes_cmd)
                
                output = stdout.read().decode().strip()
                error_output = stderr.read().decode().strip()
                
                if error_output:
                    return jsonify({
                        "status": "error",
                        "message": f"Routes command error: {error_output}",
                        "router_ip": router_ip
                    }), 500
                
                routes = output.split('\n') if output.strip() else []
                
                return jsonify({
                    "status": "success",
                    "message": f"Retrieved {len(routes)} static routes",
                    "router_ip": router_ip,
                    "routes": routes,
                    "routes_count": len(routes)
                })
                
        finally:
            router.close()
    
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": f"Route verification failed: {str(e)}"
        }), 500


if __name__ == '__main__':
    print("Starting VyOS Router Configuration Server...")
    print("Available endpoints:")
    print("  POST /configure-router - Configure router from NetBox webhook")
    print("  GET  /health - Health check")
    print("  GET  /history - Get configuration history")
    print("  POST /test-connection - Test router connection")
    print("  POST /verify-routes - Verify existing routes on router")
    print("  POST /reset - Reset configuration history")
    print()
    print("Example webhook payload:")
    print("  {")
    print('    "prefix": "2001:db8:56::/56",')
    print('    "router_ipv6": "2001:470:731b:4000:10:1:255:14",')
    print('    "cpe_link_local": "fe80::1234:5678:9abc:def0",')
    print('    "router_link_addr": "2001:470:731b:4000::1",')
    print('    "leasetime": 1734076800,')
    print('    "client_duid": "0001000123456789",')
    print('    "iaid": 12345')
    print("  }")
    print()
    print("Route verification examples:")
    print("  curl -X POST http://localhost:5000/verify-routes \\")
    print('    -H "Content-Type: application/json" \\')
    print('    -d \'{"router_ip": "192.168.10.76", "username": "vyos"}\'')
    print()
    print("  curl -X POST http://localhost:5000/verify-routes \\")
    print('    -H "Content-Type: application/json" \\')
    print('    -d \'{"router_ip": "192.168.10.76", "prefix_cidr": "2001:db8:56::/56"}\'')
    print()
    
    app.run(host='0.0.0.0', port=5000, debug=True)