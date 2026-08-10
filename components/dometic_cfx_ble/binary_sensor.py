import esphome.codegen as cg
from esphome.components import binary_sensor
from esphome.const import CONF_ID, CONF_TYPE
from . import CONF_DOMETIC_CFX_BLE_ID, entity_schema

DEPENDENCIES = ["dometic_cfx_ble"]

# We use the entity_schema from __init__.py and extend it with ESPHome's binary sensor schema
CONFIG_SCHEMA = entity_schema("binary_sensor").extend(binary_sensor.binary_sensor_schema())

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await binary_sensor.register_binary_sensor(var, config)

    # Link the binary sensor directly to the main Dometic Bluetooth Hub
    parent = await cg.get_variable(config[CONF_DOMETIC_CFX_BLE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_type(config[CONF_TYPE]))
