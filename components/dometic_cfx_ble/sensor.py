import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE
from . import dometic_cfx_ble_ns, CONF_DOMETIC_CFX_BLE_ID, entity_schema

DEPENDENCIES = ["dometic_cfx_ble"]

DometicCfxBleSensor = dometic_cfx_ble_ns.class_("DometicCfxBleSensor", sensor.Sensor, cg.PollingComponent)

CONFIG_SCHEMA = entity_schema("sensor").extend(sensor.sensor_schema(DometicCfxBleSensor)).extend({
    cv.GenerateID(): cv.declare_id(DometicCfxBleSensor)
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)

    parent = await cg.get_variable(config[CONF_DOMETIC_CFX_BLE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_topic(config[CONF_TYPE])) 
