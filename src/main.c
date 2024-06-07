#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <usb_midi/usb_midi.h>

#define LED_FLASH_DURATION_MS         60

/* Cable number to use for sysex test messages */
#define SYSEX_TX_TEST_MSG_CABLE_NUM   0
/* Size in bytes of outgoing sysex test messages */
#define SYSEX_TX_TEST_MSG_SIZE        170000

/* Echo incoming sysex messages? */
#define SYSEX_ECHO_ENABLED 			  0
/* Echo at most this many bytes of incoming sysex messages */
#define SYSEX_ECHO_MAX_LENGTH         1024

/* Send note on/off periodically? */
#define TX_PERIODIC_NOTE_ENABLED      0
#define TX_PERIODIC_NOTE_INTERVAL_MS  500
#define TX_PERIODIC_NOTE_NUMBER	      69
#define TX_PERIODIC_NOTE_VELOCITY     0x7f

struct k_work button_press_work;
struct k_work event_tx_work;
struct k_work_delayable rx_led_off_work;
struct k_work_delayable tx_led_off_work;
static void send_next_sysex_chunk();

/************************ App state ************************/
struct sample_app_state_t {
	int usb_midi_is_available;
	int tx_note_off;
	
	int sysex_rx_byte_count;
	uint8_t sysex_rx_bytes[SYSEX_ECHO_MAX_LENGTH];
	int64_t sysex_rx_start_time;

	int sysex_tx_byte_count;
	int sysex_tx_in_progress;
	int64_t sysex_tx_start_time;
};

static struct sample_app_state_t sample_app_state = {.usb_midi_is_available = 0,
							 .sysex_rx_byte_count = 0,
							 .sysex_rx_start_time = 0,
						     .sysex_tx_byte_count = 0,
							 .sysex_tx_start_time = 0,
						     .sysex_tx_in_progress = 0,
						     .tx_note_off = 0,
							 .sysex_rx_byte_count = 0
							 };

/************************ LEDs ************************/
static struct gpio_dt_spec usb_midi_available_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_dt_spec midi_rx_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static struct gpio_dt_spec midi_tx_led = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static void init_leds()
{
	gpio_pin_configure_dt(&usb_midi_available_led, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&usb_midi_available_led, 0);

	gpio_pin_configure_dt(&midi_rx_led, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&midi_rx_led, 0);

	gpio_pin_configure_dt(&midi_tx_led, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&midi_tx_led, 0);
}

static void set_usb_midi_available_led(int is_available)
{
	gpio_pin_set_dt(&usb_midi_available_led, is_available);
}

static void flash_tx_led()
{
	gpio_pin_set_dt(&midi_tx_led, 1);
	k_work_cancel_delayable(&tx_led_off_work);
	k_work_schedule(&tx_led_off_work, Z_TIMEOUT_MS(LED_FLASH_DURATION_MS));
}

static void flash_rx_led()
{
	gpio_pin_set_dt(&midi_rx_led, 1);
	k_work_cancel_delayable(&rx_led_off_work);
	k_work_schedule(&rx_led_off_work, Z_TIMEOUT_MS(LED_FLASH_DURATION_MS));
}

/****************** Work queue callbacks ******************/

void on_event_tx(struct k_work *item)
{
	if (sample_app_state.usb_midi_is_available && !sample_app_state.sysex_tx_in_progress) {
		uint8_t msg[3] = {sample_app_state.tx_note_off ? 0x80 : 0x90, TX_PERIODIC_NOTE_NUMBER,
				  TX_PERIODIC_NOTE_VELOCITY};
		flash_tx_led();
		usb_midi_tx(0, msg);
		sample_app_state.tx_note_off = !sample_app_state.tx_note_off;
	}
}

void on_button_press(struct k_work *item)
{
	if (sample_app_state.usb_midi_is_available && !sample_app_state.sysex_tx_in_progress) {
		/* Send the first chunk of a sysex message that is too large
		   to be sent at once. Use the tx done callback to send the
		   next chunk repeatedly until done. */
		flash_tx_led();
		sample_app_state.sysex_tx_in_progress = 1;
		sample_app_state.sysex_tx_byte_count = 0;
		sample_app_state.sysex_tx_start_time = k_uptime_get();
		send_next_sysex_chunk();
	}
}

void on_rx_led_off(struct k_work *item)
{
	gpio_pin_set_dt(&midi_rx_led, 0);
}

void on_tx_led_off(struct k_work *item)
{
	gpio_pin_set_dt(&midi_tx_led, 0);
}

/************************ Buttons ************************/

static struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback button_cb_data;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&button_press_work);
}

static void init_button()
{
	__ASSERT_NO_MSG(device_is_ready(button.port));
	int ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	__ASSERT_NO_MSG(ret == 0);
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	__ASSERT_NO_MSG(ret == 0);

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb_data);
	__ASSERT_NO_MSG(ret == 0);
}

/****************** USB MIDI callbacks ******************/

