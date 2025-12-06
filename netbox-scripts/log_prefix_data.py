from extras.scripts import *
from ipam.models import Prefix


class LogPrefixDataScript(Script):
    class Meta:
        name = "Log Prefix Data"
        description = "Log detailed information about a NetBox prefix"

    prefix = ObjectVar(
        description="Select the prefix to log", model=Prefix, required=True
    )

    def run(self, data, commit):
        prefix_data = data["prefix"]

        # Handle case where prefix is passed as string (e.g., via API)
        if isinstance(prefix_data, str):
            try:
                prefix = Prefix.objects.get(prefix=prefix_data)
            except Prefix.DoesNotExist:
                raise ValueError(f"Prefix '{prefix_data}' does not exist in NetBox")
        else:
            prefix = prefix_data

        # Log basic prefix information
        self.log_info(f"Prefix: {prefix.prefix}")
        self.log_info(f"Prefix Length: /{prefix.prefix_length}")
        self.log_info(f"Family: {prefix.family.label} ({prefix.family.value})")
        self.log_info(f"Status: {prefix.status}")
        self.log_info(f"Role: {prefix.role.name if prefix.role else None}")
        self.log_info(f"Type: {prefix.type}")
        self.log_info(f"Site: {prefix.site.name if prefix.site else None}")
        self.log_info(
            f"VLAN: {prefix.vlan.name if prefix.vlan else None} ({prefix.vlan.vid if prefix.vlan else None})"
        )
        self.log_info(f"VRF: {prefix.vrf.name if prefix.vrf else None}")
        self.log_info(f"Tenant: {prefix.tenant.name if prefix.tenant else None}")
        self.log_info(f"Description: {prefix.description}")

        # Log tags if any
        if prefix.tags.exists():
            self.log_info(f"Tags: {', '.join(tag.name for tag in prefix.tags.all())}")

        # Log custom fields if any
        if prefix.custom_fields:
            self.log_info("Custom Fields:")
            for key, value in prefix.custom_fields.items():
                self.log_info(f"  {key}: {value}")

        # Log related objects
        if prefix.ip_addresses.exists():
            self.log_info(f"IP Addresses: {prefix.ip_addresses.count()}")
            for ip in prefix.ip_addresses.all()[:5]:  # Limit to first 5
                self.log_info(f"  - {ip.address}")

        # Log timestamps
        self.log_info(f"Created: {prefix.created}")
        self.log_info(f"Last Updated: {prefix.last_updated}")

        return f"Successfully logged data for prefix {prefix.prefix}"
