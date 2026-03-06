#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

#include "wifi_utilities.h"
#include "secret/wifi_pswd.h"
#include "zephyr/net/net_ip.h"
#include "udp_socket.h"

#include <zephyr/logging/log.h>

static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED2_NODE, gpios);

K_MSGQ_DEFINE(udp_msgq, sizeof(udp_message_t), 10, 1);
K_FIFO_DEFINE(udp_fifo);

#define LED_TURN_OFF() do { gpio_pin_set_dt(&led_red, 0); gpio_pin_set_dt(&led_green, 0); gpio_pin_set_dt(&led_blue, 0); } while(0)
#define LED_TURN_RED() do { gpio_pin_set_dt(&led_red, 1); gpio_pin_set_dt(&led_green, 0); gpio_pin_set_dt(&led_blue, 0); } while(0)
#define LED_TURN_GREEN() do { gpio_pin_set_dt(&led_red, 0); gpio_pin_set_dt(&led_green, 1); gpio_pin_set_dt(&led_blue, 0); } while(0)
#define LED_TURN_BLUE() do { gpio_pin_set_dt(&led_red, 0); gpio_pin_set_dt(&led_green, 0); gpio_pin_set_dt(&led_blue, 1); } while(0)
#define LED_TURN_YELLOW() do { gpio_pin_set_dt(&led_red, 1); gpio_pin_set_dt(&led_green, 1); gpio_pin_set_dt(&led_blue, 0); } while(0)
#define LED_TURN_CYAN() do { gpio_pin_set_dt(&led_red, 0); gpio_pin_set_dt(&led_green, 1); gpio_pin_set_dt(&led_blue, 1); } while(0)
#define LED_TURN_MAGENTA() do { gpio_pin_set_dt(&led_red, 1); gpio_pin_set_dt(&led_green, 0); gpio_pin_set_dt(&led_blue, 1); } while(0)
#define LED_TURN_WHITE() do { gpio_pin_set_dt(&led_red, 1); gpio_pin_set_dt(&led_green, 1); gpio_pin_set_dt(&led_blue, 1); } while(0)


LOG_MODULE_REGISTER(udp_socket_demo, LOG_LEVEL_DBG);

K_THREAD_DEFINE(udp_thread, CONFIG_UDP_SOCKET_THREAD_STACK_SIZE,
                run_udp_socket_demo, NULL, NULL, NULL,
                SOCKET_THREAD_PRIORITY, 0, 0);

static const char *state_to_string(communication_state_t state)
{
	switch (state) {
	case COMM_WIFI_CONNECTING:
		return "COMM_WIFI_CONNECTING";
	case COMM_WAITING_FOR_IP:
		return "COMM_WAITING_FOR_IP";
	case COMM_ESTABLISHING_SERVER:
		return "COMM_CONNECTING_TO_SERVER";
	case COMM_CONNECTING_TO_CLIENT:
		return "COMM_CONNECTING_TO_CLIENT";
	case COMM_SENDING_MESSAGES:
		return "COMM_SENDING_MESSAGES";
	case COMM_FAILURE:
		return "COMM_FAILURE";
	case COMM_CLEANUP:
		return "COMM_CLEANUP";
	case COMM_DONE:
		return "COMM_DONE";
	default:
		return "COMM_UNKNOWN";
	}
}

static communication_state_t state_wifi_connecting(communication_context_t *ctx)
{
	LED_TURN_MAGENTA();
	if (my_wifi_init() != 0) {
		LOG_ERR("Failed to initialize WiFi module");
		ctx->failure_from_state = COMM_WIFI_CONNECTING;
		return COMM_FAILURE;
	}

	LOG_INF("Connecting to WiFi...");

	if (wifi_connect(BITCRAZE_SSID, BITCRAZE_PASSWORD)) {
		LOG_ERR("Failed to connect to WiFi");
		ctx->failure_from_state = COMM_WIFI_CONNECTING;
		return COMM_FAILURE;
	}

	ctx->wifi_connected = true;
	return COMM_WAITING_FOR_IP;
}

static communication_state_t state_waiting_for_ip(communication_context_t *ctx)
{
	LED_TURN_CYAN();

	if (wifi_wait_for_ip_addr(ctx->ip_addr) != 0) {
		LOG_ERR("Failed while waiting for IPv4 address");
		ctx->failure_from_state = COMM_WAITING_FOR_IP;
		return COMM_FAILURE;
	}

	return COMM_ESTABLISHING_SERVER;
}

static communication_state_t state_establishing_server(communication_context_t *ctx)
{
	int ret;

	LED_TURN_YELLOW();

	// init socket
	ctx->sock_fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ctx->sock_fd < 0) {
		LOG_ERR("Could not create socket (errno=%d)", errno);
		ctx->failure_from_state = COMM_ESTABLISHING_SERVER;
		return COMM_FAILURE;
	}
	ctx->socket_open = true;

	// set port
	memset(&ctx->server_addr, 0, sizeof(ctx->server_addr));
	ctx->server_addr.sin_family = AF_INET;
	ctx->server_addr.sin_port = htons(SERVER_PORT);
	ctx->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	// bind socket to port
	ret = zsock_bind(ctx->sock_fd, (struct sockaddr *) &ctx->server_addr, sizeof(ctx->server_addr));
	if (ret < 0) {
		LOG_ERR("Could not establish server (errno=%d)", errno);
		ctx->failure_from_state = COMM_ESTABLISHING_SERVER;
		return COMM_FAILURE;
	}

	printk("[Server] listening at %s:%d", ctx->ip_addr, SERVER_PORT);
	return COMM_CONNECTING_TO_CLIENT;
}