static void midi_message_cb(uint8_t *bytes, uint8_t num_bytes, uint8_t cable_num)
{
	/*printk("rx non-sysex, cable %d: ", cable_num);
	for (int i = 0; i < num_bytes; i++) {
			printk("%02x ", bytes[i]);
	}
	printk("\n"); */
	flash_rx_led();
}

static void sysex_start_cb(uint8_t cable_num)
{
	sample_app_state.sysex_rx_start_time = k_uptime_get();
	sample_app_state.sysex_rx_byte_count = 0;
	sample_app_state.sysex_rx_bytes[0] = 0xf0; 
	sample_app_state.sysex_rx_byte_count = 1;
	flash_rx_led();
}

static void sysex_data_cb(uint8_t *data_bytes, uint8_t num_data_bytes, uint8_t cable_num)
{
	sample_app_state.sysex_rx_byte_count += num_data_bytes;
	// flash_rx_led();
}

static void sysex_end_cb(uint8_t cable_num)
{
	u_int64_t dt = k_uptime_get() - sample_app_state.sysex_rx_start_time;
	float bytes_per_s = ((float)sample_app_state.sysex_rx_byte_count) / (0.001 * (float)dt);
	printk("sysex rx done, cable %d: %d bytes in %d ms, %d bytes/s\n", cable_num,
		sample_app_state.sysex_rx_byte_count + 2, (int)dt, (int)bytes_per_s);
	flash_rx_led();
}

static void usb_midi_available_cb(int is_available)
{
	sample_app_state.usb_midi_is_available = is_available;
	set_usb_midi_available_led(is_available);
	if (is_available) {
		sample_app_state.tx_note_off = 0;
	}
}

static void send_next_sysex_chunk() {
	__ASSERT_NO_MSG(sample_app_state.sysex_tx_in_progress);

	while (1) {
		if (usb_midi_tx_buffer_is_full()) {
			// tx packet is full. send it. 
			usb_midi_tx_buffer_send();
			// nothing further for now. wait for tx done callback before
			// filling the next packet.
			break;
		}

		uint8_t chunk[3] = {0, 0, 0};
		for (int i = 0; i < 3; i++) {
			if (sample_app_state.sysex_tx_byte_count == 0) {
				chunk[i] = 0xf0;
			}
			else if (sample_app_state.sysex_tx_byte_count == SYSEX_TX_TEST_MSG_SIZE - 1) {
				chunk[i] = 0xf7;
			} 
			else {
				chunk[i] = sample_app_state.sysex_tx_byte_count % 128;
			}				
			sample_app_state.sysex_tx_byte_count++;

			if (sample_app_state.sysex_tx_byte_count == SYSEX_TX_TEST_MSG_SIZE) {
				break;
			}
		}

		// Add three byte sysex chunk to the current tx packet 
		usb_midi_tx_buffer_add(SYSEX_TX_TEST_MSG_CABLE_NUM, chunk);

		if (sample_app_state.sysex_tx_byte_count == SYSEX_TX_TEST_MSG_SIZE) {
			// No more data to add to tx packet. Send it, then we're done.
			usb_midi_tx_buffer_send();
			flash_tx_led();
			u_int64_t dt = k_uptime_get() - sample_app_state.sysex_tx_start_time;
			float bytes_per_s = ((float)sample_app_state.sysex_tx_byte_count) / (0.001 * (float)dt);
			printk("sysex tx done, cable %d: %d bytes in %d ms, %d bytes/s\n", SYSEX_TX_TEST_MSG_CABLE_NUM,
			sample_app_state.sysex_tx_byte_count + 2, (int)dt, (int)bytes_per_s);
			sample_app_state.sysex_tx_in_progress = 0;
			break;
		}
	}
}

static void usb_midi_tx_done_cb()
{
	if (sample_app_state.sysex_tx_in_progress) {
		send_next_sysex_chunk();
	}
}

/****************** Sample app ******************/
void main(void)
{
	init_leds();
	init_button();

	k_work_init(&button_press_work, on_button_press);
	k_work_init(&event_tx_work, on_event_tx);
	k_work_init_delayable(&rx_led_off_work, on_rx_led_off);
	k_work_init_delayable(&tx_led_off_work, on_tx_led_off);

	/* Register USB MIDI callbacks */
	struct usb_midi_cb_t callbacks = {.available_cb = usb_midi_available_cb,
					  .tx_done_cb = usb_midi_tx_done_cb,
					  .midi_message_cb = midi_message_cb,
					  .sysex_data_cb = sysex_data_cb,
					  .sysex_end_cb = sysex_end_cb,
					  .sysex_start_cb = sysex_start_cb};
	usb_midi_register_callbacks(&callbacks);

	/* Init USB */
	int enable_rc = usb_enable(NULL);
	__ASSERT(enable_rc == 0, "Failed to enable USB");

	/* Send MIDI messages periodically */
	while (1) {
		if (TX_PERIODIC_NOTE_ENABLED) {
			k_work_submit(&event_tx_work);
		}
		k_msleep(TX_PERIODIC_NOTE_INTERVAL_MS);
	}
}