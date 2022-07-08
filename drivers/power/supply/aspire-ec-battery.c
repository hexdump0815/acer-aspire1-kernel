// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>

#define ACER_FG_STATIC  0x08
#define ACER_FG_DYNAMIC 0x07

#define ACER_FG_FLAG_PRESENT		BIT(0)
#define ACER_FG_FLAG_FULL		BIT(1)
#define ACER_FG_FLAG_DISCHARGING	BIT(2)
#define ACER_FG_FLAG_CHARGING		BIT(3)

struct aspire_battery {
	struct i2c_client	*client;
	struct power_supply	*psy;
};

struct fg_static_data {
	u8 unk1;
	u8 flags;
	__le16 unk2;
	__le16 voltage_design;
	__le16 capacity_full;
	__le16 unk3;
	__le16 serial;
	u8 model_id;
	u8 vendor_id;
};

struct fg_dynamic_data {
	u8 unk1;
	u8 flags;
	u8 unk2;
	__be16 capacity_now;
	__be16 voltage_now;
	__be16 current_now;
	__be16 unk3;
	__be16 unk4;
};

static int acpi_gsb_i2c_read_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[2];
	int ret;
	u8 *buffer;

	buffer = kzalloc(data_len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = data_len;
	msgs[1].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		dev_err(&client->adapter->dev, "i2c read failed\n");
	else
		memcpy(data, buffer, data_len);

	kfree(buffer);
	return ret;
}

static int aspire_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct aspire_battery *battery = power_supply_get_drvdata(psy);
	struct fg_static_data sdat = {};
	struct fg_dynamic_data ddat = {};

	acpi_gsb_i2c_read_bytes(battery->client, ACER_FG_STATIC, (u8*)&sdat, sizeof(sdat));
	acpi_gsb_i2c_read_bytes(battery->client, ACER_FG_DYNAMIC, (u8*)&ddat, sizeof(ddat));

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		if (ddat.flags & ACER_FG_FLAG_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (ddat.flags & ACER_FG_FLAG_DISCHARGING)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (ddat.flags & ACER_FG_FLAG_FULL)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = be16_to_cpu(ddat.voltage_now) * 1000;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = le16_to_cpu(sdat.voltage_design) * 1000;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = (s16)be16_to_cpu(ddat.current_now) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = be16_to_cpu(ddat.capacity_now) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = le16_to_cpu(sdat.capacity_full) * 1000;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = be16_to_cpu(ddat.capacity_now) * 100 / le16_to_cpu(sdat.capacity_full);
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property aspire_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CAPACITY,
};

static const struct power_supply_desc aspire_battery_desc = {
	.name		= "aspire-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.get_property	= aspire_battery_get_property,
	.properties	= aspire_battery_props,
	.num_properties	= ARRAY_SIZE(aspire_battery_props),
};

static int aspire_battery_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct power_supply_config psy_cfg = {};
	struct aspire_battery *battery;

	battery = devm_kzalloc(&client->dev, sizeof(*battery), GFP_KERNEL);
	if (!battery)
		return -ENOMEM;

	battery->client = client;

	i2c_set_clientdata(client, battery);
	psy_cfg.drv_data = battery;

	battery->psy = power_supply_register(&client->dev,
					     &aspire_battery_desc, &psy_cfg);
	if (IS_ERR(battery->psy)) {
		dev_err(&client->dev, "Failed to register power supply\n");
		return PTR_ERR(battery->psy);
	}

	return 0;
}

static int aspire_battery_remove(struct i2c_client *client)
{
	struct aspire_battery *battery = i2c_get_clientdata(client);

	power_supply_unregister(battery->psy);

	return 0;
}

static const struct i2c_device_id aspire_battery_id[] = {
	{ "aspire1-battery", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aspire_battery_id);

static const struct of_device_id aspire_battery_of_match[] = {
	{ .compatible = "acer,aspire1-battery", },
	{ }
};
MODULE_DEVICE_TABLE(of, aspire_battery_of_match);

static struct i2c_driver aspire_battery_driver = {
	.driver = {
		.name = "aspire-battery",
		.of_match_table = aspire_battery_of_match,
	},
	.probe = aspire_battery_probe,
	.remove = aspire_battery_remove,
	.id_table = aspire_battery_id,
};
module_i2c_driver(aspire_battery_driver);

MODULE_DESCRIPTION("Some other fuel gauge driver");
MODULE_AUTHOR("Nikita Travkin <nikita@trvn.ru>");
MODULE_LICENSE("GPL");
