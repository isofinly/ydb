import pathlib

from os import listdir
from ..utils import generic_filter

from moto.utilities.utils import load_resource
from ..exceptions import InvalidFilter, InvalidInstanceTypeError

import library.python.resource as _ya_res
import os
import json

INSTANCE_TYPES = load_resource("moto.ec2", "resources/instance_types.json")
INSTANCE_FAMILIES = list(set([i.split(".")[0] for i in INSTANCE_TYPES.keys()]))

root = pathlib.Path(__file__).parent.parent
offerings_path = "resources/instance_type_offerings"
INSTANCE_TYPE_OFFERINGS = {}
for entry in _ya_res.resfs_files(prefix=str(root / offerings_path)):
    rel_path = os.path.relpath(entry, root / offerings_path)
    path_parts = os.path.normpath(rel_path).split(os.path.sep)
    if len(path_parts) != 2:
        continue
    _location_type, _region = path_parts
    if _location_type not in INSTANCE_TYPE_OFFERINGS:
        INSTANCE_TYPE_OFFERINGS[_location_type] = {}
    res = json.loads(_ya_res.find(f"resfs/file/{entry}"))
    for instance in res:
        instance["LocationType"] = _location_type
        INSTANCE_TYPE_OFFERINGS[_location_type][_region.replace(".json", "")] = res


class InstanceType(dict):
    _filter_attributes = {
        "auto-recovery-supported": ["AutoRecoverySupported"],
        "bare-metal": ["BareMetal"],
        "burstable-performance-supported": ["BurstablePerformanceSupported"],
        "current-generation": ["CurrentGeneration"],
        "ebs-info.ebs-optimized-info.baseline-bandwidth-in-mbps": [
            "EbsInfo", "EbsOptimizedInfo", "BaselineBandwidthInMbps"
        ],
        "ebs-info.ebs-optimized-info.baseline-iops": [
            "EbsInfo", "EbsOptimizedInfo", "BaselineIops"
        ],
        "ebs-info.ebs-optimized-info.baseline-throughput-in-mbps": [
            "EbsInfo", "EbsOptimizedInfo", "BaselineThroughputInMBps"
        ],
        "ebs-info.ebs-optimized-info.maximum-bandwidth-in-mbps": [
            "EbsInfo", "EbsOptimizedInfo", "MaximumBandwidthInMbps"
        ],
        "ebs-info.ebs-optimized-info.maximum-iops": [
            "EbsInfo", "EbsOptimizedInfo", "MaximumIops"
        ],
        "ebs-info.ebs-optimized-info.maximum-throughput-in-mbps": [
            "EbsInfo", "EbsOptimizedInfo", "MaximumThroughputInMBps"
        ],
        "ebs-info.ebs-optimized-support": ["EbsInfo", "EbsOptimizedSupport"],
        "ebs-info.encryption-support": ["EbsInfo", "EncryptionSupport"],
        "ebs-info.nvme-support": ["EbsInfo", "NvmeSupport"],
        "free-tier-eligible": ["FreeTierEligible"],
        "hibernation-supported": ["HibernationSupported"],
        "hypervisor": ["Hypervisor"],
        "instance-storage-info.disk.count": ["InstanceStorageInfo", "Disks", "Count"],
        "instance-storage-info.disk.size-in-gb": [
            "InstanceStorageInfo", "Disks", "SizeInGB"
        ],
        "instance-storage-info.disk.type": ["InstanceStorageInfo", "Disks", "Type"],
        "instance-storage-info.encryption-support": [
            "InstanceStorageInfo", "EncryptionSupport"
        ],
        "instance-storage-info.nvme-support": ["InstanceStorageInfo", "NvmeSupport"],
        "instance-storage-info.total-size-in-gb": [
            "InstanceStorageInfo", "TotalSizeInGB"
        ],
        "instance-storage-supported": ["InstanceStorageSupported"],
        "instance-type": ["InstanceType"],
        "memory-info.size-in-mib": ["MemoryInfo", "SizeInMiB"],
        "network-info.efa-info.maximum-efa-interfaces": [
            "NetworkInfo", "EfaInfo", "MaximumEfaInterfaces"
        ],
        "network-info.efa-supported": ["NetworkInfo", "EfaSupported"],
        "network-info.ena-support": ["NetworkInfo", "EnaSupport"],
        "network-info.encryption-in-transit-supported": [
            "NetworkInfo", "EncryptionInTransitSupported"
        ],
        "network-info.ipv4-addresses-per-interface": [
            "NetworkInfo", "Ipv4AddressesPerInterface"
        ],
        "network-info.ipv6-addresses-per-interface": [
            "NetworkInfo", "Ipv6AddressesPerInterface"
        ],
        "network-info.ipv6-supported": ["NetworkInfo", "Ipv6Supported"],
        "network-info.maximum-network-cards": ["NetworkInfo", "MaximumNetworkCards"],
        "network-info.maximum-network-interfaces": [
            "NetworkInfo", "MaximumNetworkInterfaces"
        ],
        "network-info.network-performance": ["NetworkInfo", "NetworkPerformance"],
        "processor-info.supported-architecture": [
            "ProcessorInfo", "SupportedArchitectures"
        ],
        "processor-info.sustained-clock-speed-in-ghz": [
            "ProcessorInfo", "SustainedClockSpeedInGhz"
        ],
        "supported-boot-mode": ["SupportedBootModes"],
        "supported-root-device-type": ["SupportedRootDeviceTypes"],
        "supported-usage-class": ["SupportedUsageClasses"],
        "supported-virtualization-type": ["SupportedVirtualizationTypes"],
        "vcpu-info.default-cores": ["VCpuInfo", "DefaultCores"],
        "vcpu-info.default-threads-per-core": ["VCpuInfo", "DefaultThreadsPerCore"],
        "vcpu-info.default-vcpus": ["VCpuInfo", "DefaultVCpus"],
        "vcpu-info.valid-cores": ["VCpuInfo", "ValidCores"],
        "vcpu-info.valid-threads-per-core": ["VCpuInfo", "ValidThreadsPerCore"],
    }  # fmt: skip

    def __init__(self, name):
        self.name = name
        self.update(INSTANCE_TYPES[name])

    def __getattr__(self, name):
        return self[name]

    def __setattr__(self, name, value):
        self[name] = value

    def __repr__(self):
        return "<InstanceType: %s>" % self.name

    def get_filter_value(self, filter_name):
        def stringify(v):
            if isinstance(v, (bool, int)):
                return str(v).lower()
            elif isinstance(v, list):
                return [stringify(i) for i in v]
            return v

        path = self._filter_attributes.get(filter_name)
        if not path:
            raise InvalidFilter(filter_name, error_type="InvalidParameterValue")
        value = self
        for key in path:
            value = (value or {}).get(key)
        return stringify(value)


