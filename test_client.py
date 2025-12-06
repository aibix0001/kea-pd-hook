#!/usr/bin/env python3
"""
Test script for the Flask NetBox simulation server
"""

import requests
import json
import time

BASE_URL = "http://localhost:5000"

def test_health():
    """Test health endpoint"""
    print("Testing health endpoint...")
    response = requests.get(f"{BASE_URL}/health")
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_create_prefix():
    """Test creating a prefix"""
    print("Testing prefix creation...")
    prefix_data = {
        "custom_fields": {
            "dhcpv6_client_duid": "0001000123456789",
            "dhcpv6_iaid": 12345,
            "dhcpv6_cpe_link_local": "fe80::1234:5678:9abc:def0",
            "dhcpv6_router_ip": "2001:db8:1::1",
            "dhcpv6_leasetime": 1734076800
        }
    }
    
    response = requests.post(f"{BASE_URL}/api/ipam/prefixes/2001:db8:56::/56", json=prefix_data)
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_log_script():
    """Test the NetBox script simulation"""
    print("Testing NetBox script simulation...")
    script_data = {
        "prefix": "2001:db8:56::/56"
    }
    
    response = requests.post(
        f"{BASE_URL}/api/extras/scripts/log-prefix-data/",
        json=script_data,
        headers={"Content-Type": "application/json"}
    )
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_list_prefixes():
    """Test listing all prefixes"""
    print("Testing prefix listing...")
    response = requests.get(f"{BASE_URL}/api/ipam/prefixes/")
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def test_update_prefix():
    """Test updating a prefix"""
    print("Testing prefix update...")
    update_data = {
        "custom_fields": {
            "dhcpv6_iaid": 54321,  # Updated IAID
            "dhcpv6_router_ip": "2001:db8:1::2"  # Updated router IP
        }
    }
    
    response = requests.patch(f"{BASE_URL}/api/ipam/prefixes/2001:db8:56::/56", json=update_data)
    print(f"Status: {response.status_code}")
    print(f"Response: {response.json()}")
    print()

def main():
    """Run all tests"""
    print("Testing Flask NetBox Simulation Server")
    print("=" * 40)
    
    try:
        test_health()
        test_create_prefix()
        test_log_script()
        test_list_prefixes()
        test_update_prefix()
        test_log_script()  # Test again to see updated data
        
        print("All tests completed successfully!")
        
    except requests.exceptions.ConnectionError:
        print("Error: Could not connect to the Flask server.")
        print("Make sure the server is running on http://localhost:5000")
    except Exception as e:
        print(f"Error during testing: {e}")

if __name__ == "__main__":
    main()