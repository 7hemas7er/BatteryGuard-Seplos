# Modified from esphome-seplos-bms by Sebastian Syska (@syssi),
# licensed under the Apache License, Version 2.0.
#
# Changes in this fork: adds the warning / protection / system_fault binary
# sensors fed by the telesignalization frame (CID2 0x44).
#
# See NOTICE and README.md for details.

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_SEPLOS_BMS_ID, SEPLOS_BMS_COMPONENT_SCHEMA

DEPENDENCIES = ["seplos_bms"]

CODEOWNERS = ["@syssi"]

CONF_ONLINE_STATUS = "online_status"
CONF_WARNING = "warning"
CONF_PROTECTION = "protection"
CONF_SYSTEM_FAULT = "system_fault"

# key: binary_sensor_schema kwargs
BINARY_SENSOR_DEFS = {
    CONF_ONLINE_STATUS: {
        "device_class": DEVICE_CLASS_CONNECTIVITY,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
    # Aggregated alarm flags decoded from the telesignalization frame (CID2 0x44)
    CONF_WARNING: {"device_class": DEVICE_CLASS_PROBLEM},
    CONF_PROTECTION: {"device_class": DEVICE_CLASS_PROBLEM},
    CONF_SYSTEM_FAULT: {
        "device_class": DEVICE_CLASS_PROBLEM,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
}

CONFIG_SCHEMA = SEPLOS_BMS_COMPONENT_SCHEMA.extend(
    {
        cv.Optional(key): binary_sensor.binary_sensor_schema(**kwargs)
        for key, kwargs in BINARY_SENSOR_DEFS.items()
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SEPLOS_BMS_ID])
    for key in BINARY_SENSOR_DEFS:
        if key in config:
            conf = config[key]
            sens = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_binary_sensor")(sens))
