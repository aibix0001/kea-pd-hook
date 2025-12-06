#!/usr/bin/env python3
"""
Test script to verify route configuration functionality
"""

import json
import requests

def test_route_verification():
    """Test route verification endpoint"""
    
    # Test data for route verification
    test_data = {
        "router_ip": "192.168.10.76",  # Replace with actual router IP
        "username": "vyos"
    }
    
    # Test 1: Get all routes
    print("1. Testing get all routes...")
    try:
        response = requests.post(
            "http://localhost:5000/verify-routes",
            json=test_data,
            timeout=30
        )
        
        print(f"Status: {response.status_code}")
        result = response.json()
        print(f"Response: {json.dumps(result, indent=2)}")
        
        if response.status_code == 200 and result.get("routes"):
            print(f"✅ Found {result['routes_count']} routes")
            for route in result['routes']:
                print(f"   Route: {route}")
        else:
            print("⚠️ No routes found or error occurred")
            
    except Exception as e:
        print(f"❌ Error: {str(e)}")
    
    # Test 2: Verify specific route
    print("\n2. Testing specific route verification...")
    test_data_with_prefix = {
        "router_ip": "192.168.10.76",
        "username": "vyos",
        "prefix_cidr": "2001:db8:56::/56"
    }
    
    try:
        response = requests.post(
            "http://localhost:5000/verify-routes",
            json=test_data_with_prefix,
            timeout=30
        )
        
        print(f"Status: {response.status_code}")
        result = response.json()
        print(f"Response: {json.dumps(result, indent=2)}")
        
        if response.status_code == 200:
            if result.get("route_exists"):
                print(f"✅ Route exists: {result.get('route_config')}")
            else:
                print("❌ Route does not exist")
        else:
            print("⚠️ Error occurred")
            
    except Exception as e:
        print(f"❌ Error: {str(e)}")

def test_webhook_with_verification():
    """Test webhook with enhanced verification"""
    
    webhook_payload = {
        "prefix": "2001:db8:56::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14",
        "cpe_link_local": "fe80::1234:5678:9abc:def0",
        "router_link_addr": "2001:470:731b:4000::1",
        "leasetime": int(time.time()) + 3600,
        "client_duid": "0001000123456789",
        "iaid": 12345,
        "netbox_prefix_id": 123
    }
    
    print("\n3. Testing webhook with route verification...")
    try:
        response = requests.post(
            "http://localhost:5000/configure-router",
            json=webhook_payload,
            timeout=60
        )
        
        print(f"Status: {response.status_code}")
        result = response.json()
        print(f"Response: {json.dumps(result, indent=2)}")
        
        if response.status_code == 200:
            print("✅ Webhook processed successfully")
            if result.get("status") == "success":
                print(f"   Route: {result.get('route_added')}")
                print(f"   Interface: {result.get('interface_name')}")
        else:
            print("❌ Webhook failed")
            
    except Exception as e:
        print(f"❌ Error: {str(e)}")

if __name__ == "__main__":
    import time
    
    print("Route Configuration Verification Test")
    print("=" * 50)
    
    # Test route verification endpoints
    test_route_verification()
    
    # Test webhook with verification
    test_webhook_with_verification()
    
    print("\nTest completed!")
    print("\nManual verification commands:")
    print("1. SSH to router: ssh vyos@<router_ip>")
    print("2. Check routes: show configuration commands | match 'set protocols static route6'")
    print("3. Check specific route: show configuration commands | match 'set protocols static route6 <prefix>'")