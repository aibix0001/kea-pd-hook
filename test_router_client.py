#!/usr/bin/env python3
"""
Test client for the VyOS router configuration server
"""

import requests
import json
import time
from datetime import datetime, timedelta

BASE_URL = "http://localhost:5000"

def test_health():
    """Test health endpoint"""
    print("Testing health endpoint...")
    response = requests.get(f"{BASE_URL}/health")
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_router_configuration():
    """Test router configuration webhook"""
    print("Testing router configuration webhook...")
    
    # Create test payload
    future_timestamp = int((datetime.now() + timedelta(hours=1)).timestamp())
    
    payload = {
        "prefix": "2001:db8:56::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14",  # IPv6 address for GraphQL lookup
        "cpe_link_local": "fe80::1234:5678:9abc:def0",
        "leasetime": future_timestamp,
        "client_duid": "0001000123456789",
        "iaid": 12345,
        "netbox_prefix_id": 42
    }
    
    print(f"Sending payload: {json.dumps(payload, indent=2)}")
    
    response = requests.post(
        f"{BASE_URL}/configure-router",
        json=payload,
        headers={"Content-Type": "application/json"}
    )
    
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_expired_lease():
    """Test with expired lease"""
    print("Testing with expired lease...")
    
    # Create payload with expired timestamp
    past_timestamp = int((datetime.now() - timedelta(hours=1)).timestamp())
    
    payload = {
        "prefix": "2001:db8:57::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:15",
        "cpe_link_local": "fe80::1234:5678:9abc:def1",
        "leasetime": past_timestamp,
        "client_duid": "0001000123456789",
        "iaid": 12346
    }
    
    response = requests.post(
        f"{BASE_URL}/configure-router",
        json=payload,
        headers={"Content-Type": "application/json"}
    )
    
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_invalid_payload():
    """Test with invalid payload"""
    print("Testing with invalid payload...")
    
    # Missing required fields
    payload = {
        "prefix": "2001:db8:58::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14"
        # Missing cpe_link_local and leasetime
    }
    
    response = requests.post(
        f"{BASE_URL}/configure-router",
        json=payload,
        headers={"Content-Type": "application/json"}
    )
    
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_connection():
    """Test router connection"""
    print("Testing router connection...")
    
    payload = {
        "router_ip": "192.168.1.100",  # Test IP - change to actual router
        "username": "vyos"
    }
    
    response = requests.post(
        f"{BASE_URL}/test-connection",
        json=payload,
        headers={"Content-Type": "application/json"}
    )
    
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_history():
    """Test configuration history"""
    print("Testing configuration history...")
    
    response = requests.get(f"{BASE_URL}/history")
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def main():
    """Run all tests"""
    print("Testing VyOS Router Configuration Server")
    print("=" * 50)
    
    try:
        test_health()
        test_router_configuration()
        test_expired_lease()
        test_invalid_payload()
        test_connection()
        test_history()
        
        print("All tests completed!")
        print("\nNote: Router connection tests will fail unless you have")
        print("a VyOS router running at the specified IP address.")
        
    except requests.exceptions.ConnectionError:
        print("Error: Could not connect to the Flask server.")
        print("Make sure the server is running on http://localhost:5000")
        print("Run: uv run python router_config_server.py")
    except Exception as e:
        print(f"Error during testing: {e}")

if __name__ == "__main__":
    main()