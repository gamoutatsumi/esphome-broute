import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import sensor, uart
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_POWER,
    CONF_ENERGY,
    CONF_UPDATE_INTERVAL,
    UNIT_WATT,
    UNIT_KILOWATT_HOURS,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_ENERGY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

AUTO_LOAD = ["sensor", "uart"]
DEPENDENCIES = ["sensor", "uart"]
MULTI_CONF = True

CONF_RBID = "rbid"
CONF_REJOIN_COUNT = "rejoin_count"
CONF_REJOIN_TIMEOUT = "rejoin_timeout"
CONF_RESCAN_TIMEOUT = "rescan_timeout"
CONF_RESET_PIN = "reset_pin"
CONF_RESTART_TIMEOUT = "restart_timeout"

b_route_ns = cg.esphome_ns.namespace("b_route")
BRouteComponent = b_route_ns.class_("BRoute", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BRouteComponent),
            cv.Required(CONF_RBID): cv.All(cv.string_strict, cv.Length(min=32, max=32)),
            cv.Required(CONF_PASSWORD): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
            cv.Optional(CONF_POWER): sensor.sensor_schema(
                unit_of_measurement=UNIT_WATT,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
            ).extend({cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.positive_time_period_seconds}),
            cv.Optional(CONF_ENERGY): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOWATT_HOURS,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                accuracy_decimals=1,
            ).extend({cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.positive_time_period_seconds}),
            cv.Optional(CONF_REJOIN_COUNT, default=10): cv.int_range(min=0, max=127),
            cv.Optional(CONF_REJOIN_TIMEOUT, default="120s"): cv.positive_time_period_seconds,
            cv.Optional(CONF_RESCAN_TIMEOUT, default="240s"): cv.positive_time_period_seconds,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RESTART_TIMEOUT, default="360s"): cv.positive_time_period_seconds,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_rbid(config[CONF_RBID], config[CONF_PASSWORD]))
    cg.add(var.set_rejoin_miss_count(config[CONF_REJOIN_COUNT]))
    cg.add(var.set_rejoin_timeout_sec(config[CONF_REJOIN_TIMEOUT]))
    cg.add(var.set_rescan_timeout_sec(config[CONF_RESCAN_TIMEOUT]))
    cg.add(var.set_restart_timeout_sec(config[CONF_RESTART_TIMEOUT]))
    if reset_pin := config.get(CONF_RESET_PIN):
        pin = await cg.gpio_pin_expression(reset_pin)
        cg.add(var.set_reset_pin(pin))
    if c := config.get(CONF_POWER):
        s = await sensor.new_sensor(c)
        cg.add(var.set_power_sensor(s))
        cg.add(var.set_power_sensor_interval_sec(c[CONF_UPDATE_INTERVAL]))
    if c := config.get(CONF_ENERGY):
        s = await sensor.new_sensor(c)
        cg.add(var.set_energy_sensor(s))
        cg.add(var.set_energy_sensor_interval_sec(c[CONF_UPDATE_INTERVAL]))
