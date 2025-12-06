from extras.scripts import *
from ipam.models import Prefix
from pprint import pprint


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
            self.log_info(f"Received prefix as string: {prefix_data}")
            try:
                prefix = Prefix.objects.get(prefix=prefix_data)
            except Prefix.DoesNotExist:
                raise ValueError(f"Prefix '{prefix_data}' does not exist in NetBox")
        else:
            prefix = prefix_data

        # Log prefix information
        self.log_info(f"Prefix: {prefix.prefix}")
        
        # METHOD 1: Using the cf property (RECOMMENDED)
        self.log_info("=== Method 1: Using cf property ===")
        if hasattr(prefix, 'cf') and prefix.cf:
            self.log_info("Custom Fields (via cf):")
            for field_name, field_value in prefix.cf.items():
                self.log_info(f"  {field_name}: {field_value}")
        else:
            self.log_info("No custom fields found via cf property")
        
        return f"Successfully logged data for prefix {prefix.prefix}"