static communication_state_t state_connecting_to_client(communication_context_t *ctx)
{
	int ret;

	ctx->failure_from_state = COMM_CONNECTING_TO_CLIENT;

	/* Wait for a start message so we learn the client's address and port */
	LED_TURN_WHITE();
	ctx->client_addr_len = sizeof(ctx->client_addr);
	ret = zsock_recvfrom(ctx->sock_fd, ctx->buffer, sizeof(ctx->buffer) - 1, 0,
			     &ctx->client_addr, &ctx->client_addr_len);
	if (ret <= 0) {
		LOG_ERR("recvfrom failed waiting for start message (errno=%d)", errno);
		return COMM_FAILURE;
	}
	ctx->buffer[ret] = '\0';

	if (net_addr_ntop(ctx->client_addr.sa_family,
			  &((struct sockaddr_in *)&ctx->client_addr)->sin_addr,
			  ctx->client_ip_addr, NET_IPV4_ADDR_LEN) == NULL) {
		LOG_ERR("Failed to convert client address to string");
	}
	printk("[Server] Received start command '%s' from %s",
		ctx->buffer, ctx->client_ip_addr);

	udp_message_t message;
	message.enabled = 1;
	k_msgq_put(&udp_msgq, &message, K_NO_WAIT);
	return COMM_SENDING_MESSAGES;
}


static communication_state_t state_sending_messages(communication_context_t *ctx)
{
	int ret;

	ctx->failure_from_state = COMM_SENDING_MESSAGES;

	udp_data_t *udp_data_ptr;

	for (;;) {
		/* Receive frame size */
		LED_TURN_GREEN();
		udp_data_ptr = k_fifo_get(&udp_fifo, K_FOREVER);

		LED_TURN_RED();
		ret = zsock_sendto(ctx->sock_fd, &udp_data_ptr->packet, sizeof(iris_packet_t), 0,
					&ctx->client_addr, ctx->client_addr_len);
		if (ret < 0) {
			LOG_ERR("UDP sendto failed frame=%u packet=%u (errno=%d)",
				udp_data_ptr->packet.frame_nmbr, udp_data_ptr->packet.packet_idx, errno);
			return COMM_FAILURE;
		}
	}

	return COMM_CLEANUP;
}

static communication_state_t state_failure(communication_context_t *ctx)
{
	LOG_ERR("[Failure] Called from: %s", state_to_string(ctx->failure_from_state));
	LOG_ERR("[Failure] Context: sock_fd=%d socket_open=%d wifi_connected=%d exit_code=%d",
			ctx->sock_fd,
			ctx->socket_open,
			ctx->wifi_connected,
			ctx->exit_code);

	ctx->exit_code = -1;
	while (1) {
		/* Unique failure indication: blinking red */
		LED_TURN_RED();
		k_sleep(K_SECONDS(1));
		LED_TURN_OFF();
		k_sleep(K_SECONDS(1));
	}
	return COMM_CLEANUP;
}

static communication_state_t state_cleanup(communication_context_t *ctx)
{
	/* Unique cleanup indication: blinking blue */
	for (int i = 0; i < 3; i++) {
		LED_TURN_BLUE();
		k_sleep(K_MSEC(150));
		LED_TURN_OFF();
		k_sleep(K_MSEC(150));
	}

	if (ctx->socket_open) {
		zsock_close(ctx->sock_fd);
		ctx->socket_open = false;
		LOG_INF("[Server] Closed");
	}

	if (ctx->wifi_connected) {
		wifi_disconnect();
		ctx->wifi_connected = false;
	}

	return COMM_DONE;
}

int run_udp_socket_demo(void)
{
	communication_state_t state = COMM_WIFI_CONNECTING;

	/* static so context does not live on the thread stack */
	static communication_context_t ctx = {
		.sock_fd = -1,
		.wifi_connected = false,
		.socket_open = false,
		.exit_code = 0,
		.failure_from_state = COMM_FAILURE,
	};

	int ret = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
    ret |= gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
    ret |= gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        state = COMM_FAILURE;
    }
	LED_TURN_OFF();

	LOG_INF("UDP SPI STREAMER");

	while (state != COMM_DONE) {
		LOG_DBG("State: %s", state_to_string(state));
		switch (state) {
		case COMM_WIFI_CONNECTING:
			state = state_wifi_connecting(&ctx);
			break;
		case COMM_WAITING_FOR_IP:
			state = state_waiting_for_ip(&ctx);
			break;
		case COMM_ESTABLISHING_SERVER:
			state = state_establishing_server(&ctx);
			break;
		case COMM_CONNECTING_TO_CLIENT:
			state = state_connecting_to_client(&ctx);
			break;
		case COMM_SENDING_MESSAGES:
			state = state_sending_messages(&ctx);
			break;
		case COMM_FAILURE:
			state = state_failure(&ctx);
			break;
		case COMM_CLEANUP:
			state = state_cleanup(&ctx);
			break;
		case COMM_DONE:
			break;
		default:
			ctx.failure_from_state = state;
			state = COMM_FAILURE;
			break;
		}
	}
	LED_TURN_OFF();

	return ctx.exit_code;
}

struct k_fifo *get_udp_fifo(void)
{
	return &udp_fifo;
}

struct k_msgq *get_udp_msgq(void)
{
	return &udp_msgq;
}
