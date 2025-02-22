#include <zephyr/init.h>
#include <zephyr/usb/usb_device.h>
#include <usb_descriptor.h>
#include <usb_midi/usb_midi.h>
#include "usb_midi_types.h"
#include "usb_midi_macros.h"
#include "usb_midi_packet.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_midi, CONFIG_USB_MIDI_LOG_LEVEL);

#define LOG_DBG_PACKET(packet) LOG_DBG("%02x %02x %02x %02x | cable %02x | CIN %01x | %d MIDI bytes", \
									   packet.bytes[0], packet.bytes[1], packet.bytes[2], packet.bytes[3],            \
									   packet.cable_num, packet.cin, packet.num_midi_bytes)

USBD_CLASS_DESCR_DEFINE(primary, 0)
struct usb_midi_config usb_midi_config_data = {
	.ac_if = INIT_AC_IF,
	.ac_cs_if = INIT_AC_CS_IF,
	.ms_if = INIT_MS_IF,
	.ms_cs_if = INIT_MS_CS_IF,
	.out_jacks_emb = {
		#if CONFIG_USB_MIDI_NUM_OUTPUTS > 0
			LISTIFY(CONFIG_USB_MIDI_NUM_OUTPUTS, INIT_OUT_JACK, (, ), 0)
		#endif
	},
	.in_jacks_emb = {
		#if CONFIG_USB_MIDI_NUM_INPUTS > 0
			LISTIFY(CONFIG_USB_MIDI_NUM_INPUTS, INIT_IN_JACK, (, ), CONFIG_USB_MIDI_NUM_OUTPUTS)
		#endif
	},
	.element = INIT_ELEMENT,
	.in_ep = INIT_IN_EP,
	.in_cs_ep = {
		.bLength = sizeof(struct usb_midi_bulk_in_ep_descriptor),
		.bDescriptorType = USB_DESC_CS_ENDPOINT,
		.bDescriptorSubtype = 0x01,
		.bNumEmbMIDIJack = CONFIG_USB_MIDI_NUM_OUTPUTS,
		.BaAssocJackID = {
			#if CONFIG_USB_MIDI_NUM_OUTPUTS > 0
				LISTIFY(CONFIG_USB_MIDI_NUM_OUTPUTS, IDX_WITH_OFFSET, (, ), 1)
			#endif
		}
	},
	.out_ep = INIT_OUT_EP,
	.out_cs_ep = {
		.bLength = sizeof(struct usb_midi_bulk_out_ep_descriptor),
		.bDescriptorType = USB_DESC_CS_ENDPOINT,
		.bDescriptorSubtype = 0x01,
		.bNumEmbMIDIJack = CONFIG_USB_MIDI_NUM_INPUTS,
		.BaAssocJackID = {
			#if CONFIG_USB_MIDI_NUM_INPUTS > 0
				LISTIFY(CONFIG_USB_MIDI_NUM_INPUTS, IDX_WITH_OFFSET, (, ), 1 + CONFIG_USB_MIDI_NUM_OUTPUTS)
			#endif
		}
	}
};

static int temp_tx_buffer_size = 0;
static uint8_t temp_tx_buffer[EP_MAX_PACKET_SIZE];

static int usb_midi_is_available = false;
static struct usb_midi_cb_t user_callbacks = {
	.available_cb = NULL,
	.midi_message_cb = NULL,
	.tx_done_cb = NULL,
	.sysex_data_cb = NULL,
	.sysex_end_cb = NULL,
	.sysex_start_cb = NULL};

static void availability_changed(int is_available) {
	if (usb_midi_is_available == is_available) {
		return;
	}

	LOG_INF("device became %s ", is_available ? "available" : "unavailable");

	if (is_available) {
		temp_tx_buffer_size = 0;
	}
	if (user_callbacks.available_cb) {
		user_callbacks.available_cb(is_available);
	}
	usb_midi_is_available = is_available;
}

void usb_midi_register_callbacks(struct usb_midi_cb_t *cb)
{
	user_callbacks.available_cb = cb->available_cb;
	user_callbacks.midi_message_cb = cb->midi_message_cb;
	user_callbacks.tx_done_cb = cb->tx_done_cb;
	user_callbacks.sysex_start_cb = cb->sysex_start_cb;
	user_callbacks.sysex_data_cb = cb->sysex_data_cb;
	user_callbacks.sysex_end_cb = cb->sysex_end_cb;
}

static void midi_out_ep_cb(uint8_t ep, enum usb_dc_ep_cb_status_code ep_status)
{
	if (ep_status == USB_DC_EP_DATA_OUT) {
		uint8_t buf[4];
		uint32_t num_read_bytes = 1;
		while (num_read_bytes > 0)
		{
			int read_rc = usb_read(ep, buf, 4, &num_read_bytes);
			if (read_rc != 0) {
				LOG_ERR("Failed to read from endpoint %d with error %d", ep, read_rc);
				return;
			}
			if (num_read_bytes == 0)
			{
				break;
			}
			struct usb_midi_packet_t packet;
			enum usb_midi_error_t error = usb_midi_packet_from_usb_bytes(buf, &packet);

			if (error != USB_MIDI_SUCCESS)
			{
				LOG_ERR("Failed to read packet with error %d", error);
			}
			else
			{
				LOG_DBG_PACKET(packet);
				struct usb_midi_parse_cb_t parse_cb = {
					.message_cb = user_callbacks.midi_message_cb,
					.sysex_data_cb = user_callbacks.sysex_data_cb,
					.sysex_end_cb = user_callbacks.sysex_end_cb,
					.sysex_start_cb = user_callbacks.sysex_start_cb};
				error = usb_midi_parse_packet(packet.bytes, &parse_cb);
				if (error != USB_MIDI_SUCCESS)
				{
					LOG_ERR("Failed to parse packet with error %d", error);
				}
			}
		}
	} else {
		// printk("USB ep status %d\n", ep_status);
	}
}

