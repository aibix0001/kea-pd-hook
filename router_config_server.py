#!/usr/bin/env python3
"""
Flask server to handle VyOS router configuration from NetBox webhook data
Receives prefix data from NetBox and configures VyOS routers via SSH
"""

from flask import Flask, request, jsonify
import logging
import time
import paramiko
from datetime import datetime
from typing import Dict, Any, Optional

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# In-memory storage for configuration history
configuration_history = []


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
    required_fields = ["prefix", "router_ip", "cpe_link_local", "leasetime"]
    
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
        router_ip = data["router_ip"]
        cpe_link_local = data["cpe_link_local"]
        leasetime = data["leasetime"]
        client_duid = data.get("client_duid", "")
        iaid = data.get("iaid", "")
        netbox_prefix_id = data.get("netbox_prefix_id", "")
        
        logger.info(f"Processing configuration request:")
        logger.info(f"  Prefix: {prefix_cidr}")
        logger.info(f"  Router IP: {router_ip}")
        logger.info(f"  CPE Link-Local: {cpe_link_local}")
        logger.info(f"  Client DUID: {client_duid}")
        logger.info(f"  IAID: {iaid}")
        
        # Configure router
        router = VyOSRouter(router_ip)
        
        if not router.connect():
            return jsonify({
                "status": "error",
                "message": f"Failed to connect to router {router_ip}",
                "prefix": prefix_cidr,
                "router_ip": router_ip
            }), 500
        
        try:
            success = router.configure_route(prefix_cidr, cpe_link_local)
            
            if success:
                # Record configuration in history
                config_record = {
                    "timestamp": datetime.now().isoformat(),
                    "prefix": prefix_cidr,
                    "router_ip": router_ip,
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
                    "router_ip": router_ip,
                    "cpe_link_local": cpe_link_local,
                    "route_added": f"{prefix_cidr} via {cpe_link_local}",
                    "timestamp": datetime.now().isoformat()
                })
            else:
                return jsonify({
                    "status": "error",
                    "message": "Failed to configure route",
                    "prefix": prefix_cidr,
                    "router_ip": router_ip
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
    print('    "router_ip": "192.168.1.1",')
    print('    "cpe_link_local": "fe80::1234:5678:9abc:def0",')
    print('    "leasetime": 1734076800,')
    print('    "client_duid": "0001000123456789",')
    print('    "iaid": 12345')
    print("  }")
    print()
    
    app.run(host='0.0.0.0', port=5000, debug=True)