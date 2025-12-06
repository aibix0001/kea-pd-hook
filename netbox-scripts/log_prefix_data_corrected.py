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
        
        # METHOD 2: Using custom_fields property
        self.log_info("=== Method 2: Using custom_fields property ===")
        if hasattr(prefix, 'custom_fields') and prefix.custom_fields:
            try:
                # Try to iterate as dictionary
                if hasattr(prefix.custom_fields, 'items'):
                    self.log_info("Custom Fields (via custom_fields.items()):")
                    for field_name, field_value in prefix.custom_fields.items():
                        self.log_info(f"  {field_name}: {field_value}")
                else:
                    # If it's not a dict, try to convert or access differently
                    self.log_info(f"custom_fields type: {type(prefix.custom_fields)}")
                    self.log_info(f"custom_fields value: {prefix.custom_fields}")
            except AttributeError as e:
                self.log_info(f"Error accessing custom_fields.items(): {e}")
        
        # METHOD 3: Using _custom_field_data (fallback)
        self.log_info("=== Method 3: Using _custom_field_data ===")
        custom_data = getattr(prefix, '_custom_field_data', {})
        if custom_data:
            self.log_info("Custom Fields (via _custom_field_data):")
            for field_name, field_value in custom_data.items():
                self.log_info(f"  {field_name}: {field_value}")
        else:
            self.log_info("No custom fields found via _custom_field_data")
        
        # METHOD 4: Debug information
        self.log_info("=== Debug Information ===")
        self.log_info(f"Prefix object type: {type(prefix)}")
        self.log_info(f"Has cf attribute: {hasattr(prefix, 'cf')}")
        self.log_info(f"Has custom_fields attribute: {hasattr(prefix, 'custom_fields')}")
        self.log_info(f"Has _custom_field_data attribute: {hasattr(prefix, '_custom_field_data')}")
        
        if hasattr(prefix, 'cf'):
            self.log_info(f"cf type: {type(prefix.cf)}")
            self.log_info(f"cf value: {prefix.cf}")
        
        if hasattr(prefix, 'custom_fields'):
            self.log_info(f"custom_fields type: {type(prefix.custom_fields)}")
            self.log_info(f"custom_fields dir: {dir(prefix.custom_fields)}")

        return f"Successfully logged data for prefix {prefix.prefix}"
