#!/usr/bin/env python3
"""
Test script for the updated router configuration server with interface name resolution
"""

import json
import requests
import time

def test_webhook_with_interface_resolution():
    """Test the webhook with interface name resolution"""
    
    # Test webhook payload including router_link_addr
    test_payload = {
        "prefix": "2001:db8:56::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14",
        "cpe_link_local": "fe80::1234:5678:9abc:def0",
        "router_link_addr": "2001:470:731b:4000::1",  # This will be used to find the interface
        "leasetime": int(time.time()) + 3600,  # Valid for 1 hour from now
        "client_duid": "0001000123456789",
        "iaid": 12345,
        "netbox_prefix_id": 123
    }
    
    webhook_url = "http://localhost:5000/configure-router"
    
    print("Testing webhook with interface name resolution...")
    print(f"Payload: {json.dumps(test_payload, indent=2)}")
    print(f"URL: {webhook_url}")
    print()
    
    try:
        response = requests.post(
            webhook_url,
            json=test_payload,
            headers={"Content-Type": "application/json"},
            timeout=30
        )
        
        print(f"Status Code: {response.status_code}")
        print(f"Response: {json.dumps(response.json(), indent=2)}")
        
        if response.status_code == 200:
            result = response.json()
            if result.get("status") == "success":
                print("✅ Test passed! Route configured with interface name.")
                print(f"   Route: {result.get('route_added')}")
                print(f"   Interface: {result.get('interface_name')}")
            else:
                print("❌ Test failed! Server returned error.")
        else:
            print("❌ Test failed! HTTP error.")
            
    except requests.exceptions.ConnectionError:
        print("❌ Connection failed! Make sure the router config server is running on localhost:5000")
    except Exception as e:
        print(f"❌ Test failed with error: {str(e)}")

def test_health_check():
    """Test the health check endpoint"""
    try:
        response = requests.get("http://localhost:5000/health", timeout=10)
        if response.status_code == 200:
            print("✅ Health check passed")
            print(f"   Response: {response.json()}")
        else:
            print("❌ Health check failed")
    except Exception as e:
        print(f"❌ Health check failed: {str(e)}")

if __name__ == "__main__":
    print("Router Configuration Server Test")
    print("=" * 40)
    
    print("\n1. Testing health check...")
    test_health_check()
    
    print("\n2. Testing webhook with interface resolution...")
    test_webhook_with_interface_resolution()
    
    print("\nTest completed!")