class InstanceTypeBackend:
    instance_types = list(map(InstanceType, INSTANCE_TYPES.keys()))

    def describe_instance_types(self, instance_types=None, filters=None):
        matches = self.instance_types
        if instance_types:
            matches = [t for t in matches if t.get("InstanceType") in instance_types]
            if len(instance_types) > len(matches):
                unknown_ids = set(instance_types) - set(
                    t.get("InstanceType") for t in matches
                )
                raise InvalidInstanceTypeError(
                    unknown_ids, error_type="InvalidInstanceType"
                )
        if filters:
            matches = generic_filter(filters, matches)
        return matches


class InstanceTypeOfferingBackend:
    def describe_instance_type_offerings(self, location_type=None, filters=None):
        location_type = location_type or "region"
        matches = INSTANCE_TYPE_OFFERINGS[location_type]
        matches = matches.get(self.region_name, [])
        matches = [
            o for o in matches if self.matches_filters(o, filters or {}, location_type)
        ]
        return matches

    def matches_filters(self, offering, filters, location_type):
        def matches_filter(key, values):
            if key == "location":
                if location_type in ("availability-zone", "availability-zone-id"):
                    return offering.get("Location") in values
                elif location_type == "region":
                    return any(
                        v for v in values if offering.get("Location").startswith(v)
                    )
                else:
                    return False
            elif key == "instance-type":
                return offering.get("InstanceType") in values
            else:
                return False

        return all([matches_filter(key, values) for key, values in filters.items()])
