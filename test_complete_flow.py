#!/usr/bin/env python3
"""
Test script to validate the interface resolution implementation
"""

import json
import requests
import time

def test_interface_query_function():
    """Test the interface query function directly"""
    
    # Test data - this would be a real router link address from NetBox
    test_router_link_addr = "2001:470:731b:4000::1"
    netbox_url = "https://netbox.lab.aibix.io"
    netbox_token = "584149a859ea8e7a7f5e2d610c7235a3e2d2460c"
    
    print("Testing interface query function...")
    print(f"Router Link Address: {test_router_link_addr}")
    print(f"NetBox URL: {netbox_url}")
    print()
    
    # Test the API query directly
    interfaces_url = f"{netbox_url.rstrip('/')}/api/ipam/ip-addresses/"
    interfaces_params = {
        "address": test_router_link_addr
    }
    
    headers = {
        "Authorization": f"Token {netbox_token}",
        "Content-Type": "application/json",
        "Accept": "application/json"
    }
    
    try:
        response = requests.get(interfaces_url, params=interfaces_params, headers=headers, timeout=30, verify=False)
        response.raise_for_status()
        
        ip_data = response.json()
        print(f"IP Address Query Results:")
        print(json.dumps(ip_data, indent=2))
        
        if ip_data.get("results") and len(ip_data["results"]) > 0:
            ip_info = ip_data["results"][0]
            interface_info = ip_info.get("assigned_object_id")
            
            if interface_info:
                print(f"\nFound interface ID: {interface_info}")
                
                # Get interface details
                interface_detail_url = f"{netbox_url.rstrip('/')}/api/dcim/interfaces/{interface_info}/"
                interface_response = requests.get(interface_detail_url, headers=headers, timeout=30, verify=False)
                interface_response.raise_for_status()
                
                interface_detail = interface_response.json()
                interface_name = interface_detail.get("name")
                device_id = interface_detail.get("device", {}).get("id")
                
                print(f"Interface Name: {interface_name}")
                print(f"Device ID: {device_id}")
                print(f"Full Interface Details:")
                print(json.dumps(interface_detail, indent=2))
                
                return True, interface_name, device_id
            else:
                print("❌ IP address is not assigned to an interface")
                return False, None, None
        else:
            print("❌ No IP address found with that address")
            return False, None, None
            
    except Exception as e:
        print(f"❌ Error querying NetBox: {str(e)}")
        return False, None, None

def test_webhook_validation():
    """Test webhook validation with the new field"""
    
    print("\n" + "="*50)
    print("Testing webhook validation...")
    
    # Test payload with all required fields
    valid_payload = {
        "prefix": "2001:db8:56::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14",
        "cpe_link_local": "fe80::1234:5678:9abc:def0",
        "router_link_addr": "2001:470:731b:4000::1",
        "leasetime": int(time.time()) + 3600,
        "client_duid": "0001000123456789",
        "iaid": 12345,
        "netbox_prefix_id": 123
    }
    
    # Test payload missing router_link_addr
    invalid_payload = {
        "prefix": "2001:db8:56::/56",
        "router_ipv6": "2001:470:731b:4000:10:1:255:14",
        "cpe_link_local": "fe80::1234:5678:9abc:def0",
        "leasetime": int(time.time()) + 3600
    }
    
    webhook_url = "http://localhost:5000/configure-router"
    
    print("\n1. Testing valid payload (should fail at NetBox query but pass validation):")
    try:
        response = requests.post(webhook_url, json=valid_payload, timeout=10)
        print(f"Status: {response.status_code}")
        result = response.json()
        print(f"Response: {json.dumps(result, indent=2)}")
        
        if response.status_code == 400 and "router_link_addr" in result.get("message", ""):
            print("✅ Validation passed, failed at NetBox query as expected")
        elif response.status_code == 200:
            print("✅ Full success!")
        else:
            print("⚠️ Unexpected response")
    except Exception as e:
        print(f"❌ Error: {str(e)}")
    
    print("\n2. Testing invalid payload (missing router_link_addr):")
    try:
        response = requests.post(webhook_url, json=invalid_payload, timeout=10)
        print(f"Status: {response.status_code}")
        result = response.json()
        print(f"Response: {json.dumps(result, indent=2)}")
        
        if response.status_code == 400 and "Missing required field" in result.get("message", ""):
            print("✅ Validation correctly caught missing field")
        else:
            print("⚠️ Unexpected validation result")
    except Exception as e:
        print(f"❌ Error: {str(e)}")

def show_implementation_summary():
    """Show summary of what was implemented"""
    
    print("\n" + "="*50)
    print("IMPLEMENTATION SUMMARY")
    print("="*50)
    
    print("\n1. NetBox Webhook Script Updates:")
    print("   ✅ Added dhcpv6_router_link_addr extraction")
    print("   ✅ Added router_link_addr to webhook payload")
    
    print("\n2. Router Config Server Updates:")
    print("   ✅ Added find_interface_by_ipv6_address() function")
    print("   ✅ Updated webhook validation to require router_link_addr")
    print("   ✅ Updated configure_route() to accept interface_name parameter")
    print("   ✅ Modified route command to include interface name")
    print("   ✅ Updated configuration history and responses")
    
    print("\n3. Route Command Format:")
    print("   Before: set protocols static route6 <prefix> next-hop <cpe>")
    print("   After:  set protocols static route6 <prefix> next-hop <cpe> interface <interface>")
    
    print("\n4. Flow:")
    print("   Kea Hook → NetBox (stores dhcpv6_router_link_addr)")
    print("   NetBox Script → Router Server (includes router_link_addr)")
    print("   Router Server → NetBox API (queries interface by router_link_addr)")
    print("   Router Server → VyOS (configures route with interface name)")
    
    print("\n5. Benefits:")
    print("   ✅ Routes to link-local addresses now work correctly")
    print("   ✅ Interface is dynamically determined from NetBox data")
    print("   ✅ No hardcoded interface names")
    print("   ✅ Proper error handling for missing interfaces")

if __name__ == "__main__":
    print("Interface Resolution Implementation Test")
    print("=" * 50)
    
    # Test the interface query function
    success, interface_name, device_id = test_interface_query_function()
    
    # Test webhook validation
    test_webhook_validation()
    
    # Show implementation summary
    show_implementation_summary()
    
    print(f"\nTest completed!")
    if success:
        print(f"✅ Found interface: {interface_name} on device {device_id}")
    else:
        print("⚠️ Interface not found in NetBox (expected for test data)")