static void midi_in_ep_cb(uint8_t ep, enum usb_dc_ep_cb_status_code ep_status)
{
	if (ep_status == USB_DC_EP_DATA_IN && user_callbacks.tx_done_cb)
	{
		user_callbacks.tx_done_cb();
	}
}

static struct usb_ep_cfg_data midi_ep_cfg[] = {
	{
		.ep_cb = midi_in_ep_cb,
		.ep_addr = 0x81,
	},
	{
		.ep_cb = midi_out_ep_cb,
		.ep_addr = 0x01,
	}};

void usb_status_callback(struct usb_cfg_data *cfg,
						 enum usb_dc_status_code cb_status,
						 const uint8_t *param)
{
	switch (cb_status)
	{
	/** USB error reported by the controller */
	case USB_DC_ERROR:
		LOG_DBG("USB_DC_ERROR");
		break;
	/** USB reset */
	case USB_DC_RESET:
		LOG_DBG("USB_DC_RESET");
		break;
	/** USB connection established, hardware enumeration is completed */
	case USB_DC_CONNECTED:
		LOG_DBG("USB_DC_CONNECTED");
		break;
	/** USB configuration done */
	case USB_DC_CONFIGURED:
		LOG_DBG("USB_DC_CONFIGURED");
		availability_changed(1);
		break;
	/** USB connection lost */
	case USB_DC_DISCONNECTED:
		LOG_DBG("USB_DC_DISCONNECTED");
		break;
	/** USB connection suspended by the HOST */
	case USB_DC_SUSPEND:
		availability_changed(0);
		break;
	/** USB connection resumed by the HOST */
	case USB_DC_RESUME:
		LOG_DBG("USB_DC_RESUME");
		break;
	/** USB interface selected */
	case USB_DC_INTERFACE:
		LOG_DBG("USB_DC_INTERFACE");
		break;
	/** Set Feature ENDPOINT_HALT received */
	case USB_DC_SET_HALT:
		LOG_DBG("USB_DC_SET_HALT");
		break;
	/** Clear Feature ENDPOINT_HALT received */
	case USB_DC_CLEAR_HALT:
		LOG_DBG("USB_DC_CLEAR_HALT");
		break;
	/** Start of Frame received */
	case USB_DC_SOF:
		LOG_DBG("USB_DC_SOF");
		break;
	/** Initial USB connection status */
	case USB_DC_UNKNOWN:
		LOG_DBG("USB_DC_UNKNOWN");
		break;
	}
}

int usb_midi_tx(uint8_t cable_number, uint8_t *midi_bytes)
{
	struct usb_midi_packet_t packet;
	enum usb_midi_error_t error = usb_midi_packet_from_midi_bytes(midi_bytes, cable_number, &packet);
	if (error != USB_MIDI_SUCCESS)
	{
		LOG_ERR("Building packet from MIDI bytes %02x %02x %02x failed with error %d", midi_bytes[0], midi_bytes[1], midi_bytes[2], error);
		return -EINVAL;
	}
	LOG_DBG_PACKET(packet);
	int write_result = usb_write(0x81, packet.bytes, 4, NULL);
	return write_result;
}

int usb_midi_tx_buffer_is_full() {
	return temp_tx_buffer_size == EP_MAX_PACKET_SIZE;
}

int usb_midi_tx_buffer_add(uint8_t cable_number, uint8_t* midi_bytes) {
	if (usb_midi_tx_buffer_is_full()) {
		return -1;
	}

	struct usb_midi_packet_t packet;
	enum usb_midi_error_t error = usb_midi_packet_from_midi_bytes(midi_bytes, cable_number, &packet);
	if (error != USB_MIDI_SUCCESS)
	{
		LOG_ERR("Building packet from MIDI bytes %02x %02x %02x failed with error %d", midi_bytes[0], midi_bytes[1], midi_bytes[2], error);
		return -EINVAL;
	}

	for (int i = 0; i < 4; i++) {
		temp_tx_buffer[temp_tx_buffer_size] = packet.bytes[i];
		temp_tx_buffer_size++;
	}
	return 0;
}

int usb_midi_tx_buffer_send() {
	if (temp_tx_buffer_size > 0) {
		int write_result = usb_write(0x81, temp_tx_buffer, temp_tx_buffer_size, NULL);
		if (write_result == 0) {
			temp_tx_buffer_size = 0;
		}
		return write_result;
	}
	return 0;
}

USBD_DEFINE_CFG_DATA(usb_midi_config) = {
	.usb_device_description = NULL,
	.interface_config = NULL,
	.interface_descriptor = &usb_midi_config_data.ac_if,
	.cb_usb_status = usb_status_callback,
	.interface = {
		.class_handler = NULL,
		.custom_handler = NULL,
		.vendor_handler = NULL,
	},
	.num_endpoints = ARRAY_SIZE(midi_ep_cfg),
	.endpoint = midi_ep_cfg,
};