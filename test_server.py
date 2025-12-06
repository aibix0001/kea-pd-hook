#!/usr/bin/env python3
"""
Flask server to handle router configuration from NetBox webhook data
Receives prefix data from NetBox and configures VyOS routers
"""

from flask import Flask, request, jsonify
import json
import logging
import time
import subprocess
from datetime import datetime
from typing import Dict, Any, Optional

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# In-memory storage for prefixes (simulating NetBox database)
prefixes_db = {}
configuration_history = []

class MockPrefix:
    """Mock NetBox Prefix object for testing"""
    def __init__(self, prefix_str: str, custom_fields: Optional[Dict[str, Any]] = None):
        self.prefix = prefix_str
        self.custom_fields = custom_fields or {}
        self.cf = self.custom_fields  # NetBox uses cf property for custom fields
        
    def to_dict(self):
        return {
            'prefix': self.prefix,
            'custom_fields': self.custom_fields
        }

def log_info(message: str):
    """Mock NetBox script logging"""
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    logger.info(f"[{timestamp}] {message}")

def find_prefix(prefix_identifier: str) -> Optional[MockPrefix]:
    """Find prefix by string or ID"""
    # Try to find by exact prefix string
    if prefix_identifier in prefixes_db:
        return prefixes_db[prefix_identifier]
    
    # Try to find by treating as ID (for testing)
    try:
        prefix_id = int(prefix_identifier)
        for prefix in prefixes_db.values():
            if id(prefix) == prefix_id:
                return prefix
    except ValueError:
        pass
    
    return None

@app.route('/api/extras/scripts/log-prefix-data/', methods=['POST'])
def log_prefix_data():
    """Simulate NetBox script execution endpoint"""
    try:
        data = request.get_json()
        
        if not data or 'prefix' not in data:
            return jsonify({'error': 'prefix parameter is required'}), 400
        
        prefix_data = data['prefix']
        log_info(f"Received request with prefix data: {prefix_data}")
        
        # Handle different input types (string, int, or dict)
        if isinstance(prefix_data, str):
            log_info(f"Received prefix as string: {prefix_data}")
            prefix = find_prefix(prefix_data)
            if not prefix:
                # Create a mock prefix for testing
                prefix = MockPrefix(prefix_data, {
                    'dhcpv6_client_duid': '0001000123456789',
                    'dhcpv6_iaid': 12345,
                    'dhcpv6_cpe_link_local': 'fe80::1234:5678:9abc:def0',
                    'dhcpv6_router_ip': '2001:db8:1::1',
                    'dhcpv6_leasetime': 1734076800
                })
                prefixes_db[prefix_data] = prefix
                log_info(f"Created mock prefix: {prefix_data}")
                
        elif isinstance(prefix_data, int):
            log_info(f"Received prefix as integer: {prefix_data}")
            prefix = find_prefix(str(prefix_data))
            if not prefix:
                return jsonify({'error': f'Prefix with ID {prefix_data} not found'}), 404
                
        elif isinstance(prefix_data, dict):
            # Handle full prefix object
            prefix_str = prefix_data.get('prefix', '')
            log_info(f"Received prefix as dict: {prefix_str}")
            prefix = MockPrefix(prefix_str, prefix_data.get('custom_fields', {}))
            prefixes_db[prefix_str] = prefix
            
        else:
            return jsonify({'error': 'Invalid prefix data type'}), 400
        
        # Log prefix information (simulating the NetBox script)
        log_info(f"Prefix: {prefix.prefix}")
        log_info(f"prefix object type: {type(prefix)}")
        log_info(f"prefix has cf attribute: {hasattr(prefix, 'cf')}")
        
        if hasattr(prefix, 'cf') and prefix.cf:
            log_info("Custom Fields:")
            custom_fields_dict = {}
            for field_name, field_value in prefix.cf.items():
                log_info(f"  {field_name}: {field_value}")
                custom_fields_dict[field_name] = field_value
        else:
            log_info("No custom fields found")
            custom_fields_dict = {}
        
        return jsonify({
            'status': 'success',
            'prefix': str(prefix.prefix),
            'custom_fields': custom_fields_dict,
            'message': f"Successfully logged data for prefix {prefix.prefix}"
        })
        
    except Exception as e:
        log_info(f"Error processing request: {str(e)}")
        return jsonify({'error': str(e)}), 500

