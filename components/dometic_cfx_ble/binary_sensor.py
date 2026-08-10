import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, CONF_TYPE
from . import dometic_cfx_ble_ns, CONF_DOMETIC_CFX_BLE_ID, entity_schema

DEPENDENCIES = ["dometic_cfx_ble"]

DometicCfxBleBinarySensor = dometic_cfx_ble_ns.class_("DometicCfxBleBinarySensor", binary_sensor.BinarySensor, cg.PollingComponent)

CONFIG_SCHEMA = entity_schema("binary_sensor").extend(binary_sensor.binary_sensor_schema(DometicCfxBleBinarySensor)).extend({
    cv.GenerateID(): cv.declare_id(DometicCfxBleBinarySensor)
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await binary_sensor.register_binary_sensor(var, config)

    parent = await cg.get_variable(config[CONF_DOMETIC_CFX_BLE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_topic(config[CONF_TYPE]))
    cg.add(parent.add_entity(config[CONF_TYPE], var))
