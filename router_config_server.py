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
from datetime import datetime
from typing import Dict, Any, Optional

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# In-memory storage for configuration history
configuration_history = []

# NetBox GraphQL configuration
NETBOX_GRAPHQL_URL = "https://netbox.example.com/graphql/"
NETBOX_TOKEN = "your-netbox-token-here"  # For testing


def find_router_management_ip(router_ipv6_address: str, netbox_url: str, netbox_token: str) -> dict:
    """
    Find router management IP using GraphQL query
    
    Args:
        router_ipv6_address: IPv6 address to search for (from dhcpv6_router_ip custom field)
        netbox_url: NetBox GraphQL endpoint
        netbox_token: NetBox API token
    
    Returns:
        {
            'success': bool,
            'management_ip': str or None,
            'device_id': str or None,
            'error': str or None
        }
    """
    query = """
    query FindRouterManagementIP($routerIpv6: String!) {
      device_list(
        filters: {interfaces: {ip_addresses: {address: {exact: $routerIpv6}}}
      ) {
        id
        interfaces(filters: {vrf: {name: {exact: "MGMT"}}}) {
          id
          ip_addresses {
            address
            family
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
    
    try:
        response = requests.post(netbox_url, json=payload, headers=headers, timeout=30)
        response.raise_for_status()
        
        data = response.json()
        
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
            return True
            
        except Exception as e:
            logger.error(f"Failed to connect to {self.router_ip}: {str(e)}")
            return False
    
    def configure_route(self, prefix_cidr: str, cpe_link_local: str) -> bool:
        """Configure static route on VyOS router"""
        if not self.ssh:
            logger.error("SSH connection not established")
            return False
        
        try:
            # Enter configuration mode
            logger.info("Entering configuration mode")
            stdin, stdout, stderr = self.ssh.exec_command("configure")
            time.sleep(1)
            
            # Add the route
            route_cmd = f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local}"
            logger.info(f"Executing: {route_cmd}")
            stdin, stdout, stderr = self.ssh.exec_command(route_cmd)
            
            # Check for errors
            error_output = stderr.read().decode().strip()
            if error_output:
                logger.error(f"Command error: {error_output}")
                return False
            
            # Commit and save
            logger.info("Committing configuration")
            stdin, stdout, stderr = self.ssh.exec_command("commit")
            time.sleep(2)
            
            logger.info("Saving configuration")
            stdin, stdout, stderr = self.ssh.exec_command("save")
            
            # Exit configuration mode
            stdin, stdout, stderr = self.ssh.exec_command("exit")
            
            logger.info(f"Successfully configured route for {prefix_cidr} via {cpe_link_local}")
            return True
            
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
    required_fields = ["prefix", "router_ipv6", "cpe_link_local", "leasetime"]
    
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
        leasetime = data["leasetime"]
        client_duid = data.get("client_duid", "")
        iaid = data.get("iaid", "")
        netbox_prefix_id = data.get("netbox_prefix_id", "")
        
        logger.info(f"Processing configuration request:")
        logger.info(f"  Prefix: {prefix_cidr}")
        logger.info(f"  Router IPv6: {router_ipv6}")
        logger.info(f"  CPE Link-Local: {cpe_link_local}")
        
        # Find management IP using GraphQL
        mgmt_result = find_router_management_ip(
            router_ipv6, 
            NETBOX_GRAPHQL_URL, 
            NETBOX_TOKEN
        )
        
        if not mgmt_result['success']:
            return jsonify({
                "status": "error",
                "message": mgmt_result['error'],
                "router_ipv6": router_ipv6
            }), 400
        
        management_ip = mgmt_result['management_ip']
        device_id = mgmt_result['device_id']
        
        logger.info(f"Found management IP: {management_ip} for device {device_id}")
        
        # Configure router using management IP
        router = VyOSRouter(management_ip)
        
        if not router.connect():
            return jsonify({
                "status": "error",
                "message": f"Failed to connect to router at management IP {management_ip}",
                "router_ipv6": router_ipv6,
                "management_ip": management_ip
            }), 500
        
        try:
            success = router.configure_route(prefix_cidr, cpe_link_local)
            
            if success:
                # Record configuration in history
                config_record = {
                    "timestamp": datetime.now().isoformat(),
                    "prefix": prefix_cidr,
                    "router_ipv6": router_ipv6,
                    "management_ip": management_ip,
                    "device_id": device_id,
                    "cpe_link_local": cpe_link_local,
                    "client_duid": client_duid,
                    "iaid": iaid,
                    "netbox_prefix_id": netbox_prefix_id,
                    "status": "success"
                }
                configuration_history.append(config_record)
                
                return jsonify({
                    "status": "success",
                    "message": f"Successfully configured route for {prefix_cidr} via {cpe_link_local}",
                    "prefix": prefix_cidr,
                    "router_ipv6": router_ipv6,
                    "management_ip": management_ip,
                    "device_id": device_id,
                    "cpe_link_local": cpe_link_local,
                    "route_added": f"{prefix_cidr} via {cpe_link_local}",
                    "timestamp": datetime.now().isoformat()
                })
            else:
                return jsonify({
                    "status": "error",
                    "message": "Failed to configure route",
                    "router_ipv6": router_ipv6,
                    "management_ip": management_ip
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


if __name__ == '__main__':
    print("Starting VyOS Router Configuration Server...")
    print("Available endpoints:")
    print("  POST /configure-router - Configure router from NetBox webhook")
    print("  GET  /health - Health check")
    print("  GET  /history - Get configuration history")
    print("  POST /test-connection - Test router connection")
    print("  POST /reset - Reset configuration history")
    print()
    print("Example webhook payload:")
    print("  {")
    print('    "prefix": "2001:db8:56::/56",')
    print('    "router_ipv6": "2001:470:731b:4000:10:1:255:14",')
    print('    "cpe_link_local": "fe80::1234:5678:9abc:def0",')
    print('    "leasetime": 1734076800,')
    print('    "client_duid": "0001000123456789",')
    print('    "iaid": 12345')
    print("  }")
    print()
    
    app.run(host='0.0.0.0', port=5000, debug=True)