@app.route('/api/ipam/prefixes/', methods=['GET'])
def list_prefixes():
    """List all stored prefixes"""
    return jsonify({
        'count': len(prefixes_db),
        'results': [prefix.to_dict() for prefix in prefixes_db.values()]
    })

@app.route('/api/ipam/prefixes/<path:prefix>', methods=['GET'])
def get_prefix(prefix):
    """Get specific prefix"""
    prefix_obj = find_prefix(prefix)
    if not prefix_obj:
        return jsonify({'error': 'Prefix not found'}), 404
    
    return jsonify(prefix_obj.to_dict())

@app.route('/api/ipam/prefixes/<path:prefix>', methods=['POST'])
def create_prefix(prefix):
    """Create new prefix (simulating NetBox API)"""
    try:
        data = request.get_json() or {}
        
        if find_prefix(prefix):
            return jsonify({'error': 'Prefix already exists'}), 409
        
        custom_fields = data.get('custom_fields', {})
        prefix_obj = MockPrefix(prefix, custom_fields)
        prefixes_db[prefix] = prefix_obj
        
        log_info(f"Created prefix: {prefix} with custom fields: {custom_fields}")
        
        return jsonify({
            'id': id(prefix_obj),
            'prefix': prefix,
            'custom_fields': custom_fields,
            'status': 'active'
        }), 201
        
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/api/ipam/prefixes/<path:prefix>', methods=['PATCH'])
def update_prefix(prefix):
    """Update existing prefix (simulating NetBox API)"""
    try:
        data = request.get_json() or {}
        prefix_obj = find_prefix(prefix)
        
        if not prefix_obj:
            return jsonify({'error': 'Prefix not found'}), 404
        
        # Update custom fields
        if 'custom_fields' in data:
            prefix_obj.custom_fields.update(data['custom_fields'])
            prefix_obj.cf = prefix_obj.custom_fields
        
        log_info(f"Updated prefix: {prefix}")
        
        return jsonify({
            'id': id(prefix_obj),
            'prefix': prefix,
            'custom_fields': prefix_obj.custom_fields,
            'status': 'active'
        })
        
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({'status': 'healthy', 'timestamp': datetime.now().isoformat()})

@app.route('/reset', methods=['POST'])
def reset_data():
    """Reset all stored data (for testing)"""
    global prefixes_db
    prefixes_db.clear()
    log_info("Reset all prefix data")
    return jsonify({'status': 'reset', 'message': 'All data cleared'})

if __name__ == '__main__':
    print("Starting Flask server for NetBox prefix testing...")
    print("Available endpoints:")
    print("  POST /api/extras/scripts/log-prefix-data/ - Simulate NetBox script")
    print("  GET  /api/ipam/prefixes/ - List all prefixes")
    print("  GET  /api/ipam/prefixes/<prefix> - Get specific prefix")
    print("  POST /api/ipam/prefixes/<prefix> - Create prefix")
    print("  PATCH /api/ipam/prefixes/<prefix> - Update prefix")
    print("  GET  /health - Health check")
    print("  POST /reset - Reset all data")
    print()
    print("Example usage:")
    print("  curl -X POST http://localhost:5000/api/extras/scripts/log-prefix-data/ \\")
    print("    -H 'Content-Type: application/json' \\")
    print("    -d '{\"prefix\":\"2001:db8:56::/56\"}'")
    print()
    
    app.run(host='0.0.0.0', port=5000, debug=True)