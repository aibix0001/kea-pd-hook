from extras.scripts import *
from ipam.models import Prefix
from datetime import datetime
import requests
import json


class ExtractPrefixDataWebhookScript(Script):
    class Meta:
        name = "Extract Prefix Data and Send Webhook"
        description = "Extract prefix custom fields and send to configuration server"

    prefix = ObjectVar(
        description="Select the prefix to process", model=Prefix, required=True
    )
    
    webhook_url = StringVar(
        description="Webhook URL to send configuration data to",
        default="http://localhost:5000/configure-router"
    )

    def run(self, data, commit):
        prefix_data = data["prefix"]
        webhook_url = data.get("webhook_url", "http://192.168.10.76:5000/configure-router")

        # Handle case where prefix is passed as string (e.g., via API)
        if isinstance(prefix_data, str):
            self.log_info(f"Received prefix as string: {prefix_data}")
            try:
                prefix = Prefix.objects.get(prefix=prefix_data)
            except Prefix.DoesNotExist:
                raise ValueError(f"Prefix '{prefix_data}' does not exist in NetBox")
        else:
            prefix = prefix_data

        # Extract required custom fields
        if not hasattr(prefix, "cf") or not prefix.cf:
            raise ValueError("No custom fields found on prefix")

        # Get required fields
        prefix_cidr = str(prefix.prefix)
        router_ip = prefix.cf.get("dhcpv6_router_ip")
        cpe_link_local = prefix.cf.get("dhcpv6_cpe_link_local")
        leasetime = prefix.cf.get("dhcpv6_leasetime")
        automation_was_here = prefix.cf.get("automation_was_here")
        client_duid = prefix.cf.get("dhcpv6_client_duid")
        iaid = prefix.cf.get("dhcpv6_iaid")

        self.log_info(f"Prefix: {prefix_cidr}")
        self.log_info(f"Router IP: {router_ip}")
        self.log_info(f"CPE Link-Local: {cpe_link_local}")
        self.log_info(f"Lease Time: {leasetime}")
        self.log_info(f"Automation Was Here: {automation_was_here}")
        self.log_info(f"Client DUID: {client_duid}")
        self.log_info(f"IAID: {iaid}")

        # Validate required fields
        if not all([router_ip, cpe_link_local, leasetime]):
            raise ValueError(
                "Missing required custom fields: dhcpv6_router_ip, dhcpv6_cpe_link_local, or dhcpv6_leasetime"
            )

        # Check conditions
        current_time = datetime.now().timestamp()
        if current_time >= leasetime:
            self.log_info(
                f"Lease expired (now: {current_time} >= leasetime: {leasetime})"
            )
            return {"status": "skipped", "reason": "lease_expired"}

        if automation_was_here:
            self.log_info(
                f"Already configured (automation_was_here: {automation_was_here})"
            )
            return {"status": "skipped", "reason": "already_configured"}

        # Prepare webhook payload
        payload = {
            "prefix": prefix_cidr,
            "router_ip": router_ip,
            "cpe_link_local": cpe_link_local,
            "leasetime": leasetime,
            "client_duid": client_duid,
            "iaid": iaid,
            "netbox_prefix_id": prefix.id,
            "timestamp": datetime.now().isoformat()
        }

        # Send webhook
        try:
            self.log_info(f"Sending webhook to: {webhook_url}")
            
            headers = {
                "Content-Type": "application/json",
                "User-Agent": "NetBox-Script/1.0"
            }
            
            response = requests.post(
                webhook_url,
                json=payload,
                headers=headers,
                timeout=30
            )
            
            response.raise_for_status()
            
            self.log_info(f"Webhook sent successfully. Status: {response.status_code}")
            self.log_info(f"Response: {response.text}")
            
            # If webhook was successful, update automation_was_here flag
            if response.status_code == 200:
                result = response.json()
                if result.get("status") == "success":
                    prefix.cf["automation_was_here"] = True
                    prefix.save()
                    self.log_info("Updated automation_was_here flag")
            
            return {
                "status": "webhook_sent",
                "prefix": prefix_cidr,
                "webhook_url": webhook_url,
                "response_status": response.status_code,
                "response_data": response.json() if response.content else None
            }

        except requests.exceptions.RequestException as e:
            self.log_error(f"Failed to send webhook: {str(e)}")
            return {
                "status": "webhook_failed",
                "prefix": prefix_cidr,
                "webhook_url": webhook_url,
                "error": str(e),
            }