from extras.scripts import *
from ipam.models import Prefix
from datetime import datetime
import paramiko
import time


class ConfigureVyosRouterScript(Script):
    class Meta:
        name = "Configure VyOS Router"
        description = "Configure VyOS router with DHCPv6 prefix route"

    prefix = ObjectVar(
        description="Select the prefix to configure", model=Prefix, required=True
    )

    def run(self, data, commit):
        prefix_data = data["prefix"]

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

        self.log_info(f"Prefix: {prefix_cidr}")
        self.log_info(f"Router IP: {router_ip}")
        self.log_info(f"CPE Link-Local: {cpe_link_local}")
        self.log_info(f"Lease Time: {leasetime}")
        self.log_info(f"Automation Was Here: {automation_was_here}")

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

        # Connect to VyOS router and configure route
        try:
            self.log_info(f"Connecting to VyOS router at {router_ip}")

            # Create SSH client
            ssh = paramiko.SSHClient()
            ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

            # Connect (assuming SSH key-based auth, adjust as needed)
            ssh.connect(router_ip, username="vyos", timeout=10)

            # Enter configuration mode
            stdin, stdout, stderr = ssh.exec_command("configure")
            time.sleep(1)

            # Add the route
            route_cmd = (
                f"set protocols static route6 {prefix_cidr} next-hop {cpe_link_local}"
            )
            self.log_info(f"Executing: {route_cmd}")
            stdin, stdout, stderr = ssh.exec_command(route_cmd)

            # Commit and save
            stdin, stdout, stderr = ssh.exec_command("commit")
            time.sleep(2)
            stdin, stdout, stderr = ssh.exec_command("save")

            # Exit configuration mode
            stdin, stdout, stderr = ssh.exec_command("exit")

            ssh.close()

            self.log_info(
                f"Successfully configured route for {prefix_cidr} via {cpe_link_local}"
            )

            # Update automation_was_here flag
            prefix.cf["automation_was_here"] = True
            prefix.save()

            return {
                "status": "success",
                "prefix": prefix_cidr,
                "router_ip": router_ip,
                "cpe_link_local": cpe_link_local,
                "route_added": f"{prefix_cidr} via {cpe_link_local}",
            }

        except Exception as e:
            self.log_error(f"Failed to configure VyOS router: {str(e)}")
            return {
                "status": "error",
                "prefix": prefix_cidr,
                "router_ip": router_ip,
                "error": str(e),
            }
