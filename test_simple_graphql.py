#!/usr/bin/env python3
"""
Simple test to validate GraphQL query syntax
"""

import requests
import json

def test_graphql_query():
    """Test the exact GraphQL query you provided"""
    
    # Try to find device by IP address using ip_address_list first
    query = """
    query FindDeviceByIP($ip: String!) {
      ip_address_list(filters: {address: $ip}) {
        id
        address
        assigned_object {
          __typename
          ... on DeviceType {
            id
            name
            interfaces {
              id
              name
              ip_addresses {
                address
                family
              }
            }
          }
        }
      }
    }
    """
    
    variables = {"ip": "2001:470:731b:4000:10:1:255:14"}
    
    headers = {
        "Authorization": "Token 584149a859ea8e7a7f5e2d610c7235a3e2d2460c",
        "Content-Type": "application/json"
    }
    
    payload = {
        "query": query,
        "variables": variables
    }
    
    try:
        response = requests.post(
            "https://netbox.lab.aibix.io/graphql/", 
            json=payload, 
            headers=headers, 
            timeout=30, 
            verify=False
        )
        
        print(f"Status Code: {response.status_code}")
        print(f"Response Headers: {dict(response.headers)}")
        print(f"Response Body: {response.text}")
        
        if response.status_code == 200:
            data = response.json()
            print(f"Parsed JSON: {json.dumps(data, indent=2)}")
        
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    test_graphql_query()