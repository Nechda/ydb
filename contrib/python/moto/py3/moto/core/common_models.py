from abc import abstractmethod
from .base_backend import InstanceTrackerMeta


class BaseModel(metaclass=InstanceTrackerMeta):
    def __new__(cls, *args, **kwargs):  # pylint: disable=unused-argument
        instance = super(BaseModel, cls).__new__(cls)
        cls.instances.append(instance)
        return instance


# Parent class for every Model that can be instantiated by CloudFormation
# On subclasses, implement all methods as @staticmethod to ensure correct behaviour of the CF parser
class CloudFormationModel(BaseModel):
    @staticmethod
    @abstractmethod
    def cloudformation_name_type():
        # https://docs.aws.amazon.com/AWSCloudFormation/latest/UserGuide/aws-properties-name.html
        # This must be implemented as a staticmethod with no parameters
        # Return None for resources that do not have a name property
        pass

    @staticmethod
    @abstractmethod
    def cloudformation_type():
        # This must be implemented as a staticmethod with no parameters
        # See for example https://docs.aws.amazon.com/AWSCloudFormation/latest/UserGuide/aws-resource-dynamodb-table.html
        return "AWS::SERVICE::RESOURCE"

    @classmethod
    @abstractmethod
    def has_cfn_attr(cls, attr):  # pylint: disable=unused-argument
        # Used for validation
        # If a template creates an Output for an attribute that does not exist, an error should be thrown
        return True

    @classmethod
    @abstractmethod
    def create_from_cloudformation_json(
        cls, resource_name, cloudformation_json, account_id, region_name, **kwargs
    ):
        # This must be implemented as a classmethod with parameters:
        # cls, resource_name, cloudformation_json, account_id, region_name
        # Extract the resource parameters from the cloudformation json
        # and return an instance of the resource class
        pass

    @classmethod
    @abstractmethod
    def update_from_cloudformation_json(
        cls,
        original_resource,
        new_resource_name,
        cloudformation_json,
        account_id,
        region_name,
    ):
        # This must be implemented as a classmethod with parameters:
        # cls, original_resource, new_resource_name, cloudformation_json, account_id, region_name
        # Extract the resource parameters from the cloudformation json,
        # delete the old resource and return the new one. Optionally inspect
        # the change in parameters and no-op when nothing has changed.
        pass

    @classmethod
    @abstractmethod
    def delete_from_cloudformation_json(
        cls, resource_name, cloudformation_json, account_id, region_name
    ):
        # This must be implemented as a classmethod with parameters:
        # cls, resource_name, cloudformation_json, account_id, region_name
        # Extract the resource parameters from the cloudformation json
        # and delete the resource. Do not include a return statement.
        pass

    @abstractmethod
    def is_created(self):
        # Verify whether the resource was created successfully
        # Assume True after initialization
        # Custom resources may need time after init before they are created successfully
        return True


class ConfigQueryModel:
    def __init__(self, backends):
        """Inits based on the resource type's backends (1 for each region if applicable)"""
        self.backends = backends

    def list_config_service_resources(
        self,
        account_id,
        resource_ids,
        resource_name,
        limit,
        next_token,
        backend_region=None,
        resource_region=None,
        aggregator=None,
    ):
        """For AWS Config. This will list all of the resources of the given type and optional resource name and region.

        This supports both aggregated and non-aggregated listing. The following notes the difference:

        - Non-Aggregated Listing -
        This only lists resources within a region. The way that this is implemented in moto is based on the region
        for the resource backend.

        You must set the `backend_region` to the region that the API request arrived from. resource_region can be set to `None`.

        - Aggregated Listing -
        This lists resources from all potential regional backends. For non-global resource types, this should collect a full
        list of resources from all the backends, and then be able to filter from the resource region. This is because an
        aggregator can aggregate resources from multiple regions. In moto, aggregated regions will *assume full aggregation
        from all resources in all regions for a given resource type*.

        The `backend_region` should be set to `None` for these queries, and the `resource_region` should optionally be set to
        the `Filters` region parameter to filter out resources that reside in a specific region.

        For aggregated listings, pagination logic should be set such that the next page can properly span all the region backends.
        As such, the proper way to implement is to first obtain a full list of results from all the region backends, and then filter
        from there. It may be valuable to make this a concatenation of the region and resource name.

        :param account_id: The account number
        :param resource_ids:  A list of resource IDs
        :param resource_name: The individual name of a resource
        :param limit: How many per page
        :param next_token: The item that will page on
        :param backend_region: The region for the backend to pull results from. Set to `None` if this is an aggregated query.
        :param resource_region: The region for where the resources reside to pull results from. Set to `None` if this is a
                                non-aggregated query.
        :param aggregator: If the query is an aggregated query, *AND* the resource has "non-standard" aggregation logic (mainly, IAM),
                                you'll need to pass aggregator used. In most cases, this should be omitted/set to `None`. See the
                                conditional logic under `if aggregator` in the moto/iam/config.py for the IAM example.

        :return: This should return a list of Dicts that have the following fields:
            [
                {
                    'type': 'AWS::The AWS Config data type',
                    'name': 'The name of the resource',
                    'id': 'The ID of the resource',
                    'region': 'The region of the resource -- if global, then you may want to have the calling logic pass in the
                               aggregator region in for the resource region -- or just us-east-1 :P'
                }
                , ...
            ]
        """
        raise NotImplementedError()

    def get_config_resource(
        self,
        account_id,
        resource_id,
        resource_name=None,
        backend_region=None,
        resource_region=None,
    ):
        """For AWS Config. This will query the backend for the specific resource type configuration.

        This supports both aggregated, and non-aggregated fetching -- for batched fetching -- the Config batching requests
        will call this function N times to fetch the N objects needing to be fetched.

        - Non-Aggregated Fetching -
        This only fetches a resource config within a region. The way that this is implemented in moto is based on the region
        for the resource backend.

        You must set the `backend_region` to the region that the API request arrived from. `resource_region` should be set to `None`.

        - Aggregated Fetching -
        This fetches resources from all potential regional backends. For non-global resource types, this should collect a full
        list of resources from all the backends, and then be able to filter from the resource region. This is because an
        aggregator can aggregate resources from multiple regions. In moto, aggregated regions will *assume full aggregation
        from all resources in all regions for a given resource type*.

        ...
        :param account_id:
        :param resource_id:
        :param resource_name:
        :param backend_region:
        :param resource_region:
        :return:
        """
        raise NotImplementedError()


class CloudWatchMetricProvider(object):
    @staticmethod
    @abstractmethod
    def get_cloudwatch_metrics(account_id):
        pass
