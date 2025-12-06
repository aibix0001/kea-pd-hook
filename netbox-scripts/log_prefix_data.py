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

        # Log prefix information
        self.log_info(f"Prefix: {prefix.prefix}")

        # Log custom fields
        self.log_info("Custom Fields:")
        for key, value in prefix.custom_fields.items():
            self.log_info(f"  {key}: {value}")

        return f"Successfully logged data for prefix {prefix.prefix}"
