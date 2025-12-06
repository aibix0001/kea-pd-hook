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
        elif isinstance(prefix_data, int):
            self.log_info(f"Received prefix as integer: {prefix_data}")
            try:
                prefix = Prefix.objects.get(pk=prefix_data)
            except Prefix.DoesNotExist:
                raise ValueError(
                    f"Prefix with ID '{prefix_data}' does not exist in NetBox"
                )
        else:
            prefix = prefix_data

        # Log prefix information
        self.log_info(f"Prefix: {prefix.prefix}")

        # Debug logging to verify object type and attributes
        self.log_info(f"prefix object type: {type(prefix)}")
        self.log_info(f"prefix has cf attribute: {hasattr(prefix, 'cf')}")
        if hasattr(prefix, "cf"):
            self.log_info(f"prefix.cf type: {type(prefix.cf)}")
            self.log_info(f"prefix.cf content: {prefix.cf}")

        # Log custom fields using the recommended cf property
        custom_fields_dict = {}
        if hasattr(prefix, "cf") and prefix.cf:
            self.log_info("Custom Fields:")
            for field_name, field_value in prefix.cf.items():
                self.log_info(f"  {field_name}: {field_value}")
                custom_fields_dict[field_name] = field_value
        # Fallback 1: Try custom_fields as dict
        elif hasattr(prefix, "custom_fields") and hasattr(
            prefix.custom_fields, "items"
        ):
            self.log_info("Custom Fields (via custom_fields property):")
            for field_name, field_value in prefix.custom_fields.items():
                self.log_info(f"  {field_name}: {field_value}")
                custom_fields_dict[field_name] = field_value
        # Fallback 2: Direct field access
        else:
            custom_data = getattr(prefix, "_custom_field_data", {})
            if custom_data:
                self.log_info("Custom Fields (via _custom_field_data):")
                for key, value in custom_data.items():
                    self.log_info(f"  {key}: {value}")
                    custom_fields_dict[key] = value
            else:
                self.log_info("No custom fields found")

        return {"prefix": str(prefix.prefix), "custom_fields": custom_fields_dict}
